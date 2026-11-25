// Sunton ESP32-S3 panel: Arduino_GFX's RGB driver (frame buffer in PSRAM, bounce buffers) and the backlight. Used by
// board_s3.cpp and the touch test.
#include "board.h"
#include "../../core/systime.h"
#include "board_pins.h"

static Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
    40 /* DE */, 41 /* VSYNC */, 39 /* HSYNC */, 42 /* PCLK */,
    45 /* R0 */, 48 /* R1 */, 47 /* R2 */, 21 /* R3 */, 14 /* R4 */,
    5 /* G0 */, 6 /* G1 */, 7 /* G2 */, 15 /* G3 */, 16 /* G4 */, 4 /* G5 */,
    8 /* B0 */, 3 /* B1 */, 46 /* B2 */, 9 /* B3 */, 1 /* B4 */,
    0 /* hsync_polarity */, 8 /* hsync_front_porch */, 4 /* hsync_pulse_width */, 8 /* hsync_back_porch */,
    0 /* vsync_polarity */, 8 /* vsync_front_porch */, 4 /* vsync_pulse_width */, 8 /* vsync_back_porch */,
    1 /* pclk_active_neg */, PANEL_PCLK_HZ /* prefer_speed */, false /* big endian */, 0 /* de idle high */, 0 /* pclk idle high */,
    PANEL_BOUNCE_PX /* bounce buffer px */);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(SCREEN_W, SCREEN_H, bus, 0 /* rotation */, true /* auto_flush */);

void board_init() {
  pin_output(PIN_BL, 0);   // keep dark until the first frame is drawn
  if (!gfx->begin()) {
    printf("gfx->begin() failed\n");
  }
  gfx->fillScreen(0x0000);
  pin_set(PIN_BL, 1);
}
