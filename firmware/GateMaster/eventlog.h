// Event log: the last 200 events, time-stamped, kept through reboots (LittleFS).
// add() works from any task and only touches RAM; the file is written by the network task while the
// motor is off (flash writes stall both cores briefly, so never while the gate moves).
#pragma once
#include <Arduino.h>

namespace eventlog {
constexpr int CAP = 200;
struct Entry {
  uint32_t seq;
  uint32_t epoch;     // seconds since 1970, 0 when the time is not known yet
  uint32_t upS;       // seconds since boot
  char text[68];
};
void begin();                                  // mount LittleFS, load the saved log
void add(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
uint32_t latest();                             // newest seq (0 = empty)
int since(uint32_t seq, Entry* out, int max);  // entries newer than seq, oldest first
String json(uint32_t seq, int max);            // {"latest":n,"events":[...]} newest last
String text();                                 // whole log as text, for download
void flushIfDirty(bool motorIdle);             // network task
void flushNow();                               // before a reboot (motor off)
}
