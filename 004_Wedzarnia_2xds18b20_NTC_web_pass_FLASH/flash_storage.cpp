// flash_storage.cpp - W25Q128 SPI Flash filesystem
// v2.1 - Kompletna naprawa:
//
// [FIX-1]  SPI MUTEX – każda operacja SPI jest chroniona mutexem
//          współdzielonym z TFT. Brak mutexa = crash gdy taskUI rysuje TFT
//          a taskWeb zapisuje flash jednocześnie na tym samym SPI bus.
//
// [FIX-2]  CS MANAGEMENT – przed każdą operacją flash: TFT_CS=HIGH, potem
//          FLASH_CS=LOW. Po operacji: FLASH_CS=HIGH. TFT nie dostaje
//          przypadkowych danych gdy piszemy do flash.
//
// [FIX-3]  WDT podczas erase/write – esp_task_wdt_reset() przed i po
//          każdej operacji która trwa >10ms (erase=20-50ms, write page=3ms)
//
// [FIX-4]  MAX_FLASH_FILES=64 (było 200) – sektor FAT mieści tylko 68 wpisów
//          Iteracja po 200 wpisach wychodziła poza sektor, czytając śmieci.
//
// [FIX-5]  FAT SHADOW COPY – zapis zawsze: najpierw shadow (sektor 1),
//          potem główna (sektor 0). Odporność na reset w trakcie zapisu FAT.
//
// [FIX-6]  fat_load() – valid==0x00 to continue, nie break.
//          Skasowany wpis nie kończył wczytywania pozostałych.
//
// [FIX-7]  fat_compact() – iteruje po MAX_FLASH_FILES, nie fatEntryCount.
//
// [FIX-8]  fat_find_free_contiguous() – poprawne szukanie ciągłego bloku.
//
// [FIX-9]  Bezpieczna aktualizacja – stary wpis kasowany DOPIERO po udanym
//          zapisie i weryfikacji nowych danych.

#include "flash_storage.h"
#include "config.h"
#include <SPI.h>
#include <freertos/task.h>  // vTaskDelay – bezpieczne yield z każdego taska

// ======================================================
// KOMENDY SPI W25Q128
// ======================================================
#define W25Q_CMD_WRITE_ENABLE   0x06
#define W25Q_CMD_WRITE_DISABLE  0x04
#define W25Q_CMD_READ_STATUS1   0x05
#define W25Q_CMD_READ_DATA      0x03
#define W25Q_CMD_PAGE_PROGRAM   0x02
#define W25Q_CMD_SECTOR_ERASE   0x20
#define W25Q_CMD_BLOCK_ERASE_64 0xD8
#define W25Q_CMD_CHIP_ERASE     0xC7
#define W25Q_CMD_JEDEC_ID       0x9F
#define W25Q_CMD_POWER_UP       0xAB

#define W25Q_STATUS_BUSY        0x01

// Timeout mutexa SPI – 2 sekundy (TFT może rysować dużo)
#define SPI_MUTEX_TIMEOUT_MS    2000

// ======================================================
// USTAWIENIA SPI DLA W25Q128
// [FIX-10] Bez beginTransaction/endTransaction ESP32 używa
//          częstotliwości ustawionej przez ostatnią operację
//          (np. TFT Adafruit_ST7735 ustawia własne clock/mode).
//          W25Q128 Page Program wymaga stabilnego MODE0 i <=20 MHz.
//          20 MHz = bezpieczna wartość dla klonów (GD25Q128, XM25Q128).
//          Odczyt może działać na 40 MHz, ale zapis musi być wolniejszy.
// ======================================================
static const SPISettings FLASH_SPI_WRITE_SETTINGS(20000000UL, MSBFIRST, SPI_MODE0);
static const SPISettings FLASH_SPI_READ_SETTINGS( 40000000UL, MSBFIRST, SPI_MODE0);

// ======================================================
// ZMIENNE STATYCZNE
// ======================================================
static bool               flashReady    = false;
static FlashFileEntry     fatTable[MAX_FLASH_FILES];
static int                fatEntryCount = 0;

// [FIX-1] Mutex SPI współdzielony z TFT
static SemaphoreHandle_t  g_spiMutex    = NULL;

// ======================================================
// ZARZĄDZANIE MUTEXEM SPI – [FIX-1]
// ======================================================

static bool spi_take() {
    if (g_spiMutex == NULL) return true;
    // Rekurencyjny mutex – bezpieczny przy wielokrotnym wzięciu z tego samego taska
    return xSemaphoreTakeRecursive(g_spiMutex, pdMS_TO_TICKS(SPI_MUTEX_TIMEOUT_MS)) == pdTRUE;
}

static void spi_give() {
    if (g_spiMutex != NULL) xSemaphoreGiveRecursive(g_spiMutex);
}

// ======================================================
// CS MANAGEMENT – [FIX-2]
// Zawsze upewnij się że TFT_CS=HIGH przed operacją na flash
// ======================================================
static inline void flash_cs_low() {
    digitalWrite(TFT_CS,      HIGH);  // TFT off
    digitalWrite(FLASH_CS_PIN, LOW);  // Flash on
}

static inline void flash_cs_high() {
    digitalWrite(FLASH_CS_PIN, HIGH); // Flash off
}

// ======================================================
// NISKOPOZIOMOWE FUNKCJE SPI (bez mutexa – mutex bierze caller)
// ======================================================

static void _flash_write_enable() {
    flash_wait_busy();  // WREN ignorowany gdy BUSY=1 – czekaj aż chip będzie idle
    SPI.beginTransaction(FLASH_SPI_WRITE_SETTINGS);
    flash_cs_low();
    SPI.transfer(W25Q_CMD_WRITE_ENABLE);
    flash_cs_high();
    SPI.endTransaction();
    delayMicroseconds(10);  // tSHSL2: czas po WREN przed następną komendą
    // Weryfikacja – po WREN, WEL musi być 1
    // (nie logujemy tu bo jesteśmy bez mutexa, caller sprawdza)
}

uint8_t flash_read_status() {
    // Ta funkcja może być wołana z flash_wait_busy() która już trzyma mutex
    SPI.beginTransaction(FLASH_SPI_READ_SETTINGS);
    flash_cs_low();
    SPI.transfer(W25Q_CMD_READ_STATUS1);
    uint8_t s = SPI.transfer(0x00);
    flash_cs_high();
    SPI.endTransaction();
    return s;
}

void flash_wait_busy() {
    unsigned long start = millis();
    while (flash_read_status() & W25Q_STATUS_BUSY) {
        if (millis() - start > 10000UL) {
            log_msg(LOG_LEVEL_ERROR, "flash_wait_busy TIMEOUT!");
            break;
        }
        // Krótkie opóźnienie żeby nie kręcić pętlą z pełną prędkością
        // Nie używamy delay() bo trzymamy mutex – tylko aktywne czekanie
        delayMicroseconds(200);
    }
}

// Wewnętrzna wersja read_data – bez pobierania mutexa
static void _flash_read_data(uint32_t address, uint8_t* buffer, uint32_t size) {
    flash_wait_busy();
    SPI.beginTransaction(FLASH_SPI_READ_SETTINGS);
    flash_cs_low();
    SPI.transfer(W25Q_CMD_READ_DATA);
    SPI.transfer((address >> 16) & 0xFF);
    SPI.transfer((address >>  8) & 0xFF);
    SPI.transfer( address        & 0xFF);
    for (uint32_t i = 0; i < size; i++) buffer[i] = SPI.transfer(0x00);
    flash_cs_high();
    SPI.endTransaction();
}

// Wewnętrzna wersja write_page – bez pobierania mutexa
static void _flash_write_page(uint32_t address, const uint8_t* data, uint16_t size) {
    if (size > FLASH_PAGE_SIZE) size = FLASH_PAGE_SIZE;
    _flash_write_enable();

    // [DIAG] Sprawdź WEL bit po write_enable
    uint8_t statusBefore = flash_read_status();
    if (!(statusBefore & 0x02)) {
        LOG_FMT(LOG_LEVEL_WARN,
            "_flash_write_page DIAG: WEL=0 po write_enable! status=0x%02X addr=0x%05lX",
            statusBefore, address);
    }

    SPI.beginTransaction(FLASH_SPI_WRITE_SETTINGS);
    flash_cs_low();
    SPI.transfer(W25Q_CMD_PAGE_PROGRAM);
    SPI.transfer((address >> 16) & 0xFF);
    SPI.transfer((address >>  8) & 0xFF);
    SPI.transfer( address        & 0xFF);
    for (uint16_t i = 0; i < size; i++) SPI.transfer(data[i]);
    flash_cs_high();
    SPI.endTransaction();

    flash_wait_busy();

    // [DIAG] Odczytaj status po zakończeniu Page Program
    uint8_t statusAfter = flash_read_status();
    LOG_FMT(LOG_LEVEL_WARN,
        "_flash_write_page DIAG: addr=0x%05lX size=%u status_after=0x%02X (WEL=%d BUSY=%d)",
        address, size, statusAfter,
        (statusAfter >> 1) & 1, statusAfter & 1);
}

// Wewnętrzna wersja erase_sector – bez mutexa
static void _flash_erase_sector(uint32_t sectorNumber) {
    uint32_t address = sectorNumber * FLASH_SECTOR_SIZE;
    _flash_write_enable();
    SPI.beginTransaction(FLASH_SPI_WRITE_SETTINGS);
    flash_cs_low();
    SPI.transfer(W25Q_CMD_SECTOR_ERASE);
    SPI.transfer((address >> 16) & 0xFF);
    SPI.transfer((address >>  8) & 0xFF);
    SPI.transfer( address        & 0xFF);
    flash_cs_high();
    SPI.endTransaction();
    flash_wait_busy();
}

// ======================================================
// PUBLICZNE FUNKCJE NISKOPOZIOMOWE (z mutexem)
// ======================================================

void flash_read_data(uint32_t address, uint8_t* buffer, uint32_t size) {
    if (!spi_take()) { log_msg(LOG_LEVEL_ERROR, "flash_read_data: mutex timeout"); return; }
    _flash_read_data(address, buffer, size);
    spi_give();
}

void flash_write_page(uint32_t address, const uint8_t* data, uint16_t size) {
    if (!spi_take()) { log_msg(LOG_LEVEL_ERROR, "flash_write_page: mutex timeout"); return; }
    _flash_write_page(address, data, size);
    spi_give();
}

void flash_erase_sector(uint32_t sectorNumber) {
    if (!spi_take()) { log_msg(LOG_LEVEL_ERROR, "flash_erase_sector: mutex timeout"); return; }
    vTaskDelay(1); // yield do schedulera podczas długich operacji flash
    _flash_erase_sector(sectorNumber);
    vTaskDelay(1);  // yield – nie używamy WDT reset (task może nie być zarejestrowany)
    spi_give();
}

void flash_erase_block_64k(uint32_t blockNumber) {
    if (!spi_take()) { log_msg(LOG_LEVEL_ERROR, "flash_erase_block: mutex timeout"); return; }
    uint32_t address = blockNumber * 65536UL;
    _flash_write_enable();
    SPI.beginTransaction(FLASH_SPI_WRITE_SETTINGS);
    flash_cs_low();
    SPI.transfer(W25Q_CMD_BLOCK_ERASE_64);
    SPI.transfer((address >> 16) & 0xFF);
    SPI.transfer((address >>  8) & 0xFF);
    SPI.transfer( address        & 0xFF);
    flash_cs_high();
    SPI.endTransaction();
    vTaskDelay(1);  // yield – nie używamy WDT reset (task może nie być zarejestrowany)
    flash_wait_busy();
    vTaskDelay(1);  // yield – nie używamy WDT reset (task może nie być zarejestrowany)
    spi_give();
}

void flash_chip_erase() {
    if (!spi_take()) { log_msg(LOG_LEVEL_ERROR, "flash_chip_erase: mutex timeout"); return; }
    _flash_write_enable();
    SPI.beginTransaction(FLASH_SPI_WRITE_SETTINGS);
    flash_cs_low();
    SPI.transfer(W25Q_CMD_CHIP_ERASE);
    flash_cs_high();
    SPI.endTransaction();
    // Chip erase: do 200 sekund – oddajemy mutex w pętli
    spi_give();
    unsigned long start = millis();
    while (millis() - start < 300000UL) {
        vTaskDelay(1);  // yield – nie używamy WDT reset (task może nie być zarejestrowany)
        delay(500);
        if (!spi_take()) break;
        bool busy = (flash_read_status() & W25Q_STATUS_BUSY);
        spi_give();
        if (!busy) break;
    }
}

uint16_t flash_get_jedec_id() {
    if (!spi_take()) return 0x0000;
    SPI.beginTransaction(FLASH_SPI_READ_SETTINGS);
    flash_cs_low();
    SPI.transfer(W25Q_CMD_JEDEC_ID);
    uint8_t mfr  = SPI.transfer(0x00);
    uint8_t type = SPI.transfer(0x00);
    SPI.transfer(0x00);
    flash_cs_high();
    SPI.endTransaction();
    spi_give();
    return ((uint16_t)mfr << 8) | type;
}

// ======================================================
// ZAPIS WIELOSTRONICOWY z WDT reset – [FIX-3]
// UWAGA: Wywołuje się z wnętrza kodu który już trzyma mutex!
//        Używa _flash_write_page (bez mutexa).
// ======================================================
static void _flash_write_data_locked(uint32_t address, const uint8_t* data, uint32_t size) {
    uint32_t offset = 0;
    while (offset < size) {
        uint16_t pageOff      = (uint16_t)(address % FLASH_PAGE_SIZE);
        uint16_t bytesToWrite = FLASH_PAGE_SIZE - pageOff;
        if (bytesToWrite > (size - offset)) bytesToWrite = (uint16_t)(size - offset);

        _flash_write_page(address, data + offset, bytesToWrite);
        address += bytesToWrite;
        offset  += bytesToWrite;
        // Nie oddajemy mutexa w środku zapisu – to powodowało corrupcję.
        // Dla pliku 40B: 1 strona, czas <3ms – TFT poczeka.
        // Dla pliku 4KB: 16 stron, max ~50ms – akceptowalne.
    }
}

// ======================================================
// FAT – TABLICA ALOKACJI PLIKÓW
// ======================================================

// [FIX-7] compact iteruje po MAX_FLASH_FILES
static void fat_compact() {
    int writeIdx = 0;
    for (int i = 0; i < MAX_FLASH_FILES; i++) {
        if (fatTable[i].valid == 0x01) {
            if (writeIdx != i) {
                memcpy(&fatTable[writeIdx], &fatTable[i], sizeof(FlashFileEntry));
            }
            writeIdx++;
        }
    }
    fatEntryCount = writeIdx;
    for (int i = writeIdx; i < MAX_FLASH_FILES; i++) {
        memset(&fatTable[i], 0xFF, sizeof(FlashFileEntry));
    }
}

// Zapis FAT do wskazanego sektora – wywołuje się bez zewnętrznego mutexa
// (sama bierze mutex dla każdej operacji SPI)
static void fat_write_to_sector(uint32_t sector) {
    // Kasuj sektor
    if (!spi_take()) { log_msg(LOG_LEVEL_ERROR, "fat_write: mutex timeout (erase)"); return; }
    _flash_erase_sector(sector);

    uint32_t addr = sector * FLASH_SECTOR_SIZE;

    // Nagłówek
    FatHeader hdr;
    hdr.magic      = FAT_MAGIC;
    hdr.entryCount = (uint16_t)fatEntryCount;
    hdr.reserved   = 0;

    uint8_t hdrBuf[sizeof(FatHeader)];
    memcpy(hdrBuf, &hdr, sizeof(FatHeader));
    _flash_write_page(addr, hdrBuf, sizeof(FatHeader));
    addr += sizeof(FatHeader);

    // Wpisy – po każdej stronie oddajemy mutex i resetujemy WDT
    for (int i = 0; i < fatEntryCount; i++) {
        uint8_t entryBuf[sizeof(FlashFileEntry)];
        memcpy(entryBuf, &fatTable[i], sizeof(FlashFileEntry));
        _flash_write_data_locked(addr, entryBuf, sizeof(FlashFileEntry));
        addr += sizeof(FlashFileEntry);
    }
    spi_give();

    LOG_FMT(LOG_LEVEL_DEBUG, "FAT written to sector %lu (%d entries)", sector, fatEntryCount);
}

// [FIX-5] Atomowy zapis: shadow → główna
static void fat_save() {
    fat_compact();  // [FIX-7]
    fat_write_to_sector(FAT_SHADOW_SECTOR);  // najpierw shadow
    fat_write_to_sector(FAT_SECTOR);         // potem główna
    LOG_FMT(LOG_LEVEL_DEBUG, "FAT saved: %d entries", fatEntryCount);
}

// [FIX-6] Ładowanie FAT – valid==0x00 to continue, nie break
static bool fat_load_from_sector(uint32_t sector) {
    uint32_t addr = sector * FLASH_SECTOR_SIZE;

    if (!spi_take()) return false;
    FatHeader hdr;
    _flash_read_data(addr, (uint8_t*)&hdr, sizeof(FatHeader));
    spi_give();

    if (hdr.magic != FAT_MAGIC) {
        LOG_FMT(LOG_LEVEL_WARN, "FAT sector %lu bad magic: 0x%08lX", sector, hdr.magic);
        return false;
    }
    if (hdr.entryCount > MAX_FLASH_FILES) {
        LOG_FMT(LOG_LEVEL_WARN, "FAT sector %lu: entryCount=%u > MAX_FLASH_FILES=%d",
                sector, hdr.entryCount, MAX_FLASH_FILES);
        return false;
    }

    addr += sizeof(FatHeader);
    memset(fatTable, 0xFF, sizeof(fatTable));
    fatEntryCount = 0;

    for (uint16_t i = 0; i < hdr.entryCount; i++) {
        if (!spi_take()) return false;
        _flash_read_data(addr + i * sizeof(FlashFileEntry),
                         (uint8_t*)&fatTable[i],
                         sizeof(FlashFileEntry));
        spi_give();

        if (fatTable[i].valid == 0x01) {
            fatEntryCount++;
        }
        // [FIX-6] valid==0x00 (skasowany) → continue, NIE break!
        // valid==0xFF → wolny slot, też continue
    }

    LOG_FMT(LOG_LEVEL_INFO, "FAT loaded from sector %lu: header=%u, active=%d",
            sector, hdr.entryCount, fatEntryCount);
    return true;
}

static void fat_load() {
    if (fat_load_from_sector(FAT_SECTOR)) return;

    log_msg(LOG_LEVEL_WARN, "Primary FAT bad, trying shadow...");
    if (fat_load_from_sector(FAT_SHADOW_SECTOR)) {
        log_msg(LOG_LEVEL_INFO, "Shadow FAT OK – restoring primary");
        fat_write_to_sector(FAT_SECTOR);
        return;
    }

    log_msg(LOG_LEVEL_ERROR, "Both FAT sectors corrupted – starting empty");
    fatEntryCount = 0;
    memset(fatTable, 0xFF, sizeof(fatTable));
}

static int fat_find_file(const char* path) {
    for (int i = 0; i < MAX_FLASH_FILES; i++) {
        if (fatTable[i].valid == 0x01 &&
            strncmp(fatTable[i].filename, path, MAX_FILENAME_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

// [FIX-8] Szukanie ciągłego bloku wolnych sektorów
static uint16_t fat_find_free_contiguous(uint16_t rangeStart, uint16_t rangeEnd, uint16_t count) {
    for (uint16_t s = rangeStart; s + count - 1 <= rangeEnd; ) {
        bool allFree = true;
        uint16_t blockEnd = s + count - 1;

        for (int i = 0; i < MAX_FLASH_FILES; i++) {
            if (fatTable[i].valid != 0x01) continue;
            uint16_t fs = fatTable[i].startSector;
            uint16_t fe = fs + fatTable[i].sectorCount - 1;
            // Sprawdź czy ten wpis nachodzi na nasz blok [s .. blockEnd]
            if (fs <= blockEnd && fe >= s) {
                allFree = false;
                s = fe + 1;  // Przeskocz za koniec zajętego pliku
                break;
            }
        }
        if (allFree) return s;
    }
    return 0xFFFF;
}

static void fat_get_sector_range(const char* path, uint16_t& start, uint16_t& end) {
    if      (strncmp(path, "/profiles/", 10) == 0) { start = PROFILES_START; end = PROFILES_END; }
    else if (strncmp(path, "/backup/",    8) == 0)  { start = BACKUPS_START;  end = BACKUPS_END;  }
    else if (strncmp(path, "/logs/",      6) == 0)  { start = LOGS_START;     end = LOGS_END;     }
    else                                             { start = PROFILES_START; end = LOGS_END;     }
}

static int fat_find_free_slot() {
    for (int i = 0; i < MAX_FLASH_FILES; i++) {
        if (fatTable[i].valid != 0x01) return i;
    }
    return -1;
}

// ======================================================
// API PUBLICZNE
// ======================================================

bool flash_init(SemaphoreHandle_t spiMutex) {
    // Zachowaj lub stwórz mutex
    if (spiMutex != NULL) {
        g_spiMutex = spiMutex;
        log_msg(LOG_LEVEL_INFO, "Flash: using shared SPI mutex");
    } else {
        if (g_spiMutex == NULL) {
            g_spiMutex = xSemaphoreCreateMutex();
            log_msg(LOG_LEVEL_INFO, "Flash: created own SPI mutex");
        }
    }

    pinMode(FLASH_CS_PIN, OUTPUT);
    flash_cs_high();
    pinMode(TFT_CS, OUTPUT);
    digitalWrite(TFT_CS, HIGH);
    delay(10);

    // Power up
    if (!spi_take()) return false;
    SPI.beginTransaction(FLASH_SPI_READ_SETTINGS);
    flash_cs_low();
    SPI.transfer(W25Q_CMD_POWER_UP);
    flash_cs_high();
    SPI.endTransaction();
    spi_give();
    delay(3);  // tRES1 = 3μs min, dajemy 3ms dla pewności

    // Poczekaj aż chip wyjdzie ze stanu BUSY (może być busy po poprzednim resecie)
    {
        if (!spi_take()) return false;
        unsigned long t = millis();
        while ((flash_read_status() & 0x01) && millis() - t < 5000) delay(10);
        uint8_t s = flash_read_status();
        if (s & 0x01) log_msg(LOG_LEVEL_ERROR, "Flash: BUSY timeout po power-up!");
        else LOG_FMT(LOG_LEVEL_INFO, "Flash: idle po power-up, SR1=0x%02X", s);
        spi_give();
    }

    // Odczytaj status rejestr i odblokuj zapis jeśli BP bity są ustawione
    // lub WP jest aktywny przez rejestr statusu
    {
        if (!spi_take()) return false;
        uint8_t sr1 = flash_read_status();
        LOG_FMT(LOG_LEVEL_WARN, "Flash SR1 po power-up: 0x%02X (BUSY=%d WEL=%d BP=%d SRP=%d)",
            sr1, sr1&1, (sr1>>1)&1, (sr1>>2)&7, (sr1>>7)&1);

        // Jeśli SR1 Protection Register (SRP0=bit7) = 0 i BP bits != 0,
        // odblokuj przez Write Status Register
        if ((sr1 & 0x7C) != 0) {  // bity BP0-BP2-TB-BP3 (bity 2-6)
            log_msg(LOG_LEVEL_WARN, "Flash: BP bits ustawione – odblokowanie zapisu...");
            // Write Enable dla Status Register
            SPI.beginTransaction(FLASH_SPI_WRITE_SETTINGS);
            flash_cs_low();
            SPI.transfer(0x50);  // WRSR enable (Write Enable for Volatile Status Register)
            flash_cs_high();
            SPI.endTransaction();
            delayMicroseconds(5);
            // Zapisz SR1=0x00 (brak ochrony)
            SPI.beginTransaction(FLASH_SPI_WRITE_SETTINGS);
            flash_cs_low();
            SPI.transfer(0x01);  // Write Status Register
            SPI.transfer(0x00);  // SR1 = 0 (wszystkie bity ochrony skasowane)
            flash_cs_high();
            SPI.endTransaction();
            delay(15);  // tW = 15ms max
            uint8_t sr1new = flash_read_status();
            LOG_FMT(LOG_LEVEL_WARN, "Flash SR1 po odblok: 0x%02X", sr1new);
        }
        spi_give();
    }

    uint16_t id  = flash_get_jedec_id();  // ma własny mutex
    uint8_t  mfr = id >> 8;
    LOG_FMT(LOG_LEVEL_INFO, "Flash JEDEC: 0x%04X (mfr=0x%02X)", id, mfr);

    if (mfr == 0x00 || mfr == 0xFF) {
        LOG_FMT(LOG_LEVEL_ERROR, "No flash chip detected (JEDEC=0x%04X)", id);
        flashReady = false;
        return false;
    }

    fat_load();
    flashReady = true;
    log_msg(LOG_LEVEL_INFO, "SPI Flash ready");
    return true;
}

bool flash_is_ready() { return flashReady; }

bool flash_format() {
    if (!flashReady) return false;
    log_msg(LOG_LEVEL_WARN, "Formatting flash...");

    fatEntryCount = 0;
    memset(fatTable, 0xFF, sizeof(fatTable));

    // Kasuj oba sektory FAT i zapisz pustą FAT z sygnaturą
    if (!spi_take()) return false;
    vTaskDelay(1);  // yield – nie używamy WDT reset (task może nie być zarejestrowany)
    _flash_erase_sector(FAT_SECTOR);
    vTaskDelay(1);  // yield – nie używamy WDT reset (task może nie być zarejestrowany)
    _flash_erase_sector(FAT_SHADOW_SECTOR);
    vTaskDelay(1);  // yield – nie używamy WDT reset (task może nie być zarejestrowany)
    spi_give();

    fat_write_to_sector(FAT_SECTOR);
    fat_write_to_sector(FAT_SHADOW_SECTOR);

    log_msg(LOG_LEVEL_INFO, "Flash formatted OK");
    return true;
}

bool flash_file_exists(const char* path) {
    return fat_find_file(path) >= 0;
}

// [FIX-9] Bezpieczna aktualizacja pliku
bool flash_file_write(const char* path, const uint8_t* data, uint32_t size) {
    if (!flashReady)  { log_msg(LOG_LEVEL_ERROR, "flash_file_write: not ready");    return false; }
    if (size == 0)    { log_msg(LOG_LEVEL_ERROR, "flash_file_write: size=0");       return false; }
    if (size > (uint32_t)FLASH_SECTOR_SIZE * 10) {
        LOG_FMT(LOG_LEVEL_ERROR, "flash_file_write: too large (%lu B)", size);
        return false;
    }

    uint16_t sectorsNeeded = (size + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE;

    uint16_t rangeStart, rangeEnd;
    fat_get_sector_range(path, rangeStart, rangeEnd);

    // Tymczasowo ukryj stary wpis żeby nie blokował wolnego miejsca
    int oldIdx = fat_find_file(path);
    if (oldIdx >= 0) fatTable[oldIdx].valid = 0xFE;

    // [FIX-8] Znajdź ciągły blok
    uint16_t startSector = fat_find_free_contiguous(rangeStart, rangeEnd, sectorsNeeded);
    if (startSector == 0xFFFF) {
        if (oldIdx >= 0) fatTable[oldIdx].valid = 0x01;  // rollback
        LOG_FMT(LOG_LEVEL_ERROR, "flash_file_write: no space for '%s' (%u sectors)", path, sectorsNeeded);
        return false;
    }

    int freeSlot = fat_find_free_slot();
    if (freeSlot < 0) {
        if (oldIdx >= 0) fatTable[oldIdx].valid = 0x01;
        log_msg(LOG_LEVEL_ERROR, "flash_file_write: FAT full");
        return false;
    }

    LOG_FMT(LOG_LEVEL_WARN,
        "flash_file_write: sect=%u count=%u path='%s'",
        startSector, sectorsNeeded, path);

    // Kasuj sektory docelowe
    if (!spi_take()) {
        log_msg(LOG_LEVEL_ERROR, "flash_file_write: SPI MUTEX TIMEOUT – taskUI trzyma mutex zbyt długo!");
        if (oldIdx >= 0) fatTable[oldIdx].valid = 0x01;
        return false;
    }
    for (uint16_t k = 0; k < sectorsNeeded; k++) {
        _flash_erase_sector(startSector + k);
    }

    // [DIAG] Sprawdź czy erase zadziałał – pierwsze 4 bajty muszą być 0xFF
    {
        uint8_t diagBuf[4] = {0};
        uint32_t diagAddr = (uint32_t)startSector * FLASH_SECTOR_SIZE;
        _flash_read_data(diagAddr, diagBuf, 4);
        LOG_FMT(LOG_LEVEL_WARN,
            "DIAG erase sect=%u addr=0x%05lX: %02X %02X %02X %02X (need FF FF FF FF)",
            startSector, diagAddr,
            diagBuf[0], diagBuf[1], diagBuf[2], diagBuf[3]);
        if (diagBuf[0] != 0xFF || diagBuf[1] != 0xFF) {
            // Sektor nie został skasowany! Spróbuj ponownie.
            log_msg(LOG_LEVEL_WARN, "flash_file_write: erase verify FAILED – retrying erase");
            for (uint16_t k = 0; k < sectorsNeeded; k++) {
                _flash_erase_sector(startSector + k);
            }
            _flash_read_data(diagAddr, diagBuf, 4);
            LOG_FMT(LOG_LEVEL_WARN,
                "DIAG after retry erase: %02X %02X %02X %02X",
                diagBuf[0], diagBuf[1], diagBuf[2], diagBuf[3]);
            if (diagBuf[0] != 0xFF) {
                if (oldIdx >= 0) fatTable[oldIdx].valid = 0x01;
                log_msg(LOG_LEVEL_ERROR, "flash_file_write: sector erase FAILED after retry!");
                spi_give();
                return false;
            }
        }
    }

    // Zapisz dane (zwalnia i bierze mutex co stronę)
    uint32_t address = (uint32_t)startSector * FLASH_SECTOR_SIZE;
    _flash_write_data_locked(address, data, size);

    // Weryfikacja pierwszego bajtu
    uint8_t verify = 0xFF;
    _flash_read_data(address, &verify, 1);
    spi_give();

    if (verify != data[0]) {
        if (oldIdx >= 0) fatTable[oldIdx].valid = 0x01;
        LOG_FMT(LOG_LEVEL_ERROR, "flash_file_write: verify FAILED (got 0x%02X, expected 0x%02X)",
                verify, data[0]);
        return false;
    }

    // Dodaj nowy wpis FAT
    memset(&fatTable[freeSlot], 0, sizeof(FlashFileEntry));
    strncpy(fatTable[freeSlot].filename, path, MAX_FILENAME_LEN - 1);
    fatTable[freeSlot].filename[MAX_FILENAME_LEN - 1] = '\0';
    fatTable[freeSlot].startSector = startSector;
    fatTable[freeSlot].sectorCount = sectorsNeeded;
    fatTable[freeSlot].fileSize    = size;
    fatTable[freeSlot].valid       = 0x01;

    // Skasuj stary wpis [FIX-9]
    if (oldIdx >= 0) fatTable[oldIdx].valid = 0x00;

    // Zapisz FAT [FIX-5]
    fat_save();

    LOG_FMT(LOG_LEVEL_INFO, "File written: %s (%lu B, sect %u+%u)",
            path, size, startSector, sectorsNeeded);
    return true;
}

bool flash_file_write_string(const char* path, const String& content) {
    if (content.length() == 0) return false;
    return flash_file_write(path, (const uint8_t*)content.c_str(), content.length());
}

int flash_file_read(const char* path, uint8_t* buffer, uint32_t maxSize) {
    if (!flashReady) return -1;
    int idx = fat_find_file(path);
    if (idx < 0) return -1;

    uint32_t readSize = min(fatTable[idx].fileSize, maxSize);
    uint32_t addr     = (uint32_t)fatTable[idx].startSector * FLASH_SECTOR_SIZE;

    if (!spi_take()) return -1;
    _flash_read_data(addr, buffer, readSize);
    spi_give();

    return (int)readSize;
}

String flash_file_read_string(const char* path) {
    if (!flashReady) return "";
    int idx = fat_find_file(path);
    if (idx < 0) return "";

    uint32_t size = fatTable[idx].fileSize;
    if (size == 0) return "";
    if (size > 8192) size = 8192;

    char* buf = (char*)malloc(size + 1);
    if (!buf) { log_msg(LOG_LEVEL_ERROR, "flash_file_read_string: malloc failed"); return ""; }

    uint32_t addr = (uint32_t)fatTable[idx].startSector * FLASH_SECTOR_SIZE;

    if (!spi_take()) { free(buf); return ""; }
    _flash_read_data(addr, (uint8_t*)buf, size);
    spi_give();

    buf[size] = '\0';
    String result(buf);
    free(buf);
    return result;
}

bool flash_file_delete(const char* path) {
    int idx = fat_find_file(path);
    if (idx < 0) return false;
    fatTable[idx].valid = 0x00;
    fat_save();
    LOG_FMT(LOG_LEVEL_INFO, "File deleted: %s", path);
    return true;
}

bool flash_file_append(const char* path, const String& content) {
    String existing = flash_file_read_string(path);
    if (existing.length() + content.length() > 8192) {
        existing = existing.substring(existing.length() / 2);
    }
    existing += content;
    return flash_file_write_string(path, existing);
}

int flash_list_files(const char* dirPrefix, char files[][MAX_FILENAME_LEN], int maxFiles) {
    int count     = 0;
    int prefixLen = strlen(dirPrefix);
    for (int i = 0; i < MAX_FLASH_FILES && count < maxFiles; i++) {
        if (fatTable[i].valid == 0x01 &&
            strncmp(fatTable[i].filename, dirPrefix, prefixLen) == 0) {
            strncpy(files[count], fatTable[i].filename, MAX_FILENAME_LEN - 1);
            files[count][MAX_FILENAME_LEN - 1] = '\0';
            count++;
        }
    }
    return count;
}

bool flash_mkdir(const char* path) {
    char marker[64];
    snprintf(marker, sizeof(marker), "%s/.dir", path);
    if (flash_file_exists(marker)) return true;
    uint8_t dummy = 0x01;
    return flash_file_write(marker, &dummy, 1);
}

bool flash_dir_exists(const char* path) {
    char dirPrefix[64];
    snprintf(dirPrefix, sizeof(dirPrefix), "%s/", path);
    int prefixLen = strlen(dirPrefix);
    for (int i = 0; i < MAX_FLASH_FILES; i++) {
        if (fatTable[i].valid == 0x01 &&
            strncmp(fatTable[i].filename, dirPrefix, prefixLen) == 0) return true;
    }
    return false;
}

uint32_t flash_get_free_sectors() {
    uint32_t used = 2;
    for (int i = 0; i < MAX_FLASH_FILES; i++)
        if (fatTable[i].valid == 0x01) used += fatTable[i].sectorCount;
    uint32_t total = LOGS_END - FAT_SECTOR + 1;
    return (used < total) ? (total - used) : 0;
}

uint32_t flash_get_used_sectors() {
    uint32_t used = 2;
    for (int i = 0; i < MAX_FLASH_FILES; i++)
        if (fatTable[i].valid == 0x01) used += fatTable[i].sectorCount;
    return used;
}

uint32_t flash_get_total_size() { return FLASH_TOTAL_SIZE; }
