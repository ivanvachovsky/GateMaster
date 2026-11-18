#include "mqtt.h"
#include "gate.h"
#include "params.h"
#include "status.h"
#include "eventlog.h"
#include "wifimgr.h"
#include "ota.h"
#include "hal.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include <map>

namespace mqtt {
static WiFiClient tcp;
static PubSubClient mq(tcp);
static String base;
static uint32_t nextTry = 0, backoff = 5000;
static uint32_t lastFull = 0, lastPub = 0, lastSeq = 0xffffffff, lastEvSeq = 0;
static String lastState, lastUpdateJson;
static std::map<std::string, std::string> sent;
static bool wasConnected = false, wantReconnect = false, configDirty = false;

bool connected() { return mq.connected(); }
void reconnect() { wantReconnect = true; }
void publishConfig() { configDirty = true; }

static bool pub(const String& topic, const String& payload, bool retain) {
  return mq.publish(topic.c_str(), (const uint8_t*)payload.c_str(), payload.length(), retain);
}

// readings that change all the time: only in the periodic full publish
static bool slowKey(const String& k) {
  return k == "uptime" || k == "free_heap" || k == "rssi" || k == "current" || k == "edge_mv" ||
         k == "baseline" || k == "threshold" || k == "peak";
}

static void publishStatus(bool full) {
  std::vector<status::Reading> r;
  status::collect(r);
  bool moving = gate::snapshot().winding != WIND_OFF;
  for (auto& x : r) {
    if (!full && slowKey(x.key) && !(moving && x.key == "current")) continue;
    String v = status::mqttValue(x);
    std::string k = x.key.c_str();
    auto it = sent.find(k);
    if (!full && it != sent.end() && it->second == v.c_str()) continue;
    if (pub(base + "/status/" + x.key, v, true)) sent[k] = v.c_str();
  }
  pub(base + "/status", status::json(), true);
  String st = gate::stateName(gate::snapshot().state);
  if (full || st != lastState) { pub(base + "/state", st, true); lastState = st; }
  // Home Assistant update entity
  const ota::Info& oi = ota::info();
  String u = "{\"installed_version\":\"" FW_VERSION "\",\"latest_version\":\"" + (oi.checked ? oi.latest : String(FW_VERSION)) +
             "\",\"title\":\"GateMaster firmware\",\"in_progress\":" + (oi.installing ? "true" : "false") + "}";
  if (full || u != lastUpdateJson) { pub(base + "/update/state", u, true); lastUpdateJson = u; }
  lastPub = millis();
}

static void publishEvents() {
  eventlog::Entry e[10];
  int n = eventlog::since(lastEvSeq, e, 10);
  for (int i = 0; i < n; i++) {
    String t = e[i].text;
    t.replace("\\", "/"); t.replace("\"", "'");
    pub(base + "/event", "{\"seq\":" + String(e[i].seq) + ",\"t\":" + String(e[i].epoch) + ",\"text\":\"" + t + "\"}", false);
    lastEvSeq = e[i].seq;
  }
}

// ------------------------------------------------------------------ Home Assistant discovery
static String devBlock() {
  String n = dev::cfg().name; n.replace("\"", "'");
  return ",\"dev\":{\"ids\":[\"gatemaster_" + dev::chipId() + "\"],\"name\":\"" + n + "\",\"mf\":\"GateMaster\",\"mdl\":\"" +
         String(hal::sim() ? "GateMaster (simulator)" : "GateMaster rev C") + "\",\"sw\":\"" FW_VERSION "\",\"cu\":\"http://" +
         wifimgr::ip() + "/\"},\"avty_t\":\"" + base + "/availability\"";
}

struct Ent { const char* comp; const char* id; const char* name; const char* extra; };
// extra: JSON fragment without the leading comma; ~ is replaced by the base topic
static const Ent ENTS[] = {
  {"cover", "gate", "Gate", "\"dev_cla\":\"gate\",\"cmd_t\":\"~/cmd\",\"pl_open\":\"open\",\"pl_cls\":\"close\",\"pl_stop\":\"stop\",\"stat_t\":\"~/state\",\"val_tpl\":\"{% set m={'partial':'open','lockout':'stopped','unknown':'stopped'} %}{{ m.get(value, value) }}\",\"pos_t\":\"~/status/position\",\"pos_tpl\":\"{{ value | int(0) if value | int(-1) >= 0 else 0 }}\""},
  {"button", "partial", "Partial open", "\"cmd_t\":\"~/cmd\",\"pl_prs\":\"partial\",\"ic\":\"mdi:gate-arrow-right\""},
  {"button", "reset", "Reset alarm", "\"cmd_t\":\"~/cmd\",\"pl_prs\":\"reset\",\"ic\":\"mdi:alarm-off\""},
  {"button", "update_check", "Check for update", "\"cmd_t\":\"~/cmd\",\"pl_prs\":\"update_check\",\"ent_cat\":\"config\""},
  {"switch", "hold_open", "Open for N minutes", "\"cmd_t\":\"~/cmd\",\"pl_on\":\"hold_on\",\"pl_off\":\"hold_off\",\"stat_t\":\"~/status/hold_open\",\"stat_on\":\"ON\",\"stat_off\":\"OFF\",\"ic\":\"mdi:gate-open\""},
  {"sensor", "hold_left", "Open for: minutes left", "\"stat_t\":\"~/status/hold_left\",\"unit_of_meas\":\"min\",\"val_tpl\":\"{{ value if value | int(-1) >= 0 else none }}\",\"ic\":\"mdi:timer-sand\""},
  {"number", "hold_min", "Open for (minutes)", "\"cmd_t\":\"~/set/holdMin\",\"stat_t\":\"~/config\",\"val_tpl\":\"{{ value_json.holdMin }}\",\"min\":1,\"max\":1440,\"step\":1,\"unit_of_meas\":\"min\",\"mode\":\"box\",\"ent_cat\":\"config\""},
  {"switch", "auto_close", "Auto-close", "\"cmd_t\":\"~/cmd\",\"pl_on\":\"autoclose_on\",\"pl_off\":\"autoclose_off\",\"stat_t\":\"~/status/auto_close\",\"stat_on\":\"ON\",\"stat_off\":\"OFF\",\"ic\":\"mdi:timer-outline\""},
  {"switch", "lock_out", "Lock out", "\"cmd_t\":\"~/cmd\",\"pl_on\":\"lockout_on\",\"pl_off\":\"lockout_off\",\"stat_t\":\"~/status/lock_out\",\"stat_on\":\"ON\",\"stat_off\":\"OFF\",\"ic\":\"mdi:lock\""},
  {"sensor", "current", "Motor current", "\"stat_t\":\"~/status/current\",\"unit_of_meas\":\"A\",\"dev_cla\":\"current\",\"stat_cla\":\"measurement\""},
  {"sensor", "position", "Position", "\"stat_t\":\"~/status/position\",\"unit_of_meas\":\"%\",\"val_tpl\":\"{{ value if value | int(-1) >= 0 else none }}\""},
  {"sensor", "autoclose_left", "Auto-close in", "\"stat_t\":\"~/status/autoclose_left\",\"unit_of_meas\":\"s\",\"val_tpl\":\"{{ value if value | int(-1) >= 0 else none }}\""},
  {"sensor", "state", "State", "\"stat_t\":\"~/state\",\"ic\":\"mdi:gate\""},
  {"sensor", "last_event", "Last event", "\"stat_t\":\"~/event\",\"val_tpl\":\"{{ value_json.text }}\",\"ic\":\"mdi:text-box-outline\""},
  {"sensor", "cycles", "Cycles", "\"stat_t\":\"~/status/cycles\",\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\""},
  {"sensor", "obstructions", "Obstructions", "\"stat_t\":\"~/status/obstructions\",\"stat_cla\":\"total_increasing\",\"ent_cat\":\"diagnostic\""},
  {"sensor", "rssi", "Wi-Fi signal", "\"stat_t\":\"~/status/rssi\",\"unit_of_meas\":\"dBm\",\"dev_cla\":\"signal_strength\",\"ent_cat\":\"diagnostic\""},
  {"sensor", "uptime", "Uptime", "\"stat_t\":\"~/status/uptime\",\"unit_of_meas\":\"s\",\"dev_cla\":\"duration\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "photo_eye", "Photo-eye", "\"stat_t\":\"~/status/input/safety\",\"dev_cla\":\"safety\""},
  {"binary_sensor", "safety_edge", "Safety edge", "\"stat_t\":\"~/status/input/edge\",\"dev_cla\":\"safety\""},
  {"binary_sensor", "exit", "EXIT loop", "\"stat_t\":\"~/status/input/exit\",\"dev_cla\":\"occupancy\""},
  {"binary_sensor", "radio", "Radio", "\"stat_t\":\"~/status/input/radio\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "in_3bt", "3BT", "\"stat_t\":\"~/status/input/3bt\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "reset_in", "RESET input", "\"stat_t\":\"~/status/input/reset\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "ped", "PED-SW", "\"stat_t\":\"~/status/input/ped\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "phantom", "PHANTOM", "\"stat_t\":\"~/status/input/phantom\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "mid", "MID reed", "\"stat_t\":\"~/status/input/mid\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "spare", "SPARE", "\"stat_t\":\"~/status/input/spare\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "open_limit", "Open limit", "\"stat_t\":\"~/status/open_limit\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "closed_limit", "Closed limit", "\"stat_t\":\"~/status/closed_limit\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "lockout", "Lockout (entrapment)", "\"stat_t\":\"~/status/fault/lockout\",\"dev_cla\":\"problem\""},
  {"binary_sensor", "motor_fault", "Motor fault", "\"stat_t\":\"~/status/fault/no_current\",\"dev_cla\":\"problem\""},
  {"binary_sensor", "forced_open", "Forced open", "\"stat_t\":\"~/status/fault/forced_open\",\"dev_cla\":\"problem\""},
  {"binary_sensor", "edge_wiring", "Edge wiring fault", "\"stat_t\":\"~/status/fault/edge_wiring\",\"dev_cla\":\"problem\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "alarm", "Alarm buzzer", "\"stat_t\":\"~/status/alarm\",\"dev_cla\":\"sound\""},
  {"binary_sensor", "lock_output", "Lock output", "\"stat_t\":\"~/status/lock\",\"ent_cat\":\"diagnostic\""},
  {"binary_sensor", "relay_output", "RELAY-1 output", "\"stat_t\":\"~/status/relay\",\"ent_cat\":\"diagnostic\""},
  {"update", "firmware", "Firmware", "\"stat_t\":\"~/update/state\",\"cmd_t\":\"~/cmd\",\"pl_inst\":\"update_install\",\"dev_cla\":\"firmware\",\"ent_cat\":\"config\""},
  {"number", "close_delay", "Auto-close delay", "\"cmd_t\":\"~/set/closeDelay\",\"stat_t\":\"~/config\",\"val_tpl\":\"{{ value_json.closeDelay }}\",\"min\":1,\"max\":60,\"step\":1,\"unit_of_meas\":\"s\",\"ent_cat\":\"config\""},
  {"number", "partial_pct", "Partial open", "\"cmd_t\":\"~/set/partialPct\",\"stat_t\":\"~/config\",\"val_tpl\":\"{{ value_json.partialPct }}\",\"min\":10,\"max\":90,\"step\":1,\"unit_of_meas\":\"%\",\"ent_cat\":\"config\""},
};

static void discovery(bool on) {
  String id = "gatemaster_" + dev::chipId();
  String devb = on ? devBlock() : "";
  for (const Ent& e : ENTS) {
    String topic = String("homeassistant/") + e.comp + "/" + id + "/" + e.id + "/config";
    if (!on) { pub(topic, "", true); continue; }
    String extra = e.extra;
    String p = "{\"~\":\"" + base + "\",\"name\":\"" + e.name + "\",\"uniq_id\":\"" + id + "_" + e.id + "\"," + extra + devb + "}";
    pub(topic, p, true);
    mq.loop();
  }
}

// ------------------------------------------------------------------ incoming
static void onMessage(char* topicC, byte* payload, unsigned int len) {
  String topic(topicC), p;
  p.reserve(len);
  for (unsigned i = 0; i < len; i++) p += (char)payload[i];
  p.trim();
  if (topic == base + "/cmd") {
    String r;
    String c = p; c.toLowerCase();
    if (c == "update_check") { ota::requestCheck(); r = "ok"; }
    else if (c == "update_install") { String e; r = ota::requestInstall(e) ? "ok" : e; }
    else {
      uint16_t arg = 0;
      Cmd cmd = gate::parseCmd(c, arg);               // "hold_on 30" = open for 30 minutes
      if (cmd == CMD_NONE) r = "unknown command " + c;
      else r = gate::resultText(gate::command(cmd, SRC_MQTT, 300, arg));
    }
    pub(base + "/cmd/result", r, false);
  } else if (topic == base + "/update") {
    String r;
    if (p.equalsIgnoreCase("check")) { ota::requestCheck(); r = "ok"; }
    else if (p.equalsIgnoreCase("install")) { String e; r = ota::requestInstall(e) ? "ok" : e; }
    else r = "use check or install";
    pub(base + "/cmd/result", r, false);
  } else if (topic.startsWith(base + "/set/")) {
    String key = topic.substring(base.length() + 5);
    if (key.endsWith("/result")) return;
    String err;
    bool ok;
    if (p.equalsIgnoreCase("default") && params::find(key) >= 0) { params::resetDefault((ParamId)params::find(key)); ok = true; }
    else ok = params::set(key, p, err);
    pub(base + "/set/" + key + "/result", ok ? String("ok") : err, false);
    if (ok) { eventlog::add("parameter %s = %s (MQTT)", key.c_str(), p.c_str()); configDirty = true; }
  } else if (topic.startsWith(base + "/sim/")) {
    String err;
    String name = topic.substring(base.length() + 5);
    bool ok = hal::sim() ? sim::command(name, p, err) : (err = "not in Simulator mode", false);
    pub(base + "/sim/result", ok ? String("ok") : err, false);
  }
}

static void tryConnect() {
  DeviceCfg& d = dev::cfg();
  base = dev::mqttBase();
  mq.setServer(d.mqttHost.c_str(), d.mqttPort);
  mq.setBufferSize(2048);
  mq.setSocketTimeout(5);
  mq.setKeepAlive(30);
  mq.setCallback(onMessage);
  String will = base + "/availability";
  bool ok = d.mqttUser.length()
    ? mq.connect(dev::hostName().c_str(), d.mqttUser.c_str(), d.mqttPass.c_str(), will.c_str(), 1, true, "offline")
    : mq.connect(dev::hostName().c_str(), will.c_str(), 1, true, "offline");
  if (!ok) {
    nextTry = millis() + backoff;
    if (backoff == 5000 || backoff >= 60000) eventlog::add("MQTT: cannot connect to %s:%u (state %d)", d.mqttHost.c_str(), d.mqttPort, mq.state());
    backoff = min<uint32_t>(backoff * 2, 60000);
    return;
  }
  backoff = 5000;
  eventlog::add("MQTT connected to %s, topic %s", d.mqttHost.c_str(), base.c_str());
  pub(will, "online", true);
  mq.subscribe((base + "/cmd").c_str());
  mq.subscribe((base + "/update").c_str());
  mq.subscribe((base + "/set/+").c_str());
  if (hal::sim()) mq.subscribe((base + "/sim/#").c_str());
  sent.clear();
  lastEvSeq = eventlog::latest();
  discovery(d.haDiscovery);
  pub(base + "/config", params::json(), true);
  configDirty = false;
  publishStatus(true);
  lastFull = millis();
}

void begin() { base = dev::mqttBase(); }

void loop() {
  bool want = dev::cfg().mqttHost.length() && wifimgr::connected();
  if (wantReconnect) {
    wantReconnect = false;
    if (mq.connected()) { pub(base + "/availability", "offline", true); mq.disconnect(); }
    nextTry = 0; backoff = 5000;
  }
  if (!want) { if (mq.connected()) mq.disconnect(); status::setMqttConnected(false); wasConnected = false; return; }
  if (!mq.connected()) {
    status::setMqttConnected(false);
    if (wasConnected) { eventlog::add("MQTT disconnected"); wasConnected = false; }
    if ((int32_t)(millis() - nextTry) >= 0) tryConnect();
    if (!mq.connected()) return;
  }
  wasConnected = true;
  status::setMqttConnected(true);
  mq.loop();

  GateStatus s = gate::snapshot();
  uint32_t now = millis();
  uint32_t interval = (uint32_t)params::geti(P_STATUS_INT) * 1000;
  if (now - lastFull >= interval) { publishStatus(true); lastFull = now; lastSeq = s.changeSeq; }
  else if (s.winding != WIND_OFF && now - lastPub >= 1000) { publishStatus(false); lastSeq = s.changeSeq; }
  else if (s.changeSeq != lastSeq && now - lastPub >= 500) { publishStatus(false); lastSeq = s.changeSeq; }
  publishEvents();
  if (configDirty) { configDirty = false; pub(base + "/config", params::json(), true); }
}
}  // namespace mqtt
