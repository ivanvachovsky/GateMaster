#!/bin/sh
# Builds and runs the PC gate tests. Exit code 0 = all passed.
set -e
cd "$(dirname "$0")"
FW=../firmware/GateMaster
g++ -std=c++17 -O1 -Wall -Wno-unused-function -Wno-unused-variable -DHOST_TEST -Ihost -I$FW \
  gate_test.cpp $FW/gate.cpp $FW/inputs.cpp $FW/params.cpp $FW/hal.cpp $FW/hal_sim.cpp $FW/eventlog.cpp \
  -o /tmp/gatemaster_test
/tmp/gatemaster_test
