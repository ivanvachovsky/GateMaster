// GateMaster firmware - fixed configuration: version, pins, timings.
// Board: GateMaster rev C (ESP32-WROOM-32UE). Also runs on any 4 MB ESP32 DevKit in Simulator mode.
#pragma once
#include <Arduino.h>

#define FW_NAME    "GateMaster"
#define FW_VERSION "0.2.1"

// Hardware mode on the very first start (no saved setting yet): 0 = GateMaster board, 1 = Simulator.
// The DevKit build on the flashing page is compiled with -DDEFAULT_HW_MODE=1. Saved at first start,
// so later updates (one update file for both) keep it.
#ifndef DEFAULT_HW_MODE
#define DEFAULT_HW_MODE 0
#endif

// ---------------- outputs (GateMaster board) ----------------
constexpr int PIN_MOT_A    = 25;  // opto pair drive A  (A high + B low = MOTOR-1)
constexpr int PIN_MOT_B    = 26;  // opto pair drive B  (B high + A low = MOTOR 2)
constexpr int PIN_LOCK     = 27;  // LOCK opto (120 VAC switched to the lock's supply)
constexpr int PIN_RELAY    = 21;  // RELAY-1 (J6, dry contact)
constexpr int PIN_ALARM    = 12;  // ALARM buzzer driver (J5). Strap pin: must stay LOW at boot.
constexpr int PIN_STAT     = 2;   // STATUS LED + 1 kHz heartbeat for the hardware watchdog (DevKit: blue LED)
constexpr int PIN_TRIP_RST = 23;  // pull LOW 10 ms to clear the latched hardware overcurrent trip
constexpr int PIN_BOOT     = 0;   // BOOT button (DevKit factory reset in Simulator mode)

// ---------------- analog ----------------
constexpr int PIN_CURRENT  = 36;  // ACS724 via 10k/22k divider (x0.6875), 100 mV/A, zero = Vcc/2
constexpr int PIN_BUTTONS  = 39;  // OPEN / STOP / CLOSE buttons + DIP1 (OPEN L/R) resistor ladder

// ---------------- dry-contact inputs (LOW = contact closed) ----------------
enum InputId : uint8_t {
  IN_3BT, IN_LIM1, IN_LIM2, IN_PED, IN_SAFE, IN_PHAN, IN_EXIT,
  IN_RESET, IN_RADIO, IN_EDGE, IN_MID, IN_SPARE, IN_COUNT
};
constexpr int INPUT_PINS[IN_COUNT] = {13, 4, 16, 17, 34, 32, 33, 19, 18, 35, 5, 22};
static const char* const INPUT_NAMES[IN_COUNT] = {
  "3bt", "limit1", "limit2", "ped", "safety", "phantom", "exit",
  "reset", "radio", "edge", "mid", "spare"
};
static const char* const INPUT_LABELS[IN_COUNT] = {
  "3BT (single button)", "LIMIT 1", "LIMIT 2", "PED-SW", "SAFETY (photo-eye)", "PHANTOM", "EXIT LOOP / OPEN",
  "RESET (radio)", "RELAY (radio)", "EDGE (safety edge)", "MID (reed)", "SPARE"
};

enum Winding : uint8_t { WIND_OFF = 0, WIND_M1 = 1, WIND_M2 = 2 };
enum Button : uint8_t { BTN_NONE = 0, BTN_OPEN, BTN_STOP, BTN_CLOSE };

// ---------------- timing ----------------
constexpr uint32_t CONTROL_PERIOD_MS   = 10;    // control loop 100 Hz
constexpr uint32_t DEBOUNCE_MS         = 30;
constexpr uint32_t DIRECTION_DEAD_MS   = 600;   // motor off before reversing (>= 500 ms)
constexpr uint32_t INRUSH_IGNORE_MS    = 1000;  // ignore start-up surge (worst-case reversal stays < 2 s)
constexpr uint32_t NO_CURRENT_MS       = 700;   // commanded but no current -> inhibit / trip
constexpr uint32_t STOP_RESET_HOLD_MS  = 3000;  // STOP held 3 s = Reset alarm
constexpr uint32_t FACTORY_HOLD_MS     = 10000; // STOP (or BOOT on a DevKit) held 10 s at power-up
constexpr uint32_t ALARM_MAX_MS        = 5UL * 60UL * 1000UL;  // UL 325 entrapment alarm: 5 min
constexpr uint32_t GRAPH_PERIOD_MS     = 100;   // motor current graph: 10 samples a second
constexpr uint16_t GRAPH_LEN           = 1200;  // 120 s per run
