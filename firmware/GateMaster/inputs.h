// Inputs: reads the hardware interface every control tick (10 ms, control task only) and turns raw
// levels into debounced contacts and "active / clear" using each input's contact-type parameter.
// Also the on-board OPEN / STOP / CLOSE buttons and DIP1 (both on the IO39 ladder).
#pragma once
#include "config.h"

namespace inputs {
void begin();                       // settle readings (call after hal::begin and params::load)
void poll();                        // every tick
bool closed(InputId i);             // debounced contact closed
bool active(InputId i);             // after the contact type (photo-eye broken, edge pressed, EXIT made...)
uint32_t sinceMs(InputId i);        // how long the debounced level has lasted
Button button();                    // debounced board button
uint32_t buttonHeldMs();
uint16_t buttonMv();
bool dip1();                        // DIP1 = OPEN LEFT/RIGHT (read while no button is pressed)
int edgeMv();                       // monitored edge voltage, -1 when digital
bool edgeFault();                   // monitored edge: wire open (counts as active)
}
