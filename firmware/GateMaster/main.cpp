// GateMaster firmware v0.2 - ESP32 controller for the All-O-Matic SL-100 (replaces the ALL-AC2K board).
// Board: GateMaster rev C, or any 4 MB ESP32 DevKit in Simulator mode. Arduino-ESP32 core 2.0.9,
// board "ESP32 Dev Module", partition scheme "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)".
//
// Safety model: the board's hardware (limit cut-offs, overcurrent latch, heartbeat watchdog, opto
// interlock) works without this code. The firmware adds obstruction reversal (UL 325), timers and the
// app. The gate logic runs alone in its own task on core 1; Wi-Fi, web, MQTT and updates run here,
// on core 0, and talk to it only through a command queue and a status snapshot.
#include "config.h"
#include "params.h"
#include "hal.h"
#include "inputs.h"
#include "gate.h"
#include "eventlog.h"
#include "wifimgr.h"
#include "improv.h"
#include "web.h"
#include "mqtt.h"
#include "ota.h"
#include "status.h"

// STOP on the board (BOOT on a DevKit) held for 10 s while powering up: erase Wi-Fi, password, MQTT
// and name. Gate parameters and learned values are kept.
static void checkFactoryReset() {
  if (hal::sim()) return;                                    // DevKit: see devkitFactoryButton()
  if (!hal::factoryButtonHeld() || dev::isBlank()) return;   // nothing to reset on a fresh board
  Serial.println("STOP held at power-up: keep holding 10 s, then release, for a factory reset");
  uint32_t t0 = millis();
  while (hal::factoryButtonHeld()) {
    hal::heartbeat(((millis() / 100) & 1) ? 140 : 15);     // fast blink; heartbeat keeps running
    if (millis() - t0 > FACTORY_HOLD_MS) {
      Serial.println("10 s reached - release the button to reset (keep holding to cancel)");
      uint32_t t1 = millis();
      while (hal::factoryButtonHeld()) {                    // a pin that never releases is not a person
        hal::heartbeat(((millis() / 50) & 1) ? 140 : 15);
        if (millis() - t1 > 20000) { Serial.println("button never released: no factory reset"); return; }
        delay(10);
      }
      dev::factoryReset();
      eventlog::begin();
      eventlog::add("FACTORY RESET (button held at power-up): Wi-Fi, password, MQTT and name cleared");
      eventlog::flushNow();
      Serial.println("Factory reset done");
      return;
    }
    delay(10);
  }
  Serial.println("released early: no factory reset");
}

// DevKit (Simulator mode): BOOT held at power-up would start the ESP32's download mode, so here it is
// "hold BOOT for 10 s within the first minute after power-up", checked while running.
static void devkitFactoryButton() {
  static uint32_t pressedAt = 0;
  if (!hal::sim()) return;
  bool held = hal::factoryButtonHeld();
  if (!held) { pressedAt = 0; return; }
  if (!pressedAt) { if (millis() > 60000) return; pressedAt = millis(); Serial.println("BOOT held: keep holding 10 s for a factory reset"); }
  hal::heartbeat(((millis() / 100) & 1) ? 140 : 15);
  if (millis() - pressedAt > FACTORY_HOLD_MS && gate::idle()) {
    dev::factoryReset();
    eventlog::add("FACTORY RESET (BOOT held 10 s): Wi-Fi, password, MQTT and name cleared");
    eventlog::flushNow();
    delay(200);
    ESP.restart();
  }
}

void setup() {
  Serial.setTxBufferSize(2048);          // log lines from the control task never wait on the UART
  Serial.begin(115200);
  uint8_t hw = dev::bootHwMode();
  hal::begin(hw);                        // outputs off, heartbeat running
  params::load();
  checkFactoryReset();
  eventlog::begin();
  eventlog::add("GateMaster %s on %s, restart reason: %s", FW_VERSION, hw == HW_SIM ? "SIMULATOR (DevKit)" : hw == HW_SIM_IO ? (hal::simRealButtons() ? "SIMULATOR with real inputs and buttons" : "SIMULATOR with real inputs (buttons simulated)") : "GateMaster board", status::resetReason());
  inputs::begin();
  gate::begin();
  wifimgr::begin();
  web::begin();
  mqtt::begin();
  ota::begin();
  Serial.printf("\n%s %s - %s - http://%s.local\n", FW_NAME, FW_VERSION, dev::cfg().name.c_str(), dev::hostName().c_str());
}

void loop() {
  improv::loop();
  devkitFactoryButton();
  wifimgr::loop();
  web::loop();
  mqtt::loop();
  ota::loop();
  bool idle = gate::idle();
  if (idle && params::dirty()) params::saveIfDirty();     // flash writes only while the motor is off
  eventlog::flushIfDirty(idle);
  delay(2);
}
