// Device / network settings (General tab). Used by the network task only.
#include "params.h"
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md5.h>
#include <esp_system.h>

namespace dev {
static DeviceCfg C;
static bool loaded = false;
static uint8_t bootMode = HW_BOARD;

static void loadNow() {
  loaded = true;
  Preferences p;
  p.begin("dev", true);
  DeviceCfg d;
  C.name = p.getString("name", d.name);
  C.wifiSsid = p.getString("ssid", "");
  C.wifiPass = p.getString("wpass", "");
  C.adminHash = p.getString("pwHash", "");
  C.adminSalt = p.getString("pwSalt", "");
  C.adminMd5 = p.getString("pwMd5", "");
  C.mqttHost = p.getString("mqHost", "");
  C.mqttPort = p.getUShort("mqPort", d.mqttPort);
  C.mqttUser = p.getString("mqUser", "");
  C.mqttPass = p.getString("mqPass", "");
  C.mqttBase = p.getString("mqBase", "");
  C.haDiscovery = p.getBool("ha", d.haDiscovery);
  C.tz = p.getString("tz", d.tz);
  C.hwMode = p.getUChar("hw", d.hwMode);
  C.arduinoOta = p.getBool("aOta", false);
  bool hwSaved = p.isKey("hw");
  p.end();
  bootMode = C.hwMode;
  if (!hwSaved) {                      // first start: remember the build's default so updates keep it
    Preferences w;
    w.begin("dev", false);
    w.putUChar("hw", C.hwMode);
    w.end();
  }
}

DeviceCfg& cfg() { if (!loaded) loadNow(); return C; }
uint8_t bootHwMode() { if (!loaded) loadNow(); return bootMode; }
bool isBlank() { cfg(); return !C.wifiSsid.length() && !C.adminHash.length() && !C.mqttHost.length(); }

void save() {
  Preferences p;
  p.begin("dev", false);
  p.putString("name", C.name);
  p.putString("ssid", C.wifiSsid);
  p.putString("wpass", C.wifiPass);
  p.putString("pwHash", C.adminHash);
  p.putString("pwSalt", C.adminSalt);
  p.putString("pwMd5", C.adminMd5);
  p.putString("mqHost", C.mqttHost);
  p.putUShort("mqPort", C.mqttPort);
  p.putString("mqUser", C.mqttUser);
  p.putString("mqPass", C.mqttPass);
  p.putString("mqBase", C.mqttBase);
  p.putBool("ha", C.haDiscovery);
  p.putString("tz", C.tz);
  p.putUChar("hw", C.hwMode);
  p.putBool("aOta", C.arduinoOta);
  p.end();
}

void factoryReset() {
  // keeps gate parameters, learned travel and the hardware mode; clears everything a new owner sets
  cfg();
  uint8_t hw = C.hwMode;
  C = DeviceCfg();
  C.hwMode = hw;
  save();
}

String chipId() {
  uint64_t mac = ESP.getEfuseMac();
  char b[5];
  snprintf(b, sizeof b, "%02x%02x", (unsigned)((mac >> 32) & 0xff), (unsigned)((mac >> 40) & 0xff));
  return b;
}
String hostName() { return "gatemaster-" + chipId(); }

static String slug(const String& s) {
  String o;
  for (char c : s) {
    if (isalnum((unsigned char)c) || c == '-' || c == '_') o += (char)tolower(c);
    else if (c == ' ') o += '_';
  }
  return o.length() ? o : chipId();
}
String mqttBase() {
  cfg();
  if (C.mqttBase.length()) return C.mqttBase;
  return "gatemaster/" + slug(C.name);
}

static String hashOf(const String& salt, const String& pw) {
  uint8_t out[32];
  String in = salt + ":" + pw;
  mbedtls_sha256((const unsigned char*)in.c_str(), in.length(), out, 0);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", out[i]);
  return String(hex);
}

bool setPassword(const String& pw, String& err) {
  if (pw.length() < 8) { err = "Password must be at least 8 characters"; return false; }
  if (pw.length() > 64) { err = "Password is too long"; return false; }
  cfg();
  char s[17];
  for (int i = 0; i < 16; i += 8) snprintf(s + i, 9, "%08x", (unsigned)esp_random());
  C.adminSalt = s;
  C.adminHash = hashOf(C.adminSalt, pw);
  uint8_t m[16];
  mbedtls_md5((const unsigned char*)pw.c_str(), pw.length(), m);
  char mh[33];
  for (int i = 0; i < 16; i++) sprintf(mh + 2 * i, "%02x", m[i]);
  C.adminMd5 = mh;
  save();
  return true;
}

bool checkPassword(const String& pw) {
  cfg();
  if (!C.adminHash.length()) return false;
  String h = hashOf(C.adminSalt, pw);
  // constant-time compare
  uint8_t d = h.length() ^ C.adminHash.length();
  for (size_t i = 0; i < h.length() && i < C.adminHash.length(); i++) d |= h[i] ^ C.adminHash[i];
  return d == 0;
}

static String q(const String& s) {
  String o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
  return o + "\"";
}

String json() {
  cfg();
  String j = "{";
  j += "\"name\":" + q(C.name);
  j += ",\"host\":" + q(hostName());
  j += ",\"wifiSsid\":" + q(C.wifiSsid);
  j += ",\"mqttHost\":" + q(C.mqttHost);
  j += ",\"mqttPort\":" + String(C.mqttPort);
  j += ",\"mqttUser\":" + q(C.mqttUser);
  j += ",\"mqttPassSet\":" + String(C.mqttPass.length() ? "true" : "false");
  j += ",\"mqttBase\":" + q(C.mqttBase);
  j += ",\"mqttBaseEffective\":" + q(mqttBase());
  j += ",\"haDiscovery\":" + String(C.haDiscovery ? "true" : "false");
  j += ",\"tz\":" + q(C.tz);
  j += ",\"hwMode\":" + String(C.hwMode);
  j += ",\"hwModeRunning\":" + String(bootMode);
  j += ",\"arduinoOta\":" + String(C.arduinoOta ? "true" : "false");
  return j + "}";
}

static bool toBool(const String& v) { return v == "1" || v.equalsIgnoreCase("true") || v.equalsIgnoreCase("on"); }

bool setKey(const String& k, const String& vIn, String& err, bool& needReboot, bool& netChanged) {
  cfg();
  String v = vIn;
  if (k != "wifiPass" && k != "mqttPass") v.trim();
  if (k == "name") {
    if (!v.length() || v.length() > 32) { err = "Name: 1 to 32 characters"; return false; }
    C.name = v; netChanged = true;
  } else if (k == "wifiSsid") {
    if (v.length() > 32) { err = "Wi-Fi name: up to 32 characters"; return false; }
    C.wifiSsid = v; netChanged = true;
  } else if (k == "wifiPass") {
    if (v.length() > 64) { err = "Wi-Fi password: up to 64 characters"; return false; }
    C.wifiPass = v; netChanged = true;
  } else if (k == "mqttHost") { C.mqttHost = v; netChanged = true; }
  else if (k == "mqttPort") {
    long n = v.toInt();
    if (n < 1 || n > 65535) { err = "MQTT port: 1 to 65535"; return false; }
    C.mqttPort = n; netChanged = true;
  } else if (k == "mqttUser") { C.mqttUser = v; netChanged = true; }
  else if (k == "mqttPass") { C.mqttPass = v; netChanged = true; }
  else if (k == "mqttBase") {
    if (v.indexOf('#') >= 0 || v.indexOf('+') >= 0 || v.endsWith("/")) { err = "Base topic: no # + or trailing /"; return false; }
    C.mqttBase = v; netChanged = true;
  } else if (k == "haDiscovery") { C.haDiscovery = toBool(v); netChanged = true; }
  else if (k == "tz") {
    if (!v.length() || v.length() > 60) { err = "Time zone: not valid"; return false; }
    C.tz = v;
  } else if (k == "hwMode") {
    long n = v.toInt();
    if (v == "sim" || v == "simulator") n = HW_SIM; else if (v == "board") n = HW_BOARD;
    if (n != HW_BOARD && n != HW_SIM) { err = "Hardware: board or simulator"; return false; }
    if (C.hwMode != n) needReboot = true;
    C.hwMode = n;
  } else if (k == "arduinoOta") { C.arduinoOta = toBool(v); needReboot = true; }
  else { err = "unknown setting " + k; return false; }
  return true;
}
}  // namespace dev
