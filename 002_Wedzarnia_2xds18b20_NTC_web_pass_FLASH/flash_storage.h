// flash_storage.h - Warstwa abstrakcji dla W25Q128 SPI Flash
// Emuluje prosty system plików na pamięci flash 16MB
#pragma once
#include <Arduino.h>

// ======================================================
// KONFIGURACJA W25Q128
// ======================================================
#define FLASH_CS_PIN        5       // CS pin (ten sam co był dla SD)
#define FLASH_TOTAL_SIZE    (16UL * 1024UL * 1024UL)  // 16 MB
#define FLASH_SECTOR_SIZE   4096    // 4 KB sektor (jednostka kasowania)
#define FLASH_PAGE_SIZE     256     // 256 B strona (jednostka zapisu)
#define FLASH_TOTAL_SECTORS (FLASH_TOTAL_SIZE / FLASH_SECTOR_SIZE) // 4096

// ======================================================
// ALOKACJA SEKTORÓW
// ======================================================
// Sektor 0:        Katalog plików (FAT - File Allocation Table)
// Sektory 1-100:   Profile (.prof) - max 100 profili × 4KB = 400KB
// Sektory 101-120: Backupy (.bak) - max 20 backupów × 4KB = 80KB  
// Sektory 121-200: Logi - max 80 logów × 4KB = 320KB
// Sektory 201+:    Wolne / rezerwowe

#define FAT_SECTOR          0
#define PROFILES_START      1
#define PROFILES_END        100
#define BACKUPS_START        101
#define BACKUPS_END          120
#define LOGS_START           121
#define LOGS_END             200

#define MAX_FLASH_FILES      200
#define MAX_FILENAME_LEN     48

// ======================================================
// STRUKTURA WPISU W KATALOGU (FAT)
// ======================================================
struct FlashFileEntry {
    char filename[MAX_FILENAME_LEN];  // Nazwa pliku np. "/profiles/test.prof"
    uint16_t startSector;             // Pierwszy sektor z danymi
    uint16_t sectorCount;             // Ile sektorów zajmuje (zazwyczaj 1)
    uint32_t fileSize;                // Rozmiar danych w bajtach
    uint8_t  valid;                   // 0xFF = wolny, 0x01 = zajęty, 0x00 = skasowany
    uint8_t  reserved[3];
};
// sizeof(FlashFileEntry) = 48 + 2 + 2 + 4 + 1 + 3 = 60 bytes
// W jednym sektorze 4KB zmieści się: 4096/60 = 68 wpisów

// ======================================================
// API FLASH STORAGE
// ======================================================

// Inicjalizacja - wywołaj w setup()
bool flash_init();

// Sprawdź czy flash jest zainicjalizowany
bool flash_is_ready();

// Formatowanie - kasuje wszystko i tworzy pustą FAT
bool flash_format();

// Operacje plikowe (emulacja SD)
bool flash_file_exists(const char* path);
bool flash_file_write(const char* path, const uint8_t* data, uint32_t size);
bool flash_file_write_string(const char* path, const String& content);
String flash_file_read_string(const char* path);
int flash_file_read(const char* path, uint8_t* buffer, uint32_t maxSize);
bool flash_file_delete(const char* path);
bool flash_file_append(const char* path, const String& content);

// Katalog - listowanie plików z danym prefiksem
int flash_list_files(const char* dirPrefix, char files[][MAX_FILENAME_LEN], int maxFiles);

// Tworzenie "katalogów" (w naszym flat FS to tylko konwencja nazewnictwa)
bool flash_mkdir(const char* path);
bool flash_dir_exists(const char* path);

// Diagnostyka
uint32_t flash_get_free_sectors();
uint32_t flash_get_used_sectors();
uint32_t flash_get_total_size();
uint16_t flash_get_jedec_id();

// Niskopoziomowe
void flash_read_data(uint32_t address, uint8_t* buffer, uint32_t size);
void flash_write_page(uint32_t address, const uint8_t* data, uint16_t size);
void flash_erase_sector(uint32_t sectorNumber);
void flash_erase_block_64k(uint32_t blockNumber);
void flash_chip_erase();
uint8_t flash_read_status();
void flash_wait_busy();
