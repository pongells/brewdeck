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
behind), `quit`. Lines starting with `#` are comments. Every command the UI asks the core for is printed as
`ui -> core: ...` (and written to the file in `EMU_CMDLOG`); two checks look at them: `expect_no_cmd` (no command
since the previous check; reads of stored shots do not count) and `expect_cmd <text>` (a command starting with that
text, e.g. `expect_cmd select id=7`). Checks print PASS / FAIL, and the emulator exits with 1 if one failed.
Tags (the names a UI gives its objects, [ui-features.md](ui-features.md#how-the-checks-work)): `tags` lists them
(shown / covered / hidden, position, text), `tap_tag <tag>`, `hold_tag <tag> <ms>`, `expect_visible <tag> [ms]`,
`expect_hidden <tag> [ms]`, `expect_text <tag> <a|b> [ms]` (waiting up to ms, default 1.5 s), `wait_for <tag> [ms]` (waits until shown; not a check, and goes on at once when the UI has no such tag). A tag may hold spaces
(`row.wifi.Home Wi-Fi 2`); one that ends in a number needs the time given. `type <tag> <text>` sends each character
to the shown input as an LVGL key event (`LV_EVENT_KEY`, as a hardware keyboard would), `clear <tag>` sends
`LV_KEY_BACKSPACE` 80 times. `expect_backlight <op> <pct> [ms]` checks the last level the UI set (`<`, `<=`, `=`,
`>=`, `>`). `fast <n>` runs the UI's clock n times faster (not the fake machine's recordings; 1x during a tap), and
waits and check times count in screen time: after `fast 60`, `wait 600000` is 10 minutes on the screen and 10 s
here. A tag the UI does not have (still absent when a check's time is up) prints `MISSING <tag>` and ends the
script (exit code 3). `machine shot [10]` / `machine steam [10]` start the
recorded shot / steam on the fake machine without touching the screen (the panel buttons go back to Ready first);
`machine link off|on` makes it unreachable while it goes on (the panel's "Machine on/off" switches it off, stopping a
shot or steam); `machine readiness <n>` reports that readiness (-1: its own again); `machine temp <c>|auto` holds
the boiler temperature; `machine profile <id>` picks a profile as the web UI would. `tap_under <tag>` taps where the
tag is even when something covers it (the idle clock, screen off). `machine ignore on|off`: the machine ignores
commands (as `EMU_IGNORE=1`). `back_to_ready [taps]` (a check) taps the shown Back, else Close, until Ready shows.
`if_shown <tag> : <command>` / `if_hidden <tag> : <command>` run a command only then (UIs reach a control in
different ways). `repeat <n>` ... `end` runs the lines between n times. `mem_mark` / `expect_mem_growth <KB>` check
the heap in use. `expect_order <tag> <tag> ...` checks that they are shown top to bottom in that order;
`expect_touch_targets <px>` that every shown object a tap acts on (clickable, with an event handler) is at least px
square, its extra click area included. `expect_text` looks at all the shown labels under the tagged object; `expect_no_text <tag> <a|b> [ms]` checks it
holds none of them. `expect_cmds <n> <command ...>`: 1 to n commands of that kind since the previous check, the
last one starting with that text (a burst of taps). Fake machine: `machine mode <MODE>` (FLUSH, DESCALE, HOME),
`machine hotwater on|off`, `machine stopweight <g>` (the active profile changed elsewhere; 0 = none). `update` plays
a firmware update arriving over Wi-Fi (`BD_EV_UPDATE`: 1.6 MB in 6 s, installed, the page closes 2.5 s later as at the
restart); `update stop` fails it at 38 %.

**Shared UI scenarios.** `tests/ui/run.sh [ui]` runs every script in `tests/ui/scenarios` against a UI (a folder
under `firmware/src`, default Brewdeck's `ui`, or a path to a UI folder) without a window, then prints the UI's
status table: per feature of [ui-features.md](ui-features.md) done / partial / FAILED / missing / omitted / not
checked yet / by hand, with Required features that are missing or failing listed first. It reads the UI's exceptions
from `ui-features.txt` in its folder (format and rules: [Leaving features out](ui-features.md#leaving-features-out))
and writes `status.md`, `sheet.html` (the screenshots each scenario takes with `shot <caption>`, plus one at every
failed check, for the checks done by looking), the logs and the UI's commands to `tests/ui/out/<emulator>/`. Each scenario names what it
checks in a `# checks: X1` line, and is skipped when all of those are omitted or missing. The scenarios run side by
side (`JOBS=<n>`, default two per CPU core; `JOBS=1` one by one): a full run takes about 2 minutes. `SCENARIO=x1` runs only
the scenarios whose file name starts with `x1`. A full run takes ~4 min.

## Building the firmware

Prerequisites and the first build: README, [Building](../README.md#building). In `firmware/`:

| Env | What |
|---|---|
| `ui` | release build for the Sunton ESP32-S3 |
| `ui-debug` | the same plus the debug web server (below) |
| `ui-p4`, `ui-p4-debug` | the Guition ESP32-P4 board |
| `p4-c6-update` | one-off: updates the P4 board's ESP32-C6 Wi-Fi firmware (`src/c6update`) |
| `touchtest` | raw touch test for the S3: shows touch coordinates, tap the corners to check the axes |

`./build.sh <env>` builds, `--flash` flashes through `flash.sh`; `UI=<folder>` builds another UI folder (a folder name under `src/`, or a path to one outside the repository). Options are
in the header of [`build.sh`](../firmware/build.sh).

Both boards are **plain ESP-IDF projects** (`framework = espidf`). ESP-IDF and the components named in
`src/idf_component.yml` (mDNS; on the P4 ESP-Hosted and esp_wifi_remote) are compiled from source together, in the env's
own build folder. Two small tools are plain Arduino builds instead: `touchtest` and `p4-c6-update`. Settings:
`sdkconfig.base.<chip>` (the settings the firmware ran on while it was built on the Arduino core, as the lines that
differ from ESP-IDF's defaults; how it was made is in its header), plus `sdkconfig.brewdeck.<chip>`:
- **S3** (`sdkconfig.brewdeck.esp32s3`): octal PSRAM, and what keeps the RGB panel's picture steady while Wi-Fi is busy
  (code and read-only data run from PSRAM, 64 B data cache line, an IRAM-safe panel interrupt, Wi-Fi / lwIP buffers in
  internal RAM); heap and ring buffer code out of IRAM, for internal RAM.
- **P4** (`sdkconfig.brewdeck.esp32p4`): XIP from PSRAM (writing flash, as an update does, otherwise paused the panel's
  reads and it flashed light blue), ESP-Hosted receive double buffering off (internal RAM).

Component versions are locked in `dependencies.lock.<chip>` (commit it; delete it to take the newest allowed versions,
then test). `src/CMakeLists.txt` picks the sources (core, board, UI; `ui_select.py` passes the choice). `build.sh`
makes the generated `sdkconfig.<env>` again when `sdkconfig.brewdeck.*` changes. First build of each env: a few
minutes. The S3 touch test (`touchtest`) is a plain Arduino build on the stock prebuilt libraries.

### Over Wi-Fi (debug builds)

A screen running a debug build takes a new debug build over Wi-Fi, without a cable:

```sh
./build.sh ui-p4-debug --ota            # HOST=<name or address> for another screen (default brewdeck-p4.local / brewdeck.local)
```

The image goes into the free app slot and the screen restarts into it. It keeps the new firmware once it has run
properly for 60 s (UI loop alive, Wi-Fi up); if the UI loop stops for 20 s, or Wi-Fi is not up within 3 minutes, it
goes back to the previous one by itself, so a bad build cannot lock you out. `build.sh` waits and says which happened
(`GET /heap`, line `firmware:`). Refused while a shot, steam, hot water or a flush runs. To check the way back:
`PLATFORMIO_BUILD_FLAGS=-DOTA_TEST_FREEZE ./build.sh ui-p4-debug --ota` sends a build whose UI loop stops after 15 s;
the screen returns to the previous firmware about 20 s later (`/log/prev` says why).

The screen asks for a password: `include/ota_token.h`, made by `build.sh` on first use (git-ignored, never printed).
A screen learns it from one flash by USB; after that only builds with the same file are accepted. Release builds have
no web server and take no updates over Wi-Fi.

## Serial log

115200 baud over USB: `/dev/cu.usbserial-*` (S3) or `/dev/cu.usbmodem*` (P4) on macOS, `/dev/ttyUSB*` /
`/dev/ttyACM*` on Linux. On macOS the port forgets its speed when closed, so keep it open while reading:

```sh
(stty 115200 raw -echo; cat) < /dev/cu.usbserial-XXXX
```

Each line starts with the time (the clock once NTP has set it, else `up <seconds>`). The first lines say why the
screen started (power-on, crash, watchdog) and which core and UI API versions it runs.

### Without a cable (debug builds)

Debug builds also keep the log in memory and can send it over the network, so a problem that happens while the
screen is on the machine is still recorded:

- `GET /log`: the last 256 KB of the log (`/log?from=N` continues where an earlier read ended: `X-Log-Next`).
- `GET /log/prev`: the last 3 KB from before a crash or watchdog restart (kept in RTC memory; not over a power cut,
  a flash or a reset by USB).
- A log server: every line as a UDP datagram (syslog, RFC 5424). With Docker, VictoriaLogs keeps them and shows them
  in a web page (30 days):

  ```sh
  cd tools/victorialogs && docker compose up -d   # then http://<computer>:9428/select/vmui
  ```

  Query e.g. `hostname:brewdeck-p4 AND stalled`, or from a shell:
  `curl http://<computer>:9428/select/logsql/query -d 'query=_time:1h hostname:brewdeck-p4'`.
  Without Docker, `python3 tools/logserver.py logs` writes one file per screen and day (rsyslog works too).

  Then point the screen at it once: `curl -4 "http://brewdeck-p4.local/loghost?to=<computer's address>"` (saved on
  the screen; `?to=` with nothing turns it off), or set `LOG_HOST` in `wifi_config.h`. Port 5140 unless given
  (`to=host:port`). Lines written while Wi-Fi is down are sent when it is back; the log server stamps each line when
  it arrives, the screen's own time starts the text.

If the UI loop stops for 5 s, a line says where (`core: UI loop stalled for 5 s in "UI drawing"; last flush ...`),
written from another task so it reaches the log server; another when it runs again. A loop pass of 250 ms or more is
logged by section.

## Debug web server

Debug builds (`ui-debug`, `ui-p4-debug`) run a small web server at `http://brewdeck.local` (P4: `brewdeck-p4.local`).
It has no password and can press the screen: use it only on a network you trust. Use `curl -4`: macOS otherwise
waits about 5 s for IPv6 over mDNS.

| Route | What |
|---|---|
| `/screen.bmp` | screenshot of the frame buffer (`tools/screenshot.sh out.png`) |
| `/rec?every=ms[&max=n]`, `/rec`, `/rec/stop`, `/rec.bin`, `/rec/free` | screen recorder: frames kept in PSRAM, downloaded at the end (`tools/record.sh`) |
| `/heap`, `/tasks`, `/flash` | free memory, Wi-Fi link, uptime and the raw reset reason (`esp_reset_reason`); stack left per task; flash mode and read speed |
| `/touch?x=&y=&ms=` | presses the screen |
| `/frames` | frame times (`bd_ui_tick`) of the last 4 shot / steam sessions: count, longest, slow ones; `tools/frames.sh` after a shot says whether they were fast enough |
| `/roam` | starts a Wi-Fi roaming check (result in the serial log) |
| `/i2c` | every address that answers on the touch controller's I2C bus, with a guess at what it is (audio codec, ...) |
| `/ppatest?n=300&seed=1&hmin=1&hmax=40` | P4: rotates `n` random rectangles (heights `hmin`..`hmax`) with the 2D engine into a spare buffer (same settings as the display) and checks each against the CPU's rotation; lists wrong or unfinished copies (a copy that never finishes switches the display to the CPU path). Blocks the UI while it runs |
| `/flashtest?op=erase\|write&ms=5000[&pre=1&chunk=256]` | flash erases or writes alone on the free app slot, to see what the panel does meanwhile (longest pause in the answer) |
| `/ppatest?stall=1` | P4: the next 2D-engine copy hangs, as the known PPA bugs make it; checks that the screen switches to CPU rotation within 100 ms and keeps running (until a restart) |
| `POST /ota` | a new firmware image (`build.sh <env> --ota`, see [Over Wi-Fi](#over-wi-fi-debug-builds)) |
| `/log`, `/log/prev`, `/loghost?to=` | the memory log, the log from before a crash, the log server (see [Serial log](#serial-log)) |
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
