// flash_storage.cpp - Implementacja warstwy abstrakcji dla W25Q128
// Prosty system plików oparty na sektorach 4KB z tablicą FAT w sektorze 0
#include "flash_storage.h"
#include "config.h"
#include <SPI.h>

// ======================================================
// KOMENDY SPI W25Q128
// ======================================================
#define W25Q_CMD_WRITE_ENABLE   0x06
#define W25Q_CMD_WRITE_DISABLE  0x04
#define W25Q_CMD_READ_STATUS1   0x05
#define W25Q_CMD_READ_DATA      0x03
#define W25Q_CMD_PAGE_PROGRAM   0x02
#define W25Q_CMD_SECTOR_ERASE   0x20  // 4KB
#define W25Q_CMD_BLOCK_ERASE_64 0xD8  // 64KB
#define W25Q_CMD_CHIP_ERASE     0xC7
#define W25Q_CMD_JEDEC_ID       0x9F
#define W25Q_CMD_POWER_UP       0xAB
#define W25Q_CMD_POWER_DOWN     0xB9

#define W25Q_STATUS_BUSY        0x01

// ======================================================
// ZMIENNE STATYCZNE
// ======================================================
static bool flashReady = false;
static FlashFileEntry fatTable[MAX_FLASH_FILES];
static int fatEntryCount = 0;

// ======================================================
// NISKOPOZIOMOWE FUNKCJE SPI
// ======================================================

static inline void flash_cs_low()  { digitalWrite(FLASH_CS_PIN, LOW);  }
static inline void flash_cs_high() { digitalWrite(FLASH_CS_PIN, HIGH); }

static void flash_write_enable() {
    flash_cs_low();
    SPI.transfer(W25Q_CMD_WRITE_ENABLE);
    flash_cs_high();
    delayMicroseconds(1);
}

uint8_t flash_read_status() {
    flash_cs_low();
    SPI.transfer(W25Q_CMD_READ_STATUS1);
    uint8_t status = SPI.transfer(0x00);
    flash_cs_high();
    return status;
}

void flash_wait_busy() {
    unsigned long start = millis();
    while (flash_read_status() & W25Q_STATUS_BUSY) {
        if (millis() - start > 10000) {
            log_msg(LOG_LEVEL_ERROR, "Flash wait_busy timeout!");
            break;
        }
        delayMicroseconds(100);
    }
}

uint16_t flash_get_jedec_id() {
    flash_cs_low();
    SPI.transfer(W25Q_CMD_JEDEC_ID);
    uint8_t mfr = SPI.transfer(0x00);
    uint8_t type = SPI.transfer(0x00);
    uint8_t cap = SPI.transfer(0x00);
    flash_cs_high();
    return (mfr << 8) | type;  // W25Q128: 0xEF17
}

void flash_read_data(uint32_t address, uint8_t* buffer, uint32_t size) {
    flash_wait_busy();
    flash_cs_low();
    SPI.transfer(W25Q_CMD_READ_DATA);
    SPI.transfer((address >> 16) & 0xFF);
    SPI.transfer((address >> 8) & 0xFF);
    SPI.transfer(address & 0xFF);
    for (uint32_t i = 0; i < size; i++) {
        buffer[i] = SPI.transfer(0x00);
    }
    flash_cs_high();
}

void flash_write_page(uint32_t address, const uint8_t* data, uint16_t size) {
    if (size > FLASH_PAGE_SIZE) size = FLASH_PAGE_SIZE;
    
    flash_write_enable();
    flash_cs_low();
    SPI.transfer(W25Q_CMD_PAGE_PROGRAM);
    SPI.transfer((address >> 16) & 0xFF);
    SPI.transfer((address >> 8) & 0xFF);
    SPI.transfer(address & 0xFF);
    for (uint16_t i = 0; i < size; i++) {
        SPI.transfer(data[i]);
    }
    flash_cs_high();
    flash_wait_busy();
}

void flash_erase_sector(uint32_t sectorNumber) {
    uint32_t address = sectorNumber * FLASH_SECTOR_SIZE;
    flash_write_enable();
    flash_cs_low();
    SPI.transfer(W25Q_CMD_SECTOR_ERASE);
    SPI.transfer((address >> 16) & 0xFF);
    SPI.transfer((address >> 8) & 0xFF);
    SPI.transfer(address & 0xFF);
    flash_cs_high();
    flash_wait_busy();
}

void flash_erase_block_64k(uint32_t blockNumber) {
    uint32_t address = blockNumber * 65536UL;
    flash_write_enable();
    flash_cs_low();
    SPI.transfer(W25Q_CMD_BLOCK_ERASE_64);
    SPI.transfer((address >> 16) & 0xFF);
    SPI.transfer((address >> 8) & 0xFF);
    SPI.transfer(address & 0xFF);
    flash_cs_high();
    flash_wait_busy();
}

void flash_chip_erase() {
    flash_write_enable();
    flash_cs_low();
    SPI.transfer(W25Q_CMD_CHIP_ERASE);
    flash_cs_high();
    // Chip erase może trwać do 200 sekund
    unsigned long start = millis();
    while (flash_read_status() & W25Q_STATUS_BUSY) {
        if (millis() - start > 300000) break;
        delay(100);
    }
}

// ======================================================
// ZAPIS WIELOSTRONICOWY (dla danych > 256 bajtów)
// ======================================================

static void flash_write_data(uint32_t address, const uint8_t* data, uint32_t size) {
    uint32_t offset = 0;
    while (offset < size) {
        // Ile bajtów do końca bieżącej strony?
        uint16_t pageOffset = address % FLASH_PAGE_SIZE;
        uint16_t bytesToWrite = FLASH_PAGE_SIZE - pageOffset;
        if (bytesToWrite > (size - offset)) {
            bytesToWrite = size - offset;
        }
        flash_write_page(address, data + offset, bytesToWrite);
        address += bytesToWrite;
        offset += bytesToWrite;
    }
}

// ======================================================
// FAT - TABLICA ALOKACJI PLIKÓW
// ======================================================

static void fat_load() {
    uint8_t buffer[sizeof(FlashFileEntry)];
    fatEntryCount = 0;
    
    uint32_t fatAddr = FAT_SECTOR * FLASH_SECTOR_SIZE;
    int maxEntries = FLASH_SECTOR_SIZE / sizeof(FlashFileEntry);
    if (maxEntries > MAX_FLASH_FILES) maxEntries = MAX_FLASH_FILES;
    
    for (int i = 0; i < maxEntries; i++) {
        flash_read_data(fatAddr + i * sizeof(FlashFileEntry), buffer, sizeof(FlashFileEntry));
        memcpy(&fatTable[i], buffer, sizeof(FlashFileEntry));
        
        if (fatTable[i].valid == 0x01) {
            fatEntryCount = i + 1;
        } else if (fatTable[i].valid == 0xFF) {
            // Koniec FAT (niezapisany flash = 0xFF)
            break;
        }
    }
    
    LOG_FMT(LOG_LEVEL_INFO, "FAT loaded: %d file entries", fatEntryCount);
}

static void fat_compact() {
    // Kompaktuj tablicę - usuń luki (wpisy z valid != 0x01)
    int writeIdx = 0;
    for (int readIdx = 0; readIdx < fatEntryCount; readIdx++) {
        if (fatTable[readIdx].valid == 0x01) {
            if (writeIdx != readIdx) {
                memcpy(&fatTable[writeIdx], &fatTable[readIdx], sizeof(FlashFileEntry));
            }
            writeIdx++;
        }
    }
    fatEntryCount = writeIdx;
    // Wyczyść resztę tablicy
    for (int i = writeIdx; i < MAX_FLASH_FILES; i++) {
        fatTable[i].valid = 0xFF;
    }
}

static void fat_save() {
    // Kompaktuj przed zapisem - eliminuje luki które powodowały utratę danych
    fat_compact();
    
    // Kasuj sektor FAT
    flash_erase_sector(FAT_SECTOR);
    yield(); // Zapobiegaj watchdog reset
    
    // Zapisz wszystkie wpisy ciągłym blokiem (bez luk)
    uint32_t fatAddr = FAT_SECTOR * FLASH_SECTOR_SIZE;
    for (int i = 0; i < fatEntryCount; i++) {
        flash_write_data(fatAddr + i * sizeof(FlashFileEntry), 
                       (uint8_t*)&fatTable[i], sizeof(FlashFileEntry));
        if (i % 10 == 9) yield(); // Co 10 wpisów oddaj czas systemowi
    }
}

static int fat_find_file(const char* path) {
    for (int i = 0; i < fatEntryCount; i++) {
        if (fatTable[i].valid == 0x01 && strcmp(fatTable[i].filename, path) == 0) {
            return i;
        }
    }
    return -1;
}

static uint16_t fat_find_free_sector(uint16_t rangeStart, uint16_t rangeEnd) {
    for (uint16_t s = rangeStart; s <= rangeEnd; s++) {
        bool used = false;
        for (int i = 0; i < fatEntryCount; i++) {
            if (fatTable[i].valid == 0x01) {
                if (s >= fatTable[i].startSector && 
                    s < fatTable[i].startSector + fatTable[i].sectorCount) {
                    used = true;
                    break;
                }
            }
        }
        if (!used) return s;
    }
    return 0xFFFF; // Brak wolnego sektora
}

static void fat_get_sector_range(const char* path, uint16_t& rangeStart, uint16_t& rangeEnd) {
    if (strncmp(path, "/profiles/", 10) == 0) {
        rangeStart = PROFILES_START;
        rangeEnd = PROFILES_END;
    } else if (strncmp(path, "/backup/", 8) == 0) {
        rangeStart = BACKUPS_START;
        rangeEnd = BACKUPS_END;
    } else if (strncmp(path, "/logs/", 6) == 0) {
        rangeStart = LOGS_START;
        rangeEnd = LOGS_END;
    } else {
        // Domyślnie w obszarze profili
        rangeStart = PROFILES_START;
        rangeEnd = LOGS_END;
    }
}

// ======================================================
// API PUBLICZNE
// ======================================================

bool flash_init() {
    pinMode(FLASH_CS_PIN, OUTPUT);
    flash_cs_high();
    
    // Upewnij się że TFT nie blokuje SPI
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
    delay(10);
    
    // Power up
    flash_cs_low();
    SPI.transfer(W25Q_CMD_POWER_UP);
    flash_cs_high();
    delay(1);
    
    // Sprawdź JEDEC ID
    uint16_t id = flash_get_jedec_id();
    LOG_FMT(LOG_LEVEL_INFO, "Flash JEDEC ID: 0x%04X", id);
    
    // Akceptujemy znanych producentów SPI Flash:
    // 0xEF = Winbond (W25Q128, W25Q64, etc.)
    // 0xE4 = Micron (M25Pxx) - JEDEC 0xE420 = prawdopodobnie M25P32 
    // 0xC8 = GigaDevice (GD25Q128)
    // 0x20 = Micron/Numonyx (M25P/N25Q)
    // 0x1F = Adesto/Atmel
    // 0x01 = Spansion/Cypress
    // 0xBF = SST/Microchip
    uint8_t mfr = id >> 8;
    bool knownChip = (mfr == 0xEF || mfr == 0xE4 || mfr == 0xC8 || 
                      mfr == 0x20 || mfr == 0x1F || mfr == 0x01 || mfr == 0xBF);
    
    if (!knownChip && mfr != 0x00 && mfr != 0xFF) {
        // Nieznany ale prawdopodobnie prawdziwy chip - zaakceptuj z ostrzeżeniem
        LOG_FMT(LOG_LEVEL_WARN, "Unknown flash manufacturer 0x%02X, proceeding anyway", mfr);
    } else if (mfr == 0x00 || mfr == 0xFF) {
        // 0x00 lub 0xFF = brak chipa lub uszkodzony
        LOG_FMT(LOG_LEVEL_ERROR, "No flash chip detected! JEDEC=0x%04X", id);
        flashReady = false;
        return false;
    }
    
    LOG_FMT(LOG_LEVEL_INFO, "Flash chip accepted: manufacturer=0x%02X, type=0x%02X", mfr, id & 0xFF);
    
    // Wczytaj FAT
    fat_load();
    
    flashReady = true;
    log_msg(LOG_LEVEL_INFO, "SPI Flash initialized OK");
    return true;
}

bool flash_is_ready() {
    return flashReady;
}

bool flash_format() {
    log_msg(LOG_LEVEL_WARN, "Formatting flash - erasing FAT sector...");
    flash_erase_sector(FAT_SECTOR);
    fatEntryCount = 0;
    memset(fatTable, 0xFF, sizeof(fatTable));
    log_msg(LOG_LEVEL_INFO, "Flash formatted");
    return true;
}

bool flash_file_exists(const char* path) {
    return fat_find_file(path) >= 0;
}

bool flash_file_write(const char* path, const uint8_t* data, uint32_t size) {
    if (!flashReady) return false;
    if (size > FLASH_SECTOR_SIZE * 10) {
        log_msg(LOG_LEVEL_ERROR, "File too large for flash storage");
        return false;
    }
    
    // Usuń stary plik jeśli istnieje (bez zapisu FAT - zapiszemy na końcu)
    int oldIdx = fat_find_file(path);
    if (oldIdx >= 0) {
        fatTable[oldIdx].valid = 0x00;
        LOG_FMT(LOG_LEVEL_DEBUG, "Old file removed from FAT: %s", path);
    }
    
    // Znajdź zakres sektorów
    uint16_t rangeStart, rangeEnd;
    fat_get_sector_range(path, rangeStart, rangeEnd);
    
    // Ile sektorów potrzeba?
    uint16_t sectorsNeeded = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;
    if (sectorsNeeded == 0) sectorsNeeded = 1;
    
    // Znajdź ciągłe wolne sektory
    uint16_t startSector = 0xFFFF;
    for (uint16_t s = rangeStart; s <= rangeEnd - sectorsNeeded + 1; s++) {
        bool allFree = true;
        for (uint16_t k = 0; k < sectorsNeeded; k++) {
            bool used = false;
            for (int i = 0; i < fatEntryCount; i++) {
                if (fatTable[i].valid == 0x01 && 
                    (s + k) >= fatTable[i].startSector && 
                    (s + k) < fatTable[i].startSector + fatTable[i].sectorCount) {
                    used = true;
                    break;
                }
            }
            if (used) { allFree = false; break; }
        }
        if (allFree) { startSector = s; break; }
    }
    
    if (startSector == 0xFFFF) {
        LOG_FMT(LOG_LEVEL_ERROR, "No free sectors for file: %s", path);
        return false;
    }
    
    // Kasuj sektory
    for (uint16_t k = 0; k < sectorsNeeded; k++) {
        flash_erase_sector(startSector + k);
        yield(); // Zapobiegaj watchdog reset
    }
    
    // Zapisz dane
    uint32_t address = (uint32_t)startSector * FLASH_SECTOR_SIZE;
    flash_write_data(address, data, size);
    yield();
    
    // Dodaj wpis do FAT
    int freeSlot = -1;
    for (int i = 0; i < MAX_FLASH_FILES; i++) {
        if (fatTable[i].valid != 0x01) {
            freeSlot = i;
            break;
        }
    }
    
    if (freeSlot < 0) {
        log_msg(LOG_LEVEL_ERROR, "FAT full - no free slots");
        return false;
    }
    
    strncpy(fatTable[freeSlot].filename, path, MAX_FILENAME_LEN - 1);
    fatTable[freeSlot].filename[MAX_FILENAME_LEN - 1] = '\0';
    fatTable[freeSlot].startSector = startSector;
    fatTable[freeSlot].sectorCount = sectorsNeeded;
    fatTable[freeSlot].fileSize = size;
    fatTable[freeSlot].valid = 0x01;
    
    if (freeSlot >= fatEntryCount) fatEntryCount = freeSlot + 1;
    
    fat_save();
    
    LOG_FMT(LOG_LEVEL_DEBUG, "File written: %s (%u bytes, sector %d)", path, size, startSector);
    return true;
}

bool flash_file_write_string(const char* path, const String& content) {
    return flash_file_write(path, (const uint8_t*)content.c_str(), content.length());
}

int flash_file_read(const char* path, uint8_t* buffer, uint32_t maxSize) {
    if (!flashReady) return -1;
    
    int idx = fat_find_file(path);
    if (idx < 0) return -1;
    
    uint32_t readSize = fatTable[idx].fileSize;
    if (readSize > maxSize) readSize = maxSize;
    
    uint32_t address = (uint32_t)fatTable[idx].startSector * FLASH_SECTOR_SIZE;
    flash_read_data(address, buffer, readSize);
    
    return readSize;
}

String flash_file_read_string(const char* path) {
    if (!flashReady) return "";
    
    int idx = fat_find_file(path);
    if (idx < 0) return "";
    
    uint32_t size = fatTable[idx].fileSize;
    if (size > 8192) size = 8192; // Limit bezpieczeństwa
    
    char* buf = (char*)malloc(size + 1);
    if (!buf) return "";
    
    uint32_t address = (uint32_t)fatTable[idx].startSector * FLASH_SECTOR_SIZE;
    flash_read_data(address, (uint8_t*)buf, size);
    buf[size] = '\0';
    
    String result(buf);
    free(buf);
    return result;
}

bool flash_file_delete(const char* path) {
    int idx = fat_find_file(path);
    if (idx < 0) return false;
    
    fatTable[idx].valid = 0x00; // Oznacz jako skasowany
    fat_save();
    
    LOG_FMT(LOG_LEVEL_DEBUG, "File deleted: %s", path);
    return true;
}

bool flash_file_append(const char* path, const String& content) {
    // Odczytaj istniejącą zawartość
    String existing = flash_file_read_string(path);
    existing += content;
    return flash_file_write_string(path, existing);
}

int flash_list_files(const char* dirPrefix, char files[][MAX_FILENAME_LEN], int maxFiles) {
    int count = 0;
    int prefixLen = strlen(dirPrefix);
    
    for (int i = 0; i < fatEntryCount && count < maxFiles; i++) {
        if (fatTable[i].valid == 0x01) {
            if (strncmp(fatTable[i].filename, dirPrefix, prefixLen) == 0) {
                strncpy(files[count], fatTable[i].filename, MAX_FILENAME_LEN - 1);
                files[count][MAX_FILENAME_LEN - 1] = '\0';
                count++;
            }
        }
    }
    return count;
}

bool flash_mkdir(const char* path) {
    // W naszym flat FS katalogi nie istnieją fizycznie - to konwencja nazewnictwa
    // Ale zapisujemy "marker" żeby flash_dir_exists() działało
    char marker[64];
    snprintf(marker, sizeof(marker), "%s/.dir", path);
    uint8_t dummy = 0x01;
    return flash_file_write(marker, &dummy, 1);
}

bool flash_dir_exists(const char* path) {
    // Sprawdź marker lub czy istnieje jakikolwiek plik z tym prefiksem
    char dirPrefix[64];
    snprintf(dirPrefix, sizeof(dirPrefix), "%s/", path);
    
    for (int i = 0; i < fatEntryCount; i++) {
        if (fatTable[i].valid == 0x01 && 
            strncmp(fatTable[i].filename, dirPrefix, strlen(dirPrefix)) == 0) {
            return true;
        }
    }
    return false;
}

uint32_t flash_get_free_sectors() {
    uint32_t used = 0;
    for (int i = 0; i < fatEntryCount; i++) {
        if (fatTable[i].valid == 0x01) {
            used += fatTable[i].sectorCount;
        }
    }
    // Sektor 0 (FAT) jest zawsze zajęty
    return (LOGS_END - FAT_SECTOR) - used;
}

uint32_t flash_get_used_sectors() {
    uint32_t used = 1; // FAT sector
    for (int i = 0; i < fatEntryCount; i++) {
        if (fatTable[i].valid == 0x01) {
            used += fatTable[i].sectorCount;
        }
    }
    return used;
}

uint32_t flash_get_total_size() {
    return FLASH_TOTAL_SIZE;
}
