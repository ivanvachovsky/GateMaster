// Hardware interface. Everything above this layer (inputs, gate logic, web, MQTT) is the same code
// whether it runs on the GateMaster board or on a plain ESP32 DevKit with the built-in gate simulator.
// The backend is chosen once at boot from the "Hardware" setting.
#pragma once
#include "config.h"

namespace hal {
void begin(uint8_t hwMode);        // HW_BOARD or HW_SIM; outputs off and heartbeat on at once
bool sim();                        // running the simulator backend (either simulator mode)?
bool simRealInputs();              // simulator with the real input pins
bool simRealButtons();             // ... and the real button ladder on GPIO 39 (pull-up found at start)

// raw inputs (not debounced)
bool contactClosed(InputId i);     // true = contact closed (pin LOW on the board)
uint16_t buttonMv();               // IO39 ladder voltage
void setEdgeAnalog(bool on);       // 8.2 kOhm monitored edge: read IO35 as analog
int edgeMv();                      // edge node voltage in analog mode, else -1
bool factoryButtonHeld();          // board: STOP on the ladder; DevKit: BOOT (GPIO 0)

// outputs
void drive(Winding w);             // never both: the other is dropped first
Winding driving();
void lock(bool on);
void relay(bool on);
void alarm(bool on);
bool lockOn();
bool relayOn();
bool alarmOn();
void heartbeat(uint8_t duty);      // 1 kHz PWM on GPIO 2, duty 13..153 only - never stopped
void tripReset();                  // 10 ms LOW pulse on TRIP_RST

float currentA();                  // motor current, RMS over the last ~100 ms
void tick(uint32_t dtMs);          // once per control tick (moves the simulated gate)
}

// ------------------------------------------------------------------ simulator controls
// Called from the web page / MQTT (network task). Only meaningful in Simulator mode.
namespace sim {
enum Action : uint8_t { ACT_OFF, ACT_ON, ACT_PULSE };
bool setInput(const String& name, Action a);   // safety, edge, exit, radio, reset, 3bt, ped, phantom, mid, spare
bool button(const String& name, Action a);     // open, stop, close
void obstacle(float pos, bool blocksOpening);  // pos 0..1, < 0 removes it
void trip();                                   // latch the overcurrent trip
void push(float pos);                          // move the gate by hand (forced open)
void setLimitsNC(bool nc);
void setTravel(float seconds);
void setDip1(bool on);
void setEdgeFault(bool on);                    // monitored edge: wire break
String json();
bool command(const String& name, const String& value, String& err);   // generic entry for API / MQTT
}
