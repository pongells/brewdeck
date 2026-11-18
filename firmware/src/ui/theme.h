// Brewdeck design tokens: the only fonts, text colours and radii the screens use. See docs/design-system.md.
#pragma once
#include <lvgl.h>

extern "C" {
LV_FONT_DECLARE(it_200_120);   // digits, '.' and ':' only
LV_FONT_DECLARE(it_300_40);
LV_FONT_DECLARE(it_300_28);
LV_FONT_DECLARE(it_400_20);
LV_FONT_DECLARE(it_600_20);
LV_FONT_DECLARE(it_400_16);
LV_FONT_DECLARE(it_600_16);
LV_FONT_DECLARE(it_400_13);
LV_FONT_DECLARE(it_200_64);    // only "B r e w d c k" (the splash's name)
}

// ---- type scale: six steps, three weights (200 hero, 300 large numbers, 400 text, 600 emphasis) ----
#define F_HERO    (&it_200_120)   // the one big number: Ready temperature, keypad entry, idle clock
#define F_VALUE   (&it_300_40)    // live / review values, clocks, Brew temp / Stop weight (+ U+221E), keypad keys
#define F_TITLE   (&it_300_28)    // page titles, the shot result, offline title, idle status line
#define F_ROW     (&it_400_20)    // list rows, keyboard keys, units next to a Value
#define F_ROW_B   (&it_600_20)    // the selected row, primary button text (Done, Join)
#define F_TEXT    (&it_400_16)    // labels, captions, notices, details (+ U+221E: no stop weight)
#define F_TEXT_B  (&it_600_16)    // pills, buttons, toggles
#define F_SMALL   (&it_400_13)    // chart axis, tags
#define F_SPLASH  (&it_200_64)    // the splash name only

// ---- colours ----
#define COL_BG      lv_color_hex(0x0A0A0B)
#define COL_TEXT    lv_color_hex(0xF2F2F3)   // text: values, names
#define COL_MUTED   lv_color_hex(0x8E8E96)   // text: labels, units, secondary
#define COL_OUTLINE lv_color_hex(0x5E5E66)   // text: disabled, offline, compared values, axis; also icon strokes
#define COL_FAINT   COL_OUTLINE
#define COL_AXIS    COL_FAINT
#define COL_CARD    lv_color_hex(0x1A1A1D)   // surfaces: buttons, cards
#define COL_CARD_PR lv_color_hex(0x2A2A2F)   // pressed
#define COL_LINE    lv_color_hex(0x1E1E21)   // dividers, chart baseline
#define COL_GOLD    lv_color_hex(0xF5C04A)
#define COL_GOLD_PR lv_color_hex(0xD9A93C)
#define COL_BLUE    lv_color_hex(0x7CC4FF)
#define COL_GREEN   lv_color_hex(0x4ADE80)
#define COL_RED     lv_color_hex(0xFF6B6B)   // water temperature (coral: the deep #E5484D looked dark on black)
#define COL_PRESS   COL_BLUE   // one colour per value: pressure blue, flow gold, temperature red, weight white
#define COL_FLOW    COL_GOLD
#define COL_PILL_BG lv_color_hex(0x10291A)   // Ready / Done pill
#define COL_PILL_TX lv_color_hex(0x7EE2A8)
#define COL_WARN_BG lv_color_hex(0x33200F)   // needs attention
#define COL_WARN_TX lv_color_hex(0xFFC089)
#define COL_WARN    lv_color_hex(0xFFA94D)
#define COL_BREW_BG lv_color_hex(0x3A2E10)   // Brewing pill
#define COL_BREW_TX lv_color_hex(0xFFDD8A)
// a toggle's selected segment (review, Display page), the selected profile row, tags
#define COL_PK_BTN  lv_color_hex(0x26262A)
#define COL_PK_SEL  lv_color_hex(0x202024)
#define COL_PK_TAG  lv_color_hex(0x2C2C31)
#define COL_IDLE_TX lv_color_hex(0x7A7A82)   // idle clock line (dimmed screen; the clock itself takes IDLE_TINTS)

// ---- radii ----
#define RAD_CTRL  14                  // buttons and other controls
#define RAD_INNER 11                  // a segment inside a control (RAD_CTRL minus its 3 px padding)
#define RAD_CARD  20                  // cards and panels
#define RAD_PILL  LV_RADIUS_CIRCLE    // pills and tags (information, never a control)
