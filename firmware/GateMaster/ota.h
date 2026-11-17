// Firmware updates: online check against the release site (never installs on its own), install with
// SHA-256 check, manual upload from the web page, and the rollback guard for a new image.
// Every path is refused while the gate moves, and the gate is held still while installing.
#pragma once
#include <Arduino.h>

namespace ota {
constexpr const char* RELEASE_BASE = "https://ivanvachovsky.github.io/GateMaster/";

struct Info {
  bool checked = false, available = false;
  String latest, notes, url, sha256, date;
  uint32_t size = 0;
  uint32_t checkedAtS = 0;          // uptime seconds of the last check
  String lastError;
  bool installing = false;
  uint8_t progress = 0;             // %
  bool pendingVerify = false;       // this image is new and not yet confirmed
};

void begin();
void loop();                        // network task
const Info& info();
void requestCheck();                // done in loop()
bool requestInstall(String& err);   // done in loop(); refused while moving
String json();

// manual upload (web page)
bool uploadBegin(size_t size, String& err);
bool uploadWrite(const uint8_t* data, size_t len);
bool uploadEnd(String& err);
void uploadAbort();

void rebootWhenIdle(uint32_t delayMs = 800);
bool rebootPending();
}
