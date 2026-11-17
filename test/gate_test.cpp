// PC tests: the real gate logic, inputs, parameters and event log, against the built-in gate simulator.
// Build and run: test/run.sh   (any failure stops a release)
#include "Arduino.h"
#include "Preferences.h"
#include "../firmware/GateMaster/gate.h"
#include "../firmware/GateMaster/hal.h"
#include "../firmware/GateMaster/inputs.h"
#include "../firmware/GateMaster/params.h"
#include "../firmware/GateMaster/eventlog.h"

uint32_t g_now = 0;
SerialStub Serial;
void delay(uint32_t ms) { g_now += ms; }

namespace sim { float testPos(); bool testTrip(); void testPowerCut(); }

static int fails = 0, passes = 0;
static void run(double s) { int n = (int)(s * 100 + 0.5); for (int i = 0; i < n; i++) { g_now += 10; gate::tickOnce(); } }
static GateStatus S() { return gate::snapshot(); }
static GateState st() { return S().state; }
static float pos() { return sim::testPos(); }
#define CHECK(cond, msg) do { if (cond) { passes++; printf("  ok   %s\n", msg); } else { fails++; printf("  FAIL %s  (state=%s pos=%.2f motor=%d)\n", msg, gate::stateName(st()), pos(), hal::driving()); } } while (0)
static void pulse(const char* in) { sim::setInput(in, sim::ACT_PULSE); run(0.5); }
static CmdResult cmd(Cmd c, CmdSource s = SRC_WEB) { CmdResult r = gate::commandNow(c, s); run(0.05); return r; }
static void setP(ParamId p, float v) { params::setValue(p, v); }
static void openFully() { if (st() != ST_OPEN) { cmd(CMD_OPEN); run(25); } }
static void closeFully() { if (st() != ST_CLOSED) { cmd(CMD_CLOSE); run(25); } }
static void reboot() { gate::powerCutForTest(); sim::testPowerCut(); gate::begin(); }

int main() {
  params::load();
  setP(P_CLOSE_DELAY, 10);
  hal::begin(HW_SIM);
  inputs::begin();
  gate::begin();

  printf("S1 first run from closed, limit type unknown (NC switches), auto-close\n");
  CHECK(st() == ST_UNKNOWN, "boot: unknown until the limit type is known");
  pulse("exit");
  run(3);
  CHECK(st() == ST_OPENING && pos() > 0.05, "EXIT pulse opens the gate");
  run(20);
  CHECK(learned::get().limitDetected == 2, "limit switches auto-detected as NC");
  CHECK(st() == ST_OPEN && pos() >= 1.0, "reaches the open limit");
  CHECK(S().autoCloseLeftS > 0, "auto-close countdown running");
  run(11);
  CHECK(st() == ST_CLOSING, "auto-close starts after the delay");
  run(22);
  CHECK(st() == ST_CLOSED && pos() <= 0, "closes fully");
  CHECK(learned::get().closeMs > 18000 && learned::get().closeMs < 22000, "close travel learned");

  printf("S2 photo-eye broken while closing\n");
  pulse("exit"); run(22);
  CHECK(learned::get().openMs > 18000, "open travel learned (run started at the closed limit)");
  CHECK(S().midPct > 45 && S().midPct < 55, "MID reed position learned (~50 %)");
  run(11); run(5);
  CHECK(st() == ST_CLOSING, "closing");
  sim::setInput("safety", sim::ACT_ON); run(0.5);
  CHECK(hal::driving() != WIND_M2, "photo-eye stops the closing motor");
  run(10);
  CHECK(st() == ST_OPEN, "reopens fully");
  run(15);
  CHECK(st() == ST_OPEN, "stays open while the beam is broken");
  CHECK(cmd(CMD_CLOSE) == RES_PHOTO_EYE, "close refused: photo-eye active");
  sim::setInput("safety", sim::ACT_OFF); run(11);
  CHECK(st() == ST_CLOSING, "auto-close resumes once the beam is clear");
  run(22);
  CHECK(st() == ST_CLOSED, "closed");

  printf("S3 obstruction while closing -> reverse; second obstruction before a limit -> lockout\n");
  pulse("exit"); run(22); sim::obstacle(0.5f, false); run(11);
  // measure the reversal: from the gate stalling on the obstacle to the motor driving open
  uint32_t t0 = 0, t1 = 0;
  for (int i = 0; i < 1500 && !t1; i++) {
    g_now += 10; gate::tickOnce();
    if (!t0 && pos() <= 0.512f) t0 = g_now;          // first contact (edge or bumper)
    if (t0 && hal::driving() == WIND_M1) t1 = g_now;
  }
  printf("       reversal took %u ms\n", (unsigned)(t1 - t0));
  CHECK(t0 && t1 && t1 - t0 < 2000, "UL 325: reverses within 2 s of touching the obstacle");
  CHECK(!sim::testTrip(), "hardware overcurrent latch not reached");
  run(15);
  CHECK(st() == ST_OPEN && S().obstructionsThisRun == 0, "fully open, obstruction count reset at the limit");
  sim::obstacle(-1, false);
  cmd(CMD_CLOSE); run(8);
  cmd(CMD_STOP); run(1);
  sim::obstacle(0.75f, true);
  cmd(CMD_OPEN); run(8);
  CHECK(S().obstructionsThisRun == 1, "obstruction while opening counted");
  run(3);
  CHECK(st() == ST_STOPPED, "brief reverse, then stop");
  cmd(CMD_OPEN); run(8);
  CHECK(st() == ST_LOCKOUT && hal::alarmOn(), "second obstruction before a limit -> lockout + buzzer");
  CHECK(cmd(CMD_OPEN) == RES_LOCKOUT, "commands refused in lockout (result reported)");
  run(2);
  CHECK(hal::driving() == WIND_OFF, "gate stays stopped");
  float p0 = pos();
  pulse("reset"); run(0.3);
  CHECK(st() == ST_STOPPED && !hal::alarmOn() && fabsf(pos() - p0) < 0.001f, "RESET input clears lockout without moving the gate");
  sim::obstacle(-1, false);
  cmd(CMD_OPEN); run(12);
  CHECK(st() == ST_OPEN, "normal open after reset");

  printf("S4 EXIT held keeps the gate open\n");
  sim::setInput("exit", sim::ACT_ON); run(25);
  CHECK(st() == ST_OPEN, "open while EXIT is held, past the delay");
  CHECK(cmd(CMD_CLOSE) == RES_EXIT, "close refused while EXIT is held");
  sim::setInput("exit", sim::ACT_OFF); run(11);
  CHECK(st() == ST_CLOSING, "auto-close after release + delay");
  run(10);
  pulse("exit");
  run(1);
  CHECK(st() == ST_OPENING, "EXIT while closing reverses to open");
  run(15); run(11); run(22);
  CHECK(st() == ST_CLOSED, "closed again");

  printf("S5 hardware overcurrent latch -> inhibit; twice in a row -> fault until Reset\n");
  pulse("exit"); run(4);
  sim::trip(); run(2);
  CHECK(S().fInhibit && hal::driving() == WIND_OFF, "lost current detected, motor command dropped");
  CHECK(!sim::testTrip(), "trip latch cleared");
  CHECK(cmd(CMD_OPEN) == RES_OK, "next command works");
  run(25);
  CHECK(st() == ST_OPEN && !S().fInhibit, "opens fully, fault cleared");
  cmd(CMD_CLOSE); run(4);
  sim::trip(); run(2);
  cmd(CMD_OPEN); run(3);
  sim::trip(); run(2);
  CHECK(S().fInhibitLatched, "second trip in a row latches the fault");
  CHECK(cmd(CMD_OPEN) == RES_FAULT, "open refused until Reset alarm");
  cmd(CMD_RESET);
  CHECK(!S().fInhibitLatched && cmd(CMD_OPEN) == RES_OK, "Reset alarm clears it");
  run(25); closeFully();
  CHECK(st() == ST_CLOSED, "closed");

  printf("S6 forced open + RELAY-1 forced-open alarm + buzzer\n");
  setP(P_RELAY_MODE, RELAY_FORCED_OPEN); setP(P_FORCED_BUZZER, 1);
  sim::push(0.05f); run(1);
  CHECK(hal::relayOn() && S().forcedOpen, "RELAY-1 on when the gate is pushed off the closed limit");
  CHECK(hal::alarmOn(), "forced-open buzzer on");
  cmd(CMD_CLOSE); run(3);
  CHECK(st() == ST_CLOSED && !hal::relayOn() && !hal::alarmOn(), "closing again clears relay and buzzer");
  setP(P_RELAY_MODE, RELAY_OFF); setP(P_FORCED_BUZZER, 0);

  printf("S7 magnetic lock released before moving\n");
  setP(P_LOCK_TYPE, LOCK_MAG); run(0.2);
  CHECK(hal::lockOn(), "lock energised while closed");
  cmd(CMD_OPEN); run(0.3);
  CHECK(!hal::lockOn() && hal::driving() == WIND_OFF, "lock released, motor waits");
  run(1);
  CHECK(hal::driving() == WIND_M1, "motor starts after the release delay");
  run(25); closeFully();
  CHECK(hal::lockOn(), "locked again when closed");
  setP(P_LOCK_TYPE, LOCK_NONE);

  printf("S8 partial open\n");
  CHECK(cmd(CMD_PARTIAL) == RES_OK, "partial open accepted");
  run(15);
  CHECK(st() == ST_PARTIAL && pos() > 0.30f && pos() < 0.50f, "stops near 40 % (within 10 %)");
  CHECK(S().autoCloseLeftS > 0, "auto-close countdown from partly open");
  cmd(CMD_OPEN); run(15);
  CHECK(st() == ST_OPEN, "Open from partly open completes the opening");
  closeFully();
  cmd(CMD_PARTIAL); run(15); run(11);
  CHECK(st() == ST_CLOSING, "partly open auto-closes");
  run(15);
  CHECK(st() == ST_CLOSED, "closed");

  printf("S9 Hold open\n");
  cmd(CMD_HOLD_ON); run(22);
  CHECK(st() == ST_OPEN, "Hold open opens the gate");
  run(20);
  CHECK(st() == ST_OPEN && S().autoCloseLeftS < 0, "stays open, no auto-close");
  CHECK(cmd(CMD_CLOSE) == RES_HOLD, "close refused while Hold open is on");
  cmd(CMD_HOLD_OFF);
  CHECK(S().autoCloseLeftS > 0, "release starts the close delay");
  run(11);
  CHECK(st() == ST_CLOSING, "auto-closes after the delay");
  run(22);

  printf("S10 Auto-close off / on\n");
  cmd(CMD_AC_OFF);
  CHECK(!params::getb(P_AUTO_CLOSE), "Auto-close parameter turned off");
  cmd(CMD_OPEN); run(22); run(20);
  CHECK(st() == ST_OPEN, "stays open with auto-close off");
  cmd(CMD_AC_ON);
  CHECK(S().autoCloseLeftS > 0, "turning it on starts the countdown");
  run(11); run(22);
  CHECK(st() == ST_CLOSED, "auto-closed");

  printf("S11 Lock out mode\n");
  cmd(CMD_LOCKOUT_ON);
  pulse("exit"); run(2);
  CHECK(st() == ST_CLOSED && hal::driving() == WIND_OFF, "EXIT ignored");
  pulse("radio"); run(2);
  CHECK(st() == ST_CLOSED, "radio ignored");
  pulse("3bt"); run(2);
  CHECK(st() == ST_OPENING, "3BT still works (wired station)");
  run(22); run(11); run(5);
  CHECK(st() == ST_CLOSING, "auto-close still runs");
  pulse("exit"); run(1);
  CHECK(st() == ST_OPENING, "EXIT still reverses a closing gate");
  run(15);
  cmd(CMD_LOCKOUT_OFF);
  closeFully();

  printf("S12 3BT single button: open -> stop -> close -> stop -> open\n");
  pulse("3bt"); run(4);
  CHECK(st() == ST_OPENING, "closed: press opens");
  pulse("3bt"); run(0.5);
  CHECK(st() == ST_STOPPED, "opening: press stops");
  pulse("3bt"); run(2);
  CHECK(st() == ST_CLOSING, "stopped after opening: press closes");
  pulse("3bt"); run(0.5);
  CHECK(st() == ST_STOPPED, "closing: press stops");
  pulse("3bt"); run(2);
  CHECK(st() == ST_OPENING, "stopped after closing: press opens");
  run(25);
  pulse("3bt"); run(2);
  CHECK(st() == ST_CLOSING, "open: press closes");
  run(25);

  printf("S13 safety edge\n");
  openFully(); cmd(CMD_CLOSE); run(5);
  pulse("edge"); run(1);
  CHECK(st() == ST_OPENING, "edge while closing: reverses to open");
  run(7);
  CHECK(st() == ST_OPEN, "fully open");
  closeFully(); cmd(CMD_OPEN); run(5);
  pulse("edge"); run(3);
  CHECK(st() == ST_STOPPED && S().obstructionsThisRun == 1, "edge while opening: brief reverse, then stop");
  cmd(CMD_OPEN); run(25);
  CHECK(st() == ST_OPEN && S().obstructionsThisRun == 0, "opens fully after");
  setP(P_EDGE_TYPE, CONTACT_8K2); run(0.2);
  sim::setEdgeFault(true); run(0.2);
  CHECK(S().edgeFault && S().inActive[IN_EDGE], "monitored edge: wire break reported, counts as active");
  CHECK(cmd(CMD_CLOSE) == RES_EDGE, "close refused with a broken edge wire");
  sim::setEdgeFault(false); run(0.2);
  CHECK(!S().inActive[IN_EDGE], "monitored edge intact again");
  setP(P_EDGE_TYPE, CONTACT_NC);
  closeFully();

  printf("S13b edge still squeezed after reversing is not a second obstruction\n");
  openFully(); sim::obstacle(0.5f, false); cmd(CMD_CLOSE);
  bool squeezed = false;
  for (int i = 0; i < 1200 && st() != ST_OPENING; i++) { run(0.01); squeezed |= S().inActive[IN_EDGE]; }
  CHECK(squeezed, "closing into the obstacle squeezes the edge");
  bool stillSqueezedWhileOpening = false;
  for (int i = 0; i < 200; i++) { run(0.01); if (hal::driving() == WIND_M1 && S().inActive[IN_EDGE]) stillSqueezedWhileOpening = true; }
  CHECK(stillSqueezedWhileOpening, "edge still squeezed for a moment while reopening (realistic)");
  CHECK(st() == ST_OPENING && S().obstructionsThisRun == 1, "only one obstruction counted");
  run(12);
  CHECK(st() == ST_OPEN && S().state != ST_LOCKOUT, "fully open, no false lockout");
  sim::obstacle(-1, false);
  closeFully();

  printf("S13c photo-eye during the short reverse after an opening obstruction\n");
  sim::obstacle(0.4f, true); cmd(CMD_OPEN); run(9.5);
  sim::setInput("safety", sim::ACT_ON); run(2);
  CHECK(st() == ST_STOPPED && hal::driving() == WIND_OFF, "short reverse stopped by the photo-eye");
  sim::setInput("safety", sim::ACT_OFF); sim::obstacle(-1, false);
  cmd(CMD_RESET); openFully(); closeFully();

  printf("S14 power cut\n");
  cmd(CMD_HOLD_ON); cmd(CMD_LOCKOUT_ON); run(25);
  reboot(); run(1);
  CHECK(st() == ST_OPEN && S().holdOpen && S().lockOutMode, "Hold open and Lock out restored");
  run(20);
  CHECK(st() == ST_OPEN, "Hold open: no auto-close after the power cut");
  cmd(CMD_HOLD_OFF); cmd(CMD_LOCKOUT_OFF);
  reboot(); run(1);
  CHECK(st() == ST_OPEN && S().autoCloseLeftS > 0, "found at the open limit: normal auto-close countdown");
  run(11); run(6);
  reboot(); run(5);
  float pc = pos();
  CHECK(st() == ST_UNKNOWN && hal::driving() == WIND_OFF && pc > 0.05f && pc < 0.95f, "cut part-way: waits, no movement");
  cmd(CMD_CLOSE); run(1);
  CHECK(st() == ST_OPENING, "first command opens");
  run(25); closeFully();
  CHECK(st() == ST_CLOSED, "closed");

  printf("S15 radio open-stop-close, board buttons, STOP held 3 s\n");
  setP(P_RADIO_ACTION, 1);
  pulse("radio"); run(3);
  CHECK(st() == ST_OPENING, "radio opens");
  pulse("radio");
  CHECK(st() == ST_STOPPED, "radio stops");
  setP(P_RADIO_ACTION, 0);
  sim::button("open", sim::ACT_PULSE); run(1);
  CHECK(st() == ST_OPENING, "board OPEN button");
  sim::button("stop", sim::ACT_PULSE); run(0.5);
  CHECK(st() == ST_STOPPED, "board STOP button");
  sim::button("close", sim::ACT_PULSE); run(1);
  CHECK(st() == ST_CLOSING, "board CLOSE button");
  run(20);
  sim::obstacle(0.6f, true);
  cmd(CMD_OPEN); run(14); cmd(CMD_OPEN); run(14);
  CHECK(st() == ST_LOCKOUT, "lockout set up");
  sim::button("stop", sim::ACT_ON); run(3.5); sim::button("stop", sim::ACT_OFF); run(0.3);
  CHECK(st() == ST_STOPPED && !hal::alarmOn(), "STOP held 3 s resets the alarm");
  sim::obstacle(-1, false);
  openFully(); closeFully();

  printf("S16 max run time\n");
  setP(P_MAX_RUN, 10);
  cmd(CMD_OPEN); run(12);
  CHECK(st() == ST_STOPPED && S().fTimeout, "stops at the max run time, fault reported");
  setP(P_MAX_RUN, 60);
  cmd(CMD_RESET); openFully(); closeFully();

  printf("S17 NO limit switches, fresh board\n");
  Preferences::wipe();
  sim::setLimitsNC(false);
  reboot(); setP(P_CLOSE_DELAY, 10); run(0.5);
  CHECK(st() == ST_UNKNOWN, "limit type not known yet");
  cmd(CMD_OPEN); run(25);
  CHECK(learned::get().limitDetected == 1 && st() == ST_OPEN, "detected as NO, opened");
  run(11); run(22);
  CHECK(st() == ST_CLOSED, "auto-closed");

  printf("S18 event log\n");
  CHECK(eventlog::latest() > 50, "events recorded");
  String j = eventlog::json(0, 5);
  CHECK(j.indexOf("\"events\":[") > 0 && j.indexOf("closed") > 0, "JSON export");

  printf("\n%s: %d passed, %d failed\n", fails ? "FAILED" : "ALL PASSED", passes, fails);
  return fails ? 1 : 0;
}
