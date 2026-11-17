// Hardware interface dispatch + the GateMaster board backend.
#include "hal.h"
#include "params.h"

// simulator backend (hal_sim.cpp)
namespace simhw {
void begin();
bool contactClosed(InputId i);
uint16_t buttonMv();
int edgeMv();
void drive(Winding w);
float currentA();
void tick(uint32_t dtMs);
void tripReset();
}

#ifndef HOST_TEST
#include <esp_timer.h>
#include <driver/adc.h>
#include <esp_adc_cal.h>

namespace board {
static esp_adc_cal_characteristics_t adcChars;
static volatile float rmsA = 0;
static double accSum = 0, accSq = 0;
static uint16_t accN = 0;
static constexpr uint16_t WINDOW = 200;          // 200 x 0.5 ms = 100 ms = 5 / 6 mains cycles
static constexpr float VOLT_PER_A_AT_ADC = 0.100f * 22.0f / 32.0f;  // 100 mV/A through 10k/22k
static bool edgeAnalog = false;

static void sampleCb(void*) {
  int raw = adc1_get_raw(ADC1_CHANNEL_0);        // GPIO36
  uint32_t mv = esp_adc_cal_raw_to_voltage(raw, &adcChars);
  accSum += mv; accSq += (double)mv * mv; accN++;
  if (accN >= WINDOW) {
    double mean = accSum / accN;
    double var = accSq / accN - mean * mean;
    rmsA = (var > 0 ? sqrt(var) : 0) / 1000.0f / VOLT_PER_A_AT_ADC;
    accSum = accSq = 0; accN = 0;
  }
}

static void begin() {
  for (int p : {PIN_MOT_A, PIN_MOT_B, PIN_LOCK, PIN_RELAY, PIN_ALARM}) { digitalWrite(p, LOW); pinMode(p, OUTPUT); }
  pinMode(PIN_TRIP_RST, INPUT);
  for (int i = 0; i < IN_COUNT; i++) pinMode(INPUT_PINS[i], INPUT);   // external pull-ups via dividers
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);
  esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adcChars);
  analogSetPinAttenuation(PIN_BUTTONS, ADC_11db);
  esp_timer_create_args_t a = {};
  a.callback = sampleCb; a.name = "isense";
  esp_timer_handle_t t;
  esp_timer_create(&a, &t);
  esp_timer_start_periodic(t, 500);              // 2 kHz
}
static bool contactClosed(InputId i) {
  if (i == IN_EDGE && edgeAnalog) return false;
  return digitalRead(INPUT_PINS[i]) == LOW;
}
static void drive(Winding w) {
  digitalWrite(PIN_MOT_A, LOW); digitalWrite(PIN_MOT_B, LOW);
  if (w == WIND_M1) digitalWrite(PIN_MOT_A, HIGH);
  else if (w == WIND_M2) digitalWrite(PIN_MOT_B, HIGH);
}
}  // namespace board
#endif

namespace hal {
static bool simMode = false;
static Winding drv = WIND_OFF;
static bool lockS = false, relayS = false, alarmS = false;
static bool edgeAnalogS = false;

void begin(uint8_t hwMode) {
  simMode = hwMode == HW_SIM;
#ifdef HOST_TEST
  simMode = true;
#else
  if (!simMode) board::begin();
  // GPIO 2: hardware-watchdog heartbeat on the board, blue LED on a DevKit. Always running.
  ledcSetup(0, 1000, 8);
  ledcAttachPin(PIN_STAT, 0);
  ledcWrite(0, 25);
  if (simMode) pinMode(PIN_BOOT, INPUT_PULLUP);
#endif
  if (simMode) simhw::begin();
}

bool sim() { return simMode; }

bool contactClosed(InputId i) {
  if (simMode) return simhw::contactClosed(i);
#ifndef HOST_TEST
  return board::contactClosed(i);
#else
  return false;
#endif
}

uint16_t buttonMv() {
  if (simMode) return simhw::buttonMv();
#ifndef HOST_TEST
  return analogReadMilliVolts(PIN_BUTTONS);
#else
  return 3300;
#endif
}

void setEdgeAnalog(bool on) {
  if (on == edgeAnalogS) return;
  edgeAnalogS = on;
#ifndef HOST_TEST
  if (!simMode) {
    board::edgeAnalog = on;
    if (on) analogSetPinAttenuation(INPUT_PINS[IN_EDGE], ADC_11db);
    else pinMode(INPUT_PINS[IN_EDGE], INPUT);
  }
#endif
}

int edgeMv() {
  if (!edgeAnalogS) return -1;
  if (simMode) return simhw::edgeMv();
#ifndef HOST_TEST
  return analogReadMilliVolts(INPUT_PINS[IN_EDGE]);
#else
  return -1;
#endif
}

bool factoryButtonHeld() {
#ifndef HOST_TEST
  if (simMode) return digitalRead(PIN_BOOT) == LOW;
  return analogReadMilliVolts(PIN_BUTTONS) < 450;
#else
  return false;
#endif
}

void drive(Winding w) {
  drv = w;
  if (simMode) { simhw::drive(w); return; }
#ifndef HOST_TEST
  board::drive(w);
#endif
}
Winding driving() { return drv; }

static void out(int pin, bool on) {
#ifndef HOST_TEST
  if (!simMode) digitalWrite(pin, on);
#endif
  (void)pin; (void)on;
}
void lock(bool on) { lockS = on; out(PIN_LOCK, on); }
void relay(bool on) { relayS = on; out(PIN_RELAY, on); }
void alarm(bool on) { alarmS = on; out(PIN_ALARM, on); }
bool lockOn() { return lockS; }
bool relayOn() { return relayS; }
bool alarmOn() { return alarmS; }

void heartbeat(uint8_t duty) {
#ifndef HOST_TEST
  ledcWrite(0, constrain(duty, 13, 153));
#endif
  (void)duty;
}

void tripReset() {
  if (simMode) { simhw::tripReset(); return; }
#ifndef HOST_TEST
  pinMode(PIN_TRIP_RST, OUTPUT); digitalWrite(PIN_TRIP_RST, LOW);
  delay(10);
  pinMode(PIN_TRIP_RST, INPUT);
#endif
}

float currentA() {
  if (simMode) return simhw::currentA();
#ifndef HOST_TEST
  return board::rmsA;
#else
  return 0;
#endif
}

void tick(uint32_t dtMs) { if (simMode) simhw::tick(dtMs); }
}  // namespace hal
