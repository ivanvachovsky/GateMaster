// MQTT: every control, reading and parameter (spec 7), plus Home Assistant discovery.
// Plain TCP (no TLS in v0.2). The gate keeps working normally when the broker is down.
#pragma once
#include <Arduino.h>

namespace mqtt {
void begin();
void loop();             // network task
void reconnect();        // settings changed
bool connected();
void publishConfig();    // parameters changed
}
