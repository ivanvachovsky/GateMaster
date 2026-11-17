#include "inputs.h"
#include "hal.h"
#include "params.h"

namespace inputs {
static bool deb[IN_COUNT], pend[IN_COUNT];
static uint8_t cnt[IN_COUNT];
static uint32_t since[IN_COUNT];
static Button btn = BTN_NONE, btnPend = BTN_NONE;
static uint8_t btnCnt = 0;
static uint32_t btnSince = 0;
static bool dip = false;
static uint16_t btnMv = 3300;
static int edgeMvV = -1;
static uint8_t edgeCnt = 0;
static bool edgeAct = false, edgeFlt = false;
static constexpr uint8_t NEED = DEBOUNCE_MS / CONTROL_PERIOD_MS;   // 3 ticks

static bool typed(bool contactClosed, int type) { return type == CONTACT_NC ? !contactClosed : contactClosed; }

static void readEdgeAnalog() {
  edgeMvV = hal::edgeMv();
  // 8.2 kOhm monitored edge: ~1.4 V intact, < 0.4 V pressed (short), > 2.3 V wire open
  bool pressed = edgeMvV >= 0 && edgeMvV < 400;
  bool open = edgeMvV > 2300;
  bool a = pressed || open;
  if (a == edgeAct) edgeCnt = 0;
  else if (++edgeCnt >= NEED) { edgeAct = a; edgeFlt = open; edgeCnt = 0; since[IN_EDGE] = millis(); }
  if (!a) edgeFlt = false;
}

void begin() {
  hal::setEdgeAnalog(params::geti(P_EDGE_TYPE) == CONTACT_8K2);
  for (int i = 0; i < IN_COUNT; i++) { deb[i] = pend[i] = hal::contactClosed((InputId)i); cnt[i] = 0; since[i] = millis(); }
  btnMv = hal::buttonMv();
  dip = btnMv < 3000 && btnMv > 2450;
  if (hal::edgeMv() >= 0) { edgeCnt = NEED; readEdgeAnalog(); }
}

void poll() {
  hal::setEdgeAnalog(params::geti(P_EDGE_TYPE) == CONTACT_8K2);
  uint32_t now = millis();
  for (int i = 0; i < IN_COUNT; i++) {
    bool c = hal::contactClosed((InputId)i);
    if (c == deb[i]) { cnt[i] = 0; pend[i] = c; continue; }
    if (c != pend[i]) { pend[i] = c; cnt[i] = 1; continue; }
    if (++cnt[i] >= NEED) { deb[i] = c; since[i] = now; cnt[i] = 0; }
  }
  if (hal::edgeMv() >= 0) readEdgeAnalog(); else { edgeMvV = -1; edgeFlt = false; }

  // button ladder: idle 3.3 V (DIP1 off) / 2.72 V (DIP1 on), CLOSE ~2.27/1.98, OPEN ~1.05/0.99, STOP 0
  btnMv = hal::buttonMv();
  Button b;
  if (btnMv < 450) b = BTN_STOP;
  else if (btnMv < 1500) b = BTN_OPEN;
  else if (btnMv < 2450) b = BTN_CLOSE;
  else b = BTN_NONE;
  if (b == btnPend) { if (btnCnt < 255) btnCnt++; } else { btnPend = b; btnCnt = 1; }
  if (btnCnt >= NEED && b != btn) { btn = b; btnSince = now; }
  if (btn == BTN_NONE && btnCnt >= NEED) dip = btnMv < 3000;
}

bool closed(InputId i) { return deb[i]; }

bool active(InputId i) {
  switch (i) {
    case IN_SAFE: return typed(deb[i], params::geti(P_SAFETY_TYPE));
    case IN_EDGE: {
      int t = params::geti(P_EDGE_TYPE);
      if (t == CONTACT_8K2) return edgeMvV >= 0 ? edgeAct : true;   // analog not ready yet: safe side
      return typed(deb[i], t);
    }
    case IN_EXIT: return typed(deb[i], params::geti(P_EXIT_TYPE));
    case IN_RADIO: return typed(deb[i], params::geti(P_RADIO_TYPE));
    default: return deb[i];
  }
}

uint32_t sinceMs(InputId i) { return millis() - since[i]; }
Button button() { return btn; }
uint32_t buttonHeldMs() { return millis() - btnSince; }
uint16_t buttonMv() { return btnMv; }
bool dip1() { return dip; }
int edgeMv() { return edgeMvV; }
bool edgeFault() { return edgeFlt; }
}  // namespace inputs
