// flash_storage.h - Warstwa abstrakcji dla W25Q128 SPI Flash
// v2.1 - SPI mutex (dzielony z TFT) + wszystkie poprawki krytyczne
#pragma once
#include <Arduino.h>
#include <freertos/semphr.h>

// ======================================================
// KONFIGURACJA W25Q128
// ======================================================
#define FLASH_CS_PIN        5
#define FLASH_TOTAL_SIZE    (16UL * 1024UL * 1024UL)
#define FLASH_SECTOR_SIZE   4096
#define FLASH_PAGE_SIZE     256
#define FLASH_TOTAL_SECTORS (FLASH_TOTAL_SIZE / FLASH_SECTOR_SIZE)

// ======================================================
// ALOKACJA SEKTORÓW
// ======================================================
#define FAT_SECTOR          0    // FAT główna
#define FAT_SHADOW_SECTOR   1    // FAT kopia zapasowa
#define PROFILES_START      2
#define PROFILES_END        101
#define BACKUPS_START       102
#define BACKUPS_END         121
#define LOGS_START          122
#define LOGS_END            201

// Sektor 4096B / 60B wpis = 68 wpisów max → MAX_FLASH_FILES=64
#define MAX_FLASH_FILES     64
#define MAX_FILENAME_LEN    48
#define FAT_MAGIC           0x46415432UL  // "FAT2"

// ======================================================
// STRUKTURY
// ======================================================
struct __attribute__((packed)) FlashFileEntry {
    char     filename[MAX_FILENAME_LEN]; // 48
    uint16_t startSector;                //  2
    uint16_t sectorCount;                //  2
    uint32_t fileSize;                   //  4
    uint8_t  valid;                      //  1  0x01=OK 0x00=skasowany 0xFF=wolny
    uint8_t  reserved[3];                //  3
};
// sizeof = 60 bajtów

struct __attribute__((packed)) FatHeader {
    uint32_t magic;       // FAT_MAGIC
    uint16_t entryCount;  // liczba zapisanych wpisów (aktywnych + skasowanych)
    uint16_t reserved;
};
// sizeof = 8 bajtów; wpisy od offsetu 8

// ======================================================
// API PUBLICZNE
// ======================================================

// spiMutex – przekaż istniejący mutex współdzielony z TFT
// Jeśli NULL – flash_init() tworzy własny (tylko jeśli TFT nie używa SPI)
bool     flash_init(SemaphoreHandle_t spiMutex = NULL);
bool     flash_is_ready();
bool     flash_format();

bool     flash_file_exists(const char* path);
bool     flash_file_write(const char* path, const uint8_t* data, uint32_t size);
bool     flash_file_write_string(const char* path, const String& content);
String   flash_file_read_string(const char* path);
int      flash_file_read(const char* path, uint8_t* buffer, uint32_t maxSize);
bool     flash_file_delete(const char* path);
bool     flash_file_append(const char* path, const String& content);

int      flash_list_files(const char* dirPrefix, char files[][MAX_FILENAME_LEN], int maxFiles);
bool     flash_mkdir(const char* path);
bool     flash_dir_exists(const char* path);

uint32_t flash_get_free_sectors();
uint32_t flash_get_used_sectors();
uint32_t flash_get_total_size();
uint16_t flash_get_jedec_id();

// Niskopoziomowe
void     flash_read_data(uint32_t address, uint8_t* buffer, uint32_t size);
void     flash_write_page(uint32_t address, const uint8_t* data, uint16_t size);
void     flash_erase_sector(uint32_t sectorNumber);
void     flash_erase_block_64k(uint32_t blockNumber);
void     flash_chip_erase();
uint8_t  flash_read_status();
void     flash_wait_busy();
