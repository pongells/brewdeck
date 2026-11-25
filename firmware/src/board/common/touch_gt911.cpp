#include "touch_gt911.h"
#include "../../core/systime.h"
#include "board_pins.h"
#include "board.h"
#include <driver/i2c_master.h>

// I2C on ESP-IDF's master driver, as Arduino's Wire set it up: 400 kHz, internal pull-ups, 50 ms per transfer
static const int I2C_MS = 50;
static i2c_master_bus_handle_t g_bus;
static i2c_master_dev_handle_t g_dev;
static uint8_t g_addr = 0;
static bool    g_down = false;
static int16_t g_rx = 0, g_ry = 0;
static uint32_t g_last_ready = 0;
static int g_xmax = SCREEN_W, g_ymax = SCREEN_H;   // from the chip's own config


static bool probe(uint8_t a) { return g_bus && i2c_master_probe(g_bus, a, I2C_MS) == ESP_OK; }

static bool reg_write(uint16_t reg, uint8_t val) {
  const uint8_t b[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
  return g_dev && i2c_master_transmit(g_dev, b, sizeof b, I2C_MS) == ESP_OK;
}

static bool reg_read(uint16_t reg, uint8_t *buf, size_t n) {   // register address, then a repeated start and the read
  const uint8_t r[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
  return g_dev && i2c_master_transmit_receive(g_dev, r, sizeof r, buf, n, I2C_MS) == ESP_OK;
}

uint8_t touch_address() { return g_addr; }

// what usually sits at an address on these boards (a guess from the address alone)
static const char *i2c_guess(uint8_t a) {
  switch (a) {
    case 0x14: case 0x5D: return "GT911 touch";
    case 0x18: case 0x19: return "audio codec? (ES8311)";
    case 0x40: case 0x41: return "microphone ADC? (ES7210)";
    default: return "";
  }
}

size_t board_i2c_scan(char *out, size_t cap) {
  size_t n = 0;
  int found = 0;
  for (uint8_t a = 0x08; a < 0x78 && n + 48 < cap; a++) {
    if (!probe(a)) continue;
    found++;
    n += snprintf(out + n, cap - n, "0x%02X %s\n", a, i2c_guess(a));
  }
  n += snprintf(out + n, cap - n, "%d device%s on SDA %d / SCL %d\n", found, found == 1 ? "" : "s", PIN_TOUCH_SDA,
                PIN_TOUCH_SCL);
  return n < cap ? n : cap - 1;
}
void touch_cfg(int *xmax, int *ymax) { *xmax = g_xmax; *ymax = g_ymax; }

bool touch_begin() {
  i2c_master_bus_config_t bc = {};
  bc.i2c_port = -1;                                 // any free controller
  bc.sda_io_num = (gpio_num_t)PIN_TOUCH_SDA;
  bc.scl_io_num = (gpio_num_t)PIN_TOUCH_SCL;
  bc.clk_source = I2C_CLK_SRC_DEFAULT;
  bc.glitch_ignore_cnt = 7;
  bc.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bc, &g_bus) != ESP_OK) { g_bus = NULL; printf("touch: I2C bus not started\n"); return false; }
  if (PIN_TOUCH_RST >= 0) {
    pin_output(PIN_TOUCH_RST, 0);
    wait_ms(10);
    pin_set(PIN_TOUCH_RST, 1);
    wait_ms(80);
  }

  const uint8_t candidates[2] = {0x5D, 0x14};
  for (int i = 0; i < 2; i++) {
    if (probe(candidates[i])) {
      g_addr = candidates[i];
      break;
    }
  }
  if (g_addr == 0) return false;
  i2c_device_config_t dc = {};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = g_addr;
  dc.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(g_bus, &dc, &g_dev) != ESP_OK) { g_dev = NULL; g_addr = 0; return false; }

  uint8_t pid[4] = {0, 0, 0, 0};
  if (reg_read(0x8140, pid, 4)) {
    printf("GT911 at 0x%02X, product id %c%c%c%c\n", g_addr, pid[0], pid[1], pid[2], pid[3]);
  }
  uint8_t cfg[4];
  if (reg_read(0x8048, cfg, 4)) {
    int xm = cfg[0] | (cfg[1] << 8), ym = cfg[2] | (cfg[3] << 8);
    printf("GT911 config range %d x %d\n", xm, ym);
    if (xm >= 100 && xm <= 4096 && ym >= 100 && ym <= 4096) { g_xmax = xm; g_ymax = ym; }
  }
  reg_write(0x814E, 0);
  return true;
}

bool touch_raw(int16_t *rx, int16_t *ry) {
  if (g_addr == 0) return false;
  uint8_t st = 0;
  if (reg_read(0x814E, &st, 1)) {
    if (st & 0x80) {
      g_last_ready = clock_ms();
      uint8_t n = st & 0x0F;
      if (n > 0 && n <= 5) {
        uint8_t p[8];
        if (reg_read(0x8150, p, 8)) {
          g_rx = (int16_t)(p[0] | (p[1] << 8));
          g_ry = (int16_t)(p[2] | (p[3] << 8));
          g_down = true;
        }
      } else {
        g_down = false;
      }
      reg_write(0x814E, 0);
    }
  }
  if (g_down && clock_ms() - g_last_ready > 120) g_down = false;   // no report for a while (or the bus failing): lifted
  if (g_down) {
    *rx = g_rx;
    *ry = g_ry;
  }
  return g_down;
}

void touch_map(int16_t rx, int16_t ry, int16_t *x, int16_t *y) {
  // scale from the chip's own range to screen pixels
#if TOUCH_SWAP_XY
  int32_t mx = (int32_t)ry * SCREEN_W / g_ymax, my = (int32_t)rx * SCREEN_H / g_xmax;
#else
  int32_t mx = (int32_t)rx * SCREEN_W / g_xmax, my = (int32_t)ry * SCREEN_H / g_ymax;
#endif
#if TOUCH_FLIP_X
  mx = (SCREEN_W - 1) - mx;
#endif
#if TOUCH_FLIP_Y
  my = (SCREEN_H - 1) - my;
#endif
  if (mx < 0) mx = 0;
  if (my < 0) my = 0;
  if (mx > SCREEN_W - 1) mx = SCREEN_W - 1;
  if (my > SCREEN_H - 1) my = SCREEN_H - 1;
  *x = (int16_t)mx;
  *y = (int16_t)my;
}

bool touch_pressed(int16_t *x, int16_t *y) {
  int16_t rx, ry;
  if (!touch_raw(&rx, &ry)) return false;
  touch_map(rx, ry, x, y);
  return true;
}
