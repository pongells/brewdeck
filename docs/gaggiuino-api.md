# Gaggiuino machine interfaces: what Brewdeck uses

Official docs (a docsify site; each page's Markdown source can be read directly, without a browser):

| | Page | Markdown source |
|---|---|---|
| REST | <https://gaggiuino.github.io/#/rest-api/rest-api> | <https://gaggiuino.github.io/rest-api/rest-api.md> |
| WebSocket | <https://gaggiuino.github.io/#/rest-api/websocket> | <https://gaggiuino.github.io/rest-api/websocket.md> |
| MQTT | <https://gaggiuino.github.io/#/rest-api/MQTT> | <https://gaggiuino.github.io/rest-api/MQTT.md> |

The WebSocket fields Brewdeck uses that those pages do not list are in [ws/README.md](ws/README.md).
Docs read on 2026-11-03. On the machine these notes come from, the embedded UI and the web UI were firmware
`6ea560e`, PCB v4; its status JSON reports coreType STM32U585-PCB-PCA, coreVersion 8aaf6f0. Re-read the docs before relying
on anything below: they track the current firmware and a given machine may run an older build.

## Architecture (from the docs)
- STM32 "core" does control (pump, heater, valves, sensors) and ENFORCES SAFETIES (e.g. refuses component tests
  unless idle; manual pump mode only works while a shot runs; releasing the brew switch exits manual mode).
- ESP32 "frontend" runs Wi-Fi, web server (REST + WebSocket), MQTT client and the embedded touchscreen UI.
- Everything we do goes through the ESP32's network interfaces, so the core's safeties still apply.
- The documented interfaces have no authentication: keep the machine on a trusted network.

## REST (http://gaggiuino.local, JSON, CORS *)
Verified live: `GET /api/system/status` -> array with ONE flat object, numbers often as strings. See status-sample.json.
Fields used by the firmware: temperature, targetTemperature, pressure, weight, waterLevel (percent), profileName,
machineName, operationMode, brewActive, steamActive, hotWaterActive, scalesPresent, warmupRemainingSeconds,
readinessState (names from the web UI bundle, enum TE: 0 WAITING, 1 WARMING (shown "WARMING UP" + countdown),
2 HEATING ("Waiting for temperature"), 3 READY ("Warm + at temperature"), 4 STANDBY, 5 STEAMING, 6 FLUSHING,
7 MAINTENANCE; above 7 the web UI shows WAITING; same values in WS d_sys_state.dashboardReadinessState), plus faults (thermocoupleFaulted ...) and power fields.
Verified live 2026-11-04 (used by the firmware, GET only):
- `GET /api/profiles/all` -> `[{"id":1,"name":"IUIUIU Classic","selected":"false"}, ...]` (selected is a string;
  community profiles carry a " (community)" name suffix; status `profileName` equals the list name exactly).
- `GET /api/profile/<id>` -> full definition, 0.7-3 KB: `name`, `phases` [..], `globalStopConditions.weight`,
  `waterTemperature`, `recipe`. Phases have their own stop conditions, so read the weight outside the array.
- USED: `POST /api/profile-select/<id>`, empty body -> `200`, text/plain `OK`.
  Verified live: the switch is immediate (status `profileName` follows), mode stays HOME, and the boot profile
  (`persistedActiveProfileId` over WS) is NOT changed: same as a switch in the web UI (whose "set as default" is the
  separate WS command `c_save_act_prof`). The web UI itself switches over WS (`c_upd_act_prof_id`), not REST.
- Saving (observed 2026-11-16, receive-only listener + GETs): the web UI dashboard's temperature -/+ sends only
  `c_upd_act_prof` ("Updated active profile"), RAM only, no save; the stored profile and `persistedActiveProfileId`
  stay as they were. The machine's BUILT-IN screen's Save (temp 91 -> 92 on LMD Single, id 28) wrote the stored
  profile: `GET /api/profile/28` -> waterTemperature 92, a `d_notif` SUCCESS "Saved" to all WS clients, d_act_prof 92,
  `persistedActiveProfileId` stayed 28 (it was already the boot profile). The built-in screen talks to the core
  directly, so its command was not seen.
- `c_save_act_prof` (PersistActiveProfile; tried once by hand while idle in HOME):
  WebSocketMessageDto with only the action, 17 bytes `0A 0F "c_save_act_prof"`, as the web UI's persistActiveProfile()
  (whose toast says "Profile set as default"). CONFIRMED it saves the ACTIVE profile's values into the stored profile:
  active 91 degC (set with c_upd_act_prof), stored 92 -> `GET /api/profile/28` 91 right after. Answer: d_resp with the
  action echoed and an empty error field (`12 13 0A 0F "c_save_act_prof" 1A 00`); no "Saved" d_notif this time (that
  one came with the built-in screen's Save). Boot profile not checked (28 was already the default).
- NEVER: `DELETE /api/profile-select/<id>` DELETES the profile (same URL!). The firmware's http() can only send GET/POST.
- USED: `GET /api/shots/latest` -> `[{"lastShotId":298}]`;
  `GET /api/shots/<id>` -> `{"id","timestamp","duration" (0.1 s),"datapoints":{"timeInShot","pressure","pumpFlow",
  "weightFlow","temperature","shotWeight","waterPumped","targetTemperature","targetPumpFlow","targetPressure",
  "phaseIndex"} (equal-length arrays of x10 ints, ~7 points/s),"profile":{full profile as stored}}`, ~6 KB per 20 s,
  ~0.3 s (fixtures `sim/data/shot_297.json`, `shot_298.json`). Read up to `Content-Length`: the machine keeps the
  connection open after the body. Also used by the shot history (on a tap, idle only). Stored on the machine's SD card. NEVER `DELETE
  /api/shots/<id>` (deletes the shot). The WebSocket's `d_shot_hist_index` lists the newest 50 (id, profileName,
  timestamp, duration), newest first.
  Short shots are NOT stored (seen 2026-11-05: empty shots of 3.5 s and 4.3 s, stopped by weight, never appeared;
  `lastShotId` stayed 302, a 10.5 s shot from 14:43; the cut-off lies between 4.6 and 8.4 s (an 8.4 s shot, #303, was stored 2026-11-06), guessed to be on
  purpose so flushes are not kept). `GET /api/shots/<lastShotId + 1>` still answers 200 with an OLD record
  (303 = a 39 s shot from 2025-04-22, likely a reused slot on the SD card): only ids up to `lastShotId` are valid.
Settings POSTs need the FULL body (partial bodies are not accepted) and persist immediately. Other endpoint groups in
the docs: shots, profiles, system status, settings, maintenance, firmware, health. No start/stop-shot endpoint seen.

## WebSocket ws://<ip>/ws  (best for live push; harder)
- Every frame both ways = binary protobuf `WebSocketMessageDto { string action = 1; bytes data = 2; }`, data is a second
  protobuf whose type depends on `action`. No JSON fallback. Max 3 concurrent clients (4th gets HTTP 429).
- The docs give the schemas of most messages; Brewdeck decodes the few it uses by hand (`firmware/src/core/pb.cpp`),
  with the field numbers the docs do not list in [ws/README.md](ws/README.md).
- Server pushes: d_sensor_snap (SensorStateSnapshotDto: temperature, pressure, pumpFlow, weightFlow, weight,
  waterLevel, ...), d_shot_snap (x10 scaled ints), d_sys_state (operationMode ...), d_act_prof, d_settings,
  d_notif, d_prof_dict ...
- Commands (c_*, each answered by d_resp): c_opmode, c_tare_pend, c_upd_manual_prof, c_upd_act_prof_id,
  c_upd_settings (RAM only) + c_save_settings, c_service_test (idle only), ...
- Requests (g_*) with empty payload ask for a d_* push.
- OperationMode enum: BREW_AUTO 0, BREW_MANUAL 1, FLUSH 2, DESCALE 3, STEAM 4, FLUSH_AUTO 5, HOT_WATER 6, HOME 7,
  PUMP_CALIBRATION 8, POWER_OFF 9.

## MQTT (needs a broker on the LAN; machine is the client)
Topics under prefix `gaggiuino`: status, sensors (1 Hz, retained), system, shot, profile/active, maintenance,
notification, settings. Commands: cmd/profile/select {"id":3}, cmd/opmode {"mode":"STEAM"}, cmd/tare,
cmd/manual {"pressure":9,"flow":2,"useFlowControl":false}. Component tests are deliberately NOT on MQTT/REST.

## What Brewdeck uses
1. Reading: GET /api/system/status, the profile list and details, stored shots; the WebSocket's live data.
2. Commands (README, "What Brewdeck sends to the machine"): POST /api/profile-select, and over the WebSocket
   `c_upd_act_prof` (stop weight / brew temperature only), `c_save_act_prof`, `c_opmode` (flush only), `c_tare_pend`.
3. Never: manual brew control, component tests, steam / descale modes, deleting anything (a hot, pressurised
   appliance).
