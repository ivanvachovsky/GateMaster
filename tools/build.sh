#!/bin/sh
# Builds the firmware: PC tests, web page, then two compiles:
#   build/         GateMaster board (starts in board mode)
#   build-devkit/  ESP32 DevKit (starts in Simulator mode on its first start)
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ARDUINO_CLI="${ARDUINO_CLI:-arduino-cli}"
sh "$ROOT/test/run.sh" | tail -1
python3 "$ROOT/tools/gzpage.py"
EXTRA=""
[ -n "$CTAGS_DIR" ] && EXTRA="--build-property runtime.tools.ctags.path=$CTAGS_DIR"
FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs,CPUFreq=240,FlashMode=dio,FlashFreq=80"
"$ARDUINO_CLI" compile --fqbn "$FQBN" --warnings default $EXTRA --output-dir "$ROOT/build" "$ROOT/firmware/GateMaster"
"$ARDUINO_CLI" compile --fqbn "$FQBN" --warnings default $EXTRA --build-property "compiler.cpp.extra_flags=-DDEFAULT_HW_MODE=1" \
  --output-dir "$ROOT/build-devkit" "$ROOT/firmware/GateMaster"
