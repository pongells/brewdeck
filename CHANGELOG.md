# Changelog

Each release has a section here; its text becomes the release notes on GitHub.

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
