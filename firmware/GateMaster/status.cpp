#include "status.h"
#include "gate.h"
#include "params.h"
#include "wifimgr.h"
#include "ota.h"
#include "hal.h"
#include <esp_system.h>

namespace status {
static bool mqttUp = false;
void setMqttConnected(bool c) { mqttUp = c; }

const char* resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "restart";
    case ESP_RST_PANIC: return "crash";
    case ESP_RST_INT_WDT: case ESP_RST_TASK_WDT: case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout (power dip)";
    default: return "other";
  }
}

static const char* windName(Winding w) { return w == WIND_M1 ? "MOTOR-1" : w == WIND_M2 ? "MOTOR 2" : "off"; }

void collect(std::vector<Reading>& o) {
  GateStatus s = gate::snapshot();
  auto S = [&](const String& k, const String& v) { o.push_back({k, v, 's'}); };
  auto N = [&](const String& k, const String& v) { o.push_back({k, v, 'n'}); };
  auto B = [&](const String& k, bool v) { o.push_back({k, v ? "true" : "false", 'b'}); };
  o.reserve(80);
  // gate
  S("state", gate::stateName(s.state));
  N("position", s.posPct < 0 ? "-1" : String(s.posPct, 0));
  S("direction", s.winding == WIND_OFF ? "none" : s.movingOpen ? "opening" : "closing");
  N("autoclose_left", String(s.autoCloseLeftS));
  B("hold_open", s.holdOpen);
  B("auto_close", s.autoClose);
  B("lock_out", s.lockOutMode);
  // motor
  S("motor", windName(s.winding));
  N("current", String(s.currentA, 2));
  N("peak", String(s.peakA, 2));
  N("baseline", String(s.baselineA, 2));
  N("threshold", String(s.thresholdA, 2));
  // limits
  B("open_limit", s.openLimit);
  B("closed_limit", s.closeLimit);
  S("limit_type", s.limitType == 1 ? "NO" : s.limitType == 2 ? "NC" : "unknown");
  B("dip1", s.dip1);
  // inputs
  for (int i = 0; i < IN_COUNT; i++) {
    B(String("input/") + INPUT_NAMES[i], s.inActive[i]);
    S(String("input/") + INPUT_NAMES[i] + "/contact", s.inClosed[i] ? "closed" : "open");
  }
  N("edge_mv", String(s.edgeMv));
  S("button", s.button == BTN_OPEN ? "open" : s.button == BTN_STOP ? "stop" : s.button == BTN_CLOSE ? "close" : "none");
  // outputs
  B("lock", s.lockOut);
  B("relay", s.relayOut);
  B("alarm", s.alarmOut);
  // faults
  B("fault/lockout", s.state == ST_LOCKOUT);
  N("fault/obstructions_run", String(s.obstructionsThisRun));
  B("fault/no_current", s.fInhibit || s.fInhibitLatched);
  B("fault/motor_latched", s.fInhibitLatched);
  B("fault/max_run", s.fTimeout);
  B("fault/both_limits", s.fLimits);
  B("fault/forced_open", s.forcedOpen);
  B("fault/edge_wiring", s.edgeFault);
  // counters / learned
  N("cycles", String(s.cycles));
  N("obstructions", String(s.obstructions));
  N("last_obstruction", String(s.lastObstruction));
  N("travel_open", String(s.learnOpenMs / 1000.0f, 1));
  N("travel_close", String(s.learnCloseMs / 1000.0f, 1));
  N("mid_position", s.midPct < 0 ? "-1" : String(s.midPct, 0));
  // device
  S("firmware", FW_VERSION);
  S("hardware", hal::sim() ? "simulator" : "board");
  N("uptime", String(s.uptimeS));
  S("reset_reason", resetReason());
  N("free_heap", String(ESP.getFreeHeap()));
  S("name", dev::cfg().name);
  S("ssid", wifimgr::ssid());
  N("rssi", String(wifimgr::rssi()));
  S("ip", wifimgr::ip());
  S("mac", wifimgr::mac());
  S("host", dev::hostName() + ".local");
  B("mqtt", mqttUp);
  B("setup_network", wifimgr::apActive());
  B("update_available", ota::info().available);
  S("latest_version", ota::info().latest);
}

static void esc(String& o, const String& s) {
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
}

String json() {
  std::vector<Reading> r;
  collect(r);
  String j;
  j.reserve(2600);
  j = "{";
  for (size_t i = 0; i < r.size(); i++) {
    if (i) j += ",";
    j += "\""; j += r[i].key; j += "\":";
    if (r[i].kind == 's') { j += "\""; esc(j, r[i].val); j += "\""; }
    else j += r[i].val;
  }
  return j + "}";
}

String mqttValue(const Reading& r) {
  if (r.kind == 'b') return r.val == "true" ? "ON" : "OFF";
  return r.val;
}
}  // namespace status
