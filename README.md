# GateMaster

ESP32 firmware for the **GateMaster** board, a drop-in replacement for the All-O-Matic SL-100 control board (ALL-AC2K). It also runs on any 4 MB ESP32 DevKit in **Simulator** mode, for testing without a gate.

**Install:** https://ivanvachovsky.github.io/GateMaster/ (Chrome or Edge, USB cable, about two minutes).

## What it does

- **Gate logic, as the original board:**
  - Open, stop and close, with auto-close (1–60 s).
  - EXIT loop: a pulse opens; holding it keeps the gate open; active while closing, it reverses the gate.
  - Radio: open, or open-stop-close.
  - 3BT single button: open → stop → close → stop.
  - RESET clears the alarm and never moves the gate.
  - Magnetic or solenoid lock.
  - RELAY-1 in the original's four modes.
- **Safety (UL 325):**
  - The motor current is sampled 2000 times a second.
  - Closing into an obstruction (current rise or safety edge) reverses the gate to fully open.
  - Opening into one stops the gate and reverses it briefly.
  - A second obstruction before a limit causes a lockout with the buzzer.
  - The photo-eye reopens a closing gate and pauses auto-close.
  - The monitored safety edge reports wiring faults.
  - Wired inputs are handled by the control task (100 Hz, its own CPU core). The network never sits between an input and the motor.
- **Controls:**
  - Open, Stop, Close, Partial open and Reset alarm.
  - Open for N minutes, Auto-close on/off and Lock out. Open for and Lock out survive a power cut.
  - Auto-close rule: nothing leaves the gate open, fully or part-way, longer than the close delay, except Open for N minutes, EXIT held, or Auto-close switched off. The countdown waits while the photo-eye or edge is active, and pauses during lockout, a latched motor fault or a firmware update.
- **Web page** (stored in the firmware) with four tabs:
  - **Controls**.
  - **Status:** every input and output, faults, a live motor-current graph and the event log.
  - **Parameters:** every setting, with its default.
  - **General:** name, password, Wi-Fi, MQTT, time zone, firmware, backup/restore.
- **MQTT:** every control, reading and parameter, plus **Home Assistant discovery**.
- **Setup and updates:**
  - Wi-Fi is set while flashing (Improv), and **Visit device** opens the gate's page. It is also always at `http://gatemaster-xxxx.local`.
  - Online updates via **Check for update**, or a manual upload.
  - A new version that fails to start properly is replaced by the previous one automatically.

The specification and design documents are the reference for behaviour; `test/gate_test.cpp` checks it.

## Layout

| Path | What |
|---|---|
| `firmware/GateMaster/` | The firmware (Arduino sketch folder). `main.cpp` is the entry point |
| `firmware/web/index.html` | Web page source (gzipped into `webpage_gz.h` at build time) |
| `test/` | PC tests of the gate logic against the simulator (`test/run.sh`), and a mock of the board's API for working on the page (`test/mock_server.py`) |
| `tools/` | `build.sh`, `release.py` (packs the flashing page files), `gzpage.py`, `make_certs.py` |
| `docs/` | The flashing page (GitHub Pages). `manifest.json`, `version.json` and `firmware/` are generated |

| Firmware file | Job |
|---|---|
| `config.h` | Pins, timings, version |
| `params.*` / `devcfg.cpp` | The single parameter table, device settings, learned values (NVS) |
| `hal.*`, `hal_sim.cpp` | Hardware interface: GateMaster board backend and gate simulator backend |
| `inputs.*` | Debounce, contact types, safety-edge analog, button ladder, DIP1 |
| `gate.*` | State machine, safety, modes, position, command queue |
| `eventlog.*` | 200-event log in LittleFS |
| `wifimgr.*`, `improv.*` | Wi-Fi, setup network, mDNS, time; Improv provisioning |
| `web.*`, `status.*` | Web server, API, status readings |
| `mqtt.*` | MQTT topics and Home Assistant discovery |
| `ota.*`, `ca_certs.h` | Updates and rollback guard |

## Build

You need the Arduino IDE or arduino-cli with these:

- the **esp32 core 2.0.9** (board manager URL `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`);
- the **PubSubClient 2.8** library.

In the IDE, set these options:

- Board: *ESP32 Dev Module*
- Partition Scheme: *Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)*

```sh
sh test/run.sh                 # PC tests (g++)
sh tools/build.sh              # tests + web page + compile -> build/
python3 tools/release.py "notes"   # docs/firmware/*.bin, docs/manifest.json, docs/version.json
```

## Release

1. Raise `FW_VERSION` in `firmware/GateMaster/config.h` and push to `main`.
2. On GitHub, open **Releases → Draft a new release**:
   - create the tag `v` + the version (e.g. `v0.2.1`);
   - give it a one-line title (it becomes the update notes the boards show);
   - click **Publish release**.

GitHub Actions then:

- runs the tests;
- builds both variants;
- checks the tag matches `FW_VERSION`;
- publishes the flashing page and update files to GitHub Pages;
- attaches the .bin files to the release.

Boards in the field see the update with their next daily check, or when someone presses **Check for update**. They never install by themselves.

## API summary

- **Authentication:** HTTP Basic, user `admin`, with the password set on the first visit.
- **Status and settings:**
  - `GET /api/status`: all readings.
  - `GET /api/config`: parameters, the parameter table and device settings.
  - `POST /api/config`: `key=value` pairs.
- **Commands:**
  - `POST /api/cmd` takes `cmd=` with one of: `open`, `stop`, `close`, `partial`, `reset`, `hold_on` (optionally `hold_on 30`, or `minutes=30`), `hold_off`, `autoclose_on`, `autoclose_off`, `lockout_on`, `lockout_off`, `toggle`, `relearn`, `update_check`, `update_install`, `reboot`.
- **Logs and data:**
  - `GET /api/current`: motor current graph.
  - `GET /api/log`, `/api/log.txt`: the event log.
  - `GET /api/backup`, `POST /api/restore`: settings file.
- **Firmware:** `POST /api/update` takes a multipart `.bin` upload.
- **Simulator:** `POST /api/sim` takes `name=`, `value=` (Simulator mode only).

## MQTT summary

The base topic is `gatemaster/<name>` by default.

| Topic | Direction | Content |
|---|---|---|
| `<base>/availability` | out | `online` / `offline` |
| `<base>/state` | out | the gate state |
| `<base>/status` | out | all readings as JSON |
| `<base>/status/<reading>` | out | each reading on its own |
| `<base>/event` | out | event log entries |
| `<base>/config` | out | all parameters as JSON |
| `<base>/cmd` | in | the API commands; the result comes back on `<base>/cmd/result` |
| `<base>/set/<param>` | in | a new value; the result comes back on `<base>/set/<param>/result` |
| `<base>/sim/<name>` | in | Simulator mode only |

Home Assistant entities are published under `homeassistant/…/gatemaster_<chip id>/…`.
