#include "improv.h"
#include "wifimgr.h"
#include "params.h"
#include "config.h"
#include <WiFi.h>

namespace improv {
enum : uint8_t { T_STATE = 1, T_ERROR = 2, T_RPC = 3, T_RESULT = 4 };
enum : uint8_t { S_READY = 2, S_PROVISIONING = 3, S_PROVISIONED = 4 };
enum : uint8_t { E_NONE = 0, E_INVALID = 1, E_UNKNOWN_RPC = 2, E_CONNECT = 3, E_UNKNOWN = 0xff };
enum : uint8_t { C_WIFI = 1, C_STATE = 2, C_INFO = 3, C_SCAN = 4 };

static uint8_t buf[256];
static uint16_t len = 0;

static void send(uint8_t type, const uint8_t* data, uint8_t n) {
  uint8_t f[6 + 3 + 255 + 1];
  memcpy(f, "IMPROV", 6);
  f[6] = 1; f[7] = type; f[8] = n;
  memcpy(f + 9, data, n);
  uint8_t sum = 0;
  for (int i = 0; i < 9 + n; i++) sum += f[i];
  f[9 + n] = sum;
  f[10 + n] = '\n';
  Serial.write(f, 11 + n);          // one write: never split by log lines from the other core
  Serial.flush();
}
static void sendState(uint8_t s) { send(T_STATE, &s, 1); }
static void sendError(uint8_t e) { send(T_ERROR, &e, 1); }

static void sendResult(uint8_t cmd, const String* strs, int n) {
  uint8_t d[255];
  int p = 2;
  for (int i = 0; i < n; i++) {
    int l = min((int)strs[i].length(), 250 - p);
    if (l < 0) break;
    d[p++] = l;
    memcpy(d + p, strs[i].c_str(), l);
    p += l;
  }
  d[0] = cmd; d[1] = p - 2;
  send(T_RESULT, d, p);
}

static String url() { return "http://" + WiFi.localIP().toString() + "/"; }

static void handleRpc(const uint8_t* d, uint8_t n) {
  if (n < 2 || d[1] + 2 > n) { sendError(E_INVALID); return; }
  uint8_t cmd = d[0];
  const uint8_t* p = d + 2;
  uint8_t pl = d[1];
  switch (cmd) {
    case C_WIFI: {
      if (pl < 1) { sendError(E_INVALID); return; }
      uint8_t sl = p[0];
      if (1 + sl + 1 > pl) { sendError(E_INVALID); return; }
      String ssid((const char*)p + 1, sl);
      uint8_t pwl = p[1 + sl];
      if (2 + sl + pwl > pl) { sendError(E_INVALID); return; }
      String pass((const char*)p + 2 + sl, pwl);
      sendError(E_NONE);
      sendState(S_PROVISIONING);
      if (wifimgr::connectNow(ssid, pass, 20000)) {
        sendState(S_PROVISIONED);
        String u = url();
        sendResult(C_WIFI, &u, 1);
      } else {
        sendError(E_CONNECT);
        sendState(S_READY);
      }
      break;
    }
    case C_STATE:
      sendError(E_NONE);
      if (wifimgr::connected()) { sendState(S_PROVISIONED); String u = url(); sendResult(C_STATE, &u, 1); }
      else sendState(S_READY);
      break;
    case C_INFO: {
      sendError(E_NONE);
      String s[4] = {FW_NAME, FW_VERSION, "ESP32", dev::cfg().name + " (" + dev::hostName() + ")"};
      sendResult(C_INFO, s, 4);
      break;
    }
    case C_SCAN: {
      sendError(E_NONE);
      int n = WiFi.scanNetworks(false, false);
      for (int i = 0; i < n && i < 20; i++) {
        String s[3] = {WiFi.SSID(i), String(WiFi.RSSI(i)), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"};
        if (s[0].length()) sendResult(C_SCAN, s, 3);
      }
      WiFi.scanDelete();
      sendResult(C_SCAN, nullptr, 0);
      break;
    }
    default:
      sendError(E_UNKNOWN_RPC);
  }
}

void loop() {
  while (Serial.available()) {
    uint8_t b = Serial.read();
    if (len < 6) {                                   // hunt for "IMPROV"
      if (b == (uint8_t)"IMPROV"[len]) buf[len++] = b;
      else len = (b == 'I') ? (buf[0] = b, 1) : 0;
      continue;
    }
    buf[len++] = b;
    if (len >= 9) {
      uint16_t need = 9 + buf[8] + 1;
      if (len >= need) {
        uint8_t sum = 0;
        for (int i = 0; i < need - 1; i++) sum += buf[i];
        bool ok = sum == buf[need - 1] && buf[6] == 1;
        if (ok && buf[7] == T_RPC) handleRpc(buf + 9, buf[8]);
        else if (!ok) sendError(E_INVALID);
        len = 0;
      }
    }
    if (len >= sizeof buf) len = 0;
  }
}
}  // namespace improv
