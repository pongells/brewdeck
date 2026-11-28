# Writing your own UI

Brewdeck's firmware is split into a **core** (Wi-Fi, finding the machine, its REST and WebSocket APIs, settings, and
every command sent to the machine), a **board** layer (panel, touch, backlight) and a **UI**. You can replace the UI
with your own and keep everything else: the core does the networking and the safety checks, your UI only draws and
reacts to touches. How the parts fit together: [architecture](architecture.md).

The API is one header, [`firmware/src/core/brewdeck_core.h`](../firmware/src/core/brewdeck_core.h) (version 1). This
page is the tour; the header is the reference.

## Start from the example

[`firmware/src/ui_minimal/minimal.cpp`](../firmware/src/ui_minimal/minimal.cpp) is a complete UI in about 150 lines
of LVGL 9: the link state, the machine's live values, the active profile, the running shot, and the profile list
where a tap selects a profile. It is under the MIT license, so you can copy it as the start of yours.

1. Copy the folder: `cp -r firmware/src/ui_minimal firmware/src/myui`.
2. In `BD_UI`, give it its own name (at most 15 characters: it is also the namespace of its saved settings).
3. Try it in the emulator: `UI=myui sim/build-emu.sh && sim/emu-myui`.
4. Build the firmware: `UI=myui ./build.sh ui` in `firmware/`. Add `--flash` to flash it: the script asks the
   board whether its flash is encrypted and writes accordingly, and wants a backup first
   ([README, Installing](../README.md#installing);
   `DRY_RUN=1` shows what it would do).

## Your own repository

The quickest start: [brewdeck-ui-template](https://github.com/pongells/brewdeck-ui-template), a GitHub template with
the example UI, scripts that fetch Brewdeck at a pinned release and build your UI into the emulator and the firmware,
and CI for both boards. Press "Use this template", then follow its README.

**Which Brewdeck you build against.** Pin a Brewdeck release tag, `v0.2.0` or later (v0.1.0 cannot build a UI from
outside the repository; v0.3.0 builds its emulator but not its firmware, fixed after v0.3.0). Within API version 1 every newer release keeps your UI compiling and working (see
"Compatibility" in `brewdeck_core.h`), so moving to a newer release is a one-line change; the release notes say when
the API gains something, and a new major version of the API would be announced there first. Building against `main`
works too, but it can change under you.

How it works, if you set it up yourself: give `UI` a path instead of a folder name, e.g.
`UI=../../my-ui ./build.sh ui` or `UI=../my-ui sim/build-emu.sh` (the emulator is then `sim/emu-my-ui`). A path is
anything with a `/` in it, taken from where you run the script. The firmware build compiles the folder as part of
the firmware's component. Include the core's headers by
name (`#include "brewdeck_core.h"`, `#include "bd_lvgl9.h"`): both builds put `src/core` on the include path.

Naming a UI in its own repository, so people can find it and tell UIs apart:
- **Repository:** `brewdeck-ui-<name>` (e.g. `brewdeck-ui-ledger`), with the GitHub topic `brewdeck-ui`.
- **`BD_UI` name:** `<name>` alone (at most 15 characters, lower case; it is also the namespace of the UI's saved
  settings, so keep it once people use the UI).
- **Emulator:** the binary is named after the UI's folder (`sim/emu-<folder>`; a folder called `ui`, as in the
  template, gives `sim/emu-ui-ext`).

Brewdeck's own UI (`firmware/src/ui`) is the large example: every screen, the settings, the shot review.
[Ledger](https://github.com/pongells/brewdeck-ui-ledger) is a complete UI in its own repository, built like the
template: a different look, its own fonts, every screen in one file.

## Getting your UI listed

[`uis.json`](../uis.json) lists UIs kept in their own repositories. Brewdeck's CI (the "UIs" workflow) builds each one
at its pinned commit against the **current** core, so a core change that would break a listed UI shows up before a
release: the emulator with the shared UI scenarios ([UI features](ui-features.md)), and the firmware for both boards.
Its status table is in that workflow's run summary.

To be listed, a UI must:
- live in a public repository named as above, with the `brewdeck-ui` topic and a license that lets Brewdeck's CI build
  it and offer the built image from the screen;
- use its `BD_UI` name as its `name` in `uis.json` (the screen finds its own entry by it, for updates);
- build against the current core (emulator, ESP32-S3 and ESP32-P4 firmware);
- pass **S1** (says it is unofficial) and **X1** (sends no command without a user action). Everything else in the
  table is reported, not required: an experimental UI is welcome.

Open a pull request adding your entry to `uis.json` (`tools/uis.py validate` checks it): `name` (your `BD_UI` name),
`title`, `repo` (`owner/name`), `commit` (a full commit SHA, never a branch: what was reviewed is what gets built),
`folder` (the UI folder in your repository, e.g. `ui`), `license` and a one-line `description` (title up to 23 and
description up to 95 characters, without `{ } [ ] " \\`: the screen shows them). A newer version of
your UI is another pull request moving the commit. If a listed UI stops building after a core change, the workflow
says so: a core bug gets fixed here, anything else is raised with the UI's author.

**On the screen.** Each release builds every listed UI against its core (in a job without the signing key), takes
pictures of it in the emulator with the fake machine (`tools/release-preview.sh`: its Ready screen, then brewing
during the recorded shot and steam during the recorded session; make sure those screens are tagged `screen.ready`,
`screen.brewing` and `screen.steam` and look good there; a screen it does not reach is left out) and signs a catalogue of them, `catalog-esp32p4.txt`. A P4 screen lists them under Menu > Interface (or your UI's own page, with
`bd_ui_catalog`): installing one replaces the firmware with that UI, keeps Wi-Fi and the machine's address, and
starts with the UI's own settings. A UI that does not build for a release is left out of that release's catalogue.
Updates follow the installed UI: a screen running yours gets your image of the next release.

## What a UI implements

```cpp
extern "C" const bd_ui_desc BD_UI = {"myui", "1.0", 1};   // name, your version, the API version you wrote it for
void bd_ui_init(void);                 // once at start: set up the display and build your screens
void bd_ui_tick(uint32_t now_ms);      // every loop pass (~5 ms): look at what changed, draw
void bd_ui_event(const bd_event *e);   // something happened: a shot point, a notice, a command's answer, ...
void bd_ui_wake(void);                 // the core wants the screen on (a shot, steam, a flush, a notice)
```

## The rules

- **One task.** The core calls your four functions on one task, and you call `bd_*` only from inside them (LVGL's
  callbacks run there too). No locks needed.
- **Keep `bd_ui_tick` short.** The core handles the machine's data between two ticks; a tick that takes 100 ms delays
  everything by 100 ms. Brewdeck's full-screen redraws take 50-100 ms; most ticks take less than one.
- **Pointers are borrowed.** What `bd_machine_state()`, `bd_profile_list()` and the events hand you is valid until the
  next tick, or until `bd_ui_event` returns. Copy what you keep.
- **Commands never block.** `bd_select_profile` and the others return at once, `BD_OK` (queued) or the reason they
  were refused (`BD_ERR_OFFLINE`, `BD_ERR_BUSY`, ...). The machine's answer comes later as `BD_EV_COMMAND`.
- **Only safe commands exist.** Select a profile, set the stop weight or brew temperature, flush, tare, and read
  stored shots. There is no pump, valve, heater, steam, descale or delete in the API, and the core sends a command
  only while the machine is online and idle, checking again just before it goes out. The core accepts a stop weight of
  0-200 g (0 = none) and a brew temperature of 1-105 °C; Brewdeck's own UI narrows the stop weight to 5-80 g.
- **Ignore what you do not know.** Later 1.x versions may add events, enum values and fields: skip unknown ones.
- **Say it is unofficial.** Brewdeck is not made by or affiliated with the Gaggiuino project, and neither is a UI
  built on its core: please say so in yours (a start screen, an About page, or a line like the example's).

## Reading the machine

| Call | What | Changes when |
|---|---|---|
| `bd_machine_state()` | link, live values (temperature, pressure, weight, water), readiness, mode, scale, the active profile as it runs, versions | `bd_seq_machine()` |
| `bd_profile_list()` | up to 24 profiles (the first 24 the machine reports; more are left out): name, active, stop weight, brew temperature, last used | `bd_seq_profiles()` |
| `bd_active_shape()` | the active profile's phases | `bd_seq_shape()` |
| `bd_profile_preview()` | those phases as polylines for a box of your size (pressure, flow, and their limits) | (computed on call) |
| `bd_shot_history()` | the machine's newest 50 stored shots | `bd_seq_history()` |
| `bd_live_shot()` | the running (or last) shot as recorded on this screen | `BD_EV_SHOT_*` events |
| `bd_network_state()` | the screen's Wi-Fi, its address, the machine's address | `bd_seq_network()` |
| `bd_core()`, `bd_local_time()` | API version, firmware version (`version`, e.g. "v0.2.0"), uptime, free memory, restart reason; the time (after NTP) | |

The `bd_seq_*` counters change whenever their data does: keep the last value you drew and redraw only when it moved.
Live values are meaningful only while `link == BD_LINK_ONLINE`.

## Events

| Event | When |
|---|---|
| `BD_EV_SHOT_START`, `BD_EV_SHOT_POINT`, `BD_EV_SHOT_END` | a shot begins, each new point (~7 per second), it ends |
| `BD_EV_NOTICE` | a machine notification (the core drops the repeated noise; the firmware-update notice is marked `update`) |
| `BD_EV_PRESSURE_RELEASE` | the machine counts down to releasing pressure ("water is coming"): light the screen up |
| `BD_EV_COMMAND` | the machine's answer to a command: done (it reports the change) or not taken (it did not within 5 s; 7 s for weight / temperature) |
| `BD_EV_SHOT_LOADED` | a stored shot asked for with `bd_load_previous_shot` / `bd_load_stored_shot` arrived |
| `BD_EV_WIFI_SCAN`, `BD_EV_WIFI_JOIN` | answers to `bd_wifi_scan` / `bd_wifi_join` |
| `BD_EV_UPDATE` | a firmware update being written: a release (`bd_release_install`; `version` names it) or, on debug builds, a push from the computer (`build.sh --ota`); show it above everything with its progress (`RECEIVING`), then `INSTALLED` (the screen restarts ~2 s later) or `STOPPED` (nothing changed); `NONE` = nothing to show. Optional (feature X7): a UI that ignores it still updates |

## The screen

- **With LVGL 9:** include [`bd_lvgl9.h`](../firmware/src/core/bd_lvgl9.h) in one file, call `bd_lvgl9_setup()` in
  `bd_ui_init` and `bd_lvgl9_tick(now)` in `bd_ui_tick`. LVGL's configuration is shared,
  [`firmware/include/lv_conf.h`](../firmware/include/lv_conf.h): enable what you need there (widgets, fonts). Fonts
  and widgets a UI does not use are left out of its firmware.
- **Without a graphics library:** draw RGB565 pixels into your own buffer and hand finished rectangles to
  `bd_display_flush(x1, y1, x2, y2, px)`; read touches with `bd_touch_read(&x, &y)`. `bd_display()` gives the size
  (800 x 480, landscape) and how many rows of a strip fit in fast memory.
- **Interfaces** (`BD_HAS_UI_CATALOG`, ESP32-P4): `bd_ui_catalog()` lists the interfaces of the newest release (Brewdeck's own is `screen`; `current` marks the running one), `bd_ui_install(name)` installs one (same rules and progress as a release install). `bd_ui_preview(name)` gives its pictures once downloaded: `frames` of 320 x 192 RGB565 one after another (Ready, then brewing and steam when the release has them); ask only while you show them (asking starts the download) and copy them to keep them on screen. Your UI does not have to offer this: holding the screen for 5 s while it starts always boots the previous firmware. Optional (feature X9).
- **Releases** (`BD_HAS_RELEASES`): `bd_release_info()` says whether a newer Brewdeck release is out (the core looks at start and once a day; `bd_seq_release()` changes with it), `bd_release_install()` installs it (refused with `BD_ERR_BUSY` while the machine is busy; progress as `BD_EV_UPDATE`). Ask for a confirmation first: the screen restarts. Optional (feature X8); ESP32-P4 only for now.
- **Brightness:** `bd_set_backlight(level)`: 0 = off, 20-100 = the panel's dimmest useful level up to fully on (the board maps levels onto its panel in steps that look even; below 20 = 20). Dimming and turning the screen off when idle are up to your UI;
  `bd_ui_wake` tells you when the core wants it on.

## Memory, settings, logs

- **Memory:** `bd_alloc(n, BD_MEM_PSRAM)` for anything big (charts, shot copies, images). Internal RAM is scarce: Wi-Fi
  needs it, so keep only small, hot things there.
- **Settings:** `bd_store_get_int` / `bd_store_set_int` and the `_blob` pair keep values in flash, in your UI's own
  namespace (keys at most 15 characters). Other UIs' settings and the core's (Wi-Fi, the machine address) are separate.
- **Logs:** `bd_log("...")` writes to the [serial log](development.md#serial-log) with the time in front. Debug builds
  also have a small [web server](development.md#debug-web-server); `bd_debug_route("/mypath", fn)` adds one page of
  your own to it.

## Testing

- **Emulator:** `UI=myui sim/build-emu.sh && sim/emu-myui` runs your UI in a window against a fake machine with
  recorded data ([keys and script commands](development.md#without-hardware)). The panel's "Fake machine" buttons
  work with any UI; its "Go to screen" buttons only with Brewdeck's.
- **Firmware:** `UI=myui ./build.sh ui` builds it; CI builds the example UI on every change, so a change to the
  core that breaks a UI shows up there.
- **On the screen:** a debug build (`UI=myui ./build.sh ui-debug`) takes screenshots, records the screen and
  accepts remote touches ([debug web server](development.md#debug-web-server)); it has no password, so use it only on
  a network you trust.
- **Shared checks:** `tests/ui/run.sh <your UI folder>` runs the same scenarios against every UI and prints your
  UI's status against the [feature list](ui-features.md): what is done, what failed, what is missing; it writes
  `status.md` and `sheet.html` (screenshots). In a repo made from the template, `./checks.sh` does it against the
  Brewdeck in `BREWDECK_REF` and writes `checks/`; the template's CI runs it on every push. They find your
  objects by **tags**, LVGL object names from the list's tag table (`lv_obj_set_name_static(obj, "screen.ready")`;
  `LV_USE_OBJ_NAME` is on in `lv_conf.h`). Tags change nothing on the screen. Name your screens, the values and
  warnings, and the buttons the scenarios press; a tag you leave out makes its feature count as missing, never as
  failed. Inputs (`input.password`, `input.address`) also need to take LVGL key events (`LV_EVENT_KEY`: a
  character or `LV_KEY_BACKSPACE`), which is how the scenarios type. Features you leave out on purpose go in `ui-features.txt` next to your code
  ([format and rules](ui-features.md#leaving-features-out)). The tag set is part of the API contract: tags are only
  added within 1.x, never renamed.

## License

The core, and the rest of Brewdeck, is under the [PolyForm Noncommercial License 1.0.0](../LICENSE): free for any
noncommercial use, not for selling or commercial products. The example UI is MIT. Your own UI's code is yours to
license as you like, but a firmware built with the core stays under the core's noncommercial terms.
