# Changelog

Each release has a section here; its text becomes the release notes on GitHub.

Versions follow [semantic versioning](https://semver.org): a release is a git tag `vMAJOR.MINOR.PATCH`. While Brewdeck is
0.x (early), a new minor version (0.2, 0.3) brings features and a patch version (0.2.1) only fixes; 1.0 comes when it
is ready for wider use. The screen shows its version on the splash and in Menu > About (between releases, e.g.
`v0.2.0-3-gabc1234`: 3 commits after v0.2.0). The UI API (`BD_API_VERSION` in `brewdeck_core.h`) has its own number,
which changes only when a custom UI would need changes.

## Unreleased

On the screen:
- ESP32-P4: other interfaces. Menu > Interface (a sixth menu row; the rows are a little lower) lists the interfaces of
  the newest release, Brewdeck's own and the UIs listed in `uis.json` (for now Ledger), each with a picture of its
  Ready screen; a row opens its details, where Install (tap twice) installs it like an update.
  Holding the screen for 5 s while it starts boots the previous firmware, whatever interface is installed.

For developers:
- API 1.x additions (`BD_HAS_UI_CATALOG`): `bd_ui_catalog`, `bd_ui_install`, `bd_ui_preview`, and `title` in the
  update event.
- Releases carry `catalog-esp32p4.txt` (signed) and an image per listed UI; the UIs are built in release jobs
  without the signing key.
- A UI kept in its own repository builds into the firmware again (since the move to ESP-IDF its files were compiled
  twice and the link failed; the emulator was fine). CI now builds an outside UI's firmware for both boards.
- `uis.json` lists UIs in their own repositories; the "UIs" workflow builds each at its pinned commit against the
  current core and checks it (docs/custom-ui.md, "Getting your UI listed"). First entry: Ledger.
- `BREWDECK_VERSION=...` overrides the firmware's version (tests of the updates over Wi-Fi).

## v0.3.0 (2026-11-27)

Install this version by USB once (as before, with your backup). On the ESP32-P4 board, also run the C6 update once
(INSTALL.txt): this version talks to the board's Wi-Fi chip with ESP-Hosted 3.1.0.

On the screen:
- ESP32-P4: updates over Wi-Fi. The screen looks for a newer release once a day and offers it in Menu > About (and
  once in the notices); Install (tap twice) downloads it, checks its signature and checksum, and restarts into it. If
  the new version does not run properly, the screen goes back to the old one by itself. Later releases install this
  way, without USB.
- ESP32-P4: the picture is turned by the chip's 2D engine instead of the CPU (full redraws ~42 -> ~15 ms), and the
  firmware runs from PSRAM, so writing flash no longer makes the panel flash light blue.
- ESP32-P4: the C6 update tool carries the C6 firmware itself (built from source, no network needed).
- Brewing temperature, stop weight, flush and tare are greyed out until the machine can take a change (right after it
  comes back online); the screen reconnects to the machine as soon as it answers again.
- Profile taps are no longer lost while the profile list refreshes.
- The profile preview estimates phase lengths from recorded shots (basket fill before pressure, the last phase sized to
  the stop weight).
- Brightness levels are spread evenly from each panel's dimmest useful level.
- ESP32-S3: fixed a rare hang when flash was read while the screen drew, and the picture jumping on full redraws.

Under the hood:
- Both boards are plain ESP-IDF projects: Wi-Fi, networking, display, touch, settings and updates use ESP-IDF
  directly instead of the Arduino core (more free memory, mostly on the S3). The build keeps its own PlatformIO folder,
  `~/.platformio-brewdeck`.

For developers:
- API 1.x additions (`BD_HAS_RELEASES`): `bd_release_info`, `bd_seq_release`, `bd_release_check`,
  `bd_release_install`, and `version` in the update event.
- Releases carry `brewdeck-<tag>-esp32p4.bin` and its signed description `update-esp32p4.txt` (`tools/sign-update.sh`).
- Debug builds: updates pushed over Wi-Fi (`build.sh <env> --ota`), a memory log, a log server, the release route
  `/release`.
- Shared UI scenarios (`tests/ui/run.sh`) check any UI against the feature list in `docs/ui-features.md`.

## v0.2.0 (2026-11-21)

On the screen:
- Profiles: the list is ordered by, and shows, when each profile was last selected (on the screen or in the web
  UI) or used for a shot; the column is now "Last selected".
- A Wi-Fi drop of a few seconds mid-shot no longer ends the shot early, and a resent point of a finished shot no
  longer opens a new brewing screen over its review.
- Dragging (the review's scrubber, scrolling) follows the finger more closely: touch is read right before each redraw.
- A network chosen from the "No Wi-Fi set up" or "Not connected" screen closes the menu once joined.
- A router that blocks or drops the screen no longer shows "did not accept the password": that message now needs the
  network to have refused the password before the screen's first join since it started.
- A dropped first Wi-Fi join is retried after 1 s instead of 15 s.
- The screen shows its version (the release tag, or how far past it a build is) on the splash, in Menu > About and
  in the serial log at start.
- No placeholder profile name on Ready: the name stays empty until the machine reports it.
- The brewing chart's time axis starts at 30 s and grows in 15 s steps (fewer full redraws during a shot).
- Shot history: a stored shot with a larger stop weight than the active profile's is drawn to its own scale.
- The review finds the previous shot when the machine's shot list missed the new shot (a WebSocket gap as the shot
  ended showed "Previous n/a"), and for profile names of 48 characters or more.
- Starting steam closes the last shot's review, so the steam screen shows at once.
- The steam chart redraws only its newest second while its scales hold (frames had grown from 40 to 90 ms over a
  session on the P4); its time axis is 1 min, then grows in 30 s steps; after a Wi-Fi gap it stays in step with the
  steam clock, and past 4 min its labels follow the rolling window. A set point changed in the web UI is redrawn.
- A pressure reading that is not a number no longer reaches the charts.
- After a profile is saved, its stored values are read again even if the first read fails.
- About no longer says "after a freeze" after flashing a P4 board: the system watchdog reset that ends every flash
  there now reads "after a system reset"; a freeze is a hang of the screen's own code.

Custom UIs:
- A UI can live outside this repository: `UI=<path>` for `./build.sh` and `sim/build-emu.sh` (docs/custom-ui.md).
- Start one from [brewdeck-ui-template](https://github.com/pongells/brewdeck-ui-template): the example UI, scripts that
  fetch Brewdeck at a release tag, and CI. UIs pin a Brewdeck release (this one is the first that can be pinned).

New board:
- The Guition JC4880P443 (ESP32-P4, 4.3" MIPI-DSI panel shown in landscape, Wi-Fi through its ESP32-C6): a release
  package `brewdeck-<version>-esp32p4.zip`, or `./build.sh ui-p4`. Tested like the Sunton: shots, review, previous
  shot, history and steam. The picture has the board's SD card slot at the bottom (`PANEL_ROT` in
  `src/board/p4/board.h`; 1 turns it 180°).
- `p4-c6-update`: a one-off tool that updates the P4 board's Wi-Fi chip firmware; with the firmware these boards ship
  with, the first Wi-Fi join dropped at every start (now retried after 1 s, so it is only slower). In the P4 package
  as `c6-update/`.

Releases and licenses:
- `flash.sh` refuses images built for another chip and writes the bootloader where that chip expects it.
- `flash.sh` without `PORT` writes to the board it checked (with two boards plugged in, the write could reach the
  other one).
- THIRD_PARTY_NOTICES.txt: the licenses of the libraries and font compiled into the firmware and the web demo, in
  every release package (with LICENSE).

Web demo:
- Opens on the screen's start-up screen and starts on a click; then the screen to use, scenes beside it (a recorded
  shot with a stop button, steam, the rarer ones under "Other screens", keys S / M / R / H) and switches for the fake
  machine, with a line under the screen that explains what it shows. How to install it, how it works, and how to
  build your own interface or bring it to another screen.

For developers:
- Board code split into `firmware/src/board/common` (shared) and one folder per board (`s3`, `p4`); the screen's
  Wi-Fi status is read by the network task, not the UI task.
- Scripts renamed: `firmware/build-pio.sh` is now `firmware/build.sh`, `sim/build.sh` is `sim/build-render.sh`,
  `tools/make_release.sh` is `tools/make-release.sh`.
- A screen recorder in debug builds (`tools/record.sh`), [docs/development.md](docs/development.md), shared command
  rules between the device and the emulator. The UI API (`brewdeck_core.h`, version 1) gains `bd_core()->version`;
  nothing else changed, its comments are clearer.

## v0.1.0

The first release. Early: in daily use on one machine (Gaggiuino core 8aaf6f0, release c929939), tested on one screen
(a Gaggiuino Gen 3 screen with encrypted flash). A Sunton ESP32-8048S043 bought on its own should work but is untested.

- Ready screen: boiler temperature and state, the active profile with a preview of its curve, brew temperature and
  stop weight (+/- or keypad, saved on the machine, each profile's original values kept for Reset), pressure / scale /
  water status, notices.
- Live brewing chart (pressure, flow, weight, temperature with the profile's targets) and a shot review with a
  scrubber, against the profile's targets or your previous shot; the machine's stored shots in Shot history.
- Steam screen, flush button (tap: flush mode, hold: timed flush), the pressure-release warning, idle clock and
  screen off.
- Setup on the screen: Wi-Fi from a scan list with an on-screen keyboard, the machine by name or IP, and clear
  messages when something is wrong.
- The core / board / UI split with API v1 and an example UI to start your own (docs/custom-ui.md).
