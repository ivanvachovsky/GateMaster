// Improv Wi-Fi over serial (https://www.improv-wifi.com/serial/): lets the flashing page (ESP Web Tools)
// ask for the Wi-Fi network right after flashing, and show "Visit device". Always listening, so the
// flashing page can also change the Wi-Fi later without reflashing.
#pragma once
namespace improv {
void loop();   // network task: read serial, answer
}
