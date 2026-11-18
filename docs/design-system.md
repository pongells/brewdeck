# Brewdeck design system

The rules every screen follows. The tokens live in `firmware/src/ui/theme.h`; use them by name (`F_VALUE`,
`COL_MUTED`, `RAD_CTRL`), never as raw fonts, colours or numbers, so a change in one place changes every screen.

## Principles

- **Read at a glance from a metre away.** Each screen answers one question (can I pull now, what is the shot
  doing, how did it go). One big number per screen; everything else stays quiet.
- **One colour per value.** Pressure is blue, flow gold, water temperature red, weight white, on every screen and in
  every chart. A colour never changes within a value.
- **Controls and information look different.** Things you can tap are rounded rectangles on a card background.
  Information is plain text, and becomes a pill only while it needs attention.
- **Attention is orange.** Refill, no scale, pressure left in the system, the offline screen's problem badge, warning
  notices and actions that failed ("Could not join", "Machine busy"). Nothing else is orange.
- **Few sizes, few weights.** A new text element takes one of the roles below. If none fits, the layout is
  wrong, not the scale.

## Type

One family, [Inter Tight](https://github.com/rsms/inter), in six steps and three weights (four with the hero's
200). Weight 300 is for large numbers, 400 for text, 600 for emphasis (pills, buttons, the selected row).

| Token | Size / weight | Use |
|---|---|---|
| `F_HERO` | 120 / 200 | the one big number: Ready temperature, keypad entry, idle clock (digits, `.` and `:` only) |
| `F_VALUE` | 40 / 300 | live and review values (brewing, steam, shot review), clocks, Brew temp and Stop weight, keypad keys |
| `F_TITLE` | 28 / 300 | page titles, the shot result, the offline title, the idle status line |
| `F_ROW` | 20 / 400 | list rows (menu, profiles, Wi-Fi, history), keyboard keys, units next to `F_VALUE` |
| `F_ROW_B` | 20 / 600 | the selected row, primary button text (Done, Join), Ready's profile name, the restore panel's title |
| `F_TEXT` | 16 / 400 | labels, captions, notices, details, units next to `F_TITLE` |
| `F_TEXT_B` | 16 / 600 | pills, buttons, toggles |
| `F_SMALL` | 13 / 400 | chart axes, tags, the Profiles column header, small notes in lists |
| `F_SPLASH` | 64 / 200 | the splash name only (the letters of "Brewdeck") |

Rules:

- **Sentence case everywhere.** No capitals for labels or tags.
- **A unit sits on its number's baseline**, one step smaller and muted: `21.0 g`, `90 °C`. In a bottom-aligned
  flex row use `base_pad(big, small)` as the unit's bottom padding; it is computed from the font metrics.
- **A value's dot is centred on its digits:** `digit_dot(row, font, colour, 8)`.
- **Separators:** ` · ` between short facts in one line (`19 phases · 21 g · 90°`).
- The big fonts hold only the glyphs listed in `theme.h`: never render other characters in them.

### Adding or changing a font

The fonts are generated with [lv_font_conv](https://github.com/lvgl/lv_font_conv) from the
[@fontsource/inter-tight](https://fontsource.org/fonts/inter-tight) files; the exact options are in each file's
header. For example:

```sh
npm i lv_font_conv @fontsource/inter-tight
npx lv_font_conv --bpp 4 --size 20 --font node_modules/@fontsource/inter-tight/files/inter-tight-latin-400-normal.woff \
  -r 0x20-0x7E,0xB0,0xB7 --format lvgl --no-compress --lv-font-name it_400_20 --lv-include lvgl.h -o firmware/src/ui/fonts/it_400_20.c
python3 tools/font_contrast.py firmware/src/ui/fonts/it_400_20.c   # text fonts (13-40 px): steeper edges, sharper on the panel
```

The 40 px value font and the 16 px text font also carry "∞" (U+221E, no stop weight). The @fontsource subsets do not
have it, so it comes from the complete Inter Tight (Google Fonts, `ofl/intertight/InterTight[wght].ttf`, same SIL
Open Font License), cut to the font's weight with fonttools: `fonttools varLib.instancer InterTight[wght].ttf
wght=300 -o full/InterTight-300.ttf` (`wght=400` for the 16 px text font), then a second
`--font full/InterTight-300.ttf -r 0x221E` in the command above.

The text fonts get steeper antialiased edges (`tools/font_contrast.py`, noted in the header as "Edge contrast"): with
plain coverage the light and regular weights looked soft on the 4.3" 800x480 panel. The big digits and the splash
name are left as converted.

Declare it in `theme.h` and give it a role there. A new size needs a good reason: prefer an existing step.

## Colour

| Token | Hex | Use |
|---|---|---|
| `COL_BG` | `#0A0A0B` | screen background |
| `COL_CARD` | `#1A1A1D` | controls (buttons, steppers, keys), cards |
| `COL_CARD_PR` | `#2A2A2F` | a control while pressed |
| `COL_LINE` | `#1E1E21` | dividers, chart baseline |
| `COL_TEXT` | `#F2F2F3` | values, names, button text |
| `COL_MUTED` | `#8E8E96` | labels, units, secondary text |
| `COL_FAINT` | `#5E5E66` | disabled and offline text, compared values, chart axes |
| `COL_BLUE` | `#7CC4FF` | pressure (and water), information |
| `COL_GOLD` | `#F5C04A` | flow, the primary button (Done, Join, the likely fix on the offline screen) |
| `COL_RED` | `#FF6B6B` | water temperature (error notices keep a deeper `#E5484D` dot) |
| `COL_GREEN` | `#4ADE80` | ready, connected |
| `COL_WARN` | `#FFA94D` | needs attention (with `COL_WARN_BG` / `COL_WARN_TX` for a pill) |

Pills have their own pairs: `COL_PILL_BG` / `COL_PILL_TX` (Ready, Done), `COL_BREW_BG` / `COL_BREW_TX` (Brewing).
Aliases: `COL_PRESS` = blue and `COL_FLOW` = gold (use them for those values), `COL_OUTLINE` / `COL_AXIS` = `COL_FAINT`;
`COL_GOLD_PR` is the gold button pressed; `COL_PK_BTN` / `COL_PK_SEL` / `COL_PK_TAG` are a toggle's selected segment,
the selected profile row and tags. A few tints stay local to their screen and are not tokens: the other state pills
(Heating, Standby, Maintenance), the notice colours, the offline icon's grey, the flush button's blue. The idle clock
is dimmed on purpose (`COL_IDLE_TX`, and its tints `IDLE_TINTS` in `screens/idle.cpp`). The panel shows 16-bit colour: large soft
gradients band, so use flat colour (the release glow dithers).

## Shape

| Token | Value | Use |
|---|---|---|
| `RAD_CTRL` | 14 | every control: buttons, steppers, keys, the Profiles button, a selected row |
| `RAD_INNER` | 11 | a segment inside a control (a toggle's selected half: `RAD_CTRL` minus its 3 px padding) |
| `RAD_CARD` | 20 | cards and panels (the notices list) |
| `RAD_PILL` | round | pills and tags: information, never a control |

No shadows, and no borders on the screens themselves: surfaces are told apart by colour. Panels that float over
another screen (the notices list, a centred notice, the restore panel) and the password field get a 1 px `#3A3A40`
edge, so they stand off what is behind them.

## Logo

Three wisps in the value colours (blue pressure, gold flow, coral temperature) on the controls' card square
(`COL_CARD`, radius 22 on a 96 grid). Source: [`docs/images/logo.svg`](images/logo.svg); also `logo-512.png` (avatar)
and `social-card.png` (1280 x 640, the repository's social preview). On the screen it is drawn, not an image
(`logo()` in `screens/common.cpp`, same geometry): 96 px on the splash, 32 px in About. Keep it on the dark background; do not
recolour the wisps.

### Offline icons

The offline screen's pictures share the logo's tile (`COL_CARD`, 112 px, radius 26) and its 96-unit grid: a cup on a
saucer with the logo's three wisps as steam, and the Wi-Fi arcs. While trying they move (wisps rise and fade in turn,
2.4 s; arcs light up from the inside, 1.8 s); when something is wrong the drawing turns grey and a 30 px badge sits on
the tile's corner: orange "?" / "!" for a problem, gold "+" for something to set up. The title stays white. The cup
and saucer are pre-rendered (LVGL draws no ellipses): `python3 tools/gen_cup.py` writes `screens/cup_art.h`; the steam
and arcs are drawn live. Positions are fixed in every state (tile 72, title 212, text 256, buttons 328, details 430),
so nothing moves when the buttons change; the splash uses the same details line for its build date.

## Layout

- **800 x 480, margin 24 px** on every side.
- **Header:** at y 24, 52 px tall: status pill or back button and title on the left, 52 px buttons on the right
  (on Ready: the notices bell, *Profiles* and the settings cog; elsewhere close or scan).
- **Content** starts at y 96: list pages, charts, the profile preview.
- **Shot chart:** brewing and the shot review draw the same chart (752 x 288 at y 96, baseline 261, the same
  scales, grid at 3 / 6 / 9), so the lines stay where they are when the review opens after a shot.
- **Rows:** 56-64 px for lists you pick from (profiles 56, Wi-Fi and shot history 60, menu 64), 44-60 px for details
  (About 44, Wi-Fi details 52, Scale 60), dividers in `COL_LINE`.
- **Values row** (brewing, steam, review): one row at y 404 under a full-width chart, on four equal columns
  (the review's scrub time is drawn on the chart, on top of the scrubber). Each value: dot, number, then what it is compared
  with in `COL_FAINT` and the unit: `1.3 / 21.0 g` (stop weight), `153 / 155 °C` (steam set point),
  `2.4 / 3.6 bar` (target or previous shot).
- **Touch targets:** at least 48 px; steppers and keys 64-82 px (the Display page's small steppers 52 px). A few
  secondary controls are smaller: the Profiles page's Restore (88 x 36), the review's toggle (38 px tall), the password
  field's eye (40 px).
- **No divider lines between areas:** spacing separates them. Lines are for lists (between rows) and chart baselines.

## Components

- **Stepper** (Brew temp, Stop weight): one control, a single `COL_CARD` card with three parts, `−`, the value
  (tap to type) and `+`, each darkening to `COL_CARD_PR` while pressed. The label sits above it in `F_TEXT` /
  `COL_MUTED`.
- **Status readout** (Ready footer): icon, name in `F_TEXT` / `COL_MUTED`, value in `F_TEXT_B` / `COL_TEXT`: the
  number is the information. While it needs attention it becomes an orange pill (`COL_WARN_BG` / `COL_WARN_TX`).
- **Status pill** (Ready, Heating, Brewing, Done, Steam): a dot and a word, round ends, never an icon.
- **List row** (menu): 64 px, a 24 px line icon in `COL_BLUE`, the name in `F_ROW`, the status in `F_TEXT` /
  `COL_MUTED` and a chevron on the right; hairlines between rows. Icons are drawn with lines and arcs
  (`line_icon`, 2 px strokes with round ends), not images.
- **Empty state** (a list with nothing to show): a 32 px line icon in `COL_MUTED`, a title in `F_ROW` and one line in
  `F_TEXT` / `COL_MUTED` saying what happens next ("They come from the machine, a few seconds after it answers.").
- **Colour key:** a 14 x 3 px swatch in the line's colour and its name in `F_TEXT` / `COL_MUTED` (Ready preview).

## Charts

- Solid line = measured, or what the phase drives. Dashed = compared with (the target or the previous shot).
- In the Ready preview: bright 3 px = what the phase drives, dim 2 px = only a limit.
- Quiet axes: time labels under the baseline in `F_SMALL` / `COL_FAINT` (no "0" at the start), hairline grid
  lines, the scale numbers at the right end.
- The live end of each line has a dot in the value's colour.
- A secondary value (pressure on the steam screen) is drawn thinner and at about 45 % (mixed with the background);
  its dot and its value under the chart keep the full colour.

## Writing

- Plain words from the user's side: "Stop weight", not "target weight"; "Machine not found", not an error code.
- Say what to do: an error names the fix ("Machine address", "Try again").
- Brewdeck is unofficial: it never claims to be part of Gaggiuino.
