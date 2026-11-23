#ifndef LV_CONF_H
#define LV_CONF_H
// LVGL 9 configuration for the UIs built on the core (src/ui, src/ui_minimal). Anything not set here takes LVGL's default (lv_conf_internal.h).
#define LV_COLOR_FORMAT_DEFAULT LV_COLOR_FORMAT_RGB565   // the panel's format; native byte order (no swap)
#define LV_DISABLE_API_MAPPING 1   // LVGL 9 names only (no LVGL 8 / 9.0 / 9.1 aliases)

// LVGL's heap in PSRAM: internal RAM is needed by Wi-Fi / lwIP (lv_mem_psram.c; plain malloc on the desktop)
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CUSTOM
#define LV_USE_STDLIB_STRING LV_STDLIB_CLIB
#define LV_USE_STDLIB_SPRINTF LV_STDLIB_CLIB

#define LV_DEF_REFR_PERIOD 16

#if !defined(LV_SIM_HOST) && !defined(__ASSEMBLY__)
// LVGL's hot loops (blending, masks, letters) in internal RAM instead of flash behind the 16 KB instruction cache:
// ~10 % faster redraws for ~15 KB of internal RAM (paid for by trimmed task stacks)
#include "esp_attr.h"
#define LV_ATTRIBUTE_FAST_MEM IRAM_ATTR
#endif

// draw only into what the UI uses: RGB565 (the panel), A8 (glyph / mask coverage) and ARGB8888 (LVGL's own scratch);
// no images, no layers with opacity. Less code for the 16 KB instruction cache (and for LV_ATTRIBUTE_FAST_MEM).
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED 0
#define LV_DRAW_SW_SUPPORT_RGB565A8 0
#define LV_DRAW_SW_SUPPORT_RGB888 0
#define LV_DRAW_SW_SUPPORT_XRGB8888 0
#define LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED 0
#define LV_DRAW_SW_SUPPORT_L8 0
#define LV_DRAW_SW_SUPPORT_AL88 0
#define LV_DRAW_SW_SUPPORT_I1 0
// one software draw unit, no OS: two draw units on FreeRTOS were slower on the ESP32-S3 (Ready 137 ms vs 100) and
// took ~40 KB more internal RAM (their task stacks)
#define LV_USE_OS LV_OS_NONE

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 0
#define LV_USE_ASSERT_MALLOC 0
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0
#define LV_USE_SYSMON 0
#define LV_USE_OBSERVER 0
// object names: the tags the shared UI scenarios look for (docs/ui-features.md); one pointer per named object
#define LV_USE_OBJ_NAME 1

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1   // the example UI (src/ui_minimal); unused fonts are dropped at link time
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

#define LV_USE_THEME_DEFAULT 0
#define LV_USE_THEME_SIMPLE 0
#define LV_USE_THEME_MONO 0
#define LV_USE_FLEX 1
#define LV_USE_GRID 0

/* widgets: only what the UI uses */
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_IMAGE 1
#define LV_USE_BUTTON 1
#define LV_USE_ANIMIMG 0
#define LV_USE_ARC 0
#define LV_USE_ARCLABEL 0
#define LV_USE_BAR 0
#define LV_USE_BUTTONMATRIX 0
#define LV_USE_CALENDAR 0
#define LV_USE_CANVAS 0
#define LV_USE_CHART 0
#define LV_USE_CHECKBOX 0
#define LV_USE_DROPDOWN 0
#define LV_USE_IMAGEBUTTON 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 0
#define LV_USE_LIST 0
#define LV_USE_MENU 0
#define LV_USE_MSGBOX 0
#define LV_USE_ROLLER 0
#define LV_USE_SCALE 0
#define LV_USE_SLIDER 0
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 0
#define LV_USE_SWITCH 0
#define LV_USE_TEXTAREA 0
#define LV_USE_TABLE 0
#define LV_USE_TABVIEW 0
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0
#endif
