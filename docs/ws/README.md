# Gaggiuino WebSocket: what Brewdeck uses

The protocol is documented by Gaggiuino: [WebSocket API](https://gaggiuino.github.io/#/rest-api/websocket) (the page's
Markdown source, readable without a browser: <https://gaggiuino.github.io/rest-api/websocket.md>). It gives the exact
schemas of the envelope, `SensorStateSnapshotDto`, `ShotSnapshotDto`, `SystemStateDto`, `NotificationDto`,
`ProfileDto`, `UpdateSystemStateCommandDto` and `WebSocketResponseDto`, and the list of actions. This page has
Brewdeck's own findings: rates, quirks and what the machine actually sends.

### Fields Brewdeck uses that the docs do not list (checked 2026-11-18)

Found in the machine's own web UI (its compiled protobuf metadata; `tools/extract_proto.py` rebuilds the whole schema
from it on your own machine). Field numbers as decoded by `firmware/src/core/pb.cpp`:

| Message | Field | Used for |
|---|---|---|
| `ShotSnapshotDto` | `phaseIndex = 11` (0-based phase of the profile) | brewing screen |
| `SystemStateDto` | `dashboardReadinessState = 33` (0 waiting, 1 warming, 2 heating, 3 ready, ...: see `docs/gaggiuino-api.md`), `dashboardWarmupRemainingSeconds = 35` | Ready / Heating / Warming up |
| `ShotHistoryIndexDto` (`d_shot_hist_index`) | `records = 1`: `{ uint32 id = 1; string profileName = 2; uint32 timestamp = 3; uint32 duration = 4; }`, newest first | shot history, previous shot |
| `GaggiaSettingsDto` (`d_settings`) | only `scales = 4` (`forcePredictive = 1`, `hwScalesEnabled = 2`, `btScalesEnabled = 5`), `boiler = 1` (`steamSetPoint = 1`), `versions = 7` (`frontVersion = 2`, `staticVersion = 3`); nothing else is read | scale icon, steam screen, About |

The web UI bundle was `index-BaW141oT.js` on firmware `6ea560e` and release `c929939` (Vite names it by a hash of its
content, so an unchanged name means an unchanged protocol). After a Gaggiuino update, re-read the docs and re-check
these fields (`tools/extract_proto.py`, usage in its header).

## Protocol
- `ws://<machine>/ws`, no auth, no subscribe step: the server starts pushing right after the handshake.
- Every frame (both ways) is binary `WebSocketMessageDto { string action = 1; bytes data = 2; }`; `data` is the
  payload message for that action. Max 3 concurrent clients (4th gets HTTP 429): a screen holds one permanently.

## Measured (passive listen, 30 s, machine idle in HOME, `tools/ws_listen.py`)
| action | rate | size | payload |
|---|---|---|---|
| `d_sensor_snap` | 3.3/s (median gap 300 ms) | 54 B | SensorStateSnapshotDto: temperature, pressure, flows, weight, waterLevel, brew/steam flags |
| `d_sys_state` | 2.7/s | 276 B | SystemStateDto: operationMode, tarePending, readiness, warmup, persistedActiveProfileId |
| `d_esp_mem`, `d_pump_cal_progr` | 1/s | ~30 B | ESP heap stats, pump calibration progress |
| `d_act_prof`, `d_settings`, `d_prof_dict`, `d_shot_hist_index`, `d_service_log` | once on connect | 0.2-2 KB | active profile (full), all settings, profile id+name list, shot history index |

Total idle traffic about 1 KB/s.

## Measured during a shot (150 s listen, Leva 9 SINGLE v0.9, 2026-11-04)
- `d_shot_snap`: ~7/s while the shot runs (median gap 144 ms, max 307 ms), 38 B each; 167 snapshots for a 25.5 s
  shot. Only sent during the shot. All values are integers x10: timeInShot 255 = 25.5 s, pressure 66 = 6.6 bar,
  pumpFlow/weightFlow 10/13 = 1.0/1.3 g/s, temperature 900 = 90.0 C, shotWeight 211 = 21.1 g (stopped on the 21 g
  target), waterPumped 518 = 51.8 ml, plus targetPressure/targetPumpFlow/targetTemperature and phaseIndex (0-based).
- `d_sensor_snap` and `d_sys_state` keep their idle rates (~3/s) during the shot; no burst.
- `d_notif` (e.g. "Releasing pressure!") at the end of the shot; `d_shot_hist_index` re-sent after the shot.
- Notifications seen: after power-on and after a shot, INFO "Dropping beats in: 3", "2", "1" (usually ~1 s apart,
  sometimes ~30 s), then INFO "Releasing pressure!". The machine's update notice ("Release <version> now available for
  download!") is a plain INFO notification too, repeated every 2 min.
- `d_ble_scls` when the Bluetooth scale connects. `d_sensor_snap.weight` is the raw scale reading (cup included).
- Peak traffic stays well under 2 KB/s: the brewing screen can take the live curve straight from `d_shot_snap`.
`d_settings` contains MQTT credentials and API tokens: never log or store it (the listener redacts them). The screen
decodes only `scales` (field 4: flags 1/2/5, forcePredictive, hwScalesEnabled, btScalesEnabled; `pb_scale_cfg`),
`boiler.steamSetPoint` (1.1, `pb_steam_set`) and `versions` 7.2 / 7.3 (`pb_versions`); nothing else of it is read.

## Commands the web UI sends (the screen uses only some of them, see the README)
- Tare: `c_tare_pend` + `UpdateSystemStateCommandDto { tarePending: true }` (operationMode left out). USED by the
  screen: 17 bytes `0A 0B "c_tare_pend" 12 02 10 01`, sent as one masked binary frame. The web
  UI sends it from its scale tile, shot dialog and settings; Brewdeck only while idle with a scale connected.
- Profile select: `c_upd_act_prof_id` + `WebSocketProfileIdCommandDto { id }`; `c_save_act_prof` only when the user
  sets it as the default (a switch alone does not change the boot profile)
- Mode: `c_opmode` + `UpdateSystemStateCommandDto { operationMode }`. Each command is answered by `d_resp`.
