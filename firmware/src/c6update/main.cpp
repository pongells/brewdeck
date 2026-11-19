// One-off tool for the Guition ESP32-P4 board (env p4-c6-update): updates the board's ESP32-C6, the Wi-Fi chip the P4
// talks to over SDIO (ESP-Hosted), to the ESP-Hosted version this Arduino core's host side expects. Boards ship with
// older C6 firmware (seen: 2.3.2 against 2.12.13); with it the first Wi-Fi join dropped at every boot.
//   ./build-pio.sh p4-c6-update --flash, then read the log (the screen stays dark), then flash Brewdeck again:
//   ./build-pio.sh ui-p4 --flash
// Wi-Fi: the network Brewdeck saved on the screen (NVS "wifi", read only), else the one in wifi_config.h.
//
// The image comes from Espressif (the URL the core names). Its size and SHA-256 are pinned below: the hash is computed
// while the image is written to the C6, and the C6 is told to boot it only when both match. On a mismatch, a failed
// download or a write error, the new image is never activated and the C6 keeps its current firmware. A newer core
// expects another version: add its image's size and hash to IMAGES (sha256 of the downloaded .bin) first.
// -DC6UPDATE_DRY_RUN (PLATFORMIO_BUILD_FLAGS): download and check the image even when the C6 is current, write nothing.
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <mbedtls/sha256.h>
#include "esp32-hal-hosted.h"
#include "wifi_config.h"

static const struct { const char *url; int size; const char *sha256; } IMAGES[] = {
  {"https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.13.bin", 1226976,
   "f8509949826c197a4fa3c7958943b4e8985cffdfdf4204c2847d8953905121c9"},
};

static void versions(const char *when) {
  uint32_t a, b, c, x, y, z;
  hostedGetHostVersion(&a, &b, &c);
  hostedGetSlaveVersion(&x, &y, &z);
  Serial.printf("c6update: %s: host %u.%u.%u, C6 %u.%u.%u (%s)\n", when, (unsigned)a, (unsigned)b, (unsigned)c,
                (unsigned)x, (unsigned)y, (unsigned)z, hostedGetSlaveTargetName());
}

// download the image, write it to the C6 while hashing it; activate only a complete image with the pinned hash
static bool update_c6(const char *url, int want_size, const char *want_sha) {
  NetworkClientSecure tls;
  tls.setInsecure();   // the pinned SHA-256 checks the image itself, whoever served it
  HTTPClient http;
  if (!http.begin(tls, url)) { Serial.println("c6update: could not start the download"); return false; }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) { Serial.printf("c6update: download failed (HTTP %d)\n", code); http.end(); return false; }
  int left = http.getSize();
  if (left != want_size) {
    Serial.printf("c6update: image is %d bytes, expected %d: not the pinned image, nothing written\n", left, want_size);
    http.end();
    return false;
  }
#ifndef C6UPDATE_DRY_RUN
  if (!hostedBeginUpdate()) { Serial.println("c6update: the C6 refused to start the update"); http.end(); return false; }
#endif
  static uint8_t buf[2048];
  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, 0);
  NetworkClient *in = http.getStreamPtr();
  bool ok = true;
  uint32_t last = millis();
  while (left > 0) {
    const size_t avail = in->available();
    if (!avail) {
      if (!http.connected() || millis() - last > 15000) { Serial.println("\nc6update: download stopped"); ok = false; break; }
      delay(5);
      continue;
    }
    const int n = in->readBytes(buf, min((int)avail, min(left, (int)sizeof buf)));
    if (n <= 0) continue;
    last = millis();
    mbedtls_sha256_update(&sha, buf, n);
#ifndef C6UPDATE_DRY_RUN
    if (!hostedWriteUpdate(buf, n)) { Serial.println("\nc6update: writing to the C6 failed"); ok = false; break; }
#endif
    left -= n;
    if ((want_size - left) % (64 * 1024) < (int)sizeof buf) Serial.print(".");
  }
  http.end();
  uint8_t got[32];
  mbedtls_sha256_finish(&sha, got);
  mbedtls_sha256_free(&sha);
  char hex[65];
  for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", got[i]);
  Serial.println();
  if (!ok || left != 0) return false;   // incomplete: never ended or activated, the C6 keeps its firmware
  if (strcmp(hex, want_sha) != 0) {
    Serial.printf("c6update: SHA-256 %s does not match the pinned %s: NOT activated\n", hex, want_sha);
    return false;
  }
  Serial.println("c6update: SHA-256 matches the pinned image");
#ifdef C6UPDATE_DRY_RUN
  Serial.println("c6update: dry run: nothing written");
  return false;
#endif
  if (!hostedEndUpdate()) { Serial.println("c6update: the C6 rejected the image: NOT activated"); return false; }
  if (!hostedActivateUpdate()) { Serial.println("c6update: activating the new image failed"); return false; }
  return true;
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
#ifndef C6UPDATE_DRY_RUN
  if (!hostedHasUpdate()) { versions("no update needed"); return; }
#endif
  const char *url = hostedGetUpdateURL();
  for (const auto &img : IMAGES) {
    if (strcmp(url, img.url) != 0) continue;
    Serial.printf("c6update: updating from %s\n", url);
    const bool ok = update_c6(img.url, img.size, img.sha256);
#ifdef C6UPDATE_DRY_RUN
    Serial.println("c6update: dry run finished");
    return;
#endif
    Serial.printf("c6update: update %s\n", ok ? "done, restarting to check" : "FAILED (the C6 keeps its old firmware)");
    if (ok) { delay(1000); ESP.restart(); }
    return;
  }
  Serial.printf("c6update: no pinned hash for %s: add its size and SHA-256 to IMAGES first. Nothing written.\n", url);
}

void loop() { delay(1000); }
