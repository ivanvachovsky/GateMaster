// Gate logic: the state machine that runs the motor. Runs alone in the control task (100 Hz, core 1).
// Nothing on the network side sits between a wired input and the motor. Other tasks talk to it only
// through gate::command() (a queue) and gate::snapshot() (a copy of the status).
#pragma once
#include "config.h"

enum GateState : uint8_t { ST_UNKNOWN, ST_CLOSED, ST_OPENING, ST_OPEN, ST_CLOSING, ST_STOPPED, ST_PARTIAL, ST_LOCKOUT };

enum Cmd : uint8_t {
  CMD_NONE, CMD_OPEN, CMD_CLOSE, CMD_STOP, CMD_TOGGLE, CMD_RESET, CMD_PARTIAL,
  CMD_HOLD_ON, CMD_HOLD_OFF, CMD_AC_ON, CMD_AC_OFF, CMD_LOCKOUT_ON, CMD_LOCKOUT_OFF, CMD_RELEARN,
  CMD_COUNT
};
enum CmdSource : uint8_t { SRC_BUTTON, SRC_3BT, SRC_EXIT, SRC_RADIO, SRC_RESET_IN, SRC_WEB, SRC_MQTT, SRC_AUTO, SRC_COUNT };
enum CmdResult : uint8_t {
  RES_OK, RES_BUSY, RES_LOCKOUT, RES_LOCKOUT_MODE, RES_PHOTO_EYE, RES_EDGE, RES_EXIT, RES_HOLD,
  RES_NOT_LEARNED, RES_PAST_PARTIAL, RES_FAULT, RES_MOVING, RES_UPDATING, RES_UNKNOWN_CMD
};

struct GateStatus {
  GateState state;
  Winding winding;            // what the motor is driven with right now
  bool movingOpen;            // direction of the current / last run
  float posPct;               // estimated position 0..100, -1 = not known
  int16_t autoCloseLeftS;     // -1 = no countdown
  bool holdOpen, autoClose, lockOutMode;   // holdOpen = an "Open for N minutes" is running
  int16_t holdLeftMin;                     // minutes left of it, -1 = none
  float currentA, peakA, baselineA, thresholdA;
  uint8_t obstructionsThisRun;
  bool inClosed[IN_COUNT], inActive[IN_COUNT];
  Button button;
  bool dip1;
  uint8_t limitType;          // 1 NO, 2 NC, 0 not yet known
  bool openLimit, closeLimit;
  bool lockOut, relayOut, alarmOut;
  bool fInhibit, fInhibitLatched, fTimeout, fLimits, forcedOpen, edgeFault;
  int edgeMv;
  uint32_t cycles, obstructions, lastObstruction;
  uint32_t learnOpenMs, learnCloseMs;
  float midPct;               // -1 = not learned
  uint32_t uptimeS;
  uint32_t changeSeq;         // goes up whenever something worth publishing changes
};

namespace gate {
void begin();                                              // starts the control task
// thread-safe, queued, waits for the result. arg: minutes for CMD_HOLD_ON (0 = the default parameter)
CmdResult command(Cmd c, CmdSource src, uint32_t waitMs = 300, uint16_t arg = 0);
Cmd parseCmd(const String& s, uint16_t& arg);              // "hold_on 30", "hold_on:30", "open"...
GateStatus snapshot();
bool idle();                                               // motor off and nothing pending
void setUpdating(bool on);                                 // firmware update: movement refused, STOP and safety still work

const char* stateName(GateState s);
const char* cmdName(Cmd c);
Cmd cmdFromName(const String& s);
const char* srcName(CmdSource s);
const char* resultText(CmdResult r);

// motor current graph (10 samples/s): current run and the run before it, amps x10 and threshold x10
struct Graph { uint32_t runId; uint16_t len; uint8_t amps[GRAPH_LEN]; uint8_t thr[GRAPH_LEN]; };
void graph(bool last, Graph& out, uint16_t from);          // copies samples from index 'from' on

#ifdef HOST_TEST
void tickOnce();
CmdResult commandNow(Cmd c, CmdSource src, uint16_t arg = 0);
void powerCutForTest();
#endif
}
