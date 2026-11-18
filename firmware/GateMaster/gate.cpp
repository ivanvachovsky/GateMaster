#include "gate.h"
#include "hal.h"
#include "inputs.h"
#include "params.h"
#include "eventlog.h"
#include <time.h>

#ifndef HOST_TEST
#include <esp_task_wdt.h>
#include <esp_timer.h>
#endif

#define ev eventlog::add

namespace gate {

// ------------------------------------------------------------------ command queue with results
struct Msg { uint8_t cmd, src, slot; uint16_t arg; uint32_t id; };
struct Slot { volatile uint32_t id; volatile uint8_t result; };
static Slot slots[8];
static uint32_t nextId = 1;
static QueueHandle_t cmdQ;

// ------------------------------------------------------------------ state
static portMUX_TYPE snapMux = portMUX_INITIALIZER_UNLOCKED;
static GateStatus snap;
static uint32_t changeSeq = 0;

static GateState state = ST_UNKNOWN;
static bool runOpen = true;              // direction of the current / last run
static Winding pendingWind = WIND_OFF;   // waiting for dead time / lock release
static uint32_t startAt = 0, motorOnAt = 0, motorOffAt = 0, runStartAt = 0;
static Winding lastWind = WIND_OFF;
static uint32_t briefReverseUntil = 0;   // 1 = armed, > 1 = deadline of the brief reverse after an opening obstruction
static bool runFromLimit = false;        // run started at the opposite limit (travel learning)
static bool edgeArmed = true;            // an edge already pressed when the motor started counts only after it clears

static float baseline = 0, thresholdA = 0, runPeak = 0, lastRunPeak = 0;
static uint32_t overSince = 0, lowSince = 0;
static uint8_t entrapCount = 0;
static uint32_t lockoutAt = 0;
static bool lockoutAlarm = false;

static bool autoCloseArmed = false;
static uint32_t autoCloseAt = 0;
static uint32_t holdUntil = 0;           // "Open for N minutes" ends here (0 = none)
static uint8_t faultStreak = 0;          // runs in a row that ended in a fault: auto-close stops retrying at 2

static float pos = -1;                   // estimated position %, -1 unknown
static float partialTarget = -1;
static uint32_t midSeenMs = 0;
static uint32_t lastPosTick = 0;

static bool forcedOpen = false;
static uint32_t relayPulseUntil = 0, solenoidUntil = 0;
static bool fInhibit = false, fInhibitLatched = false, fTimeout = false, fLimits = false;
static uint8_t inhibitStreak = 0;
static uint32_t limEqualSince = 0;

static bool prevActive[IN_COUNT];
static uint32_t releasedAt[IN_COUNT];    // re-trigger guard for command inputs
static Button prevBtn = BTN_NONE;
static uint32_t btnReleasedAt = 0;
static Button btnLastReleased = BTN_NONE;   // the guard only holds back a re-press of the same button
static bool stopLongDone = false;
static volatile uint32_t lastTickMs = 0;
static volatile bool updating = false;

// motor current graph
static portMUX_TYPE graphMux = portMUX_INITIALIZER_UNLOCKED;
static Graph gCur, gLast;
static uint32_t graphNextAt = 0, runIdCounter = 0;

static Learned& L() { return learned::get(); }

// ------------------------------------------------------------------ names
const char* stateName(GateState s) {
  switch (s) {
    case ST_UNKNOWN: return "unknown"; case ST_CLOSED: return "closed"; case ST_OPENING: return "opening";
    case ST_OPEN: return "open"; case ST_CLOSING: return "closing"; case ST_STOPPED: return "stopped";
    case ST_PARTIAL: return "partial"; case ST_LOCKOUT: return "lockout";
  }
  return "?";
}
static const char* const CMD_NAMES[CMD_COUNT] = {
  "none", "open", "close", "stop", "toggle", "reset", "partial",
  "hold_on", "hold_off", "autoclose_on", "autoclose_off", "lockout_on", "lockout_off", "relearn"
};
const char* cmdName(Cmd c) { return c < CMD_COUNT ? CMD_NAMES[c] : "?"; }
Cmd cmdFromName(const String& s) {
  for (int i = 1; i < CMD_COUNT; i++) if (s.equalsIgnoreCase(CMD_NAMES[i])) return (Cmd)i;
  return CMD_NONE;
}
Cmd parseCmd(const String& in, uint16_t& arg) {
  String s = in; s.trim();
  arg = 0;
  int sep = s.indexOf(' ');
  if (sep < 0) sep = s.indexOf(':');
  if (sep > 0) { arg = (uint16_t)constrain(s.substring(sep + 1).toInt(), 0L, 1440L); s = s.substring(0, sep); }
  return cmdFromName(s);
}
const char* srcName(CmdSource s) {
  static const char* const N[SRC_COUNT] = {"button", "3BT", "EXIT", "radio", "RESET input", "web", "MQTT", "auto-close"};
  return s < SRC_COUNT ? N[s] : "?";
}
const char* resultText(CmdResult r) {
  switch (r) {
    case RES_OK: return "ok";
    case RES_BUSY: return "busy, try again";
    case RES_LOCKOUT: return "lockout active: press Reset alarm";
    case RES_LOCKOUT_MODE: return "Lock out is on: EXIT and radio are ignored";
    case RES_PHOTO_EYE: return "photo-eye active";
    case RES_EDGE: return "safety edge active";
    case RES_EXIT: return "EXIT input active";
    case RES_HOLD: return "Hold open is on";
    case RES_NOT_LEARNED: return "position not known yet: open and close fully once";
    case RES_PAST_PARTIAL: return "already past the partial-open position";
    case RES_FAULT: return "motor fault: press Reset alarm";
    case RES_MOVING: return "not while the gate is moving";
    case RES_UPDATING: return "firmware update in progress";
    case RES_UNKNOWN_CMD: return "unknown command";
  }
  return "?";
}

// ------------------------------------------------------------------ helpers
static Winding openWinding() {
  bool m2 = inputs::dip1() ^ params::getb(P_SWAP_OPEN);   // DIP1 OFF: MOTOR-1 opens; ON: MOTOR 2 opens
  return m2 ? WIND_M2 : WIND_M1;
}
static Winding closeWinding() { return openWinding() == WIND_M1 ? WIND_M2 : WIND_M1; }

static uint8_t limitType() {
  int t = params::geti(P_LIMIT_TYPE);
  if (t == LIMIT_NO) return 1;
  if (t == LIMIT_NC) return 2;
  return L().limitDetected;
}
// is the limit switch that stops winding w at its limit?
static bool limitFor(Winding w) {
  uint8_t t = limitType();
  if (!t) return false;
  bool l1ForM1 = params::geti(P_LIMIT_PAIR) == 0;
  InputId li = ((w == WIND_M1) == l1ForM1) ? IN_LIM1 : IN_LIM2;
  bool c = inputs::closed(li);
  return t == 1 ? c : !c;
}
static bool openLimit() { return limitFor(openWinding()); }
static bool closeLimit() { return limitFor(closeWinding()); }
static bool safetyActive() { return inputs::active(IN_SAFE); }
static bool edgeActive() { return inputs::active(IN_EDGE); }
static bool exitActive() { return inputs::active(IN_EXIT); }
static bool moving() { return hal::driving() != WIND_OFF || pendingWind != WIND_OFF; }
static bool closingNow() { return moving() && !runOpen && !briefReverseUntil; }

static uint32_t maxRunMs() {
  uint32_t cap = (uint32_t)params::geti(P_MAX_RUN) * 1000;
  uint32_t l = runOpen ? L().openMs : L().closeMs;
  if (l) return min(cap, (uint32_t)(l * 1.5f) + 2000);
  return cap;
}
static float midPct() {
  if (!L().midMs || !L().openMs || L().midMs >= L().openMs) return -1;
  return 100.0f * L().midMs / L().openMs;
}
static uint32_t epochNow() { time_t t = time(nullptr); return t > 1700000000 ? (uint32_t)t : 0; }
static void changed() { changeSeq++; }

static bool holdActive() { return holdUntil != 0; }
static void restartCountdown(uint32_t now) {
  autoCloseArmed = true;
  autoCloseAt = now + (uint32_t)params::geti(P_CLOSE_DELAY) * 1000;
}
static void armAutoClose(uint32_t now) { restartCountdown(now); }   // the tick drops it when not allowed

// ------------------------------------------------------------------ motor
static void motorOff() {
  if (hal::driving() != WIND_OFF) { lastWind = hal::driving(); motorOffAt = millis(); }
  hal::drive(WIND_OFF);
  pendingWind = WIND_OFF;
  briefReverseUntil = 0;
}

static void startRun(bool open) {
  uint32_t now = millis();
  Winding w = open ? openWinding() : closeWinding();
  if (hal::driving() != WIND_OFF && hal::driving() != w) motorOff();
  if (hal::driving() == w) { runOpen = open; state = open ? ST_OPENING : ST_CLOSING; changed(); return; }
  uint32_t at = now;
  if (lastWind != WIND_OFF && lastWind != w) at = max(at, motorOffAt + DIRECTION_DEAD_MS);
  int lockType = params::geti(P_LOCK_TYPE);
  if (lockType == LOCK_MAG && hal::lockOn()) { hal::lock(false); at = max(at, now + (uint32_t)(params::get(P_MAG_DELAY) * 1000)); }
  if (lockType == LOCK_SOLENOID && open) { hal::lock(true); solenoidUntil = now + (uint32_t)(params::get(P_SOL_PULSE) * 1000); }
  if (open && params::geti(P_RELAY_MODE) == RELAY_PULSE_ON_OPEN) relayPulseUntil = now + (uint32_t)(params::get(P_RELAY_PULSE) * 1000);
  pendingWind = w; startAt = at;
  runOpen = open;
  state = open ? ST_OPENING : ST_CLOSING;
  runFromLimit = open ? closeLimit() : openLimit();
  runStartAt = 0;
  autoCloseArmed = false;
  fInhibit = false; fTimeout = false;
  forcedOpen = false;
  midSeenMs = 0;
  if (!(open ? L().openMs : L().closeMs)) pos = -1;      // no learned travel time this way: can't estimate
  changed();
}

static void stopAt(GateState s, const char* why) {
  motorOff();
  state = s;
  partialTarget = -1;
  if (why) ev("stopped: %s", why);
  changed();
}

static void arrived(bool atOpen) {
  uint32_t now = millis();
  uint32_t dur = runStartAt ? now - runStartAt : 0;
  motorOff();
  entrapCount = 0;
  inhibitStreak = 0;
  faultStreak = 0;
  partialTarget = -1;
  if (runFromLimit && dur > 2000 && dur < 200000) {
    uint32_t& l = atOpen ? L().openMs : L().closeMs;
    if (!l || abs((int)dur - (int)l) > (int)l / 10) {
      l = dur; learned::markDirty();
      ev("learned %s travel %.1f s", atOpen ? "open" : "close", dur / 1000.0f);
    }
    if (atOpen && midSeenMs && (!L().midMs || abs((int)midSeenMs - (int)L().midMs) > 300)) {
      L().midMs = midSeenMs; learned::markDirty();
      ev("learned MID position %.0f %%", midPct());
    }
  }
  pos = atOpen ? 100 : 0;
  if (atOpen) {
    state = ST_OPEN;
    L().cycles++; learned::markDirty();
    armAutoClose(now);
    ev("open");
  } else {
    state = ST_CLOSED;
    ev("closed");
  }
  learned::saveIfDirty();        // the motor is off here
  changed();
}

static void partialArrived() {
  motorOff();
  partialTarget = -1;
  faultStreak = 0;
  state = ST_PARTIAL;
  armAutoClose(millis());
  ev("partly open (%.0f %%)", pos);
  changed();
}

static void lockout() {
  motorOff();
  state = ST_LOCKOUT;
  partialTarget = -1;
  lockoutAt = millis();
  lockoutAlarm = true;
  ev("LOCKOUT: second obstruction before a limit - press Reset alarm");
  changed();
}

// UL 325: closing -> stop and reverse to fully open; opening -> stop and brief reverse, then stop.
// countsForLockout: motor-current obstructions only. The J3 edge is a closing-side edge: it reverses the
// gate but never counts toward the lockout (so touching it cannot lock the gate open).
static void obstruction(const char* why, bool countsForLockout) {
  if (countsForLockout) entrapCount++;
  L().obstructions++;
  L().lastObstruction = epochNow();
  learned::markDirty();
  ev("obstruction (%s) while %s", why, runOpen ? "opening" : "closing");
  if (entrapCount >= 2) { lockout(); return; }
  bool wasOpening = runOpen;
  motorOff();
  partialTarget = -1;
  if (!wasOpening) startRun(true);
  else { startRun(false); briefReverseUntil = 1; }   // real deadline set when the motor actually starts
}

// ------------------------------------------------------------------ commands
static CmdResult doCommand(Cmd c, CmdSource src, uint16_t arg = 0) {
  uint32_t now = millis();
  switch (c) {
    case CMD_RESET:
      if (state == ST_LOCKOUT) {
        entrapCount = 0;
        state = openLimit() && !closeLimit() ? ST_OPEN : closeLimit() && !openLimit() ? ST_CLOSED : ST_STOPPED;
        ev("alarm reset (%s)", srcName(src));
      } else if (fInhibit || fInhibitLatched || fTimeout || forcedOpen) ev("faults reset (%s)", srcName(src));
      lockoutAlarm = false;
      fInhibit = fInhibitLatched = fTimeout = false;
      inhibitStreak = 0;
      faultStreak = 0;
      forcedOpen = false;
      hal::tripReset();
      changed();
      return RES_OK;
    case CMD_AC_ON: case CMD_AC_OFF: {
      bool on = c == CMD_AC_ON;
      params::setValue(P_AUTO_CLOSE, on ? 1 : 0);
      if (!on) autoCloseArmed = false;
      ev("auto-close %s (%s)", on ? "on" : "off", srcName(src));
      changed();
      return RES_OK;
    }
    case CMD_LOCKOUT_ON: case CMD_LOCKOUT_OFF:
      L().lockOutMode = c == CMD_LOCKOUT_ON; learned::markDirty();
      ev("Lock out %s (%s)", L().lockOutMode ? "on" : "off", srcName(src));
      changed();
      return RES_OK;
    case CMD_RELEARN:
      if (moving()) return RES_MOVING;
      L().openMs = L().closeMs = L().midMs = 0;
      if (params::geti(P_LIMIT_TYPE) == LIMIT_AUTO) L().limitDetected = 0;
      learned::markDirty();
      pos = -1;
      ev("learned values cleared (%s): run a full open and close", srcName(src));
      changed();
      return RES_OK;
    case CMD_HOLD_OFF:
      if (holdActive()) {
        holdUntil = 0; L().holdMin = 0; learned::markDirty();
        ev("Open for N minutes cancelled (%s): normal close delay", srcName(src));
        restartCountdown(now);
        changed();
      }
      return RES_OK;
    default: break;
  }

  // STOP always works
  if (c == CMD_STOP) {
    if (moving()) { stopAt(ST_STOPPED, srcName(src)); return RES_OK; }
    if (autoCloseArmed) {                          // STOP never cancels the auto-close, it restarts it
      restartCountdown(now);
      ev("auto-close countdown restarted (%s)", srcName(src));
      changed();
    }
    return RES_OK;
  }

  if (state == ST_LOCKOUT) return RES_LOCKOUT;
  if (fInhibitLatched) return RES_FAULT;
  if (updating) return RES_UPDATING;

  // Lock out mode: open commands from EXIT and radio are ignored. EXIT still reverses a closing gate
  // (a vehicle on the exit loop must never be closed on).
  bool exitReopen = src == SRC_EXIT && closingNow();
  if (L().lockOutMode && (src == SRC_EXIT || src == SRC_RADIO) && !exitReopen) return RES_LOCKOUT_MODE;

  if (c == CMD_HOLD_ON) {
    uint16_t min = arg ? arg : (uint16_t)params::geti(P_HOLD_MIN);
    holdUntil = now + (uint32_t)min * 60000UL;
    if (!holdUntil) holdUntil = 1;
    L().holdMin = min; learned::markDirty();
    ev("open for %u min (%s)", (unsigned)min, srcName(src));
    autoCloseArmed = false;
    changed();
    if (state == ST_OPEN || (state == ST_OPENING && partialTarget < 0 && !briefReverseUntil)) return RES_OK;
    c = CMD_OPEN;                                  // opens if needed
  }

  if (c == CMD_TOGGLE) {   // 3BT / radio open-stop-close: open -> stop -> close -> stop
    if (moving()) return doCommand(CMD_STOP, src);
    if (state == ST_OPEN || state == ST_PARTIAL) c = CMD_CLOSE;
    else if (state == ST_STOPPED) c = runOpen ? CMD_CLOSE : CMD_OPEN;
    else c = CMD_OPEN;
  }


  switch (c) {
    case CMD_OPEN:
      partialTarget = -1;
      if (state == ST_OPEN) {
        if (!autoCloseArmed || autoCloseAt - now < (uint32_t)params::geti(P_CLOSE_DELAY) * 1000) armAutoClose(now);
        changed();
        return RES_OK;
      }
      if (state == ST_OPENING && !briefReverseUntil) { ev("open: completing (%s)", srcName(src)); changed(); return RES_OK; }
      if (openLimit()) { arrived(true); return RES_OK; }
      ev("open (%s)", srcName(src));
      startRun(true);
      return RES_OK;

    case CMD_CLOSE:
      if (holdActive()) return RES_HOLD;
      if (safetyActive()) return RES_PHOTO_EYE;
      if (edgeActive()) return RES_EDGE;
      if (exitActive()) return RES_EXIT;
      partialTarget = -1;
      if (state == ST_CLOSED) return RES_OK;
      if (state == ST_CLOSING && !briefReverseUntil) return RES_OK;
      if (closeLimit()) { arrived(false); return RES_OK; }
      ev("close (%s)", srcName(src));
      startRun(false);
      return RES_OK;

    case CMD_PARTIAL: {
      float target = params::get(P_PARTIAL);
      if (!L().openMs || pos < 0) return RES_NOT_LEARNED;
      if (pos >= target - 5) {
        if (state == ST_PARTIAL) { armAutoClose(now); changed(); return RES_OK; }
        if (moving() && runOpen) return RES_PAST_PARTIAL;
        if (pos <= target + 5 && !moving()) { state = ST_PARTIAL; armAutoClose(now); changed(); return RES_OK; }
        return RES_PAST_PARTIAL;
      }
      ev("partial open to %.0f %% (%s)", target, srcName(src));
      if (!(moving() && runOpen && !briefReverseUntil)) startRun(true);
      partialTarget = target;
      return RES_OK;
    }
    default:
      return RES_UNKNOWN_CMD;
  }
}

static CmdResult runCmd(Cmd c, CmdSource src, uint16_t arg = 0) {
  CmdResult r = doCommand(c, src, arg);
  if (r != RES_OK) ev("%s from %s refused: %s", cmdName(c), srcName(src), resultText(r));
  return r;
}

CmdResult command(Cmd c, CmdSource src, uint32_t waitMs, uint16_t arg) {
  if (c == CMD_NONE || c >= CMD_COUNT) return RES_UNKNOWN_CMD;
  Msg m;
  m.cmd = c; m.src = src; m.arg = arg;
  portENTER_CRITICAL(&snapMux);
  m.id = nextId++;
  m.slot = m.id % 8;
  portEXIT_CRITICAL(&snapMux);
  slots[m.slot].result = 0xff;
  slots[m.slot].id = m.id;
  if (xQueueSend(cmdQ, &m, 0) != pdTRUE) return RES_BUSY;
  if (!waitMs) return RES_OK;
  uint32_t t0 = millis();
  while (millis() - t0 < waitMs) {
    if (slots[m.slot].id == m.id && slots[m.slot].result != 0xff) return (CmdResult)slots[m.slot].result;
    delay(2);
  }
  return RES_BUSY;
}

static void reply(const Msg& m, CmdResult r) {
  if (slots[m.slot].id == m.id) slots[m.slot].result = r;
}

// ------------------------------------------------------------------ inputs -> commands
static void handleInputs(bool stopsOnly) {
  if (stopsOnly) {
    Button b = inputs::button();
    if (b == BTN_STOP && prevBtn != BTN_STOP) { runCmd(CMD_STOP, SRC_BUTTON); stopLongDone = false; }
    if (b == BTN_STOP && !stopLongDone && inputs::buttonHeldMs() > STOP_RESET_HOLD_MS) { runCmd(CMD_RESET, SRC_BUTTON); stopLongDone = true; }
    return;
  }
  uint32_t now = millis();
  for (int i = 0; i < IN_COUNT; i++) {
    bool a = inputs::active((InputId)i);
    if (a == prevActive[i]) continue;
    prevActive[i] = a;
    if (i != IN_LIM1 && i != IN_LIM2 && i != IN_MID) ev("input %s %s", INPUT_NAMES[i], a ? "active" : "clear");
    changed();
    if (!a) { releasedAt[i] = now; continue; }
    // re-trigger guard: a command input must have been released for 250 ms before a new press counts
    // (a chattering relay or worn button must not give "open" then "stop")
    bool cmdInput = i == IN_EXIT || i == IN_RADIO || i == IN_RESET || i == IN_3BT;
    if (cmdInput && now - releasedAt[i] < RETRIGGER_MS) { ev("input %s: press ignored (released only %lu ms)", INPUT_NAMES[i], (unsigned long)(now - releasedAt[i])); continue; }
    if (i == IN_EDGE && hal::driving() != WIND_OFF && runOpen && !briefReverseUntil) ev("edge touched while opening (ignored: closing-side edge)");
    switch (i) {
      case IN_EXIT:  runCmd(CMD_OPEN, SRC_EXIT); break;
      case IN_RADIO: runCmd(params::geti(P_RADIO_ACTION) == 1 ? CMD_TOGGLE : CMD_OPEN, SRC_RADIO); break;
      case IN_RESET: runCmd(CMD_RESET, SRC_RESET_IN); break;        // never moves the gate
      case IN_3BT:   runCmd(CMD_TOGGLE, SRC_3BT); break;
      case IN_MID:
        if (hal::driving() != WIND_OFF) {
          if (runOpen && runFromLimit && runStartAt && !midSeenMs) midSeenMs = millis() - runStartAt;
          float m = midPct();
          if (m >= 0) pos = m;                                      // position reference
        }
        break;
      default: break;                                               // PED-SW, PHANTOM, SPARE: report only
    }
  }
  Button b = inputs::button();
  if (b != prevBtn) {
    bool fresh = b != btnLastReleased || now - btnReleasedAt >= RETRIGGER_MS;   // STOP is never held back (handled above)
    if (fresh && b == BTN_OPEN) runCmd(CMD_OPEN, SRC_BUTTON);
    if (fresh && b == BTN_CLOSE) runCmd(CMD_CLOSE, SRC_BUTTON);
    if (b == BTN_NONE) { btnReleasedAt = now; btnLastReleased = prevBtn; }
    prevBtn = b;
    changed();
  }
}

// ------------------------------------------------------------------ limit type auto-detect
static void detectLimitType() {
  if (params::geti(P_LIMIT_TYPE) != LIMIT_AUTO) return;
  bool c1 = inputs::closed(IN_LIM1), c2 = inputs::closed(IN_LIM2);
  bool running = hal::driving() != WIND_OFF && millis() - motorOnAt > 2000;
  if (c1 == c2 && running) {
    if (!limEqualSince) limEqualSince = millis();
    if (millis() - limEqualSince > 1500) {
      uint8_t t = c1 ? 2 : 1;            // mid-travel: both closed = NC switches, both open = NO
      if (t != L().limitDetected) {
        L().limitDetected = t; learned::markDirty();
        ev("limit switches detected as %s", t == 1 ? "NO" : "NC");
      }
    }
  } else limEqualSince = 0;
}

// ------------------------------------------------------------------ graph
static void graphStartRun() {
  portENTER_CRITICAL(&graphMux);
  if (gCur.len) { memcpy(&gLast, &gCur, sizeof(Graph)); }
  gCur.len = 0;
  gCur.runId = ++runIdCounter;
  portEXIT_CRITICAL(&graphMux);
}
static void graphSample(float a, float thr) {
  portENTER_CRITICAL(&graphMux);
  if (gCur.len < GRAPH_LEN) {
    gCur.amps[gCur.len] = (uint8_t)min(255.0f, a * 10 + 0.5f);
    gCur.thr[gCur.len] = (uint8_t)min(255.0f, thr * 10 + 0.5f);
    gCur.len++;
  }
  portEXIT_CRITICAL(&graphMux);
}
void graph(bool last, Graph& out, uint16_t from) {
  portENTER_CRITICAL(&graphMux);
  const Graph& g = last ? gLast : gCur;
  out.runId = g.runId;
  out.len = g.len;
  if (from < g.len) {
    memcpy(out.amps + from, g.amps + from, g.len - from);
    memcpy(out.thr + from, g.thr + from, g.len - from);
  }
  portEXIT_CRITICAL(&graphMux);
}

// ------------------------------------------------------------------ motion (safety first)
static void motion(uint32_t now) {
  Winding w = hal::driving();
  if (w == WIND_OFF) return;
  float a = hal::currentA();
  uint32_t on = now - motorOnAt;
  if (a > runPeak) runPeak = a;

  // 1. hardware trip / no current
  if (on > INRUSH_IGNORE_MS) {
    if (a < 0.4f) { if (!lowSince) lowSince = now; } else lowSince = 0;
    if (lowSince && now - lowSince > 300 && on > NO_CURRENT_MS && !limitFor(w)) {
      fInhibit = true;
      inhibitStreak++;
      faultStreak++;
      stopAt(ST_STOPPED, "no motor current (hardware trip, fuse or limit cut-off)");
      hal::tripReset();
      if (inhibitStreak >= 2) { fInhibitLatched = true; ev("motor fault again: press Reset alarm"); }
      return;
    }
  }

  // during the short reverse after an opening obstruction (closing direction), any safety input just stops
  if (briefReverseUntil > 1 && (edgeActive() || safetyActive())) {
    stopAt(ST_STOPPED, edgeActive() ? "safety edge during the short reverse" : "photo-eye during the short reverse");
    return;
  }

  // 2. obstruction: safety edge or motor current. An edge still squeezed from the obstruction that
  // just reversed the gate is not a new obstruction: it counts again once it has cleared.
  if (!edgeActive()) edgeArmed = true;
  if (!runOpen && edgeActive() && edgeArmed && !briefReverseUntil) {   // closing-side edge: acts only while closing
    obstruction(inputs::edgeFault() ? "edge wiring fault" : "safety edge", false);
    return;
  }
  if (on > INRUSH_IGNORE_MS) {
    if (baseline == 0) baseline = a; else if (!overSince) baseline += (a - baseline) * 0.01f;
    float factor = 1.0f + (11 - params::geti(P_SENS)) * 0.07f;   // sens 5 -> +42 %, 10 -> +7 %
    float absMax = params::get(P_ABS_MAX);
    thresholdA = baseline > 1.0f ? min(absMax, baseline * factor + 0.3f) : absMax;
    bool over = a > thresholdA;
    if (over) { if (!overSince) overSince = now; } else overSince = 0;
    uint32_t persist = 150 + (10 - params::geti(P_SENS)) * 50;   // 150..600 ms
    if (overSince && now - overSince > persist && !briefReverseUntil) { obstruction("motor current", true); return; }
  } else thresholdA = params::get(P_ABS_MAX);

  // 3. photo-eye while closing: reverse to fully open
  if (!runOpen && safetyActive() && !briefReverseUntil) {
    ev("photo-eye while closing: reopening");
    motorOff();
    partialTarget = -1;
    startRun(true);
  }
}

static void limitsAndTravel(uint32_t now) {
  // pending start (dead time / lock release)
  if (pendingWind != WIND_OFF && (int32_t)(now - startAt) >= 0) {
    if (pendingWind == closeWinding() && (safetyActive() || edgeActive())) {
      ev("%s blocked (%s)", briefReverseUntil ? "short reverse" : "close start", safetyActive() ? "photo-eye" : "safety edge");
      pendingWind = WIND_OFF; briefReverseUntil = 0; state = ST_STOPPED; changed();
    } else {
      edgeArmed = !edgeActive();
      hal::drive(pendingWind);
      lastWind = pendingWind;
      pendingWind = WIND_OFF;
      motorOnAt = now; runStartAt = now; overSince = lowSince = 0;
      baseline = 0; runPeak = 0;
      graphStartRun(); graphNextAt = now;
      lastPosTick = now;
      if (briefReverseUntil == 1) briefReverseUntil = now + (uint32_t)(params::get(P_OPEN_REV) * 1000);
      changed();
    }
  }

  Winding w = hal::driving();
  if (w != WIND_OFF) {
    // position estimate from learned travel time
    uint32_t dt = now - lastPosTick; lastPosTick = now;
    bool goingOpen = w == openWinding();
    uint32_t l = goingOpen ? L().openMs : L().closeMs;
    if (pos >= 0 && l) pos = constrain(pos + (goingOpen ? 100.0f : -100.0f) * dt / l, 0.0f, 100.0f);

    if (limitFor(w)) { arrived(w == openWinding()); return; }
    if (briefReverseUntil > 1 && (int32_t)(now - briefReverseUntil) >= 0) { stopAt(ST_STOPPED, "brief reverse done"); return; }
    if (now - motorOnAt > maxRunMs()) { fTimeout = true; faultStreak++; stopAt(ST_STOPPED, "max run time"); return; }
    if (partialTarget >= 0 && runOpen && pos >= partialTarget) { partialArrived(); return; }

    if ((int32_t)(now - graphNextAt) >= 0) { graphNextAt += GRAPH_PERIOD_MS; graphSample(hal::currentA(), thresholdA); }
  } else if (pendingWind == WIND_OFF) {
    // stationary
    if (state == ST_OPENING || state == ST_CLOSING) { state = ST_STOPPED; changed(); }
    if (state == ST_STOPPED || state == ST_UNKNOWN) {
      if (openLimit() && !closeLimit()) { state = ST_OPEN; pos = 100; changed(); }
      else if (closeLimit() && !openLimit()) { state = ST_CLOSED; pos = 0; changed(); }
    }
    if (state == ST_CLOSED && !closeLimit() && limitType()) {
      if (!forcedOpen) { forcedOpen = true; ev("FORCED OPEN: gate moved off the closed limit"); }
      state = ST_STOPPED; pos = -1;
      changed();
    }
  }
}

// ------------------------------------------------------------------ outputs
static void outputs(uint32_t now) {
  // lockout alarm: at least 5 min or until reset (UL 325)
  if (lockoutAlarm && now - lockoutAt > ALARM_MAX_MS) lockoutAlarm = false;
  bool alarm = lockoutAlarm || (forcedOpen && params::getb(P_FORCED_BUZZER));
  if (alarm != hal::alarmOn()) { hal::alarm(alarm); changed(); }

  int lt = params::geti(P_LOCK_TYPE);
  bool lk = hal::lockOn();
  if (lt == LOCK_MAG) lk = state == ST_CLOSED && !moving();
  else if (lt == LOCK_SOLENOID) { if (lk && (int32_t)(now - solenoidUntil) >= 0) lk = false; }
  else lk = false;
  if (lk != hal::lockOn()) { hal::lock(lk); changed(); }

  bool r = false;
  switch (params::geti(P_RELAY_MODE)) {
    case RELAY_PULSE_ON_OPEN: r = (int32_t)(relayPulseUntil - now) > 0; break;
    case RELAY_WHILE_MOVING:  r = moving(); break;
    case RELAY_FORCED_OPEN:   r = forcedOpen; break;
    case RELAY_NOT_CLOSED:    r = state != ST_CLOSED; break;
  }
  if (r != hal::relayOn()) { hal::relay(r); changed(); }

  // status LED / heartbeat: always a 1 kHz square wave, only the duty changes
  bool fault = state == ST_LOCKOUT || fInhibit || fInhibitLatched || fLimits;
  uint32_t period = fault ? 125 : (hal::driving() != WIND_OFF ? 250 : 1000);
  hal::heartbeat(((now / (period / 2)) & 1) ? 140 : 15);
}

// ------------------------------------------------------------------ main tick
static void publishSnapshot(uint32_t now) {
  GateStatus s;
  s.state = state; s.winding = hal::driving(); s.movingOpen = runOpen;
  s.posPct = pos;
  s.autoCloseLeftS = autoCloseArmed ? (int16_t)(((int32_t)(autoCloseAt - now) + 999) / 1000) : -1;
  s.holdOpen = holdActive();
  s.holdLeftMin = holdActive() ? (int16_t)(((int32_t)(holdUntil - now) + 59999) / 60000) : -1;
  s.autoClose = params::getb(P_AUTO_CLOSE); s.lockOutMode = L().lockOutMode;
  s.currentA = hal::currentA(); s.peakA = hal::driving() != WIND_OFF ? runPeak : lastRunPeak; s.baselineA = baseline; s.thresholdA = thresholdA;
  s.obstructionsThisRun = entrapCount;
  for (int i = 0; i < IN_COUNT; i++) { s.inClosed[i] = inputs::closed((InputId)i); s.inActive[i] = inputs::active((InputId)i); }
  s.button = inputs::button();
  s.dip1 = inputs::dip1(); s.limitType = limitType();
  s.openLimit = openLimit(); s.closeLimit = closeLimit();
  s.lockOut = hal::lockOn(); s.relayOut = hal::relayOn(); s.alarmOut = hal::alarmOn();
  s.fInhibit = fInhibit; s.fInhibitLatched = fInhibitLatched; s.fTimeout = fTimeout; s.fLimits = fLimits;
  s.forcedOpen = forcedOpen; s.edgeFault = inputs::edgeFault();
  s.edgeMv = inputs::edgeMv();
  s.cycles = L().cycles; s.obstructions = L().obstructions; s.lastObstruction = L().lastObstruction;
  s.learnOpenMs = L().openMs; s.learnCloseMs = L().closeMs; s.midPct = midPct();
  s.uptimeS = now / 1000;
  s.changeSeq = changeSeq;
  portENTER_CRITICAL(&snapMux); snap = s; portEXIT_CRITICAL(&snapMux);
}

static void tick() {
  uint32_t now = millis();
  lastTickMs = now;
  hal::tick(CONTROL_PERIOD_MS);
  inputs::poll();
  detectLimitType();
  bool wasDriving = hal::driving() != WIND_OFF;

  Msg msgs[16]; int n = 0;
  while (n < 16 && xQueueReceive(cmdQ, &msgs[n], 0) == pdTRUE) n++;

  // priority: hardware trip, edge / current obstruction, photo-eye  >  STOP  >  limits  >  other commands
  motion(now);
  for (int i = 0; i < n; i++) if (msgs[i].cmd == CMD_STOP) reply(msgs[i], runCmd(CMD_STOP, (CmdSource)msgs[i].src));
  handleInputs(true);
  limitsAndTravel(now);
  handleInputs(false);
  for (int i = 0; i < n; i++) if (msgs[i].cmd != CMD_STOP) reply(msgs[i], runCmd((Cmd)msgs[i].cmd, (CmdSource)msgs[i].src, msgs[i].arg));

  fLimits = limitType() && openLimit() && closeLimit();

  // "Open for N minutes": ends by itself; the remaining minutes are kept for a power cut
  if (holdActive()) {
    int32_t left = (int32_t)(holdUntil - now);
    if (left <= 0) {
      holdUntil = 0; L().holdMin = 0; learned::markDirty();
      ev("open for N minutes ended: normal close delay");
      changed();
    } else {
      uint16_t m = (uint16_t)((left + 59999) / 60000);
      if (m != L().holdMin) { L().holdMin = m; learned::markDirty(); changed(); }
    }
  }

  // Auto-close rule (spec 6): nothing leaves the gate open, fully or part-way, longer than the close
  // delay. Exceptions: Open for N minutes, EXIT held, auto-close switched off. Safety holds: photo-eye /
  // edge active (countdown waits), lockout, latched motor fault, firmware update. A gate whose position
  // is unknown only because the limit type is not learned yet (fresh board) waits for a command.
  bool notClosed = state == ST_OPEN || state == ST_PARTIAL || state == ST_STOPPED || (state == ST_UNKNOWN && limitType());
  bool mayCount = params::getb(P_AUTO_CLOSE) && notClosed && !moving() && !holdActive() &&
                  !fInhibitLatched && !updating && faultStreak < 2;
  if (!mayCount) { if (autoCloseArmed) { autoCloseArmed = false; changed(); } }
  else {
    if (!autoCloseArmed) { restartCountdown(now); changed(); }
    if (exitActive() || safetyActive() || edgeActive()) autoCloseAt = now + (uint32_t)params::geti(P_CLOSE_DELAY) * 1000;
    else if ((int32_t)(now - autoCloseAt) >= 0) {
      autoCloseArmed = false;
      ev("auto-close (%s)", stateName(state));
      runCmd(CMD_CLOSE, SRC_AUTO);
    }
  }

  if (wasDriving && hal::driving() == WIND_OFF) lastRunPeak = runPeak;
  if (!moving()) learned::saveIfDirty();   // counters, Hold open, Lock out (flash: only with the motor off)

  outputs(now);
  publishSnapshot(now);
}

GateStatus snapshot() {
  GateStatus s;
  portENTER_CRITICAL(&snapMux); s = snap; portEXIT_CRITICAL(&snapMux);
  return s;
}

void setUpdating(bool on) { updating = on; }

bool idle() {
  GateStatus s = snapshot();
  return s.winding == WIND_OFF && s.state != ST_OPENING && s.state != ST_CLOSING;
}

#ifdef HOST_TEST
void tickOnce() { tick(); }
CmdResult commandNow(Cmd c, CmdSource src, uint16_t arg) { return runCmd(c, src, arg); }
void powerCutForTest() {   // everything in RAM is lost; flash (Preferences) and the gate's position stay
  hal::drive(WIND_OFF); hal::lock(false); hal::relay(false); hal::alarm(false);
  state = ST_UNKNOWN; runOpen = true; pendingWind = WIND_OFF; startAt = motorOnAt = motorOffAt = runStartAt = 0;
  lastWind = WIND_OFF; briefReverseUntil = 0; runFromLimit = false; edgeArmed = true;
  baseline = thresholdA = runPeak = lastRunPeak = 0; overSince = lowSince = 0; entrapCount = 0;
  lockoutAt = 0; lockoutAlarm = false; autoCloseArmed = false; autoCloseAt = 0; holdUntil = 0; faultStreak = 0;
  pos = -1; partialTarget = -1; midSeenMs = 0; forcedOpen = false; relayPulseUntil = solenoidUntil = 0;
  fInhibit = fInhibitLatched = fTimeout = fLimits = false; inhibitStreak = 0; limEqualSince = 0;
  learned::reloadForTest();
  params::load();
}
#else
static void taskFn(void*) {
  esp_task_wdt_add(nullptr);
  TickType_t last = xTaskGetTickCount();
  for (;;) {
    tick();
    esp_task_wdt_reset();
    vTaskDelayUntil(&last, pdMS_TO_TICKS(CONTROL_PERIOD_MS));
  }
}

// Motor guard: if the control task stops ticking for 0.5 s, drop the motor outputs, stop the heartbeat
// (the board's hardware watchdog then cuts the motor too) and restart. Runs in the esp_timer task.
static void guardCb(void*) {
  if (millis() - lastTickMs < 500) return;
  hal::drive(WIND_OFF);
  ledcDetachPin(PIN_STAT);
  pinMode(PIN_STAT, OUTPUT);
  digitalWrite(PIN_STAT, LOW);
  esp_restart();
}
#endif

void begin() {
  cmdQ = xQueueCreate(16, sizeof(Msg));
  Learned& l = L();
  // settle the inputs, let the heartbeat bring the hardware OK up, then clear any latched trip
  for (int i = 0; i < 10; i++) { hal::tick(CONTROL_PERIOD_MS); inputs::poll(); delay(CONTROL_PERIOD_MS); }
  delay(300);
  hal::tripReset();
  for (int i = 0; i < IN_COUNT; i++) { prevActive[i] = inputs::active((InputId)i); releasedAt[i] = millis() - 1000; }
  prevBtn = inputs::button();
  btnReleasedAt = millis() - 1000;
  stopLongDone = true;                    // a STOP held since power-up is not a Reset
  uint32_t now = millis();
  if (l.holdMin) { holdUntil = now + (uint32_t)l.holdMin * 60000UL; if (!holdUntil) holdUntil = 1; }
  if (openLimit() && !closeLimit()) { state = ST_OPEN; pos = 100; }
  else if (closeLimit() && !openLimit()) { state = ST_CLOSED; pos = 0; }
  else { state = ST_UNKNOWN; pos = -1; }
  ev("start: gate %s, DIP1 %s, limits %s%s%s", stateName(state), inputs::dip1() ? "ON" : "OFF",
     limitType() == 1 ? "NO" : limitType() == 2 ? "NC" : "not yet known",
     l.holdMin ? ", Open for N minutes running" : "", l.lockOutMode ? ", Lock out on" : "");
  publishSnapshot(now);
  lastTickMs = millis();
#ifndef HOST_TEST
  // the core's task watchdog resets the ESP32 if this task stalls; the motor guard acts within 0.5 s
  xTaskCreatePinnedToCore(taskFn, "gate", 6144, nullptr, 10, nullptr, 1);
  esp_timer_create_args_t a = {};
  a.callback = guardCb; a.name = "motorguard";
  esp_timer_handle_t t;
  esp_timer_create(&a, &t);
  esp_timer_start_periodic(t, 100000);
#endif
}
}  // namespace gate
