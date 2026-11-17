// Web page (one gzipped file inside the firmware) and the JSON API. HTTP Basic auth, user "admin",
// with the password set on the first visit. Clients on the setup network only get the Wi-Fi setup page.
#pragma once
namespace web {
void begin();
void loop();
}
