#include "params.h"
#include <Preferences.h>

// key, label, group, type, min, max, default, step, unit, options, help
const ParamDef PARAMS[P_COUNT] = {
  {"autoClose",   "Auto-close",                 "Auto-close", PT_BOOL,  0, 1,   1,   1,    "",  nullptr, "Close automatically after the gate reaches open"},
  {"closeDelay",  "Close delay",                "Auto-close", PT_INT,   1, 60,  15,  1,    "s", nullptr, "Time at open before auto-close"},
  {"partialPct",  "Partial open",               "Auto-close", PT_INT,   10, 90, 40,  1,    "%", nullptr, "How far the Partial open control opens (by learned travel time)"},
  {"holdMin",     "Open for (default)",         "Auto-close", PT_INT,   1, 1440, 60, 1,    "min", nullptr, "Minutes the Open for control keeps the gate open when no time is given"},
  {"swapOpen",    "Swap open direction",        "Travel",     PT_BOOL,  0, 1,   0,   1,    "",  nullptr, "Reverses which winding opens, on top of DIP1"},
  {"limitPair",   "Limit pairing",              "Travel",     PT_ENUM,  0, 1,   0,   1,    "",  "normal|crossed", "Must match SW6"},
  {"limitType",   "Limit switch type",          "Travel",     PT_ENUM,  0, 2,   0,   1,    "",  "auto|NO|NC", "Must match DIP2; auto learns it on the first run"},
  {"maxRun",      "Max run time",               "Travel",     PT_INT,   10, 180, 60, 1,    "s", nullptr, "Hard stop for one run; 1.5 x learned travel once learned"},
  {"sensitivity", "Obstruction sensitivity",    "Safety",     PT_INT,   1, 10,  5,   1,    "",  nullptr, "How small a current rise counts as an obstruction (10 = most sensitive)"},
  {"absMaxA",     "Absolute current limit",     "Safety",     PT_FLOAT, 3, 12,  8,   0.5f, "A", nullptr, "Any run current above this is an obstruction"},
  {"openRevS",    "Reverse after opening obstruction", "Safety", PT_FLOAT, 0.5f, 3, 1, 0.1f, "s", nullptr, "Brief reverse when blocked while opening"},
  {"safetyType",  "Photo-eye (SAFETY) contact", "Inputs",     PT_ENUM,  0, 1,   1,   1,    "",  "NO|NC", "Contact type of the photo-eye output"},
  {"edgeType",    "Safety edge (J3) contact",   "Inputs",     PT_ENUM,  0, 2,   1,   1,    "",  "NO|NC|8.2 kOhm monitored", "Monitored edges report an open or shorted wire as a fault"},
  {"exitType",    "EXIT / OPEN contact",        "Inputs",     PT_ENUM,  0, 1,   0,   1,    "",  "NO|NC", ""},
  {"radioType",   "Radio (RELAY) contact",      "Inputs",     PT_ENUM,  0, 1,   0,   1,    "",  "NO|NC", ""},
  {"radioAction", "Radio action",               "Inputs",     PT_ENUM,  0, 1,   0,   1,    "",  "open|open-stop-close", "What a radio press does"},
  {"lockType",    "Lock type",                  "Lock",       PT_ENUM,  0, 2,   0,   1,    "",  "none|magnetic|solenoid", ""},
  {"magDelay",    "Mag-lock release delay",     "Lock",       PT_FLOAT, 0.2f, 3, 0.7f, 0.1f, "s", nullptr, "Lock released this long before the motor starts"},
  {"solPulse",    "Solenoid pulse",             "Lock",       PT_FLOAT, 0.5f, 5, 2.5f, 0.1f, "s", nullptr, "Lock energised at the start of each opening"},
  {"relayMode",   "RELAY-1 mode",               "RELAY-1",    PT_ENUM,  0, 4,   0,   1,    "",  "off|pulse on open|on while moving|forced-open alarm|on while not closed", "The original board's four modes, plus off"},
  {"relayPulse",  "RELAY-1 pulse length",       "RELAY-1",    PT_FLOAT, 0.5f, 5, 1,  0.1f, "s", nullptr, "For pulse on open"},
  {"forcedBuzzer","Forced-open alarm buzzer",   "RELAY-1",    PT_BOOL,  0, 1,   0,   1,    "",  nullptr, "Sound the buzzer when the gate is pushed off the closed limit"},
  {"statusInt",   "Status publish interval",    "Reporting",  PT_INT,   5, 300, 30,  1,    "s", nullptr, "MQTT status while idle; 1 s while moving"},
};

namespace params {
static float vals[P_COUNT];
static volatile bool dirtyFlag = false;

void load() {
  Preferences p;
  p.begin("params", true);
  for (int i = 0; i < P_COUNT; i++) vals[i] = p.getFloat(PARAMS[i].key, PARAMS[i].def);
  p.end();
  // values out of range (e.g. after a firmware change of the table) fall back to the default
  for (int i = 0; i < P_COUNT; i++) if (vals[i] < PARAMS[i].min || vals[i] > PARAMS[i].max) vals[i] = PARAMS[i].def;
}

float get(ParamId id) { return vals[id]; }
bool dirty() { return dirtyFlag; }

int find(const String& key) {
  for (int i = 0; i < P_COUNT; i++) if (key == PARAMS[i].key) return i;
  return -1;
}

static int optionIndex(const ParamDef& d, const String& v) {
  if (!d.options) return -1;
  String opts = d.options; int idx = 0, from = 0;
  for (;;) {
    int bar = opts.indexOf('|', from);
    String o = bar < 0 ? opts.substring(from) : opts.substring(from, bar);
    if (o.equalsIgnoreCase(v)) return idx;
    if (bar < 0) return -1;
    from = bar + 1; idx++;
  }
}

bool setValue(ParamId id, float v) {
  const ParamDef& d = PARAMS[id];
  if (v < d.min || v > d.max) return false;
  if (d.type != PT_FLOAT) v = roundf(v);
  if (vals[id] != v) { vals[id] = v; dirtyFlag = true; }
  return true;
}

bool set(const String& key, const String& value, String& err) {
  int i = find(key);
  if (i < 0) { err = "unknown parameter " + key; return false; }
  const ParamDef& d = PARAMS[i];
  String v = value; v.trim();
  float f;
  if (d.type == PT_BOOL) {
    if (v == "1" || v.equalsIgnoreCase("true") || v.equalsIgnoreCase("on") || v.equalsIgnoreCase("yes")) f = 1;
    else if (v == "0" || v.equalsIgnoreCase("false") || v.equalsIgnoreCase("off") || v.equalsIgnoreCase("no")) f = 0;
    else { err = String(d.label) + ": use on or off"; return false; }
  } else if (d.type == PT_ENUM) {
    int oi = optionIndex(d, v);
    if (oi >= 0) f = oi;
    else {
      char* end; f = strtof(v.c_str(), &end);
      if (!v.length() || *end) { err = String(d.label) + ": use one of " + d.options; return false; }
    }
  } else {
    char* end; f = strtof(v.c_str(), &end);
    if (!v.length() || *end) { err = String(d.label) + ": not a number"; return false; }
  }
  if (f < d.min || f > d.max) {
    err = String(d.label) + ": allowed " + String(d.min, d.type == PT_FLOAT ? 1 : 0) + " to " + String(d.max, d.type == PT_FLOAT ? 1 : 0) + (d.unit[0] ? String(" ") + d.unit : "");
    return false;
  }
  return setValue((ParamId)i, f);
}

void resetDefault(ParamId id) { setValue(id, PARAMS[id].def); }

void saveIfDirty() {
  if (!dirtyFlag) return;
  dirtyFlag = false;
  Preferences p;
  p.begin("params", false);
  for (int i = 0; i < P_COUNT; i++) {
    if (vals[i] == PARAMS[i].def) p.remove(PARAMS[i].key);   // only non-defaults are stored
    else p.putFloat(PARAMS[i].key, vals[i]);
  }
  p.end();
}

static String num(const ParamDef& d, float v) {
  if (d.type == PT_FLOAT) { String s(v, 2); while (s.endsWith("0")) s.remove(s.length() - 1); if (s.endsWith(".")) s.remove(s.length() - 1); return s; }
  if (d.type == PT_BOOL) return v != 0 ? "true" : "false";
  return String((long)lroundf(v));
}

String json() {
  String j = "{";
  for (int i = 0; i < P_COUNT; i++) {
    if (i) j += ",";
    j += "\""; j += PARAMS[i].key; j += "\":"; j += num(PARAMS[i], vals[i]);
  }
  return j + "}";
}

static String esc(const char* s) {
  String o; if (!s) return o;
  for (const char* p = s; *p; p++) { if (*p == '"' || *p == '\\') o += '\\'; o += *p; }
  return o;
}

String schemaJson() {
  static const char* T[] = {"bool", "int", "float", "enum"};
  String j = "[";
  for (int i = 0; i < P_COUNT; i++) {
    const ParamDef& d = PARAMS[i];
    if (i) j += ",";
    j += "{\"key\":\""; j += d.key; j += "\",\"label\":\""; j += esc(d.label);
    j += "\",\"group\":\""; j += d.group; j += "\",\"type\":\""; j += T[d.type];
    j += "\",\"min\":" + num(d, d.min) + ",\"max\":" + num(d, d.max) + ",\"def\":" + num(d, d.def);
    j += ",\"step\":" + String(d.step, d.type == PT_FLOAT ? 1 : 0);
    j += ",\"unit\":\""; j += d.unit; j += "\"";
    if (d.options) { j += ",\"options\":\""; j += esc(d.options); j += "\""; }
    j += ",\"help\":\""; j += esc(d.help); j += "\"}";
  }
  return j + "]";
}
}  // namespace params

// ------------------------------------------------------------------ learned
namespace learned {
static Learned L;
static bool loaded = false, dirtyFlag = false;

Learned& get() {
  if (!loaded) {
    loaded = true;
    Preferences p;
    p.begin("learned", true);
    L.openMs = p.getUInt("openMs", 0);
    L.closeMs = p.getUInt("closeMs", 0);
    L.midMs = p.getUInt("midMs", 0);
    L.limitDetected = p.getUChar("limDet", 0);
    L.holdMin = p.getUShort("holdMin", 0);
    if (!L.holdMin && p.getBool("hold", false)) L.holdMin = 60;   // v0.2.1 "Hold open" (no time limit) -> 60 min
    L.lockOutMode = p.getBool("lockOut", false);
    L.cycles = p.getUInt("cycles", 0);
    L.obstructions = p.getUInt("obst", 0);
    L.lastObstruction = p.getUInt("lastObst", 0);
    p.end();
  }
  return L;
}
void markDirty() { dirtyFlag = true; }
#ifdef HOST_TEST
void reloadForTest() { loaded = false; dirtyFlag = false; L = Learned(); }
#endif
void saveIfDirty() {
  if (!dirtyFlag) return;
  dirtyFlag = false;
  Preferences p;
  p.begin("learned", false);
  p.putUInt("openMs", L.openMs);
  p.putUInt("closeMs", L.closeMs);
  p.putUInt("midMs", L.midMs);
  p.putUChar("limDet", L.limitDetected);
  p.putUShort("holdMin", L.holdMin);
  p.remove("hold");
  p.putBool("lockOut", L.lockOutMode);
  p.putUInt("cycles", L.cycles);
  p.putUInt("obst", L.obstructions);
  p.putUInt("lastObst", L.lastObstruction);
  p.end();
}
}  // namespace learned
