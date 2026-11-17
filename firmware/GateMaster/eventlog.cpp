#include "eventlog.h"
#include <time.h>
#include <stdarg.h>

#ifndef HOST_TEST
#include <LittleFS.h>
#endif

namespace eventlog {
static Entry ring[CAP];
static uint32_t newest = 0;          // seq of the newest entry
static bool dirty = false;
static uint32_t lastFlush = 0;
static SemaphoreHandle_t mx = nullptr;
static bool fsOk = false;
static const char* PATH = "/events.bin";
static constexpr uint32_t MAGIC = 0x474d4c31;   // "GML1"

struct Lock {
  Lock() { if (!mx) mx = xSemaphoreCreateMutex(); xSemaphoreTake(mx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(mx); }
};

static uint32_t nowEpoch() {
  time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;
}

void begin() {
  Lock l;
#ifndef HOST_TEST
  fsOk = LittleFS.begin(true, "/littlefs", 4, "spiffs");
  if (!fsOk) return;
  File f = LittleFS.open(PATH, "r");
  if (!f) return;
  uint32_t magic = 0, n = 0;
  if (f.read((uint8_t*)&magic, 4) == 4 && magic == MAGIC && f.read((uint8_t*)&n, 4) == 4 && n <= CAP) {
    for (uint32_t i = 0; i < n; i++) {
      Entry e;
      if (f.read((uint8_t*)&e, sizeof e) != sizeof e) break;
      e.text[sizeof e.text - 1] = 0;
      ring[e.seq % CAP] = e;
      if (e.seq > newest) newest = e.seq;
    }
  }
  f.close();
#endif
}

void add(const char* fmt, ...) {
  char buf[sizeof(Entry::text)];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  {
    Lock l;
    Entry& e = ring[(newest + 1) % CAP];
    e.seq = ++newest;
    e.epoch = nowEpoch();
    e.upS = millis() / 1000;
    strncpy(e.text, buf, sizeof e.text);
    e.text[sizeof e.text - 1] = 0;
    dirty = true;
  }
  Serial.printf("[%lu] %s\n", (unsigned long)millis(), buf);
}

uint32_t latest() { Lock l; return newest; }

int since(uint32_t seq, Entry* out, int max) {
  Lock l;
  uint32_t first = newest >= CAP ? newest - CAP + 1 : 1;
  if (seq + 1 > first) first = seq + 1;
  int n = 0;
  for (uint32_t s = first; s <= newest && n < max; s++) {
    const Entry& e = ring[s % CAP];
    if (e.seq == s) out[n++] = e;
  }
  return n;
}

static void jsonEsc(String& o, const char* s) {
  for (const char* p = s; *p; p++) {
    if (*p == '"' || *p == '\\') { o += '\\'; o += *p; }
    else if ((uint8_t)*p < 0x20) o += ' ';
    else o += *p;
  }
}

static String timeText(const Entry& e) {
  if (!e.epoch) return "+" + String(e.upS) + " s";
  time_t t = e.epoch;
  struct tm tmv;
  localtime_r(&t, &tmv);
  char b[24];
  strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tmv);
  return b;
}

// all readers copy the ring in small chunks, so the lock is only ever held for a moment
static constexpr int CHUNK = 16;

String json(uint32_t seq, int max) {
  if (max > CAP) max = CAP;
  uint32_t n0 = latest();
  if (n0 > seq + (uint32_t)max) seq = n0 - max;   // asked for the newest N: skip older ones
  String j;
  j.reserve(60 + (n0 - seq) * 110);
  j = "{\"latest\":" + String(n0) + ",\"events\":[";
  Entry tmp[CHUNK];
  bool first = true;
  for (;;) {
    int n = since(seq, tmp, CHUNK);
    if (!n) break;
    for (int i = 0; i < n; i++) {
      if (tmp[i].seq > n0) break;
      if (!first) j += ",";
      first = false;
      j += "{\"seq\":" + String(tmp[i].seq) + ",\"t\":" + String(tmp[i].epoch) + ",\"up\":" + String(tmp[i].upS) + ",\"time\":\"" + timeText(tmp[i]) + "\",\"text\":\"";
      jsonEsc(j, tmp[i].text);
      j += "\"}";
    }
    seq = tmp[n - 1].seq;
    if (seq >= n0) break;
  }
  return j + "]}";
}

String text() {
  String s;
  s.reserve(CAP * 90);
  Entry tmp[CHUNK];
  uint32_t seq = 0;
  for (;;) {
    int n = since(seq, tmp, CHUNK);
    if (!n) break;
    for (int i = 0; i < n; i++) s += String(tmp[i].seq) + "\t" + timeText(tmp[i]) + "\t" + tmp[i].text + "\n";
    seq = tmp[n - 1].seq;
  }
  return s;
}

void flushNow() {
#ifndef HOST_TEST
  if (!fsOk) return;
  uint32_t n0 = latest();
  uint32_t first = n0 >= CAP ? n0 - CAP : 0;
  File f = LittleFS.open("/events.tmp", "w");
  if (!f) return;
  uint32_t magic = MAGIC, cnt = 0;
  f.write((uint8_t*)&magic, 4);
  f.write((uint8_t*)&cnt, 4);
  Entry tmp[CHUNK];
  uint32_t seq = first;
  for (;;) {
    int n = since(seq, tmp, CHUNK);
    if (!n) break;
    f.write((uint8_t*)tmp, sizeof(Entry) * n);
    cnt += n;
    seq = tmp[n - 1].seq;
  }
  f.seek(4);
  f.write((uint8_t*)&cnt, 4);
  f.close();
  LittleFS.remove(PATH);
  LittleFS.rename("/events.tmp", PATH);
#endif
  { Lock l; dirty = false; }
  lastFlush = millis();
}

void flushIfDirty(bool motorIdle) {
  if (!dirty || !motorIdle) return;
  if (millis() - lastFlush < 15000) return;      // at most every 15 s: spares the flash
  flushNow();
}
}  // namespace eventlog
