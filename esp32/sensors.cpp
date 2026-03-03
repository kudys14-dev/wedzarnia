// sensors.cpp - [MOD] Oba DS18B20 = komora (średnia), NTC 100k na GPIO34 = mięso
#include "sensors.h"
#include "config.h"
#include "state.h"
#include "outputs.h"
#include <nvs_flash.h>
#include <nvs.h>
#include <math.h>           // log()

struct CachedReading {
    double value;
    unsigned long timestamp;
    bool valid;
    int readAttempts;
};

static unsigned long lastTempRequest = 0;
static unsigned long lastTempReadPossible = 0;
static CachedReading cachedChamber1 = {25.0, 0, false, 0};
static CachedReading cachedChamber2 = {25.0, 0, false, 0};
static CachedReading cachedMeatNtc   = {25.0, 0, false, 0};
static int sensorErrorCount = 0;

// Filtr EMA dla NTC (stan między wywołaniami)
static float filteredNtcTemp   = 25.0f;
static bool  ntcFilterInitialized = false;

uint8_t sensorAddresses[2][8];
bool sensorsIdentified = false;
int chamberSensor1Index = DEFAULT_CHAMBER_SENSOR_1;
int chamberSensor2Index = DEFAULT_CHAMBER_SENSOR_2;

// ======================================================
// ODCZYT TEMPERATURY Z NTC 100k (GPIO34)
// ======================================================

double readNtcTemperature() {
    // ────────────────────────────────────────────────
    // 1. Zbieranie próbek – dużo próbek + małe opóźnienie
    // ────────────────────────────────────────────────
    long adcSum = 0;
    const int effectiveSamples = NTC_SAMPLES;           // zalecane: 64–256

    for (int i = 0; i < effectiveSamples; i++) {
        adcSum += analogRead(PIN_NTC);
        delayMicroseconds(140);                         // 120–200 µs stabilizuje odczyt
    }

    double adcAvg = (double)adcSum / effectiveSamples;

    // Ochrona przed absurdalnymi wartościami
    if (adcAvg < 20 || adcAvg > NTC_ADC_MAX - 20) {
        return -999.0;
    }

    // ────────────────────────────────────────────────
    // 2. Obliczenie rezystancji – konfiguracja: 3.3V → Rserie → ADC → NTC → GND
    // ────────────────────────────────────────────────
    double resistance = NTC_SERIES_R * adcAvg / (NTC_ADC_MAX - adcAvg + 1e-6);  // +epsilon chroni /0

// 🔽 DODAJ TO TUTAJ 🔽
//Serial.print("ADC: ");
//Serial.println(adcAvg);

//Serial.print("R: ");
//Serial.println(resistance);
// 🔼 KONIEC DEBUG 🔼


    // ────────────────────────────────────────────────
    // 3. Model beta (najczęściej 4060–4130 pasuje lepiej niż 3950)
    // ────────────────────────────────────────────────
    double steinhart = resistance / NTC_NOMINAL_R;
    steinhart = log(steinhart);
    steinhart /= NTC_BETA;                          // ← tu jest 4100 zamiast 3950
    steinhart += 1.0 / (NTC_NOMINAL_T + 273.15);
    steinhart = 1.0 / steinhart;
    double rawTemp = steinhart - 273.15;

    // ────────────────────────────────────────────────
    // 4. Filtr EMA – redukuje skoki w wodzie / przy szumie
    // ────────────────────────────────────────────────
    const float alpha = 0.91f;                      // 0.88–0.94 – im niższy → mocniejsze wygładzanie

    if (!ntcFilterInitialized) {
        filteredNtcTemp = (float)rawTemp;
        ntcFilterInitialized = true;
    } else {
        filteredNtcTemp = alpha * filteredNtcTemp + (1.0f - alpha) * (float)rawTemp;
    }

    // ────────────────────────────────────────────────
    // 5. Sanity check + ewentualne logowanie
    // ────────────────────────────────────────────────
    if (rawTemp < NTC_TEMP_MIN - 20 || rawTemp > NTC_TEMP_MAX + 20) {
        LOG_FMT(LOG_LEVEL_WARN, "NTC raw temp poza zakresem: %.1f °C (adc=%.0f)", rawTemp, adcAvg);
    }

    return (double)filteredNtcTemp;
}

// ======================================================
// FUNKCJE DS18B20 – bez zmian (tylko drobne kosmetyki)
// ======================================================

void identifyAndAssignSensors() {
    if (sensorsIdentified) return;

    int deviceCount = sensors.getDeviceCount();
    LOG_FMT(LOG_LEVEL_INFO, "Identifying %d DS18B20 sensor(s) (both = chamber)...", deviceCount);

    if (deviceCount >= 2) {
        for (int i = 0; i < deviceCount && i < 2; i++) {
            if (sensors.getAddress(sensorAddresses[i], i)) {
                char addrStr[24];
                snprintf(addrStr, sizeof(addrStr), "%02X%02X%02X%02X%02X%02X%02X%02X",
                         sensorAddresses[i][0], sensorAddresses[i][1],
                         sensorAddresses[i][2], sensorAddresses[i][3],
                         sensorAddresses[i][4], sensorAddresses[i][5],
                         sensorAddresses[i][6], sensorAddresses[i][7]);
                LOG_FMT(LOG_LEVEL_INFO, "DS18B20 Sensor %d: %s (CHAMBER)", i, addrStr);
            }
        }

        chamberSensor1Index = 0;
        chamberSensor2Index = 1;
        sensorsIdentified = true;

        LOG_FMT(LOG_LEVEL_INFO, "Both DS18B20 assigned to CHAMBER (avg)");
        LOG_FMT(LOG_LEVEL_INFO, "NTC 100k on GPIO%d assigned to MEAT", PIN_NTC);

        buzzerBeep(3, 200, 100);
    } else if (deviceCount == 1) {
        log_msg(LOG_LEVEL_WARN, "Only 1 DS18B20 found - using single sensor for chamber");
        chamberSensor1Index = 0;
        chamberSensor2Index = -1;
        sensorsIdentified = true;
    } else {
        log_msg(LOG_LEVEL_WARN, "No DS18B20 sensors found!");
        sensorsIdentified = false;
    }
}

// ======================================================
// GŁÓWNE FUNKCJE CZUJNIKÓW – reszta bez dużych zmian
// ======================================================

void requestTemperature() {
    unsigned long now = millis();
    if (now - lastTempRequest >= TEMP_REQUEST_INTERVAL) {
        sensors.setWaitForConversion(false);
        if (sensors.requestTemperatures()) {
            lastTempRequest = now;
            lastTempReadPossible = now + TEMP_CONVERSION_TIME;
        } else {
            log_msg(LOG_LEVEL_WARN, "Temperature request failed");
        }
    }
}

static bool isValidTemperature(double t) {
    return (t != DEVICE_DISCONNECTED_C &&
            t != 85.0 &&
            t != 127.0 &&
            t >= -20.0 &&
            t <= 200.0);
}

static double readTempWithTimeout(uint8_t sensorIndex) {
    double temp = sensors.getTempCByIndex(sensorIndex);
    if (temp == 85.0) {
        delay(10);
        temp = sensors.getTempCByIndex(sensorIndex);
    }
    return temp;
}

void readTemperature() {
    unsigned long now = millis();
    if (lastTempReadPossible == 0 || now < lastTempReadPossible) return;
    lastTempReadPossible = 0;

    if (!sensorsIdentified) {
        identifyAndAssignSensors();
        if (!sensorsIdentified) {
            log_msg(LOG_LEVEL_WARN, "Sensors not identified, using defaults");
            chamberSensor1Index = DEFAULT_CHAMBER_SENSOR_1;
            chamberSensor2Index = DEFAULT_CHAMBER_SENSOR_2;
        }
    }

    // Komora – DS18B20
    double tChamber1 = readTempWithTimeout(chamberSensor1Index);
    bool t1Valid = isValidTemperature(tChamber1);

    double tChamber2 = -999.0;
    bool t2Valid = false;
    if (chamberSensor2Index >= 0) {
        tChamber2 = readTempWithTimeout(chamberSensor2Index);
        t2Valid = isValidTemperature(tChamber2);
    }

    double chamberAvg = 25.0;
    if (t1Valid && t2Valid) {
        chamberAvg = (tChamber1 + tChamber2) / 2.0;
    } else if (t1Valid) {
        chamberAvg = tChamber1;
    } else if (t2Valid) {
        chamberAvg = tChamber2;
    }

    // Obsługa błędów i cache komory
    if (!t1Valid && !t2Valid) {
        sensorErrorCount++;
        if (sensorErrorCount >= SENSOR_ERROR_THRESHOLD) {
            if (state_lock()) {
                g_errorSensor = true;
                if (g_currentState == ProcessState::RUNNING_AUTO ||
                    g_currentState == ProcessState::RUNNING_MANUAL) {
                    g_currentState = ProcessState::PAUSE_SENSOR;
                    log_msg(LOG_LEVEL_ERROR, "Both chamber sensors error - pausing process");
                }
                state_unlock();
            }
        }
        // fallback na cache
        if (cachedChamber1.valid || cachedChamber2.valid) {
            if (state_lock()) {
                if (cachedChamber1.valid && cachedChamber2.valid) {
                    g_tChamber = (cachedChamber1.value + cachedChamber2.value) / 2.0;
                } else if (cachedChamber1.valid) {
                    g_tChamber = cachedChamber1.value;
                } else {
                    g_tChamber = cachedChamber2.value;
                }
                state_unlock();
            }
        }
    } else {
        sensorErrorCount = 0;

        if (t1Valid) {
            cachedChamber1.value = tChamber1;
            cachedChamber1.timestamp = now;
            cachedChamber1.valid = true;
            cachedChamber1.readAttempts = 0;
        }
        if (t2Valid) {
            cachedChamber2.value = tChamber2;
            cachedChamber2.timestamp = now;
            cachedChamber2.valid = true;
            cachedChamber2.readAttempts = 0;
        }

        if (state_lock()) {
            g_tChamber = chamberAvg;
            g_tChamber1 = t1Valid ? tChamber1 : cachedChamber1.value;
            g_tChamber2 = t2Valid ? tChamber2 : cachedChamber2.value;

            if (g_errorSensor && g_currentState == ProcessState::PAUSE_SENSOR) {
                g_errorSensor = false;
                log_msg(LOG_LEVEL_INFO, "Chamber sensor recovered");
            }
            state_unlock();
        }
    }

    // Mięso – NTC
    double tMeatNtc = readNtcTemperature();
    bool meatValid = (tMeatNtc > NTC_TEMP_MIN && tMeatNtc < NTC_TEMP_MAX);

    if (meatValid) {
        cachedMeatNtc.value = tMeatNtc;
        cachedMeatNtc.timestamp = now;
        cachedMeatNtc.valid = true;
        cachedMeatNtc.readAttempts = 0;

        if (state_lock()) {
            g_tMeat = tMeatNtc;
            state_unlock();
        }
    } else if (cachedMeatNtc.valid) {
        if (state_lock()) {
            g_tMeat = cachedMeatNtc.value;
            state_unlock();
        }
        LOG_FMT(LOG_LEVEL_WARN, "NTC invalid reading (%.1f), using cached: %.1f",
                tMeatNtc, cachedMeatNtc.value);
    }

    // Przegrzanie komory
    if (state_lock()) {
        if (g_tChamber > CFG_T_MAX_SOFT) {
            g_errorOverheat = true;
            g_currentState = ProcessState::PAUSE_OVERHEAT;
            LOG_FMT(LOG_LEVEL_ERROR, "OVERHEAT detected: %.1f C (avg chamber)", g_tChamber);
        }
        state_unlock();
    }
}

// ────────────────────────────────────────────────
// Pozostałe funkcje bez zmian
// ────────────────────────────────────────────────

void checkDoor() {
    bool nowOpen = (digitalRead(PIN_DOOR) == HIGH);
    bool shouldTurnOff = false;
    bool shouldBeep = false;
    bool shouldResume = false;

    if (state_lock()) {
        bool wasOpen = g_doorOpen;
        if (nowOpen && !wasOpen) {
            g_doorOpen = true;
            if (g_currentState == ProcessState::RUNNING_AUTO ||
                g_currentState == ProcessState::RUNNING_MANUAL) {
                g_currentState = ProcessState::PAUSE_DOOR;
                g_processStats.pauseCount++;
                shouldTurnOff = true;
                shouldBeep = true;
                log_msg(LOG_LEVEL_INFO, "Door opened - pausing");
            }
        } else if (!nowOpen && wasOpen) {
            g_doorOpen = false;
            if (g_currentState == ProcessState::PAUSE_DOOR) {
                g_currentState = ProcessState::SOFT_RESUME;
                shouldResume = true;
                log_msg(LOG_LEVEL_INFO, "Door closed - resuming");
            }
        }
        state_unlock();
    }

    if (shouldTurnOff) { allOutputsOff(); }
    if (shouldBeep)    { buzzerBeep(2, 100, 100); }
    if (shouldResume)  { initHeaterEnable(); }
}

unsigned long getSensorCacheAge() {
    unsigned long now = millis();
    return cachedChamber1.valid ? (now - cachedChamber1.timestamp) : 0xFFFFFFFF;
}

void forceSensorRead() {
    lastTempRequest = 0;
    lastTempReadPossible = 0;
}

String getSensorDiagnostics() {
    char buffer[384];
    snprintf(buffer, sizeof(buffer),
        "Chamber1: %.1f C (sensor: %d, valid: %d)\n"
        "Chamber2: %.1f C (sensor: %d, valid: %d)\n"
        "Chamber Avg: %.1f C\n"
        "Meat (NTC): %.1f C (GPIO%d, valid: %d)\n"
        "Error count: %d, Identified: %s",
        cachedChamber1.value, chamberSensor1Index, cachedChamber1.valid,
        cachedChamber2.value, chamberSensor2Index, cachedChamber2.valid,
        (cachedChamber1.value + cachedChamber2.value) / 2.0,
        cachedMeatNtc.value, PIN_NTC, cachedMeatNtc.valid,
        sensorErrorCount,
        sensorsIdentified ? "YES" : "NO");
    return String(buffer);
}

String getSensorAssignmentInfo() {
    char buffer[192];
    snprintf(buffer, sizeof(buffer),
        "Sensor Assignments:\n"
        "  Chamber1: DS18B20 idx %d\n"
        "  Chamber2: DS18B20 idx %d\n"
        "  Meat: NTC 100k on GPIO%d\n"
        "  Total DS18B20: %d\n"
        "  Identified: %s",
        chamberSensor1Index, chamberSensor2Index,
        PIN_NTC, sensors.getDeviceCount(),
        sensorsIdentified ? "YES" : "NO");
    return String(buffer);
}

bool autoDetectAndAssignSensors() {
    int deviceCount = sensors.getDeviceCount();
    if (deviceCount < 1) {
        log_msg(LOG_LEVEL_ERROR, "Need at least 1 DS18B20 sensor");
        return false;
    }

    sensorsIdentified = false;
    identifyAndAssignSensors();
    return sensorsIdentified;
}

int getChamberSensor1Index() { return chamberSensor1Index; }
int getChamberSensor2Index() { return chamberSensor2Index; }
int getTotalSensorCount()    { return sensors.getDeviceCount(); }
bool areSensorsIdentified()  { return sensorsIdentified; }