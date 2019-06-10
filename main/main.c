#include "freertos/FreeRTOS.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "esp_flash_data_types.h"
#include "rom/crc.h"

#include <string.h>
#include <stdlib.h>

#include "odroid_sdcard.h"
#include "odroid_display.h"
#include "input.h"

#include "../components/ugui/ugui.h"

#define ALIGN_ADDRESS(val, alignment) (((val & (alignment-1)) != 0) ? (val & ~(alignment-1)) + alignment : val)

#define ESP_PARTITION_TABLE_OFFSET CONFIG_PARTITION_TABLE_OFFSET /* Offset of partition table. Backwards-compatible name.*/
#define ESP_PARTITION_TABLE_MAX_LEN 0xC00 /* Maximum length of partition table data */
#define ESP_PARTITION_TABLE_MAX_ENTRIES (ESP_PARTITION_TABLE_MAX_LEN / sizeof(esp_partition_info_t)) /* Maximum length of partition table data, including terminating entry */

#define PART_SUBTYPE_FACTORY 0x00
#define PART_SUBTYPE_FACTORY_DATA 0xFE

#define FLASH_SIZE (16 * 1024 * 1024)

#define TILE_WIDTH (86)
#define TILE_HEIGHT (48)

#define APP_MAGIC 0x1207

#define APP_SORT_OFFSET 0x00
#define APP_SORT_SEQUENCE 0x01
#define APP_SORT_DESCRIPTION 0x02
#define APP_SORT_DIR_ASC 0x00
#define APP_SORT_DIR_DESC 0x10

#define FLASH_BLOCK_SIZE (64 * 1024)
#define ERASE_BLOCK_SIZE (4 * 1024)

#ifndef COMPILEDATE
#define COMPILEDATE "none"
#endif

#ifndef GITREV
#define GITREV "none"
#endif

#define VERSION COMPILEDATE "-" GITREV

#define FIRMWARE_HEADER_SIZE (24)
#define FIRMWARE_DESCRIPTION_SIZE (40)
#define FIRMWARE_PARTS_MAX (20)
#define FIRMWARE_TILE_SIZE (TILE_WIDTH * TILE_HEIGHT)

const char* SD_CARD = "/sd";
//const char* HEADER = "ODROIDGO_FIRMWARE_V00_00";
const char* HEADER_V00_01 = "ODROIDGO_FIRMWARE_V00_01";


// <partition type=0x00 subtype=0x00 label='name' flags=0x00000000 length=0x00000000>
// 	<data length=0x00000000>
// 		[...]
// 	</data>
// </partition>
typedef struct
{
    uint8_t type;
    uint8_t subtype;
    uint8_t _reserved0;
    uint8_t _reserved1;

    uint8_t label[16];

    uint32_t flags;
    uint32_t length;
    uint32_t dataLength;
} odroid_partition_t; // __packed

typedef struct
{
    uint16_t magic;
    uint16_t flags;
    uint32_t startOffset;
    uint32_t endOffset;
    char     description[FIRMWARE_DESCRIPTION_SIZE];
    char     filename[FIRMWARE_DESCRIPTION_SIZE];
    uint16_t tile[FIRMWARE_TILE_SIZE];
    odroid_partition_t parts[FIRMWARE_PARTS_MAX];
    uint8_t parts_count;
    uint8_t _reserved0;
    uint16_t installSeq;
} odroid_app_t; // __packed

typedef struct
{
    char header[FIRMWARE_HEADER_SIZE];
    char description[FIRMWARE_DESCRIPTION_SIZE];
    uint16_t tile[FIRMWARE_TILE_SIZE];
} odroid_fw_header_t;

typedef struct
{
    odroid_fw_header_t fileHeader;
    odroid_partition_t parts[FIRMWARE_PARTS_MAX];
    uint8_t parts_count;
    size_t flashSize;
    size_t fileSize;
    size_t dataOffset;
    uint32_t checksum;
} odroid_fw_t;

typedef struct
{
    size_t offset;
    size_t size;    
} odroid_flash_block_t;
// ------

static odroid_app_t* apps;
static int apps_count = -1;
static int apps_max = 4;
static int nextInstallSeq = 0;
static int displayOrder = 0;

static esp_partition_info_t* partition_data;
static int partition_count = -1;
static int startTableEntry = -1;
static int startFlashAddress = -1;

static odroid_fw_t *fwInfoBuffer;
static uint8_t *dataBuffer;

uint16_t fb[320 * 240];
UG_GUI gui;
char tempstring[512];

#define ITEM_COUNT (4)
char** files;
int fileCount;
const char* path = "/sd/odroid/firmware";

esp_err_t sdcardret;


static void ui_draw_title(const char*, const char*);


void indicate_error()
{
    int level = 0;
    while (true) {
        gpio_set_level(GPIO_NUM_2, level);
        level = !level;
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}

static void pset(UG_S16 x, UG_S16 y, UG_COLOR color)
{
    fb[y * 320 + x] = color;
}

static void ui_update_display()
{
    ili9341_write_frame_rectangleLE(0, 0, 320, 240, fb);
}

static void ui_draw_image(short x, short y, short width, short height, uint16_t* data)
{
    for (short i = 0 ; i < height; ++i)
    {
        for (short j = 0; j < width; ++j)
        {
            uint16_t pixel = data[i * width + j];
            UG_DrawPixel(x + j, y + i, pixel);
        }
    }
}

static void ClearScreen()
{
}

static void UpdateDisplay()
{
    ui_update_display();
}

static void DisplayError(const char* message)
{
    UG_FontSelect(&FONT_8X12);
    short left = (320 / 2) - (strlen(message) * 9 / 2);
    short top = (240 / 2) - (12 / 2);
    UG_SetForecolor(C_RED);
    UG_SetBackcolor(C_WHITE);
    UG_FillFrame(0, top, 319, top + 12, C_WHITE);
    UG_PutString(left, top, message);

    UpdateDisplay();
}

static void DisplayMessage(const char* message)
{
    UG_FontSelect(&FONT_8X12);
    short left = (320 / 2) - (strlen(message) * 9 / 2);
    short top = (240 / 2) + 8 + (12 / 2) + 16;
    UG_SetForecolor(C_BLACK);
    UG_SetBackcolor(C_WHITE);
    UG_FillFrame(0, top, 319, top + 12, C_WHITE);
    UG_PutString(left, top, message);

    UpdateDisplay();
}

static void DisplayProgress(int percent)
{
    if (percent > 100) percent = 100;

    const int WIDTH = 200;
    const int HEIGHT = 12;
    const int FILL_WIDTH = WIDTH * (percent / 100.0f);

    short left = (320 / 2) - (WIDTH / 2);
    short top = (240 / 2) - (HEIGHT / 2) + 16;
    UG_FillFrame(left - 1, top - 1, left + WIDTH + 1, top + HEIGHT + 1, C_WHITE);
    UG_DrawFrame(left - 1, top - 1, left + WIDTH + 1, top + HEIGHT + 1, C_BLACK);

    if (FILL_WIDTH > 0)
    {
        UG_FillFrame(left, top, left + FILL_WIDTH, top + HEIGHT, C_GREEN);
    }

    //UpdateDisplay();
}

static void DisplayFooter(const char* message)
{
    UG_FontSelect(&FONT_8X12);
    short left = (320 / 2) - (strlen(message) * 9 / 2);
    short top = 240 - (16 * 2) - 8;
    UG_SetForecolor(C_BLACK);
    UG_SetBackcolor(C_WHITE);
    UG_FillFrame(0, top, 319, top + 12, C_WHITE);
    UG_PutString(left, top, message);

    UpdateDisplay();
}

static void DisplayHeader(const char* message)
{
    UG_FontSelect(&FONT_8X12);
    short left = (320 / 2) - (strlen(message) * 9 / 2);
    short top = (16 + 8);
    UG_SetForecolor(C_BLACK);
    UG_SetBackcolor(C_WHITE);
    UG_FillFrame(0, top, 319, top + 12, C_WHITE);
    UG_PutString(left, top, message);

    UpdateDisplay();
}

static void DisplayTile(uint16_t *tileData)
{
    const uint16_t tileLeft = (320 / 2) - (TILE_WIDTH / 2);
    const uint16_t tileTop = (16 + 16 + 16);
    ui_draw_image(tileLeft, tileTop, TILE_WIDTH, TILE_HEIGHT, tileData);

    // Tile border
    UG_DrawFrame(tileLeft - 1, tileTop - 1, tileLeft + TILE_WIDTH, tileTop + TILE_HEIGHT, C_BLACK);
    UpdateDisplay();
}


//---------------
void cleanup_and_restart()
{
    // Turn off LED pin
    gpio_set_level(GPIO_NUM_2, 0);

    // clear and deinit display
    ili9341_clear(0x0000);
    ili9341_deinit();
    
    // Close SD card
    odroid_sdcard_close();

    esp_restart();
}


void boot_application()
{
    printf("Booting application.\n");

    // Set firmware active
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
        ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (partition == NULL)
    {
        DisplayError("NO BOOT PART ERROR");
        indicate_error();
    }

    esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK)
    {
        DisplayError("BOOT SET ERROR");
        indicate_error();
    }
    
    cleanup_and_restart();
}


int sort_app_table_by_offset(const void * a, const void * b)
{
  if ( (*(odroid_app_t*)a).startOffset < (*(odroid_app_t*)b).startOffset ) return -1;
  if ( (*(odroid_app_t*)a).startOffset > (*(odroid_app_t*)b).startOffset ) return 1;
  return 0;
}

int sort_app_table_by_sequence(const void * a, const void * b)
{
  if ( (*(odroid_app_t*)a).installSeq < (*(odroid_app_t*)b).installSeq ) return -1;
  if ( (*(odroid_app_t*)a).installSeq > (*(odroid_app_t*)b).installSeq ) return 1;
  return 0;
}


static void sort_app_table(int newMode)
{
    if (newMode == APP_SORT_OFFSET) {
        qsort(apps, apps_count, sizeof(odroid_app_t), &sort_app_table_by_offset);
    } else {
        qsort(apps, apps_count, sizeof(odroid_app_t), &sort_app_table_by_sequence);
    }
}


static void read_app_table()
{
    esp_err_t err;
    
    apps_count = 0;
    
    esp_partition_t *app_table_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE_FACTORY_DATA, NULL);

    if (!app_table_part) {
        abort();
    }

    startFlashAddress = app_table_part->address + app_table_part->size;

    apps_max = (app_table_part->size / sizeof(odroid_app_t));
    
    printf("Max apps: %d\n", apps_max);
    if (!apps) {
        apps = malloc(app_table_part->size);
    }

    if (!apps) {
        DisplayError("APP TABLE ALLOC ERROR");
        indicate_error();
    }

    err = esp_partition_read(app_table_part, 0, (void*)apps, app_table_part->size);
    if (err != ESP_OK)
    {
        DisplayError("APP TABLE READ ERROR");
        indicate_error();
    }
    
    for (int i = 0; i < apps_max; i++) {
        if (apps[i].magic != APP_MAGIC) {
            break;
        }
        if (apps[i].installSeq > nextInstallSeq) {
            nextInstallSeq = apps[i].installSeq + 1;
        }
        apps_count++;
    }

    //64K align the address (https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/partition-tables.html#offset-size)
    startFlashAddress = ALIGN_ADDRESS(startFlashAddress, 0x10000);
    
    printf("App count: %d\n", apps_count);
}


static void write_app_table()
{
    esp_err_t err;

    esp_partition_t *app_table_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE_FACTORY_DATA, NULL);

    if (!app_table_part || !apps) {
        read_app_table();
    }
    
    for (int i = apps_count; i < apps_max; ++i)
    {
        memset(&apps[i], 0xff, sizeof(odroid_app_t));
    }

    sort_app_table(APP_SORT_OFFSET);

    err = esp_partition_erase_range(app_table_part, 0, app_table_part->size);
    if (err != ESP_OK)
    {
        DisplayError("APP TABLE ERASE ERROR");
        indicate_error();
    }

    err = esp_partition_write(app_table_part, 0, (void*)apps, app_table_part->size);
    if (err != ESP_OK)
    {
        DisplayError("APP TABLE WRITE ERROR");
        indicate_error();
    }

    printf("Written app table %d\n", apps_count);
}


static void read_partition_table()
{
    esp_err_t err;

    partition_count = 0;
    
    if (!partition_data)
    {
        partition_data = (const esp_partition_info_t*)malloc(ESP_PARTITION_TABLE_MAX_LEN);
    }

    // Read table
    err = spi_flash_read(ESP_PARTITION_TABLE_OFFSET, (void*)partition_data, ESP_PARTITION_TABLE_MAX_LEN);
    if (err != ESP_OK)
    {
        DisplayError("TABLE READ ERROR");
        indicate_error();
    }

    // Find end of first partitioned

    for (int i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; ++i)
    {
        const esp_partition_info_t *part = &partition_data[i];
        if (part->magic == 0xffff) break;

        if (part->magic == ESP_PARTITION_MAGIC)
        {
            partition_count++;

            if (part->type == PART_TYPE_DATA &&
                part->subtype == PART_SUBTYPE_FACTORY_DATA)
            {
                startTableEntry = i + 1;
                break;
            }
        }
    }
}


static void write_partition_table(odroid_partition_t* parts, size_t parts_count, size_t flashOffset)
{
    esp_err_t err;

    if (!partition_data) {
        read_partition_table();
    }

    // Find end of first partitioned
    if (startTableEntry < 0)
    {
        DisplayError("NO FACTORY PARTITION ERROR");
        indicate_error();
    }

    printf("%s: startTableEntry=%d, startFlashAddress=%#08x\n",
        __func__, startTableEntry, flashOffset);

    // blank partition table entries
    for (int i = startTableEntry; i < ESP_PARTITION_TABLE_MAX_ENTRIES; ++i)
    {
        memset(&partition_data[i], 0xff, sizeof(esp_partition_info_t));
    }

    // Add partitions
    size_t offset = 0;
    for (int i = 0; i < parts_count; ++i)
    {
        esp_partition_info_t* part = &partition_data[startTableEntry + i];
        part->magic = ESP_PARTITION_MAGIC;
        part->type = parts[i].type;
        part->subtype = parts[i].subtype;
        part->pos.offset = flashOffset + offset;
        part->pos.size = parts[i].length;
        memcpy(&part->label, parts[i].label, 16);
        part->flags = parts[i].flags;

        offset += parts[i].length;
    }

    // Erase partition table
    if (ESP_PARTITION_TABLE_MAX_LEN > 4096)
    {
        DisplayError("TABLE SIZE ERROR");
        indicate_error();
    }

    err = spi_flash_erase_range(ESP_PARTITION_TABLE_OFFSET, 4096);
    if (err != ESP_OK)
    {
        DisplayError("TABLE ERASE ERROR");
        indicate_error();
    }

    // Write new table
    err = spi_flash_write(ESP_PARTITION_TABLE_OFFSET, (void*)partition_data, ESP_PARTITION_TABLE_MAX_LEN);
    if (err != ESP_OK)
    {
        DisplayError("TABLE WRITE ERROR");
        indicate_error();
    }

    esp_partition_reload_table();
}



void defrag_flash()
{
    size_t nextStartOffset = startFlashAddress;
    size_t totalBytesToMove = 0;
    size_t totalBytesMoved = 0;

    sort_app_table(APP_SORT_OFFSET);

    // First loop to get total for the progress bar
    for (int i = 0; i < apps_count; i++)
    {
        if (apps[i].startOffset > nextStartOffset)
        {
            totalBytesToMove += (apps[i].endOffset - apps[i].startOffset);
        } else {
            nextStartOffset = apps[i].endOffset + 1;
        }
    }

    sprintf(&tempstring, "Moving: %.2f MB", (float)totalBytesToMove / 1024 / 1024);
    ui_draw_title("Defragmenting flash", tempstring);
    DisplayHeader("Making some space...");

    for (int i = 0; i < apps_count; i++)
    {
        if (apps[i].startOffset > nextStartOffset)
        {
            gpio_set_level(GPIO_NUM_2, 1);

            size_t app_size = apps[i].endOffset - apps[i].startOffset;
            size_t newOffset = nextStartOffset, oldOffset = apps[i].startOffset;
            // move
            for (size_t i = 0; i < app_size; i += FLASH_BLOCK_SIZE)
            {
                printf("Moving 0x%x to 0x%x\n", oldOffset + i, newOffset + i);

                DisplayMessage("Defragmenting ... (E)");
                spi_flash_erase_range(newOffset + i, FLASH_BLOCK_SIZE);

                DisplayMessage("Defragmenting ... (R)");
                spi_flash_read(oldOffset + i, dataBuffer, FLASH_BLOCK_SIZE);

                DisplayMessage("Defragmenting ... (W)");
                spi_flash_write(newOffset + i, dataBuffer, FLASH_BLOCK_SIZE);
                
                totalBytesMoved += FLASH_BLOCK_SIZE;

                DisplayProgress((float) totalBytesMoved / totalBytesToMove  * 100.0);
            }

            apps[i].startOffset = newOffset;
            apps[i].endOffset = newOffset + app_size;

            gpio_set_level(GPIO_NUM_2, 0);
        }

        nextStartOffset = apps[i].endOffset + 1;
    }

    write_app_table();
}


void find_free_blocks(odroid_flash_block_t **blocks, size_t *count, size_t *totalFreeSpace)
{
    //read_app_table();
    size_t previousBlockEnd = startFlashAddress;
    
    (*blocks) = malloc(sizeof(odroid_flash_block_t) * 32);

    (*totalFreeSpace) = 0;
    (*count) = 0;

    sort_app_table(APP_SORT_OFFSET);
    
    for (int i = 0; i < apps_count; i++)
    {
        size_t free_space = apps[i].startOffset - previousBlockEnd;
        
        if (free_space > 0) {
            odroid_flash_block_t *block = &(*blocks)[(*count)++];
            block->offset = previousBlockEnd;
            block->size = free_space;
            (*totalFreeSpace) += block->size;
            printf("Free block: %d 0x%x %d\n", i, block->offset, free_space / 1024);
        }
        
        previousBlockEnd = apps[i].endOffset + 1;
    }

    if ((FLASH_SIZE - previousBlockEnd) > 0) {
        odroid_flash_block_t *block = &(*blocks)[(*count)++];
        block->offset = previousBlockEnd;
        block->size = (FLASH_SIZE - previousBlockEnd);
        (*totalFreeSpace) += block->size;
        printf("Free block: end 0x%x %d\n", block->offset, block->size / 1024);
    }
}


int find_free_block(size_t size, bool defragIfNeeded)
{
    //read_app_table();
    odroid_flash_block_t *blocks;
    size_t count, totalFreeSpace;

    find_free_blocks(&blocks, &count, &totalFreeSpace);
    
    int result = -1;

    for (int i = 0; i < count; i++)
    {
        if (blocks[i].size >= size) {
            result = blocks[i].offset;
            break;
        }
    }

    if (result < 0 && totalFreeSpace >= size) {
        defrag_flash();
        result = find_free_block(size, false);
    }
    
    free(blocks);
    return result;
}


bool firmware_get_info(const char* filename, odroid_fw_t* outData)
{
    size_t count, file_size;

    FILE* file = fopen(filename, "rb");
    if (!file)
    {
        return false;
    }
    
    fseek(file, 0, SEEK_END);
    file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    count = fread(outData, sizeof(outData->fileHeader), 1, file);
    if (count != 1)
    {
        goto firmware_get_info_err;
    }
    
    if (memcmp(HEADER_V00_01, outData->fileHeader.header, strlen(HEADER_V00_01)) != 0)
    {
        goto firmware_get_info_err;
    }
    
    outData->fileHeader.description[FIRMWARE_DESCRIPTION_SIZE - 1] = 0;
    outData->parts_count = 0;
    outData->flashSize = 0;
    outData->dataOffset = ftell(file);
    outData->fileSize = file_size;

    while (ftell(file) < (file_size - 4))
    {
        // Partition information
        odroid_partition_t *part = &outData->parts[outData->parts_count];

        if (fread(part, sizeof(odroid_partition_t), 1, file) != 1)
            goto firmware_get_info_err;
        
        // Check if dataLength is valid
        if (ftell(file) + part->dataLength > file_size || part->dataLength > part->length)
            goto firmware_get_info_err;

        // Check partition subtype
        if (part->type == 0xff)
            goto firmware_get_info_err;
        
        outData->flashSize += part->length;
        outData->parts_count++;

        fseek(file, part->dataLength, SEEK_CUR);
    }

    if (outData->parts_count >= FIRMWARE_PARTS_MAX)
        goto firmware_get_info_err;

    fseek(file, file_size - sizeof(outData->checksum), SEEK_SET);
    fread(&outData->checksum, sizeof(outData->checksum), 1, file);

    fclose(file);
    return true;

firmware_get_info_err:
    fclose(file);
    return false;
}


void flash_utility()
{
    // Code to flash utility.bin. 
    // Because leaving it in flash_firmware results in multiple copies being flashed.
    FILE* util = fopen("/sd/odroid/utility.bin", "rb");
    //util_part.type = ESP_PARTITION_TYPE_APP;
    //util_part.subtype = ESP_PARTITION_SUBTYPE_APP_TEST;
}


void flash_firmware(const char* fullPath)
{
    size_t count;
    bool can_proceed = true;

    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    read_partition_table();
    read_app_table();
    
    ui_draw_title("Install Application", "Destination: Pending");
    UpdateDisplay();

    printf("Opening file '%s'.\n", fullPath);

    FILE* file = fopen(fullPath, "rb");
    if (file == NULL)
    {
        DisplayError("FILE OPEN ERROR");
        indicate_error();
    }

    odroid_fw_t *fw = fwInfoBuffer;

    if (!firmware_get_info(fullPath, fw))
    {
        // To do: Make it show what is invalid
        DisplayError("INVALID FIRMWARE FILE");
        can_proceed = false;
    }

    int currentFlashAddress = find_free_block(fw->flashSize, true);

    odroid_app_t *app = &apps[apps_count];
    memset(app, 0x00, sizeof(odroid_app_t));
    
    strncpy(app->description, fw->fileHeader.description, FIRMWARE_DESCRIPTION_SIZE-1);
    strncpy(app->filename, strrchr(fullPath, '/'), FIRMWARE_DESCRIPTION_SIZE-1);
    memcpy(app->tile, fw->fileHeader.tile, FIRMWARE_TILE_SIZE * 2);
    
    printf("FirmwareDescription='%s'\n", app->description);

    sprintf(&tempstring, "Destination: 0x%x", currentFlashAddress);
    ui_draw_title("Install Application", tempstring);
    DisplayHeader(app->description);
    DisplayTile(app->tile);

    if (currentFlashAddress == -1)
    {
        DisplayError("NOT ENOUGH FREE SPACE");
        can_proceed = false;
    }
    
    if (can_proceed)
    {
        DisplayMessage("[START]");
    }
    
    DisplayFooter("[B] Cancel");

    while (1) {
        int btn = wait_for_button_press(-1);

        if (btn == ODROID_INPUT_START && can_proceed) break;
        if (btn == ODROID_INPUT_B)
        {
            fclose(file);
            return;
        }
    }


    DisplayMessage("Verifying ...");
    DisplayFooter("");


    // Verify file integerity
    printf("%s: expected_checksum=%#010x\n", __func__, fw->checksum);

    fseek(file, 0, SEEK_SET);

    uint32_t checksum = 0;
    while(true)
    {
        count = fread(dataBuffer, 1, FLASH_BLOCK_SIZE, file);
        if (ftell(file) == fw->fileSize)
        {
            count -= 4;
        }

        checksum = crc32_le(checksum, dataBuffer, count);

        if (count < FLASH_BLOCK_SIZE) break;
    }

    printf("%s: checksum=%#010x\n", __func__, checksum);

    if (checksum != fw->checksum)
    {
        DisplayError("CHECKSUM MISMATCH ERROR");
        indicate_error();
    }

    // restore location to end of description
    fseek(file, fw->dataOffset, SEEK_SET);

    app->magic = APP_MAGIC;
    app->startOffset = currentFlashAddress;

    // Copy the firmware
    while(true)
    {
        if (ftell(file) >= (fw->fileSize - sizeof(checksum)))
        {
            break;
        }

        // Partition
        odroid_partition_t *slot = &app->parts[app->parts_count];

        count = fread(slot, 1, sizeof(odroid_partition_t), file);
        if (count != sizeof(odroid_partition_t))
        {
            DisplayError("PARTITION READ ERROR");
            indicate_error();
        }

        uint32_t length = slot->dataLength;

        size_t nextEntry = ftell(file) + length;

        if (length > 0)
        {
            // turn LED off
            gpio_set_level(GPIO_NUM_2, 0);


            // erase
            int eraseBlocks = length / ERASE_BLOCK_SIZE;
            if (eraseBlocks * ERASE_BLOCK_SIZE < length) ++eraseBlocks;

            // Display
            sprintf(tempstring, "Erasing ... (%d)", app->parts_count);

            printf("%s\n", tempstring);
            DisplayProgress(0);
            DisplayMessage(tempstring);

            esp_err_t ret = spi_flash_erase_range(currentFlashAddress, eraseBlocks * ERASE_BLOCK_SIZE);
            if (ret != ESP_OK)
            {
                printf("spi_flash_erase_range failed. eraseBlocks=%d\n", eraseBlocks);
                DisplayError("ERASE ERROR");
                indicate_error();
            }


            // turn LED on
            gpio_set_level(GPIO_NUM_2, 1);


            // Write data
            int totalCount = 0;
            for (int offset = 0; offset < length; offset += FLASH_BLOCK_SIZE)
            {
                // Display
                sprintf(tempstring, "Writing (%d)", app->parts_count);

                printf("%s - %#08x\n", tempstring, offset);
                DisplayProgress((float)offset / (float)(length - FLASH_BLOCK_SIZE) * 100.0f);
                DisplayMessage(tempstring);

                // read
                //printf("Reading offset=0x%x\n", offset);
                count = fread(dataBuffer, 1, FLASH_BLOCK_SIZE, file);
                if (count <= 0)
                {
                    DisplayError("DATA READ ERROR");
                    indicate_error();
                }

                if (offset + count >= length)
                {
                    count = length - offset;
                }


                // flash
                ret = spi_flash_write(currentFlashAddress + offset, dataBuffer, count);
                if (ret != ESP_OK)
        		{
        			printf("spi_flash_write failed. address=%#08x\n", currentFlashAddress + offset);
                    DisplayError("WRITE ERROR");
                    indicate_error();
        		}

                totalCount += count;
            }

            if (totalCount != length)
            {
                printf("Size mismatch: lenght=%#08x, totalCount=%#08x\n", length, totalCount);
                DisplayError("DATA SIZE ERROR");
                indicate_error();
            }


            // TODO: verify

        }
        
        // Notify OK
        printf("OK: [%d] Length=%#08x\n", app->parts_count, length);

        app->parts_count++;
        currentFlashAddress += slot->length;

        // Seek to next entry
        if (fseek(file, nextEntry, SEEK_SET) != 0)
        {
            DisplayError("SEEK ERROR");
            indicate_error();
        }

    }

    fclose(file);

    // 64K align our endOffset
    app->endOffset = ALIGN_ADDRESS(currentFlashAddress, 0x10000) - 1;

    // Remember the install order, for display sorting
    app->installSeq = nextInstallSeq++;

    // Write app table
    apps_count++; // Everything went well, acknowledge the new app
    write_app_table();

    // turn LED off
    gpio_set_level(GPIO_NUM_2, 0);

    DisplayMessage("Ready !");
    DisplayFooter("[B] Go Back   |   [A] Boot");
    
    while (1) {
        int btn = wait_for_button_press(-1);

        if (btn == ODROID_INPUT_A) break;
        if (btn == ODROID_INPUT_B) return;
    }

    // Write partition table
    write_partition_table(app->parts, app->parts_count, app->startOffset);

    // boot firmware
    boot_application();
}


static void ui_draw_title(const char* TITLE, const char* FOOTER)
{
    UG_FillFrame(0, 0, 319, 239, C_WHITE);

    // Header
    UG_FillFrame(0, 0, 319, 15, C_MIDNIGHT_BLUE);
    UG_FontSelect(&FONT_8X8);
    const short titleLeft = (320 / 2) - (strlen(TITLE) * 9 / 2);
    UG_SetForecolor(C_WHITE);
    UG_SetBackcolor(C_MIDNIGHT_BLUE);
    UG_PutString(titleLeft, 4, TITLE);

    // Footer
    UG_FontSelect(&FONT_8X8);
    UG_SetBackcolor(C_MIDNIGHT_BLUE);
    UG_SetForecolor(C_LIGHT_GRAY);
    UG_FillFrame(0, 239 - 16, 319, 239, C_MIDNIGHT_BLUE);
    const short footerLeft = (320 / 2) - (strlen(FOOTER) * 9 / 2);
    UG_PutString(footerLeft, 240 - 4 - 8, FOOTER);
}


static void ui_draw_page(char** files, int fileCount, int currentItem)
{
    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    int page = (currentItem / ITEM_COUNT) * ITEM_COUNT;
    
    odroid_flash_block_t *blocks;
    size_t count, totalFreeSpace;

    find_free_blocks(&blocks, &count, &totalFreeSpace);
    free(blocks);
    
    sprintf(&tempstring, "Free space: %.2fMB (%d block)", (double)totalFreeSpace / 1024 / 1024, count);
    
    ui_draw_title("Select a file", tempstring);

    const int innerHeight = 240 - (16 * 2); // 208
    const int itemHeight = innerHeight / ITEM_COUNT; // 52

    const int rightWidth = (213); // 320 * (2.0 / 3.0)
    const int leftWidth = 320 - rightWidth;

    // Tile width = 86, height = 48 (16:9)
    const short imageLeft = (leftWidth / 2) - (86 / 2);
    const short textLeft = 320 - rightWidth;


	if (fileCount < 1)
	{
        DisplayMessage("SD Card Empty");
	}
	else
	{
	    for (int line = 0; line < ITEM_COUNT; ++line)
	    {
			if (page + line >= fileCount) break;

            //uint16_t id = TXB_ID_0 + line;
            short top = 16 + (line * itemHeight) - 1;

	        if ((page) + line == currentItem)
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_YELLOW);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_YELLOW);
	        }
	        else
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_WHITE);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_WHITE);
	        }

			char* fileName = files[page + line];
			if (!fileName) abort();

            sprintf(&tempstring, "%s/%s", path, fileName);
            bool valid = firmware_get_info(tempstring, fwInfoBuffer);

            ui_draw_image(imageLeft, top + 2, TILE_WIDTH, TILE_HEIGHT, fwInfoBuffer->fileHeader.tile);

            UG_FontSelect(&FONT_8X12);
            strcpy(tempstring, fileName);
            tempstring[strlen(fileName) - 3] = 0; // ".fw" = 3
            UG_PutString(textLeft, top + 2 + 2 + 7, tempstring);

            if (valid) {
                UG_SetForecolor(C_GRAY);
                sprintf(&tempstring, "%.2f MB", (float)fwInfoBuffer->flashSize / 1024 / 1024);
            } else {
                UG_SetForecolor(C_RED);
                sprintf(&tempstring, "Invalid firmware");
            }
            
            UG_PutString(textLeft, top + 2 + 2 + 23, tempstring);
	    }

        UpdateDisplay();
	}
}

const char* ui_choose_file(const char* path)
{
    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    const char* result = NULL;

    files = 0;
    fileCount = odroid_sdcard_files_get(path, ".fw", &files);
    printf("%s: fileCount=%d\n", __func__, fileCount);
    
    // Check SD card
    if (sdcardret != ESP_OK)
    {
        DisplayError("SD CARD ERROR");
        indicate_error();
    }

    // At least one firmware must be available
    if (fileCount < 1)
    {
        DisplayError("NO FILES ERROR");
        indicate_error();
    }


    // Selection
    int currentItem = 0;

    while (true)
    {
        ui_draw_page(files, fileCount, currentItem);

        int page = (currentItem / ITEM_COUNT) * ITEM_COUNT;


        int btn = wait_for_button_press(-1);

        if (btn == ODROID_INPUT_DOWN)
        {
            if (++currentItem >= fileCount) currentItem = 0;
        }
        else if(btn == ODROID_INPUT_UP)
        {
            if (--currentItem < 0) currentItem = fileCount - 1;
        }
        else if(btn == ODROID_INPUT_RIGHT)
        {
            if (page + ITEM_COUNT < fileCount) currentItem = page + ITEM_COUNT;
            else currentItem = 0;
        }
        else if(btn == ODROID_INPUT_LEFT)
        {
            if (page - ITEM_COUNT >= 0) currentItem = page - ITEM_COUNT;
            else currentItem = (fileCount - 1) / ITEM_COUNT * ITEM_COUNT;
        }
        else if(btn == ODROID_INPUT_A)
        {
            size_t fullPathLength = strlen(path) + 1 + strlen(files[currentItem]) + 1;

            char* fullPath = (char*)malloc(fullPathLength);
            if (!fullPath) abort();

            strcpy(fullPath, path);
            strcat(fullPath, "/");
            strcat(fullPath, files[currentItem]);

            result = fullPath;
            break;
        }
        else if (btn == ODROID_INPUT_B)
        {
            break;
        }
    }

    odroid_sdcard_files_free(files, fileCount);

    return result;
}



static void ui_draw_dialog(char options[], int optionCount, int currentItem)
{
    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    int border = 3;
    int itemWidth = 190;
    int itemHeight = 20;
    int width = itemWidth + (border * 2);
    int height = ((optionCount+1) * itemHeight) + (border *  2);
    int top = (240 - height) / 2;
    int left  = (320 - width) / 2;

    UG_FillFrame(left, top, left + width, top + height, C_BLUE);
    UG_FillFrame(left + border, top + border, left + width - border, top + height - border, C_WHITE);
    
    top += border;
    left += border;
    
    for (int i = 0; i < optionCount; i++) {
        int fg = (i == currentItem) ? C_WHITE : C_BLACK;
        int bg = (i == currentItem) ? C_BLUE : C_WHITE;

        UG_SetForecolor(fg);
        UG_SetBackcolor(bg);
        UG_FillFrame(left, top, left + itemWidth, top + itemHeight, bg);
        UG_FontSelect(&FONT_8X12);
        UG_PutString(left + 2, top + 2, &options[i * 32]);

        top += itemHeight;
    }

    // Display version at the bottom
    UG_SetForecolor(C_GRAY);
    UG_SetBackcolor(C_WHITE);
    UG_FontSelect(&FONT_8X8);
    UG_PutString(left + 2, top + 2, "Version:\n " VERSION);

    UpdateDisplay();
}


static int ui_choose_dialog(char options[], int optionCount, bool cancellable)
{
    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    int currentItem = 0;

    while (true)
    {
        ui_draw_dialog(options, optionCount, currentItem);

		int btn = wait_for_button_press(-1);

        if(btn == ODROID_INPUT_DOWN)
        {
            if (++currentItem >= optionCount) currentItem = 0;
        }
        else if(btn == ODROID_INPUT_UP)
        {
            if (--currentItem < 0) currentItem = optionCount - 1;
        }
        else if(btn == ODROID_INPUT_A)
        {
            return currentItem;
        }
        else if(btn == ODROID_INPUT_B)
        {
            if (cancellable) {
                return -1;
            }
        }
    }
    
    return currentItem;
}


static void ui_draw_app_page(int currentItem)
{
    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());

    int page = (currentItem / ITEM_COUNT) * ITEM_COUNT;

    ui_draw_title("ODROID-GO", "[MENU] Menu   |   [A] Boot App");

    const int innerHeight = 240 - (16 * 2); // 208
    const int itemHeight = innerHeight / ITEM_COUNT; // 52

    const int rightWidth = (213); // 320 * (2.0 / 3.0)
    const int leftWidth = 320 - rightWidth;

    // Tile width = 86, height = 48 (16:9)
    const short imageLeft = (leftWidth / 2) - (86 / 2);
    const short textLeft = 320 - rightWidth;

	if (apps_count < 1)
	{
        DisplayMessage("No apps have been flashed yet!");
	}
	else
	{
        for (int line = 0; line < ITEM_COUNT; ++line)
	    {
            
			if (page + line >= apps_count) break;

            odroid_app_t *app = &apps[page + line];
            
            short top = 16 + (line * itemHeight) - 1;

	        if ((page) + line == currentItem)
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_YELLOW);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_YELLOW);
	        }
	        else
	        {
                UG_SetForecolor(C_BLACK);
                UG_SetBackcolor(C_WHITE);
                UG_FillFrame(0, top + 2, 319, top + itemHeight - 1 - 1, C_WHITE);
	        }

            ui_draw_image(imageLeft, top + 2, TILE_WIDTH, TILE_HEIGHT, app->tile);

            UG_FontSelect(&FONT_8X12);
            UG_PutString(textLeft, top + 2 + 2 + 7, app->description);
            //sprintf(&tempstring, "%.2f MB @ 0x%x", (float)(app->endOffset - app->startOffset) / 1024 / 1024, app->startOffset);
            UG_SetForecolor(C_GRAY);
            sprintf(&tempstring, "0x%x - 0x%x", app->startOffset, app->endOffset);
            UG_PutString(textLeft, top + 2 + 2 + 23, tempstring);
	    }
	}

    UpdateDisplay();
}


void ui_choose_app()
{
    printf("%s: HEAP=%#010x\n", __func__, esp_get_free_heap_size());
    
    sort_app_table(displayOrder & 1);
    
    // Selection
    int currentItem = 0;

    while (true)
    {
        ui_draw_app_page(currentItem);

        int page = (currentItem / ITEM_COUNT) * ITEM_COUNT;


        int btn = wait_for_button_press(-1);

		if (apps_count > 0)
		{
	        if (btn == ODROID_INPUT_DOWN)
	        {
                if (++currentItem >= apps_count) currentItem = 0;
	        }
	        else if (btn == ODROID_INPUT_UP)
	        {
                if (--currentItem < 0) currentItem = apps_count - 1;
	        }
	        else if (btn == ODROID_INPUT_RIGHT)
	        {
                if (page + ITEM_COUNT < apps_count) currentItem = page + ITEM_COUNT;
                else currentItem = 0;
	        }
	        else if (btn == ODROID_INPUT_LEFT)
	        {
                if (page - ITEM_COUNT >= 0) currentItem = page - ITEM_COUNT;
                else currentItem = (apps_count - 1) / ITEM_COUNT * ITEM_COUNT;
	        }
	        else if (btn == ODROID_INPUT_A)
	        {
                ui_draw_title("ODROID-GO", VERSION);
                DisplayMessage("Updating partitions ...");

                odroid_app_t *app = &apps[currentItem];
                write_partition_table(app->parts, app->parts_count, app->startOffset);

                DisplayMessage("Setting boot partition ...");
                boot_application();
	        }
            else if (btn == ODROID_INPUT_VOLUME)
            {
                ui_draw_title("ODROID-GO", VERSION);
                DisplayMessage("Setting boot partition ...");
                boot_application();
            }
            else if (btn == ODROID_INPUT_SELECT)
            {
                sort_app_table(++displayOrder & 1);
            }
        }

        if (btn == ODROID_INPUT_MENU)
        {
            const char options[5][32] = {
                "Install from SD Card", 
                "Erase selected app",
                "Erase all apps",
                "Erase NVM",
                "Restart System"
            };
            
            int choice = ui_choose_dialog(options, 5, true);
            char* fileName;

            switch(choice) {
                case 0:
                    fileName = ui_choose_file(path);
                    if (fileName) {
                        printf("%s: fileName='%s'\n", __func__, fileName);
                        flash_firmware(fileName);
                        free(fileName);
                    }
                    break;
                case 1:
                    memmove(&apps[currentItem], &apps[currentItem + 1], 
                            (apps_max - currentItem) * sizeof(odroid_app_t));
                    apps_count--;
                    write_app_table();
                    break;
                case 2:
                    memset(apps, 0xFF, apps_max * sizeof(odroid_app_t));
                    write_app_table();
                    read_app_table();
                    break;
                case 3:
                    nvs_flash_erase();
                    break;
                case 4:
                    cleanup_and_restart();
                    break;
            }
            
            sort_app_table(displayOrder & 1);
        }
    }
}


void app_main(void)
{
    printf("odroid-go-firmware (Ver: %s). HEAP=%#010x\n", VERSION, esp_get_free_heap_size());

    nvs_flash_init();

    input_init();

    // turn LED on
    gpio_set_direction(GPIO_NUM_2, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_2, 1);

    sdcardret = odroid_sdcard_open(SD_CARD); // before LCD

    ili9341_init();
    ili9341_clear(0xffff);

    UG_Init(&gui, pset, 320, 240);

    fwInfoBuffer = malloc(sizeof(odroid_fw_t));
    dataBuffer = malloc(FLASH_BLOCK_SIZE);
    
    // If we can't allocate our basic buffers we might as well give up now
    if (!fwInfoBuffer || !dataBuffer)
    {
        DisplayError("MEMORY ALLOCATION ERROR");
        indicate_error();
    }

    read_partition_table();
    read_app_table();

    ui_choose_app();

    indicate_error();
}
