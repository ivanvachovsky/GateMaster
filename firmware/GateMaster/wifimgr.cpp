#include "wifimgr.h"
#include "params.h"
#include "config.h"
#include "eventlog.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <time.h>

namespace wifimgr {
static DNSServer dns;
static bool ap = false, everConnected = false, wasConnected = false, mdnsUp = false, sntpUp = false;
static uint32_t apSince = 0, connectedSince = 0, bootAt = 0;
static constexpr uint32_t AP_AFTER_MS = 60000;            // no connection this long after boot -> setup AP
static constexpr uint32_t AP_MAX_MS = 15UL * 60 * 1000;   // setup AP closes after 15 min

static void startAp() {
  if (ap) return;
  String name = "GateMaster-" + dev::chipId();
  WiFi.mode(dev::cfg().wifiSsid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(name.c_str());                               // open: the setup page only takes Wi-Fi details
  dns.start(53, "*", WiFi.softAPIP());                     // phones show the setup page by themselves
  ap = true; apSince = millis();
  eventlog::add("setup network %s open at %s", name.c_str(), WiFi.softAPIP().toString().c_str());
}

static void stopAp() {
  if (!ap) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap = false;
  eventlog::add("setup network closed");
}

void begin() {
  bootAt = millis();
  WiFi.persistent(false);                                  // our own NVS keeps the credentials
  WiFi.setHostname(dev::hostName().c_str());
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  if (dev::cfg().wifiSsid.length()) WiFi.begin(dev::cfg().wifiSsid.c_str(), dev::cfg().wifiPass.c_str());
  else startAp();
}

void loop() {
  bool c = WiFi.status() == WL_CONNECTED;
  if (c && !wasConnected) {
    connectedSince = millis();
    everConnected = true;
    eventlog::add("Wi-Fi connected to %s, IP %s, http://%s.local", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), dev::hostName().c_str());
    if (!mdnsUp) {
      mdnsUp = MDNS.begin(dev::hostName().c_str());
      if (mdnsUp) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "name", dev::cfg().name.c_str());
        MDNS.addServiceTxt("http", "tcp", "fw", FW_VERSION);
      }
    }
    if (!sntpUp) { configTzTime(dev::cfg().tz.c_str(), "pool.ntp.org", "time.google.com", "time.nist.gov"); sntpUp = true; }
  }
  if (!c && wasConnected) eventlog::add("Wi-Fi lost");
  wasConnected = c;

  // setup AP: no Wi-Fi saved, or never connected within 60 s of boot (e.g. wrong password)
  if (!ap && !c && !everConnected && dev::cfg().wifiSsid.length() && millis() - bootAt > AP_AFTER_MS && millis() - bootAt < AP_MAX_MS) startAp();
  if (ap) {
    dns.processNextRequest();
    if (c && millis() - connectedSince > 30000) stopAp();                 // joined the home network
    else if (millis() - apSince > AP_MAX_MS && dev::cfg().wifiSsid.length()) stopAp();
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
bool apActive() { return ap; }
String ip() { return connected() ? WiFi.localIP().toString() : ap ? WiFi.softAPIP().toString() : String("0.0.0.0"); }
String ssid() { return connected() ? WiFi.SSID() : String(""); }
int rssi() { return connected() ? WiFi.RSSI() : 0; }
String mac() { return WiFi.macAddress(); }
bool timeValid() { return time(nullptr) > 1700000000; }

bool connectNow(const String& s, const String& p, uint32_t timeoutMs) {
  eventlog::add("Wi-Fi: trying %s", s.c_str());
  WiFi.disconnect(false, false);
  delay(100);
  WiFi.mode(ap ? WIFI_AP_STA : WIFI_STA);
  WiFi.begin(s.c_str(), p.c_str());
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      DeviceCfg& d = dev::cfg();
      d.wifiSsid = s; d.wifiPass = p; dev::save();
      loop();
      return true;
    }
    delay(100);
  }
  eventlog::add("Wi-Fi: could not join %s", s.c_str());
  // back to the saved network, if any
  WiFi.disconnect(false, false);
  if (dev::cfg().wifiSsid.length()) WiFi.begin(dev::cfg().wifiSsid.c_str(), dev::cfg().wifiPass.c_str());
  return false;
}

void reconnect() {
  WiFi.disconnect(false, false);
  delay(100);
  if (dev::cfg().wifiSsid.length()) WiFi.begin(dev::cfg().wifiSsid.c_str(), dev::cfg().wifiPass.c_str());
}
}  // namespace wifimgr
