// Settings. ONE table (params.cpp) defines every gate parameter: name, type, range, default, units.
// That table drives the Parameters tab, /api/config, MQTT set/<param>, backup/restore and the
// Home Assistant number entities, so they can never drift apart.
//
// Three stores, all in NVS (flash), all surviving power loss and firmware updates:
//   params  - gate parameters (the table)
//   dev     - device / network settings (name, Wi-Fi, password, MQTT, time zone, hardware mode)
//   learned - values the gate learns or keeps by itself (travel times, counters, Hold open, Lock out)
#pragma once
#include <Arduino.h>
#include "config.h"

enum ParamType : uint8_t { PT_BOOL, PT_INT, PT_FLOAT, PT_ENUM };

struct ParamDef {
  const char* key;      // API / MQTT / NVS name (<= 15 chars)
  const char* label;
  const char* group;
  ParamType type;
  float min, max, def, step;
  const char* unit;
  const char* options;  // PT_ENUM: "a|b|c" (value = index)
  const char* help;
};

enum ParamId : uint8_t {
  P_AUTO_CLOSE, P_CLOSE_DELAY, P_PARTIAL, P_HOLD_MIN,
  P_SWAP_OPEN, P_LIMIT_PAIR, P_LIMIT_TYPE, P_MAX_RUN,
  P_SENS, P_ABS_MAX, P_OPEN_REV,
  P_SAFETY_TYPE, P_EDGE_TYPE, P_EXIT_TYPE, P_RADIO_TYPE, P_RADIO_ACTION,
  P_LOCK_TYPE, P_MAG_DELAY, P_SOL_PULSE,
  P_RELAY_MODE, P_RELAY_PULSE, P_FORCED_BUZZER,
  P_STATUS_INT,
  P_COUNT
};

// enum values
enum ContactType : uint8_t { CONTACT_NO = 0, CONTACT_NC = 1, CONTACT_8K2 = 2 };
enum LimitType : uint8_t { LIMIT_AUTO = 0, LIMIT_NO = 1, LIMIT_NC = 2 };
enum LockMode : uint8_t { LOCK_NONE = 0, LOCK_MAG = 1, LOCK_SOLENOID = 2 };
enum RelayMode : uint8_t { RELAY_OFF = 0, RELAY_PULSE_ON_OPEN = 1, RELAY_WHILE_MOVING = 2,
                           RELAY_FORCED_OPEN = 3, RELAY_NOT_CLOSED = 4 };
enum HwMode : uint8_t { HW_BOARD = 0, HW_SIM = 1, HW_SIM_IO = 2 };   // HW_SIM_IO: simulated gate, real input pins

extern const ParamDef PARAMS[P_COUNT];

namespace params {
void load();                                   // all three stores, at boot
float get(ParamId id);                         // lock-free (aligned 32-bit read)
inline int geti(ParamId id) { return (int)lroundf(get(id)); }
inline bool getb(ParamId id) { return get(id) != 0; }
int find(const String& key);                   // -1 if unknown
bool set(const String& key, const String& value, String& err);   // validates, sets, marks for saving
bool setValue(ParamId id, float v);            // from code (e.g. Auto-close control)
void resetDefault(ParamId id);
void saveIfDirty();                            // call only while the motor is off
bool dirty();
String json();                                 // {"key":value,...}
String schemaJson();                           // the table, for the web page
}

// ------------------------------------------------------------------ device settings
struct DeviceCfg {
  String name = "Gate";                 // free text label
  String wifiSsid, wifiPass;
  String adminHash, adminSalt;          // empty = password not set yet (first login)
  String adminMd5;                      // MD5 of the password: only for Arduino IDE uploads (when enabled)
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser, mqttPass;
  String mqttBase;                      // empty = gatemaster/<device name>
  bool haDiscovery = true;
  String tz = "PST8PDT,M3.2.0,M11.1.0"; // POSIX TZ string
  uint8_t hwMode = DEFAULT_HW_MODE;
  bool arduinoOta = false;
};

namespace dev {
DeviceCfg& cfg();                       // network task only
void save();
void factoryReset();                    // Wi-Fi, password, MQTT, name; keeps gate parameters
String chipId();                        // last 4 hex digits of the MAC, e.g. "a1b2"
String hostName();                      // gatemaster-a1b2
String mqttBase();                      // effective base topic
bool setPassword(const String& pw, String& err);
bool checkPassword(const String& pw);
String json();                          // General tab values (no passwords)
bool setKey(const String& key, const String& value, String& err, bool& needReboot, bool& netChanged);
uint8_t bootHwMode();                   // hardware mode read at boot (gate code uses this)
bool isBlank();                         // nothing set up yet (no Wi-Fi, password or MQTT)
}

// ------------------------------------------------------------------ learned / kept by the gate
struct Learned {
  uint32_t openMs = 0, closeMs = 0;     // travel times
  uint32_t midMs = 0;                   // MID reed reached this long after leaving the closed limit
  uint8_t limitDetected = 0;            // 1 NO, 2 NC
  uint16_t holdMin = 0;                 // Open for N minutes: minutes left (0 = not held)
  bool lockOutMode = false;
  uint32_t cycles = 0, obstructions = 0;
  uint32_t lastObstruction = 0;         // epoch seconds (0 = never / time unknown)
};
namespace learned {
Learned& get();                         // gate task only
void markDirty();
void saveIfDirty();                     // gate task, motor off only
#ifdef HOST_TEST
void reloadForTest();
#endif
}
