#pragma once
// Copy this file to wifi_config.h (git-ignored) and fill it in. build-pio.sh does the copy on first run.
// ---- EDIT THESE TWO LINES ------------------------------------------------------------------
#define WIFI_SSID "YOUR_WIFI_NAME"
#define WIFI_PASS "YOUR_WIFI_PASSWORD"
// ---------------------------------------------------------------------------------------------
// The machine is found by its mDNS name (http://gaggiuino.local). If that does not work on your
// network, put the machine's IP address here instead, e.g. "192.168.1.50". Leave "" to use the name.
#define MACHINE_HOST "gaggiuino"
#define MACHINE_IP   ""
// Leaving WIFI_SSID as "YOUR_WIFI_NAME" is fine too: the screen then starts on "No Wi-Fi set up" and you choose the
// network on the screen (it is saved there).
