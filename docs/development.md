# Development

How to build, test and debug Brewdeck. How the parts fit together: [architecture](architecture.md). Writing a UI of
your own: [custom-ui.md](custom-ui.md).

## Without hardware

Needs a C++ compiler, git and network on the first build (LVGL 9.6.0 is cloned into `sim/lvgl`), SDL2 for the
emulator (`brew install sdl2`) and Pillow for the PNG conversion (`pip install pillow`).

| What | Command |
|---|---|
| Unit tests (parsers, decoders, command rules, against real machine replies in `sim/data/`; ASan + UBSan) | `tests/run.sh` |
| Renders of every screen to PNG | `sim/build-render.sh && sim/render sim/out && python3 sim/topng.py sim/out` |
| Emulator: the real UI in a desktop window, fake machine | `sim/build-emu.sh && sim/emu` |
| Web demo (WebAssembly; needs `brew install emscripten`) | `sim/web/build.sh`, then `python3 -m http.server -d sim/web/dist` |
| User manual GIFs and stills (after a UI change; build the emulator first) | `python3 tools/gen_manual.py` |
| A fake machine with the 24 longest-named community profiles | `python3 -I tools/community_profiles.py <dir>`, then `EMU_DATA=<dir> sim/emu` |

The fake machine plays a recorded shot and a recorded steam session. Emulator keys: `S` shot, `M` steam, `O` machine
offline, `N` no Wi-Fi, `H` heating, `B` scale on / off, `P` scale type, `L` water level, `I` idle clock, `R` reload
profiles, `Q` quit. The control panel next to the screen has the same actions and "Go to screen" shortcuts (Brewdeck's
UI only). `EMU_SHOT_CUT=5` cuts the shot after 5 s; `EMU_IGNORE=1` makes the fake machine ignore commands.

For scripts the emulator reads commands on stdin, one per line: `key s`, `tap x y`, `drag x1 y1 x2 y2`, `hold x y ms`,
`click x y` (window coordinates), `button <panel label>`, `wait ms`, `save f.raw` (RGB565, `sim/topng.py` converts it),
`savewin f.bmp`, `redraw` (the whole screen; a `save` before and after checks that partial redraws left nothing
behind), `quit`.

## Building the firmware

Prerequisites and the first build: README, [Building](../README.md#building). In `firmware/`:

| Env | What |
|---|---|
| `ui` | release build for the Sunton ESP32-S3 |
| `ui-debug` | the same plus the debug web server (below) |
| `ui-p4`, `ui-p4-debug` | the Guition ESP32-P4 board |
| `p4-c6-update` | one-off: updates the P4 board's ESP32-C6 Wi-Fi firmware (`src/c6update`) |
| `touchtest` | raw touch test for the S3: shows touch coordinates, tap the corners to check the axes |

`./build.sh <env>` builds, `--flash` flashes through `flash.sh`; `UI=<folder>` builds another UI folder. Options are
in the header of [`build.sh`](../firmware/build.sh). Switching between S3 and P4 builds recompiles the Arduino
libraries (2-3 minutes).

## Serial log

115200 baud over USB: `/dev/cu.usbserial-*` (S3) or `/dev/cu.usbmodem*` (P4) on macOS, `/dev/ttyUSB*` /
`/dev/ttyACM*` on Linux. On macOS the port forgets its speed when closed, so keep it open while reading:

```sh
(stty 115200 raw -echo; cat) < /dev/cu.usbserial-XXXX
```

Each line starts with the time (the clock once NTP has set it, else `up <seconds>`). The first lines say why the
screen started (power-on, crash, watchdog) and which core and UI API versions it runs.

## Debug web server

Debug builds (`ui-debug`, `ui-p4-debug`) run a small web server at `http://brewdeck.local` (P4: `brewdeck-p4.local`).
It has no password and can press the screen: use it only on a network you trust. Use `curl -4`: macOS otherwise
waits about 5 s for IPv6 over mDNS.

| Route | What |
|---|---|
| `/screen.bmp` | screenshot of the frame buffer (`tools/screenshot.sh out.png`) |
| `/rec?every=ms[&max=n]`, `/rec`, `/rec/stop`, `/rec.bin`, `/rec/free` | screen recorder: frames kept in PSRAM, downloaded at the end (`tools/record.sh`) |
| `/heap`, `/tasks`, `/flash` | free memory and Wi-Fi link, stack left per task, flash mode and read speed |
| `/touch?x=&y=&ms=` | presses the screen |
| `/roam` | starts a Wi-Fi roaming check (result in the serial log) |
| `/i2c` | every address that answers on the touch controller's I2C bus, with a guess at what it is (audio codec, ...) |
| `/bl?pct=&hz=` | sets the backlight level and PWM frequency |

A UI can add one route of its own with `bd_debug_route` (Brewdeck's UI: `/bench`).

`tools/record.sh` records a shot without loading the Wi-Fi during it:

```sh
HOST=brewdeck-p4.local tools/record.sh start 500   # a frame every 500 ms, until stopped or memory is full
tools/record.sh status                             # frames so far
tools/record.sh fetch rec-shot                     # stop, download, write rec-shot/frame-NNNN-<s>s.png
tools/record.sh free                               # give the memory back
```

Frames are copied while the UI may be drawing, so a changing value can look torn in a frame; the panel itself is fine.

`tools/rssi_log.sh` logs the Wi-Fi signal (from `/heap`) to compare antennas or places, and prints all its runs side by
side:

```sh
HOST=brewdeck-p4.local OUT=antenna tools/rssi_log.sh none 5        # 5 min, a read every 2 s -> antenna/rssi-none.csv
HOST=brewdeck-p4.local OUT=antenna tools/rssi_log.sh fpc-3cm 5     # same place, another antenna: the table compares both
```

Compare runs on the same access point (the `ap` column): the screen may join or roam to another one.
