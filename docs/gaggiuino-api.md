# Gaggiuino machine interfaces: what Brewdeck uses

Gaggiuino's official docs (a docsify site; each page's Markdown source can be read without a browser):

| | Page | Markdown source |
|---|---|---|
| REST | <https://gaggiuino.github.io/#/rest-api/rest-api> | <https://gaggiuino.github.io/rest-api/rest-api.md> |
| WebSocket | <https://gaggiuino.github.io/#/rest-api/websocket> | <https://gaggiuino.github.io/rest-api/websocket.md> |
| MQTT | <https://gaggiuino.github.io/#/rest-api/MQTT> | <https://gaggiuino.github.io/rest-api/MQTT.md> |

This page adds what Brewdeck found on a real machine: answers, quirks and the commands it sends. The WebSocket's rates
and the fields the docs do not list are in [ws/README.md](ws/README.md). Tested with Gaggiuino core 8aaf6f0
(STM32U585-PCB-PCA) and web UI release c929939; re-read the official docs after a Gaggiuino update.

## How the machine is built

- The STM32 "core" controls pump, heater, valves and sensors and enforces the safeties (component tests only while
  idle, manual pump mode only during a shot).
- The ESP32 "frontend" runs Wi-Fi, the web server (REST and WebSocket), the MQTT client and the built-in screen.
- Brewdeck goes only through the ESP32's network interfaces, so the core's safeties still apply.
- The interfaces have no authentication: keep the machine on a trusted network.

## REST

`http://gaggiuino.local`, JSON, CORS `*`.

| Request | Answer and quirks |
|---|---|
| `GET /api/system/status` | an array with one flat object, numbers often as strings ([sample](../sim/data/status.json)). Brewdeck reads temperature, targetTemperature, pressure, weight, waterLevel (%), profileName, machineName, operationMode, brewActive, steamActive, hotWaterActive, scalesPresent, warmupRemainingSeconds, readinessState |
| `GET /api/profiles/all` | `[{"id":1,"name":"...","selected":"false"}, ...]`: `selected` is a string; community profiles end in " (community)"; status `profileName` equals the name here |
| `GET /api/profile/<id>` | the full profile, 0.7-3 KB: `name`, `phases`, `globalStopConditions.weight`, `waterTemperature`, `recipe`. Phases have stop conditions of their own, so the stop weight is read outside the array |
| `POST /api/profile-select/<id>` | empty body, answers `200 OK`. Switches at once (status follows, mode stays HOME); does not change the boot profile |
| `GET /api/shots/latest` | `[{"lastShotId":298}]` |
| `GET /api/shots/<id>` | `id`, `timestamp`, `duration` (0.1 s), `datapoints` (equal-length arrays of x10 integers, ~7 points/s: timeInShot, pressure, pumpFlow, weightFlow, temperature, shotWeight, waterPumped, targetTemperature, targetPumpFlow, targetPressure, phaseIndex) and the profile as stored. ~6 KB per 20 s of shot |

Quirks:
- The machine keeps the connection open after the body: read up to `Content-Length`, or every request waits for the
  timeout.
- Only ids up to `lastShotId` are valid: higher ids can answer 200 with an old record.
- Short shots are not stored (the cut-off lies between 4.6 and 8.4 s), so flushes do not fill the history.
- Settings POSTs need the full body and are saved at once. There is no start / stop shot endpoint.

**Never send** `DELETE /api/profile-select/<id>` (it deletes the profile, same URL) or `DELETE /api/shots/<id>`.
Brewdeck's HTTP code can only send GET and POST.

`readinessState` (also `dashboardReadinessState` over the WebSocket; names from the web UI): 0 WAITING, 1 WARMING
("Warming up" with a countdown), 2 HEATING, 3 READY, 4 STANDBY, 5 STEAMING, 6 FLUSHING, 7 MAINTENANCE; the web UI shows
anything above 7 as WAITING. 6 FLUSHING is reported for the whole of flush mode (FLUSH, the brew switch flushes),
water running or not, and stays for a moment after the mode is back to HOME: take "is it flushing" from the
operation mode (FLUSH / FLUSH_AUTO), not from the readiness (seen on the machine, 2026-11).

## WebSocket

`ws://<machine>/ws`: every frame is a protobuf envelope, `action` plus a payload message. Protocol, rates and the
undocumented fields: [ws/README.md](ws/README.md).

`OperationMode`: BREW_AUTO 0, BREW_MANUAL 1, FLUSH 2, DESCALE 3, STEAM 4, FLUSH_AUTO 5, HOT_WATER 6, HOME 7,
PUMP_CALIBRATION 8, POWER_OFF 9.

## Commands Brewdeck sends

Which action sends what: README, "What Brewdeck sends to the machine". How each behaves on the machine:

- **Profile select** (`POST /api/profile-select/<id>`): as a switch in the web UI. The web UI itself switches over the
  WebSocket (`c_upd_act_prof_id`).
- **`c_upd_act_prof`**: the active profile with one value changed (stop weight or brew temperature). It changes the
  running profile only, in RAM, as the web UI dashboard's -/+ does; the stored profile is unchanged.
- **`c_save_act_prof`** (envelope only, 17 bytes `0A 0F "c_save_act_prof"`): saves the active profile's values into
  the stored profile and makes it the boot profile, like the web UI's "Set as default". Answered by a `d_resp` with an
  empty error field.
- **`c_opmode`** (`UpdateSystemStateCommandDto { operationMode }`): only FLUSH, FLUSH_AUTO and back to HOME.
- **`c_tare_pend`** (`UpdateSystemStateCommandDto { tarePending: true }`, 17 bytes `0A 0B "c_tare_pend" 12 02 10 01`):
  as the web UI's tare.

Every command is answered by `d_resp`. Brewdeck sends none of the others (manual brewing, component tests, settings,
steam, descaling).

## MQTT

Not used: it needs a broker on the network. Topics and commands: the official MQTT page above.
