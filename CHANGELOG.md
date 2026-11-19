# Changelog

Each release has a section here; its text becomes the release notes on GitHub.

## Unreleased

- Experimental support for the Guition JC4880P443 (ESP32-P4, 4.3" MIPI-DSI panel shown in landscape, Wi-Fi through
  its ESP32-C6): `./build.sh ui-p4`. Not yet tested through a full shot.
- `p4-c6-update`: a one-off tool that updates the P4 board's Wi-Fi chip firmware; with the firmware these boards ship
  with, the first Wi-Fi join dropped at every start.
- Board code split into `firmware/src/board/common` (shared) and one folder per board (`s3`, `p4`).
- `flash.sh` refuses images built for another chip and writes the bootloader where that chip expects it.
- The screen's Wi-Fi status is read by the network task, not the UI task; a dropped first Wi-Fi join is retried after
  1 s instead of 15 s.
- The brewing chart's time axis starts at 30 s and grows in 15 s steps (fewer full redraws during a shot).
- Shot history: a stored shot with a larger stop weight than the active profile's is drawn to its own scale.
- Scripts renamed: `firmware/build-pio.sh` is now `firmware/build.sh`, `sim/build.sh` is `sim/build-render.sh`,
  `tools/make_release.sh` is `tools/make-release.sh`.
- For developers: a screen recorder in debug builds (`tools/record.sh`), [docs/development.md](docs/development.md),
  shared command rules between the device and the emulator.

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
