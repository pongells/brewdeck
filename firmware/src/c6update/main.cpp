// One-off tool for the Guition ESP32-P4 board (env p4-c6-update): updates the board's ESP32-C6, the Wi-Fi chip the P4
// talks to over SDIO (ESP-Hosted), to the ESP-Hosted version this Arduino core's host side expects, with Espressif's
// ESP_HostedOTA (image from espressif.github.io). Boards ship with older C6 firmware (seen: 2.3.2 against 2.12.13);
// with it the first Wi-Fi join dropped at every boot. The C6 switches to the new image only once it is fully written.
//   ./build-pio.sh p4-c6-update --flash, then read the log (the screen stays dark), then flash Brewdeck again:
//   ./build-pio.sh ui-p4 --flash
// Wi-Fi: the network Brewdeck saved on the screen (NVS "wifi", read only), else the one in wifi_config.h.
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include "esp32-hal-hosted.h"
#include "ESP_HostedOTA.h"
#include "wifi_config.h"

static void versions(const char *when) {
  uint32_t a, b, c, x, y, z;
  hostedGetHostVersion(&a, &b, &c);
  hostedGetSlaveVersion(&x, &y, &z);
  Serial.printf("c6update: %s: host %u.%u.%u, C6 %u.%u.%u (%s)\n", when, (unsigned)a, (unsigned)b, (unsigned)c,
                (unsigned)x, (unsigned)y, (unsigned)z, hostedGetSlaveTargetName());
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("c6update: start");
  static char ssid[33] = "", pass[65] = "";
  Preferences p;
  if (p.begin("wifi", true)) {
    p.getString("ssid", ssid, sizeof ssid);
    p.getString("pass", pass, sizeof pass);
    p.end();
  }
  if (ssid[0]) Serial.printf("c6update: using the network saved on the screen, \"%s\"\n", ssid);
  else if (strcmp(WIFI_SSID, "YOUR_WIFI_NAME") != 0) {
    snprintf(ssid, sizeof ssid, "%s", WIFI_SSID);
    snprintf(pass, sizeof pass, "%s", WIFI_PASS);
    Serial.printf("c6update: using the network in wifi_config.h, \"%s\"\n", ssid);
  } else {
    Serial.println("c6update: no network: join one on the screen with Brewdeck first, or set wifi_config.h");
    return;
  }
  WiFi.STA.begin();
  versions("before");
  for (int attempt = 1; attempt <= 5 && WiFi.STA.status() != WL_CONNECTED; attempt++) {
    Serial.printf("c6update: joining, attempt %d\n", attempt);
    WiFi.STA.connect(ssid, pass);
    for (int i = 0; i < 30 && WiFi.STA.status() != WL_CONNECTED; i++) delay(500);
  }
  memset(pass, 0, sizeof pass);
  if (WiFi.STA.status() != WL_CONNECTED) { Serial.println("c6update: could not join Wi-Fi, nothing done"); return; }
  if (!hostedHasUpdate()) { versions("no update needed"); return; }
  Serial.printf("c6update: updating from %s\n", hostedGetUpdateURL());
  const bool ok = updateEspHostedSlave();
  Serial.printf("\nc6update: update %s\n", ok ? "done, restarting to check" : "FAILED (the C6 keeps its old firmware)");
  if (ok) { delay(1000); ESP.restart(); }
}

void loop() { delay(1000); }
