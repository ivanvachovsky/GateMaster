// In-memory stand-in for the ESP32 Preferences (NVS) library. A static map survives "reboots"
// inside one test run, like flash does.
#pragma once
#include "Arduino.h"
#include <map>

class Preferences {
  static std::map<std::string, std::string>& store() { static std::map<std::string, std::string> m; return m; }
  std::string ns;
  std::string k(const char* key) const { return ns + "/" + key; }
  template <class T> T getT(const char* key, T def) const {
    auto it = store().find(k(key));
    if (it == store().end() || it->second.size() != sizeof(T)) return def;
    T v; memcpy(&v, it->second.data(), sizeof(T)); return v;
  }
  template <class T> void putT(const char* key, T v) { store()[k(key)] = std::string((const char*)&v, sizeof(T)); }

 public:
  bool begin(const char* name, bool = false) { ns = name; return true; }
  void end() {}
  bool remove(const char* key) { return store().erase(k(key)) > 0; }
  static void wipe() { store().clear(); }
  float getFloat(const char* key, float d) const { return getT(key, d); }
  void putFloat(const char* key, float v) { putT(key, v); }
  uint32_t getUInt(const char* key, uint32_t d) const { return getT(key, d); }
  void putUInt(const char* key, uint32_t v) { putT(key, v); }
  uint8_t getUChar(const char* key, uint8_t d) const { return getT(key, d); }
  void putUChar(const char* key, uint8_t v) { putT(key, v); }
  bool getBool(const char* key, bool d) const { return getT(key, d); }
  uint16_t getUShort(const char* key, uint16_t d) const { return getT(key, d); }
  void putUShort(const char* key, uint16_t v) { putT(key, v); }
  void putBool(const char* key, bool v) { putT(key, v); }
};
