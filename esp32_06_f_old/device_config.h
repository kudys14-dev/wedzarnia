// device_config.h - ZMIEŃ TYLKO TEN PLIK przed wgraniem
// Odkomentuj właściwe urządzenie:

#define DEVICE_WEDZARNIA_2
// #define DEVICE_WEDZARNIA_2

#ifdef DEVICE_WEDZARNIA_1
  constexpr const char* CFG_CLOUD_DEVICE_ID  = "wedzarnia_1";
  constexpr const char* CFG_CLOUD_DEVICE_KEY = "Lapukabra123!";
  constexpr const char* CFG_WIFI_HOSTNAME    = "wedzarnia1";
  constexpr const char* CFG_AP_SSID          = "Wedzarnia1";
  constexpr const char* CFG_AP_PASS          = "12345678";
  constexpr double      NTC_TEMP_OFFSET      = 0.0;
#elif defined(DEVICE_WEDZARNIA_2)
  constexpr const char* CFG_CLOUD_DEVICE_ID  = "wedzarnia_2";
  constexpr const char* CFG_CLOUD_DEVICE_KEY = "Lapukabra123!";
  constexpr const char* CFG_WIFI_HOSTNAME    = "wedzarnia2";
  constexpr const char* CFG_AP_SSID          = "Wedzarnia2";
  constexpr const char* CFG_AP_PASS          = "12345678";
  constexpr double      NTC_TEMP_OFFSET      = -1.0;
#elif defined(DEVICE_WEDZARNIA_3)
  constexpr const char* CFG_CLOUD_DEVICE_ID  = "wedzarnia_3";
  constexpr const char* CFG_CLOUD_DEVICE_KEY = "Lapukabra123!";
  constexpr const char* CFG_WIFI_HOSTNAME    = "wedzarnia3";
  constexpr const char* CFG_AP_SSID          = "Wedzarnia3";
  constexpr const char* CFG_AP_PASS          = "12345678";
  constexpr double      NTC_TEMP_OFFSET      = -1.0;
#else
  #error "Nie wybrano urządzenia! Odkomentuj DEVICE_WEDZARNIA_x w device_config.h"
#endif