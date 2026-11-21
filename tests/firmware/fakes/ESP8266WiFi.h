#pragma once
#include "Arduino.h"

#define WIFI_STA 1
#define WL_CONNECTED 3
#define WL_DISCONNECTED 6

namespace fake {
inline bool wifi_connected = true;
inline int rssi = -58;
}  // namespace fake

class FakeWiFi {
 public:
  void mode(int) {}
  void begin(const char*, const char*) {}
  int status() { return fake::wifi_connected ? WL_CONNECTED : WL_DISCONNECTED; }
  IPAddress localIP() { return IPAddress(10, 0, 0, 118); }
  int32_t RSSI() { return fake::rssi; }
};
inline FakeWiFi WiFi;

class WiFiClient {
 public:
  void setTimeout(unsigned long ms) { timeout_ms = ms; }
  unsigned long timeout_ms = 5000;  // ESP8266 WiFiClient default
};
