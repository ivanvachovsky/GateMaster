#include "ota.h"
#include "gate.h"
#include "params.h"
#include "eventlog.h"
#include "wifimgr.h"
#include "ca_certs.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoOTA.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>

// keep a new image "pending verify" until we have seen it run properly (see loop)
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {
static Info I;
static bool checkRequested = false, installRequested = false;
static uint32_t nextAutoCheck = 0;
static bool rebootReq = false;
static uint32_t rebootAt = 0;
static bool uploading = false;
static bool arduinoOtaOn = false;
static constexpr uint32_t CHECK_EVERY_MS = 24UL * 3600 * 1000;
static constexpr uint32_t VERIFY_MS = 90000;

const Info& info() { return I; }

static int cmpVersion(const String& a, const String& b) {
  int pa[3] = {0}, pb[3] = {0};
  sscanf(a.c_str(), "%d.%d.%d", &pa[0], &pa[1], &pa[2]);
  sscanf(b.c_str(), "%d.%d.%d", &pb[0], &pb[1], &pb[2]);
  for (int i = 0; i < 3; i++) if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
  return 0;
}

// tiny JSON field reader for version.json (flat object, string or number values)
static String field(const String& j, const char* key) {
  String k = String("\"") + key + "\"";
  int p = j.indexOf(k);
  if (p < 0) return "";
  p = j.indexOf(':', p + k.length());
  if (p < 0) return "";
  p++;
  while (p < (int)j.length() && (j[p] == ' ' || j[p] == '\n' || j[p] == '\r' || j[p] == '\t')) p++;
  if (p < (int)j.length() && j[p] == '"') {
    String out;
    for (int i = p + 1; i < (int)j.length(); i++) {
      char c = j[i];
      if (c == '\\' && i + 1 < (int)j.length()) { char n = j[++i]; out += n == 'n' ? '\n' : n; continue; }
      if (c == '"') break;
      out += c;
    }
    return out;
  }
  int e = p;
  while (e < (int)j.length() && j[e] != ',' && j[e] != '}' && j[e] != '\n') e++;
  String v = j.substring(p, e); v.trim();
  return v;
}

static bool doCheck() {
  I.lastError = "";
  if (!wifimgr::connected()) { I.lastError = "no Wi-Fi"; return false; }
  WiFiClientSecure tls;
  tls.setCACert(CA_CERTS);
  tls.setTimeout(15);
  HTTPClient http;
  String url = String(RELEASE_BASE) + "version.json";
  if (!http.begin(tls, url)) { I.lastError = "cannot reach the release site"; return false; }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != 200) { I.lastError = "release site answered " + String(code); http.end(); return false; }
  String j = http.getString();
  http.end();
  String v = field(j, "version");
  if (!v.length()) { I.lastError = "release info not readable"; return false; }
  I.checked = true;
  I.checkedAtS = millis() / 1000;
  I.latest = v;
  I.notes = field(j, "notes");
  I.date = field(j, "date");
  I.sha256 = field(j, "sha256");
  I.size = field(j, "size").toInt();
  String u = field(j, "url");
  I.url = u.startsWith("https://") ? u : String(RELEASE_BASE) + u;
  bool was = I.available;
  I.available = cmpVersion(FW_VERSION, v) < 0;
  if (I.available && !was) eventlog::add("update available: %s", v.c_str());
  return true;
}

static void finishInstall(bool ok, const String& why) {
  I.installing = false;
  gate::setUpdating(false);
  if (ok) {
    eventlog::add("firmware %s installed - restarting", I.latest.length() ? I.latest.c_str() : "upload");
    rebootWhenIdle(1500);
  } else {
    I.lastError = why;
    eventlog::add("update failed: %s", why.c_str());
  }
}

static void doInstall() {
  if (!I.available || !I.url.length()) { I.lastError = "no update available"; return; }
  if (!gate::idle()) { I.lastError = "the gate is moving"; return; }
  gate::setUpdating(true);
  I.installing = true; I.progress = 0;
  eventlog::add("installing firmware %s", I.latest.c_str());
  WiFiClientSecure tls;
  tls.setCACert(CA_CERTS);
  tls.setTimeout(20);
  HTTPClient http;
  if (!http.begin(tls, I.url)) { finishInstall(false, "cannot reach the release site"); return; }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  if (code != 200) { http.end(); finishInstall(false, "download answered " + String(code)); return; }
  int total = http.getSize();
  if (total <= 0 || (I.size && (uint32_t)total != I.size)) { http.end(); finishInstall(false, "wrong file size"); return; }
  if (!Update.begin(total)) { http.end(); finishInstall(false, String("cannot start: ") + Update.errorString()); return; }
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  WiFiClient* s = http.getStreamPtr();
  static uint8_t buf[2048];
  int got = 0;
  uint32_t lastData = millis();
  while (got < total) {
    size_t avail = s->available();
    if (!avail) {
      if (!http.connected() || millis() - lastData > 20000) break;
      delay(5);
      continue;
    }
    int n = s->readBytes(buf, min(avail, sizeof buf));
    if (n <= 0) continue;
    lastData = millis();
    mbedtls_sha256_update(&sha, buf, n);
    if (Update.write(buf, n) != (size_t)n) break;
    got += n;
    I.progress = (uint64_t)got * 100 / total;
  }
  http.end();
  uint8_t dig[32];
  mbedtls_sha256_finish(&sha, dig);
  mbedtls_sha256_free(&sha);
  if (got != total) { Update.abort(); finishInstall(false, "download interrupted"); return; }
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", dig[i]);
  if (I.sha256.length() && !I.sha256.equalsIgnoreCase(hex)) { Update.abort(); finishInstall(false, "checksum mismatch - file damaged"); return; }
  if (!Update.end(true)) { finishInstall(false, String("verify failed: ") + Update.errorString()); return; }
  finishInstall(true, "");
}

// ------------------------------------------------------------------ manual upload
bool uploadBegin(size_t size, String& err) {
  if (!gate::idle()) { err = "not while the gate is moving"; return false; }
  gate::setUpdating(true);
  if (!Update.begin(size ? size : UPDATE_SIZE_UNKNOWN)) { gate::setUpdating(false); err = Update.errorString(); return false; }
  uploading = true;
  I.installing = true; I.progress = 0;
  eventlog::add("firmware upload started");
  return true;
}
bool uploadWrite(const uint8_t* d, size_t n) {
  if (!uploading) return false;
  return Update.write((uint8_t*)d, n) == n;
}
bool uploadEnd(String& err) {
  if (!uploading) { err = "upload not started"; return false; }
  uploading = false;
  I.latest = "";
  if (!Update.end(true)) { err = Update.errorString(); finishInstall(false, "upload: " + err); return false; }
  finishInstall(true, "");
  return true;
}
void uploadAbort() {
  if (!uploading) return;
  uploading = false;
  Update.abort();
  finishInstall(false, "upload aborted");
}

// ------------------------------------------------------------------ public
void requestCheck() { checkRequested = true; }

bool requestInstall(String& err) {
  if (!gate::idle()) { err = "not while the gate is moving"; return false; }
  if (!I.available) { err = I.checked ? "already up to date" : "check for updates first"; return false; }
  if (I.installing) { err = "already installing"; return false; }
  installRequested = true;
  return true;
}

void rebootWhenIdle(uint32_t d) { rebootReq = true; rebootAt = millis() + d; }
bool rebootPending() { return rebootReq; }

static String q(const String& s) {
  String o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o + "\"";
}

String json() {
  String j = "{\"version\":\"" FW_VERSION "\"";
  j += ",\"checked\":" + String(I.checked ? "true" : "false");
  j += ",\"available\":" + String(I.available ? "true" : "false");
  j += ",\"latest\":" + q(I.latest);
  j += ",\"date\":" + q(I.date);
  j += ",\"notes\":" + q(I.notes);
  j += ",\"checkedAgoS\":" + String(I.checked ? (long)(millis() / 1000 - I.checkedAtS) : -1L);
  j += ",\"installing\":" + String(I.installing ? "true" : "false");
  j += ",\"progress\":" + String(I.progress);
  j += ",\"error\":" + q(I.lastError);
  j += ",\"pendingVerify\":" + String(I.pendingVerify ? "true" : "false");
  return j + "}";
}

void begin() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
    I.pendingVerify = true;
    eventlog::add("new firmware %s on trial: confirming within %lu s", FW_VERSION, (unsigned long)(VERIFY_MS / 1000));
  }
  nextAutoCheck = millis() + 120000;                // first daily check 2 min after boot
  if (dev::cfg().arduinoOta && dev::cfg().adminHash.length()) {
    ArduinoOTA.setHostname(dev::hostName().c_str());
    ArduinoOTA.setPasswordHash(dev::cfg().adminMd5.c_str());
    ArduinoOTA.setRebootOnSuccess(false);
    ArduinoOTA.onStart([] { gate::setUpdating(true); eventlog::add("Arduino IDE upload started"); });
    ArduinoOTA.onEnd([] { gate::setUpdating(false); eventlog::add("Arduino IDE upload done - restarting"); rebootWhenIdle(500); });
    ArduinoOTA.onError([](ota_error_t e) { gate::setUpdating(false); eventlog::add("Arduino IDE upload failed (%d)", (int)e); });
    ArduinoOTA.setMdnsEnabled(false);               // mDNS is already running (wifimgr)
    ArduinoOTA.begin();
    arduinoOtaOn = true;
  }
}

void loop() {
  if (arduinoOtaOn) ArduinoOTA.handle();

  // rollback guard: confirm the new image once it has run properly (control task ticking, Wi-Fi back);
  // otherwise go back to the previous firmware
  if (I.pendingVerify) {
    bool wifiOk = wifimgr::connected() || !dev::cfg().wifiSsid.length();
    bool gateOk = gate::snapshot().uptimeS + 2 >= millis() / 1000;
    if (millis() > 60000 && wifiOk && gateOk) {
      esp_ota_mark_app_valid_cancel_rollback();
      I.pendingVerify = false;
      eventlog::add("new firmware %s confirmed", FW_VERSION);
    } else if (millis() > VERIFY_MS && gate::idle()) {
      eventlog::add("new firmware %s not healthy - going back to the previous one", FW_VERSION);
      eventlog::flushNow();
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }

  if (wifimgr::connected() && (int32_t)(millis() - nextAutoCheck) >= 0) { nextAutoCheck = millis() + CHECK_EVERY_MS; checkRequested = true; }
  if (checkRequested && !I.installing) {
    checkRequested = false;
    if (!doCheck()) eventlog::add("update check failed: %s", I.lastError.c_str());
  }
  if (installRequested) { installRequested = false; doInstall(); }

  if (rebootReq && (int32_t)(millis() - rebootAt) >= 0 && gate::idle()) {
    params::saveIfDirty();
    eventlog::flushNow();
    delay(100);
    ESP.restart();
  }
}
}  // namespace ota
