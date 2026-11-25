// One-off tool for the Guition ESP32-P4 board (env p4-c6-update): updates the board's ESP32-C6, the Wi-Fi chip the P4
// talks to over SDIO (ESP-Hosted), to the ESP-Hosted version Brewdeck's firmware runs (esp_hosted in
// src/idf_component.yml). Boards ship with older C6 firmware (seen: 2.3.2); a C6 behind the host logs "Version
// mismatch" at every boot, and with very old ones the first Wi-Fi join dropped.
//   ./build.sh p4-c6-update --flash, read the log (the screen stays dark), then flash Brewdeck again:
//   ./build.sh ui-p4 --flash
//
// The C6 image is built from source in this repository (firmware/c6: ESP-Hosted's co-processor, same version as this
// host; Espressif publishes C6 images only for 2.x) and carried inside this program, so no network is needed: only the
// P4's SDIO link to the C6. It is sent with ESP-Hosted's co-processor OTA; the C6 checks the image (its own hash) and
// boots it only when it is complete. If anything fails, the C6 keeps its current firmware.
// -DC6UPDATE_DRY_RUN (PLATFORMIO_BUILD_FLAGS): check the image and the link, write nothing.
#include <stdio.h>
#include <string.h>
#include <esp_attr.h>
#include <esp_app_desc.h>
#include <esp_app_format.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mbedtls/sha256.h>
#include "esp_hosted.h"
#include "esp_hosted_ota.h"

// the C6 image (src/CMakeLists.txt embeds firmware/c6/.pio/build/c6/firmware.bin)
extern const uint8_t c6_bin_start[] asm("_binary_firmware_bin_start");
extern const uint8_t c6_bin_end[] asm("_binary_firmware_bin_end");

// one try per version: if the C6 still reports another version after an update and a restart, stop (no loop)
RTC_NOINIT_ATTR static uint32_t s_tried;

static bool image_ok(const uint8_t *img, size_t len) {
  esp_image_header_t h;
  if (len < sizeof h + 8 + sizeof(esp_app_desc_t)) return false;
  memcpy(&h, img, sizeof h);
  if (h.magic != ESP_IMAGE_HEADER_MAGIC || h.chip_id != ESP_CHIP_ID_ESP32C6) {
    printf("c6update: the carried image is not an ESP32-C6 app (magic 0x%02x, chip %u)\n", h.magic, (unsigned)h.chip_id);
    return false;
  }
  esp_app_desc_t d;
  memcpy(&d, img + sizeof h + 8, sizeof d);   // the first segment starts with the app description
  if (d.magic_word != ESP_APP_DESC_MAGIC_WORD) { printf("c6update: no app description in the image\n"); return false; }
  uint8_t sha[32];
  mbedtls_sha256(img, len, sha, 0);
  printf("c6update: image %u bytes, %s, built with ESP-IDF %s, SHA-256 ", (unsigned)len, d.project_name, d.idf_ver);
  for (int i = 0; i < 32; i++) printf("%02x", sha[i]);
  printf("\n");
  return true;
}

static bool send_image(const uint8_t *img, size_t len) {
  if (esp_hosted_slave_ota_begin() != ESP_OK) { printf("c6update: the C6 refused to start the update\n"); return false; }
  const size_t CHUNK = 1400;
  for (size_t off = 0; off < len; off += CHUNK) {
    const size_t n = len - off < CHUNK ? len - off : CHUNK;
    if (esp_hosted_slave_ota_write(img + off, n) != ESP_OK) {
      printf("\nc6update: writing to the C6 failed at %u of %u bytes\n", (unsigned)off, (unsigned)len);
      return false;   // never ended or activated: the C6 keeps its firmware
    }
    if (off % (64 * 1024) < CHUNK) printf(".");
  }
  printf("\n");
  if (esp_hosted_slave_ota_end() != ESP_OK) { printf("c6update: the C6 rejected the image: NOT activated\n"); return false; }
  if (esp_hosted_slave_ota_activate() != ESP_OK) { printf("c6update: activating the new image failed\n"); return false; }
  return true;
}

extern "C" void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(1500));   // time to open the serial monitor
  const uint32_t want = ESP_HOSTED_VERSION_VAL(ESP_HOSTED_VERSION_MAJOR_1, ESP_HOSTED_VERSION_MINOR_1,
                                               ESP_HOSTED_VERSION_PATCH_1);
  printf("c6update: start, host %u.%u.%u\n", (unsigned)ESP_HOSTED_VERSION_MAJOR_1, (unsigned)ESP_HOSTED_VERSION_MINOR_1,
         (unsigned)ESP_HOSTED_VERSION_PATCH_1);
  const uint8_t *img = c6_bin_start;
  const size_t len = (size_t)(c6_bin_end - c6_bin_start);
  if (!image_ok(img, len)) return;
  esp_hosted_init();
  if (esp_hosted_connect_to_slave() != 0) { printf("c6update: no link to the C6, nothing done\n"); return; }
  esp_hosted_coprocessor_fwver_t v = {};
  if (esp_hosted_get_coprocessor_fwversion(&v) != 0) { printf("c6update: the C6 did not report its version\n"); return; }
  const uint32_t have = ESP_HOSTED_VERSION_VAL(v.major1, v.minor1, v.patch1);
  printf("c6update: C6 runs %u.%u.%u, this program carries %u.%u.%u\n", (unsigned)v.major1, (unsigned)v.minor1,
         (unsigned)v.patch1, (unsigned)ESP_HOSTED_VERSION_MAJOR_1, (unsigned)ESP_HOSTED_VERSION_MINOR_1,
         (unsigned)ESP_HOSTED_VERSION_PATCH_1);
#ifdef C6UPDATE_DRY_RUN
  printf("c6update: dry run: image and link checked, nothing written\n");
  return;
#endif
  if (have == want) { s_tried = 0; printf("c6update: no update needed, now flash Brewdeck again (./build.sh ui-p4 --flash)\n"); return; }
  if (s_tried == want) {
    s_tried = 0;
    printf("c6update: the C6 still runs another version after one update: stopped (see the log above)\n");
    return;
  }
  s_tried = want;
  printf("c6update: updating the C6 ...\n");
  if (!send_image(img, len)) { s_tried = 0; printf("c6update: update FAILED (the C6 keeps its old firmware)\n"); return; }
  printf("c6update: update done, restarting to check\n");
  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
}
