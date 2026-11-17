// Built-in gate simulator: a modelled SL-100 sliding gate behind the same hardware interface as the board.
// Used on a plain ESP32 DevKit (Simulator mode) and by the PC tests. Drives no GPIO except the GPIO 2 LED.
#include "hal.h"
#include "params.h"
#include <math.h>

#ifdef HOST_TEST
#define SIM_RTC
#else
#include <esp_attr.h>
#define SIM_RTC RTC_NOINIT_ATTR
#endif

namespace {
struct Phys {
  float pos = 0;               // 0 = closed, 1 = open
  float travelS = 20;          // full travel time
  bool limitsNC = true;        // real switch type
  bool tripLatched = false;    // hardware overcurrent latch
  float overS = 0;             // time above the latch current
  float obstacleAt = -1;       // obstacle position (-1 none)
  bool obstacleBlocksOpening = false;
  bool active[IN_COUNT] = {};  // input "active" as the user set it (beam broken, edge pressed, contact made...)
  uint32_t pulseUntil[IN_COUNT] = {};
  Button btn = BTN_NONE;
  uint32_t btnUntil = 0;
  bool dip1 = false;
  bool edgeFault = false;
  bool stalled = false;
  float amps = 0;
  float runS = 0;              // time since the motor (actually) started
  Winding lastW = WIND_OFF;
  float midAt = 0.5f;          // MID reed position
};
Phys P;
volatile Winding drv = WIND_OFF;
uint32_t simT = 0;             // simulator clock, ms

struct Kept { uint32_t magic; float pos; bool limitsNC; float travelS; };
SIM_RTC Kept kept;             // gate position survives a software reboot (not a power cut)
constexpr uint32_t KEPT_MAGIC = 0x6a7e5113;

bool atOpen() { return P.pos >= 1.0f; }
bool atClosed() { return P.pos <= 0.0f; }
bool typed(bool active, int type) { return type == CONTACT_NC ? !active : active; }   // contact closed?
bool isActive(int i) { return P.active[i] || (int32_t)(P.pulseUntil[i] - simT) > 0; }
// the edge on the gate's leading edge stays squeezed until the gate has backed off ~1 % of travel
bool edgeSqueezed() { return P.obstacleAt >= 0 && !P.obstacleBlocksOpening && P.pos >= P.obstacleAt && P.pos - P.obstacleAt < 0.012f; }
}  // namespace

namespace simhw {
void begin() {
  if (kept.magic == KEPT_MAGIC) { P.pos = kept.pos; P.limitsNC = kept.limitsNC; P.travelS = kept.travelS; }
  kept.magic = KEPT_MAGIC; kept.pos = P.pos; kept.limitsNC = P.limitsNC; kept.travelS = P.travelS;
}

bool contactClosed(InputId i) {
  switch (i) {
    case IN_LIM1: return P.limitsNC ? !atOpen() : atOpen();      // LIMIT 1 stops MOTOR-1 (opening)
    case IN_LIM2: return P.limitsNC ? !atClosed() : atClosed();  // LIMIT 2 stops MOTOR 2 (closing)
    case IN_SAFE: return typed(isActive(IN_SAFE), params::geti(P_SAFETY_TYPE));
    case IN_EDGE: return typed(isActive(IN_EDGE) || edgeSqueezed(), params::geti(P_EDGE_TYPE));
    case IN_EXIT: return typed(isActive(IN_EXIT), params::geti(P_EXIT_TYPE));
    case IN_RADIO: return typed(isActive(IN_RADIO), params::geti(P_RADIO_TYPE));
    case IN_MID: return isActive(IN_MID) || fabsf(P.pos - P.midAt) < 0.015f;
    default: return isActive(i);
  }
}

uint16_t buttonMv() {
  Button b = (int32_t)(P.btnUntil - simT) > 0 || P.btnUntil == 0xffffffff ? P.btn : BTN_NONE;
  bool d = P.dip1;
  switch (b) {
    case BTN_STOP: return 60;
    case BTN_OPEN: return d ? 990 : 1050;
    case BTN_CLOSE: return d ? 1980 : 2270;
    default: return d ? 2720 : 3300;
  }
}

int edgeMv() {
  if (P.edgeFault) return 3150;                                       // wire break
  return (isActive(IN_EDGE) || edgeSqueezed()) ? 120 : 1450;   // pressed : intact
}

void drive(Winding w) { drv = w; }

void tripReset() { P.tripLatched = false; P.overS = 0; }

float currentA() { return P.amps > 0 ? P.amps + 0.08f * sinf(simT * 0.37f) : 0; }

void tick(uint32_t dtMs) {
  simT += dtMs;
  float dt = dtMs / 1000.0f;
  // board hardware: LIMIT 1 cuts MOTOR-1, LIMIT 2 cuts MOTOR 2, the overcurrent latch cuts both
  Winding w = drv;
  bool hwEnable = !P.tripLatched && !(w == WIND_M1 && atOpen()) && !(w == WIND_M2 && atClosed());
  if (!hwEnable) w = WIND_OFF;
  if (w != P.lastW) { P.runS = 0; P.lastW = w; }
  P.stalled = false;
  if (w != WIND_OFF) {
    P.runS += dt;
    float v = (w == WIND_M1 ? 1 : -1) * dt / P.travelS;
    float np = fminf(1.0f, fmaxf(0.0f, P.pos + v));
    if (P.obstacleAt >= 0) {
      bool closingInto = w == WIND_M2 && !P.obstacleBlocksOpening && P.pos >= P.obstacleAt && np <= P.obstacleAt;
      bool openingInto = w == WIND_M1 && P.obstacleBlocksOpening && P.pos <= P.obstacleAt && np >= P.obstacleAt;
      if (closingInto || openingInto) { np = P.obstacleAt; P.stalled = true; }
    }
    P.pos = np;
    P.amps = P.stalled ? 11.0f : (P.runS < 0.5f ? 9.0f : 4.5f);
  } else P.amps = 0;
  if (P.amps > 10.0f) { P.overS += dt; if (P.overS > 1.5f) P.tripLatched = true; }
  else P.overS = 0;
  // finish pulses
  for (int i = 0; i < IN_COUNT; i++) if (P.pulseUntil[i] && (int32_t)(P.pulseUntil[i] - simT) <= 0) P.pulseUntil[i] = 0;
  if (P.btnUntil && P.btnUntil != 0xffffffff && (int32_t)(P.btnUntil - simT) <= 0) { P.btnUntil = 0; P.btn = BTN_NONE; }
  kept.pos = P.pos; kept.limitsNC = P.limitsNC; kept.travelS = P.travelS;
}
}  // namespace simhw

// ------------------------------------------------------------------ controls
namespace simhw { float currentA(); }
namespace sim {
static int inputByName(const String& n) {
  for (int i = 0; i < IN_COUNT; i++) if (n.equalsIgnoreCase(INPUT_NAMES[i])) return i;
  if (n.equalsIgnoreCase("beam") || n.equalsIgnoreCase("photoeye")) return IN_SAFE;
  return -1;
}

bool setInput(const String& name, Action a) {
  int i = inputByName(name);
  if (i < 0 || i == IN_LIM1 || i == IN_LIM2) return false;
  if (a == ACT_PULSE) { P.active[i] = false; P.pulseUntil[i] = simT + 400; if (!P.pulseUntil[i]) P.pulseUntil[i] = 1; }
  else { P.active[i] = a == ACT_ON; P.pulseUntil[i] = 0; }
  return true;
}

bool button(const String& name, Action a) {
  Button b = name.equalsIgnoreCase("open") ? BTN_OPEN : name.equalsIgnoreCase("stop") ? BTN_STOP :
             name.equalsIgnoreCase("close") ? BTN_CLOSE : BTN_NONE;
  if (b == BTN_NONE) return false;
  if (a == ACT_OFF) { P.btn = BTN_NONE; P.btnUntil = 0; }
  else { P.btn = b; P.btnUntil = a == ACT_ON ? 0xffffffff : simT + 300; }
  return true;
}

void obstacle(float pos, bool blocksOpening) {
  P.obstacleAt = pos < 0 ? -1 : fminf(1.0f, pos);
  P.obstacleBlocksOpening = blocksOpening;
}
void trip() { P.tripLatched = true; }
void push(float pos) { if (drv == WIND_OFF) P.pos = fminf(1.0f, fmaxf(0.0f, pos)); }
void setLimitsNC(bool nc) { P.limitsNC = nc; }
void setTravel(float s) { P.travelS = fminf(120, fmaxf(5, s)); }
void setDip1(bool on) { P.dip1 = on; }
void setEdgeFault(bool on) { P.edgeFault = on; }

String json() {
  String j = "{";
  j += "\"pos\":" + String(P.pos * 100, 1);
  j += ",\"amps\":" + String(simhw::currentA(), 2);
  j += ",\"travel\":" + String(P.travelS, 0);
  j += ",\"limits\":\"" + String(P.limitsNC ? "NC" : "NO") + "\"";
  j += ",\"obstacle\":" + String(P.obstacleAt < 0 ? -1.0f : P.obstacleAt * 100, 0);
  j += ",\"obstacleBlocksOpening\":" + String(P.obstacleBlocksOpening ? "true" : "false");
  j += ",\"trip\":" + String(P.tripLatched ? "true" : "false");
  j += ",\"dip1\":" + String(P.dip1 ? "true" : "false");
  j += ",\"edgeFault\":" + String(P.edgeFault ? "true" : "false");
  j += ",\"stalled\":" + String(P.stalled ? "true" : "false");
  j += ",\"held\":{";
  bool first = true;
  for (int i = 0; i < IN_COUNT; i++) {
    if (i == IN_LIM1 || i == IN_LIM2) continue;
    if (!first) j += ",";
    first = false;
    j += "\"" + String(INPUT_NAMES[i]) + "\":" + (P.active[i] ? "true" : "false");
  }
  j += "}}";
  return j;
}

static Action actionOf(const String& v) {
  if (v.equalsIgnoreCase("on") || v == "1" || v.equalsIgnoreCase("true") || v.equalsIgnoreCase("hold")) return ACT_ON;
  if (v.equalsIgnoreCase("off") || v == "0" || v.equalsIgnoreCase("false") || v.equalsIgnoreCase("release")) return ACT_OFF;
  return ACT_PULSE;
}

// name / value pairs used by POST /api/sim and MQTT <base>/sim/<name>
bool command(const String& nameIn, const String& valueIn, String& err) {
  String name = nameIn, value = valueIn;
  name.trim(); value.trim();
  if (name.startsWith("input/") || name.startsWith("input:")) name = name.substring(6);
  if (name.startsWith("button/") || name.startsWith("button:")) {
    if (!button(name.substring(7), actionOf(value))) { err = "unknown button"; return false; }
    return true;
  }
  if (name == "obstacle" || name == "obstacle_opening") {
    if (value.equalsIgnoreCase("none") || value == "-1" || !value.length()) { obstacle(-1, false); return true; }
    float pct = value.toFloat();
    if (pct < 0 || pct > 100) { err = "obstacle: 0 to 100 % or none"; return false; }
    obstacle(pct / 100.0f, name == "obstacle_opening");
    return true;
  }
  if (name == "trip") { trip(); return true; }
  if (name == "push") {
    float pct = value.toFloat();
    if (pct < 0 || pct > 100) { err = "push: position 0 to 100 %"; return false; }
    if (drv != WIND_OFF) { err = "stop the gate first"; return false; }
    push(pct / 100.0f); return true;
  }
  if (name == "limits") {
    if (value.equalsIgnoreCase("NC")) setLimitsNC(true);
    else if (value.equalsIgnoreCase("NO")) setLimitsNC(false);
    else { err = "limits: NO or NC"; return false; }
    return true;
  }
  if (name == "travel") {
    float s = value.toFloat();
    if (s < 5 || s > 120) { err = "travel: 5 to 120 s"; return false; }
    setTravel(s); return true;
  }
  if (name == "dip1") { setDip1(actionOf(value) == ACT_ON); return true; }
  if (name == "edgefault") { setEdgeFault(actionOf(value) == ACT_ON); return true; }
  if (setInput(name, actionOf(value))) return true;
  err = "unknown simulator control " + name;
  return false;
}

#ifdef HOST_TEST
float testPos() { return P.pos; }
bool testTrip() { return P.tripLatched; }
void testPowerCut() { P.tripLatched = false; P.btn = BTN_NONE; P.btnUntil = 0; for (int i = 0; i < IN_COUNT; i++) P.pulseUntil[i] = 0; }
#endif
}  // namespace sim
