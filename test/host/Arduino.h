// Minimal Arduino / FreeRTOS stand-ins so the gate logic, inputs, parameters, event log and the
// gate simulator compile and run on a PC (HOST_TEST). Only what that code uses.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cmath>
#include <cctype>
#include <string>
#include <deque>
#include <vector>
#include <algorithm>
using std::min; using std::max;

extern uint32_t g_now;
inline uint32_t millis() { return g_now; }
void delay(uint32_t ms);
template <class T, class U, class V> T constrain(T v, U lo, V hi) { return v < (T)lo ? (T)lo : v > (T)hi ? (T)hi : v; }

class String {
 public:
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(char c) : s(1, c) {}
  String(int v) : s(std::to_string(v)) {}
  String(unsigned v) : s(std::to_string(v)) {}
  String(long v) : s(std::to_string(v)) {}
  String(unsigned long v) : s(std::to_string(v)) {}
  String(float v, int d = 2) { char b[48]; snprintf(b, sizeof b, "%.*f", d, v); s = b; }
  String(double v, int d = 2) { char b[48]; snprintf(b, sizeof b, "%.*f", d, v); s = b; }
  String& operator+=(const String& o) { s += o.s; return *this; }
  String& operator+=(const char* o) { s += o; return *this; }
  String& operator+=(char c) { s += c; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a.s + b.s); }
  friend String operator+(const String& a, const char* b) { return String(a.s + b); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }
  friend String operator+(const String& a, char c) { return String(a.s + c); }
  bool operator==(const char* o) const { return s == o; }
  bool operator==(const String& o) const { return s == o.s; }
  bool operator!=(const char* o) const { return s != o; }
  bool operator!=(const String& o) const { return s != o.s; }
  char operator[](size_t i) const { return i < s.size() ? s[i] : 0; }
  size_t length() const { return s.size(); }
  const char* c_str() const { return s.c_str(); }
  void reserve(size_t n) { s.reserve(n); }
  long toInt() const { return atol(s.c_str()); }
  float toFloat() const { return (float)atof(s.c_str()); }
  int indexOf(char c, size_t from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  int indexOf(const char* c, size_t from = 0) const { auto p = s.find(c, from); return p == std::string::npos ? -1 : (int)p; }
  String substring(size_t a) const { return a >= s.size() ? String() : String(s.substr(a)); }
  String substring(size_t a, size_t b) const { if (a >= s.size() || b <= a) return String(); return String(s.substr(a, b - a)); }
  bool startsWith(const String& p) const { return s.compare(0, p.s.size(), p.s) == 0; }
  bool endsWith(const String& p) const { return s.size() >= p.s.size() && s.compare(s.size() - p.s.size(), p.s.size(), p.s) == 0; }
  bool equalsIgnoreCase(const String& o) const {
    if (s.size() != o.s.size()) return false;
    for (size_t i = 0; i < s.size(); i++) if (tolower((unsigned char)s[i]) != tolower((unsigned char)o.s[i])) return false;
    return true;
  }
  void trim() { size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n"); s = a == std::string::npos ? "" : s.substr(a, b - a + 1); }
  void remove(size_t i) { if (i < s.size()) s.erase(i); }
  void remove(size_t i, size_t n) { if (i < s.size()) s.erase(i, n); }
  std::string::iterator begin() { return s.begin(); }
  std::string::iterator end() { return s.end(); }
};

struct SerialStub {
  void printf(const char* f, ...) __attribute__((format(printf, 2, 3))) {
    if (!getenv("SIMLOG")) return;
    va_list ap; va_start(ap, f); vprintf(f, ap); va_end(ap);
  }
};
extern SerialStub Serial;

// FreeRTOS
typedef void* SemaphoreHandle_t;
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return (void*)1; }
inline int xSemaphoreTake(SemaphoreHandle_t, int) { return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t) { return 1; }
#define portMAX_DELAY 0
#define pdTRUE 1
struct HostQueue { size_t item; std::deque<std::vector<uint8_t>> q; };
typedef HostQueue* QueueHandle_t;
inline QueueHandle_t xQueueCreate(int, size_t item) { auto* h = new HostQueue; h->item = item; return h; }
inline int xQueueSend(QueueHandle_t q, const void* v, int) {
  q->q.emplace_back((const uint8_t*)v, (const uint8_t*)v + q->item); return 1;
}
inline int xQueueReceive(QueueHandle_t q, void* v, int) {
  if (q->q.empty()) return 0;
  memcpy(v, q->q.front().data(), q->item); q->q.pop_front(); return 1;
}
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x)
#define portEXIT_CRITICAL(x)
