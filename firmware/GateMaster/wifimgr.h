// Wi-Fi station, setup access point (fallback), mDNS name gatemaster-xxxx.local, SNTP time.
// (Not called wifi.h: that would clash with the core's WiFi.h on Windows and macOS.)
#pragma once
#include <Arduino.h>

namespace wifimgr {
void begin();
void loop();
bool connected();
bool apActive();
String ip();                         // station IP, or the setup AP's IP
String ssid();
int rssi();
String mac();
bool connectNow(const String& ssid, const String& pass, uint32_t timeoutMs);   // Improv: try, save on success
void reconnect();                    // after the Wi-Fi settings changed
bool timeValid();
}
