// Every status reading (spec 5.1), built once for the web page and MQTT, under the same names.
#pragma once
#include <Arduino.h>
#include <vector>

namespace status {
struct Reading {
  String key;          // e.g. "state", "current", "input/safety"
  String val;          // text value
  char kind;           // 's' text, 'n' number, 'b' boolean ("true"/"false")
};
void collect(std::vector<Reading>& out);
String json();                              // all readings as one flat JSON object
String mqttValue(const Reading& r);         // booleans as ON / OFF
void setMqttConnected(bool c);
const char* resetReason();
}
