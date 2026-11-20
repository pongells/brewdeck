# Gaggiuino WebSocket: what Brewdeck uses

The protocol is documented by Gaggiuino: [WebSocket API](https://gaggiuino.github.io/#/rest-api/websocket) (the page's
Markdown source, readable without a browser: <https://gaggiuino.github.io/rest-api/websocket.md>). It gives the exact
schemas of the envelope, `SensorStateSnapshotDto`, `ShotSnapshotDto`, `SystemStateDto`, `NotificationDto`,
`ProfileDto`, `UpdateSystemStateCommandDto` and `WebSocketResponseDto`, and the list of actions. This page has
Brewdeck's own findings: rates, quirks and what the machine actually sends.

## Fields Brewdeck uses that the docs do not list

Field numbers as decoded by `firmware/src/core/pb.cpp` (they match what the machine's own web UI reads):

| Message | Field | Used for |
|---|---|---|
| `ShotSnapshotDto` | `phaseIndex = 11` (0-based phase of the profile) | shot review (the profile's planned rest after the stop) |
| `SystemStateDto` | `dashboardReadinessState = 33` (0 waiting, 1 warming, 2 heating, 3 ready, ...: see [gaggiuino-api.md](../gaggiuino-api.md)), `dashboardWarmupRemainingSeconds = 35` | Ready / Heating / Warming up |
| `ShotHistoryIndexDto` (`d_shot_hist_index`) | `records = 1`: `{ uint32 id = 1; string profileName = 2; uint32 timestamp = 3; uint32 duration = 4; }`, newest first | shot history, previous shot |
| `GaggiaSettingsDto` (`d_settings`) | only `scales = 4` (`forcePredictive = 1`, `hwScalesEnabled = 2`, `btScalesEnabled = 5`), `boiler = 1` (`steamSetPoint = 1`), `versions = 7` (`frontVersion = 2`, `staticVersion = 3`); nothing else is read | scale icon, steam screen, About |

Checked on Gaggiuino core `8aaf6f0`, release `c929939` (first worked out on core `6ea560e`). After a Gaggiuino update,
re-read the docs and re-check these fields against a recording of what the machine sends
(`python3 -I tools/ws_listen.py <machine-ip> <schema.json> 60 --raw frames.bin`; the schema is described in the script's
header) and the host tests.

## Protocol

- `ws://<machine>/ws`, no auth, no subscribe step: the server starts pushing right after the handshake.
- Every frame (both ways) is binary `WebSocketMessageDto { string action = 1; bytes data = 2; }`; `data` is the
  payload message for that action. Max 3 concurrent clients (4th gets HTTP 429): a screen holds one permanently.

## Idle traffic

Measured with `tools/ws_listen.py` (receive only), machine idle in HOME:

| action | rate | size | payload |
|---|---|---|---|
| `d_sensor_snap` | 3.3/s (median gap 300 ms) | 54 B | SensorStateSnapshotDto: temperature, pressure, flows, weight, waterLevel, brew/steam flags |
| `d_sys_state` | 2.7/s | 276 B | SystemStateDto: operationMode, tarePending, readiness, warmup, persistedActiveProfileId |
| `d_esp_mem`, `d_pump_cal_progr` | 1/s | ~30 B | ESP heap stats, pump calibration progress |
| `d_act_prof`, `d_settings`, `d_prof_dict`, `d_shot_hist_index`, `d_service_log` | once on connect | 0.2-2 KB | active profile (full), all settings, profile id+name list, shot history index |

Total idle traffic about 1 KB/s.

## During a shot

- `d_shot_snap`: ~7/s while the shot runs (median gap 144 ms, max ~300 ms), 38 B each, only during the shot. All
  values are integers x10: timeInShot 255 = 25.5 s, pressure 66 = 6.6 bar, pumpFlow 10 = 1.0 ml/s, weightFlow 13 =
  1.3 g/s, temperature 900 = 90.0 °C, shotWeight 211 = 21.1 g, waterPumped 518 = 51.8 ml, plus the targets and
  phaseIndex (0-based).
- `d_sensor_snap` and `d_sys_state` keep their idle rates during the shot; peak traffic stays under 2 KB/s, so the
  brewing screen takes the live curve straight from `d_shot_snap`.
- `d_shot_hist_index` is sent again after the shot.
- Notifications (`d_notif`): after power-on and after a shot, INFO "Dropping beats in: 3", "2", "1" (usually ~1 s
  apart, sometimes ~30 s), then INFO "Releasing pressure!". The update notice ("Release <version> now available for
  download!") is a plain INFO, repeated every 2 min.
- `d_ble_scls` when a Bluetooth scale connects. `d_sensor_snap.weight` is the raw scale reading (cup included).

`d_settings` contains MQTT credentials and API tokens: never log or store it (`tools/ws_listen.py` redacts them). Only
the fields in the table above are read.

## Commands

How the commands Brewdeck sends behave on the machine: [gaggiuino-api.md](../gaggiuino-api.md#commands-brewdeck-sends).
Every frame both ways uses the same envelope; each command is answered by `d_resp`.
