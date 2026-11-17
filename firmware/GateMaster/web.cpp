#include "web.h"
#include "gate.h"
#include "params.h"
#include "status.h"
#include "eventlog.h"
#include "wifimgr.h"
#include "mqtt.h"
#include "ota.h"
#include "hal.h"
#include "webpage_gz.h"
#include <WiFi.h>
#include <WebServer.h>
#include <mbedtls/base64.h>

namespace web {
static WebServer srv(80);
static uint8_t badTries = 0;
static uint32_t lastBad = 0;

static String q(const String& s) {
  String o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o + "\"";
}
static void sendJson(int code, const String& j) {
  srv.sendHeader("Cache-Control", "no-store");
  srv.send(code, "application/json", j);
}
static void ok() { sendJson(200, "{\"ok\":true}"); }
static void fail(int code, const String& msg) { sendJson(code, "{\"ok\":false,\"error\":" + q(msg) + "}"); }

static bool viaSetupNetwork() { return wifimgr::apActive() && srv.client().localIP() == WiFi.softAPIP(); }
static bool passwordSet() { return dev::cfg().adminHash.length() > 0; }

// HTTP Basic auth against the stored (salted, hashed) password
static bool auth() {
  if (!passwordSet()) { fail(403, "set a password first"); return false; }
  if (badTries >= 5 && millis() - lastBad < 30000) { fail(429, "too many wrong passwords - wait 30 s"); return false; }
  String h = srv.header("Authorization");
  if (h.startsWith("Basic ")) {
    String b64 = h.substring(6);
    unsigned char out[160];
    size_t n = 0;
    if (mbedtls_base64_decode(out, sizeof out - 1, &n, (const unsigned char*)b64.c_str(), b64.length()) == 0) {
      out[n] = 0;
      String up((char*)out);
      int c = up.indexOf(':');
      if (c > 0 && up.substring(0, c) == "admin" && dev::checkPassword(up.substring(c + 1))) { badTries = 0; return true; }
      badTries++; lastBad = millis();
      if (badTries == 5) eventlog::add("web: 5 wrong passwords from %s", srv.client().remoteIP().toString().c_str());
    }
  }
  srv.sendHeader("WWW-Authenticate", "Basic realm=\"GateMaster\"");
  srv.send(401, "text/plain", "Password needed (user: admin)");
  return false;
}

static void sendPage() {
  srv.sendHeader("Content-Encoding", "gzip");
  srv.sendHeader("Cache-Control", "no-cache");
  srv.send_P(200, "text/html", (const char*)WEBPAGE_GZ, sizeof(WEBPAGE_GZ));
}

// ------------------------------------------------------------------ handlers
static void hRoot() {
  if (viaSetupNetwork() || !passwordSet()) { sendPage(); return; }   // page shows the setup / first-password screen
  if (!auth()) return;
  sendPage();
}

static void hInfo() {   // no auth: what the page needs to decide which screen to show
  String j = "{\"name\":" + q(dev::cfg().name) + ",\"host\":" + q(dev::hostName()) + ",\"fw\":\"" FW_VERSION "\"";
  j += ",\"passwordSet\":" + String(passwordSet() ? "true" : "false");
  j += ",\"setupOnly\":" + String(viaSetupNetwork() ? "true" : "false");
  j += ",\"sim\":" + String(hal::sim() ? "true" : "false");
  j += ",\"wifiSet\":" + String(dev::cfg().wifiSsid.length() ? "true" : "false") + "}";
  sendJson(200, j);
}

static void hPassword() {
  if (passwordSet() && !auth()) return;
  String err;
  if (!dev::setPassword(srv.arg("password"), err)) { fail(400, err); return; }
  eventlog::add("web password %s", passwordSet() ? "changed" : "set");
  ok();
}

static void hStatus() {
  if (!auth()) return;
  String j = status::json();
  if (hal::sim()) { j.remove(j.length() - 1); j += ",\"sim\":" + sim::json() + "}"; }
  sendJson(200, j);
}

static void hConfigGet() {
  if (!auth()) return;
  sendJson(200, "{\"params\":" + params::json() + ",\"schema\":" + params::schemaJson() + ",\"dev\":" + dev::json() + ",\"update\":" + ota::json() + "}");
}

// apply key=value pairs (form args) to parameters and device settings
static void applyPairs(const std::vector<std::pair<String, String>>& kv, String& errs, bool& reboot, String src) {
  bool netChanged = false, wifiChanged = false, devChanged = false, paramChanged = false;
  for (auto& p : kv) {
    const String& k = p.first; const String& v = p.second;
    String err;
    int pi = params::find(k);
    if (pi >= 0) {
      bool okk = v.equalsIgnoreCase("default") ? (params::resetDefault((ParamId)pi), true) : params::set(k, v, err);
      if (okk) { paramChanged = true; eventlog::add("parameter %s = %s (%s)", k.c_str(), v.c_str(), src.c_str()); }
      else errs += (errs.length() ? "; " : "") + err;
      continue;
    }
    bool nr = false, nc = false;
    if (dev::setKey(k, v, err, nr, nc)) {
      devChanged = true; reboot |= nr; netChanged |= nc;
      if (k.startsWith("wifi")) wifiChanged = true;
      if (k != "wifiPass" && k != "mqttPass") eventlog::add("setting %s changed (%s)", k.c_str(), src.c_str());
    } else errs += (errs.length() ? "; " : "") + err;
  }
  if (devChanged) dev::save();
  if (paramChanged) mqtt::publishConfig();
  if (wifiChanged) wifimgr::reconnect();
  if (netChanged) mqtt::reconnect();
  if (reboot) ota::rebootWhenIdle(1500);
}

static void hConfigPost() {
  if (!auth()) return;
  std::vector<std::pair<String, String>> kv;
  for (int i = 0; i < srv.args(); i++) if (srv.argName(i) != "plain") kv.push_back({srv.argName(i), srv.arg(i)});
  String errs; bool reboot = false;
  applyPairs(kv, errs, reboot, "web");
  sendJson(errs.length() ? 400 : 200, "{\"ok\":" + String(errs.length() ? "false" : "true") + ",\"error\":" + q(errs) + ",\"reboot\":" + (reboot ? "true" : "false") + "}");
}

static void hCmd() {
  if (!auth()) return;
  String c = srv.arg("cmd");
  if (c == "update_check") { ota::requestCheck(); ok(); return; }
  if (c == "update_install") { String e; if (ota::requestInstall(e)) ok(); else fail(409, e); return; }
  if (c == "reboot") {
    if (!gate::idle()) { fail(409, "not while the gate is moving"); return; }
    eventlog::add("restart requested (web)"); ota::rebootWhenIdle(800); ok(); return;
  }
  Cmd cmd = gate::cmdFromName(c);
  if (cmd == CMD_NONE) { fail(400, "unknown command " + c); return; }
  CmdResult r = gate::command(cmd, SRC_WEB);
  if (r == RES_OK) ok(); else fail(409, gate::resultText(r));
}

static void hCurrent() {
  if (!auth()) return;
  static gate::Graph g;
  bool last = srv.arg("last") == "1";
  uint32_t run = srv.arg("run").toInt();
  uint16_t from = srv.arg("from").toInt();
  gate::graph(last, g, 0);
  if (g.runId != run) from = 0;                 // a new run started: send it all
  if (from > g.len) from = 0;
  String j;
  j.reserve(80 + (g.len - from) * 8);
  j = "{\"run\":" + String(g.runId) + ",\"from\":" + String(from) + ",\"len\":" + String(g.len) + ",\"a\":[";
  for (uint16_t i = from; i < g.len; i++) { if (i > from) j += ","; j += String(g.amps[i]); }
  j += "],\"t\":[";
  for (uint16_t i = from; i < g.len; i++) { if (i > from) j += ","; j += String(g.thr[i]); }
  sendJson(200, j + "]}");
}

static void hLog() {
  if (!auth()) return;
  sendJson(200, eventlog::json(srv.arg("since").toInt(), srv.hasArg("max") ? srv.arg("max").toInt() : 200));
}
static void hLogText() {
  if (!auth()) return;
  srv.sendHeader("Content-Disposition", "attachment; filename=\"" + dev::hostName() + "-events.txt\"");
  srv.send(200, "text/plain", eventlog::text());
}

static void hBackup() {
  if (!auth()) return;
  DeviceCfg& d = dev::cfg();
  String j = "{\"gatemaster\":\"backup\",\"fw\":\"" FW_VERSION "\",\"params\":" + params::json() + ",\"dev\":{";
  j += "\"name\":" + q(d.name) + ",\"mqttHost\":" + q(d.mqttHost) + ",\"mqttPort\":" + String(d.mqttPort);
  j += ",\"mqttUser\":" + q(d.mqttUser) + ",\"mqttBase\":" + q(d.mqttBase) + ",\"haDiscovery\":" + (d.haDiscovery ? "true" : "false");
  j += ",\"tz\":" + q(d.tz) + "}}";
  srv.sendHeader("Content-Disposition", "attachment; filename=\"" + dev::hostName() + "-backup.json\"");
  sendJson(200, j);
}

// minimal reader for {"k":v,...} objects nested one level ("params":{...},"dev":{...})
static void pairsOf(const String& j, const char* section, std::vector<std::pair<String, String>>& out) {
  int s = j.indexOf(String("\"") + section + "\"");
  if (s < 0) return;
  s = j.indexOf('{', s);
  if (s < 0) return;
  int i = s + 1, n = j.length();
  while (i < n) {
    while (i < n && (isspace(j[i]) || j[i] == ',')) i++;
    if (i >= n || j[i] == '}') break;
    if (j[i] != '"') return;
    int ke = j.indexOf('"', i + 1);
    if (ke < 0) return;
    String key = j.substring(i + 1, ke);
    i = j.indexOf(':', ke) + 1;
    if (i <= 0) return;
    while (i < n && isspace(j[i])) i++;
    String val;
    if (j[i] == '"') {
      i++;
      while (i < n && j[i] != '"') { if (j[i] == '\\' && i + 1 < n) i++; val += j[i++]; }
      i++;
    } else {
      int e = i;
      while (e < n && j[e] != ',' && j[e] != '}') e++;
      val = j.substring(i, e); val.trim();
      i = e;
    }
    out.push_back({key, val});
  }
}

static void hRestore() {
  if (!auth()) return;
  String body = srv.arg("plain");
  if (body.indexOf("\"gatemaster\"") < 0) { fail(400, "not a GateMaster backup file"); return; }
  std::vector<std::pair<String, String>> kv;
  pairsOf(body, "params", kv);
  pairsOf(body, "dev", kv);
  std::vector<std::pair<String, String>> use;
  String skipped;
  for (auto& p : kv) {
    if (params::find(p.first) >= 0 || p.first == "name" || p.first.startsWith("mqtt") || p.first == "haDiscovery" || p.first == "tz") use.push_back(p);
    else skipped += (skipped.length() ? ", " : "") + p.first;   // unknown keys (newer firmware): skipped
  }
  String errs; bool reboot = false;
  applyPairs(use, errs, reboot, "restore");
  eventlog::add("settings restored from backup (%u values)", (unsigned)use.size());
  sendJson(200, "{\"ok\":" + String(errs.length() ? "false" : "true") + ",\"applied\":" + String(use.size()) + ",\"error\":" + q(errs) + ",\"skipped\":" + q(skipped) + "}");
}

static void hFactoryReset() {
  if (!auth()) return;
  if (!gate::idle()) { fail(409, "not while the gate is moving"); return; }
  eventlog::add("factory reset (web)");
  dev::factoryReset();
  ok();
  ota::rebootWhenIdle(1000);
}

static void hUpdateInfo() { if (!auth()) return; sendJson(200, ota::json()); }

static bool uploadAuthed = false;
static void hUploadDone() {
  if (!uploadAuthed) { if (!auth()) return; }
  const ota::Info& i = ota::info();
  if (ota::rebootPending()) ok();
  else fail(400, i.lastError.length() ? i.lastError : String("upload failed"));
}
static void hUploadData() {
  HTTPUpload& up = srv.upload();
  if (viaSetupNetwork()) return;
  if (up.status == UPLOAD_FILE_START) {
    uploadAuthed = auth();     // sends 401 itself if not
    if (!uploadAuthed) return;
    String e;
    if (!ota::uploadBegin(0, e)) { uploadAuthed = false; fail(409, e); }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadAuthed && !ota::uploadWrite(up.buf, up.currentSize)) { ota::uploadAbort(); uploadAuthed = false; }
  } else if (up.status == UPLOAD_FILE_END) {
    String e;
    if (uploadAuthed) ota::uploadEnd(e);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    ota::uploadAbort();
  }
}

static void hSim() {
  if (!auth()) return;
  if (!hal::sim()) { fail(409, "not in Simulator mode"); return; }
  String err;
  if (sim::command(srv.arg("name"), srv.arg("value"), err)) sendJson(200, "{\"ok\":true,\"sim\":" + sim::json() + "}");
  else fail(400, err);
}

// setup network (no password needed until one is set): Wi-Fi details only
static void hWifi() {
  if (passwordSet() && !auth()) return;
  String s = srv.arg("ssid"), p = srv.arg("pass");
  if (!s.length()) { fail(400, "Wi-Fi name needed"); return; }
  DeviceCfg& d = dev::cfg();
  d.wifiSsid = s; d.wifiPass = p; dev::save();
  eventlog::add("Wi-Fi set to %s (setup page)", s.c_str());
  ok();
  wifimgr::reconnect();
}
static void hScan() {
  if (passwordSet() && !auth()) return;
  int n = WiFi.scanNetworks(false, false);
  String j = "[";
  for (int i = 0; i < n && i < 25; i++) {
    if (i) j += ",";
    j += "{\"ssid\":" + q(WiFi.SSID(i)) + ",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  WiFi.scanDelete();
  sendJson(200, j + "]");
}

static void hNotFound() {
  if (viaSetupNetwork()) {                       // captive portal: everything leads to the setup page
    srv.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    srv.send(302, "text/plain", "");
    return;
  }
  srv.send(404, "text/plain", "not found");
}

// setup network: only the setup page and its calls are served there
static bool setupGuard() {
  if (!viaSetupNetwork()) return false;
  fail(403, "not available on the setup network - join your home Wi-Fi");
  return true;
}
#define G(fn) [] { if (setupGuard()) return; fn(); }

void begin() {
  const char* hdrs[] = {"Authorization"};
  srv.collectHeaders(hdrs, 1);
  srv.on("/", HTTP_GET, hRoot);
  srv.on("/api/info", HTTP_GET, hInfo);
  srv.on("/api/password", HTTP_POST, G(hPassword));
  srv.on("/api/wifi", HTTP_POST, hWifi);
  srv.on("/api/scan", HTTP_GET, hScan);
  srv.on("/api/status", HTTP_GET, G(hStatus));
  srv.on("/api/config", HTTP_GET, G(hConfigGet));
  srv.on("/api/config", HTTP_POST, G(hConfigPost));
  srv.on("/api/cmd", HTTP_POST, G(hCmd));
  srv.on("/api/current", HTTP_GET, G(hCurrent));
  srv.on("/api/log", HTTP_GET, G(hLog));
  srv.on("/api/log.txt", HTTP_GET, G(hLogText));
  srv.on("/api/backup", HTTP_GET, G(hBackup));
  srv.on("/api/restore", HTTP_POST, G(hRestore));
  srv.on("/api/factory-reset", HTTP_POST, G(hFactoryReset));
  srv.on("/api/update", HTTP_GET, G(hUpdateInfo));
  srv.on("/api/update", HTTP_POST, G(hUploadDone), hUploadData);
  srv.on("/api/sim", HTTP_POST, G(hSim));
  srv.onNotFound(hNotFound);
  srv.begin();
}

void loop() { srv.handleClient(); }
}  // namespace web
