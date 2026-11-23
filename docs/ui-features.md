# UI features and checks

**Draft.** Partly built: `tests/ui/run.sh` runs the scenarios and prints a UI's status table (see
[development.md](development.md)); so far only X1 has a scenario, and no tags exist yet.

This is the list of what a UI on Brewdeck's core is expected to do, and how each item is checked. Every UI
(Brewdeck's own, Ledger, yours) is themed completely its own way, so nothing here says how a screen looks. It says what
the user must be able to see and do, so the same list and the same tests apply to every UI.

A test is written only where getting it wrong costs something: the machine, the user's shot, a wrong belief about what
the machine is doing, a screen the user cannot get out of. Layout, colours, fonts, chart styling and wording are the
UI's own and are never tested.

## How to read the tables

**Level**
- **Required**: safety, or the user would be misled or stuck without it. A UI should not ship without these.
- **Expected**: what a complete everyday UI has.
- **Optional**: nice to have; Brewdeck's own UI has it.

**Check**
- **Test**: an emulator scenario checks the behaviour (see [How the checks work](#how-the-checks-work)). The column
  "Checked" says what passes.
- **Tag**: only whether the feature is there (its tag exists). How it behaves is left to the UI.
- **Look**: no pass or fail. The scenario saves a screenshot for a person to look at.
- **Panel**: can only be measured on the real screen (timing, touch feel); part of the release checklist.
- **None**: not checked; it is harmless either way.

The IDs stay stable: new features get new IDs, removed ones are not reused. How a UI reports its status, and how
it leaves a feature out on purpose: [Leaving features out](#leaving-features-out).

## Start-up

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| S1 | Says it is unofficial (start screen or About) | Required | Tag | `info.unofficial` exists on the start screen or the About page |
| S2 | While the machine is not reachable at start, says what it is doing | Required | Test | machine off at boot: `screen.offline` shows within 6 s after the start screen closes |

## Ready

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| R1 | Machine state from its readiness: Warming up, Heating, Ready, Standby, Waiting, Maintenance | Required | Test | `value.state` shows a different text for each readiness; never the Ready text while the machine reports Heating |
| R2 | Busy states: Brewing, Steam, Hot water, Flush, Descale | Expected | Test | `value.state` differs from the idle states for each |
| R3 | Cooling after steam (boiler above the brew temperature + 3 °C) | Expected | Test | `value.state` does not show the Ready text while cooling |
| R4 | Ready hold: Ready only after 30 s of steady READY after a heating phase | Expected | Test | readiness flips READY / HEATING every 10 s after a heat-up: no Ready text until 30 s steady |
| R5 | Boiler temperature | Required | Test | `value.boiler_temp` contains the machine's temperature (whole degrees) and follows it within 2 s |
| R6 | Active profile name | Required | Test | `value.profile` starts with the profile's name (it may be shortened) and follows a change made elsewhere |
| R7 | Profile preview | Expected | Look | |
| R8 | Pressure, scale (type, connected), water level | Expected | Test | `value.pressure`, `value.scale`, `value.water` follow the machine |
| R9 | Pressure warning: pressure left in the system while idle (above 0.8 bar) | Expected | Test | 1.4 bar idle: `warn.pressure` visible; 2 s at 0.8 bar or less: hidden |
| R10 | No scale and low water (below 20 %) warnings | Expected | Test | `warn.no_scale` and `warn.refill` visible exactly while the condition holds |
| R11 | Time of day | Optional | Look | the idle clock (I1) is enough |

## Brew temperature and stop weight

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| T1 | Change the brew temperature | Expected | Test | `action.temp_up` / `action.temp_down`: the core receives the new value (saved), within 3 s of the last tap |
| T2 | Change the stop weight | Expected | Test | the same with `action.weight_up` / `action.weight_down` |
| T3 | Type a value on a keypad | Optional | Tag | `screen.keypad` |
| T4 | A profile without a stop weight is shown as none, and can get one or lose it | Expected | Test | stop weight 0: `value.stop_weight` shows no number |
| T5 | A burst of taps sends one value, not one command per tap | Expected | Test | 5 taps within 2 s: at most 2 commands, the last one with the final value |
| T6 | A change the machine did not take goes back to the machine's value | Required | Test | the fake machine ignores commands: within 8 s `value.brew_temp` / `value.stop_weight` show the machine's value again |
| T7 | "Edited" mark and reset to the profile's original values | Optional | Tag | `action.reset` on the keypad or the profile row |
| T8 | "Not saved: the profile has other unsaved changes" | Optional | Tag | |
| T9 | A change asked for while the machine is busy is visibly refused, never silently ignored | Required | Test | during a shot or steam: the controls are hidden or disabled, or a tap shows `info.refusal`; no command reaches the machine |

## Profiles

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| P1 | List of the machine's profiles; a tap selects one (saved as the boot profile) | Required | Test | `action.profiles`, tap `row.profile.<id>`: the core receives a select of that id; once the machine reports it, `value.profile` shows its name |
| P2 | Order: active first, recently used, then by name | Optional | Look | |
| P3 | Stop weight, brew temperature and last use per row | Optional | Look | |
| P4 | Originals per profile: restore or keep the current values | Optional | Tag | `action.restore` |
| P5 | Tapping the active profile just closes the list | Optional | None | selecting it again is harmless |

## Shot

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| B1 | The brewing screen opens when a shot starts, from any screen, also from the idle clock or screen off | Required | Test | shot started from Ready, the menu, the profile list, the idle clock and screen off: `screen.brewing` within 1 s of the first point, backlight on |
| B2 | Live chart (pressure, flow, weight, temperature) | Expected | Look | screenshots at 5, 15 s and the end |
| B3 | Target pressure / flow drawn against the live lines | Optional | Look | |
| B4 | Weight (against the stop weight) and shot time | Required | Test | `value.weight` and `value.shot_time` follow the shot points within 1 s |
| B5 | Weight smoothed between slow scale readings | Optional | Look | |
| B6 | Time axis grows without slow frames | Expected | Panel | longest frame during a shot (see X5) |
| B7 | End of the shot: result shown, then back to Ready by itself | Required | Test | after the end: the final weight and time are shown; 60 s untouched: `screen.ready` |

## Shot review and history

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| V1 | Review: the whole shot, compared with the target or the previous shot | Expected | Tag + Look | `screen.review` |
| V2 | The previous shot of the same profile is loaded once after a shot | Expected | Test | at most one previous-shot request per shot |
| V3 | Scrub through the shot | Optional | Look | |
| V4 | The review can be closed, and closes by itself | Required | Test | `action.close`: `screen.ready`; 60 s untouched: `screen.ready` |
| V5 | Shot history, tap to review a stored shot | Optional | Tag | `screen.history` |

## Steam, flush

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| M1 | Steam screen while steaming: boiler temperature against the set point, pressure | Required | Test | steam on: `screen.steam` within 1 s; `value.boiler_temp` follows |
| M2 | Low water warning while steaming | Expected | Test | water below 20 %: `warn.refill` visible on the steam screen |
| M3 | After steam, back to Ready showing it is still too hot to brew | Required | Test | steam off: `screen.ready`, `value.state` not the Ready text until the boiler is back near the brew temperature (R3) |
| F1 | Flush mode on / off | Expected | Test | tap `action.flush`: the core receives flush mode on; again: off; `value.state` (or the flush control) shows flush mode while it is on |
| F2 | Timed flush | Optional | Test | hold `action.flush`: the core receives the timed flush; a tap alone never starts it |

## Notices

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| N1 | Pressure-release warning ("water is coming"), on every screen, waking the screen | Required | Test | countdown event on Ready, brewing, review, menu, idle clock and screen off: `warn.release` visible within 0.5 s, backlight on; gone within 10 s after "Releasing pressure" |
| N2 | Machine notices shown | Expected | Test | a notice event: its text is in `info.notice` within 1 s, on any screen |
| N3 | Update notice kept (About or a list) | Optional | Tag | `info.update` |
| N4 | List of recent notices | Optional | Tag | `screen.notices` |

## Standby

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| I1 | Dims after a while without a touch | Expected | Test | 10 min untouched (time fast-forwarded): backlight lower than before |
| I2 | Moves the idle picture against image retention | Optional | Look | |
| I3 | Screen off after 30 min, or 2 min on the offline screen | Expected | Test | backlight 0 after those times |
| I4 | Wakes on a shot, steam, a flush, a notice, the machine coming back | Required | Test | each from screen off: backlight on and the right screen (B1, M1, N1) |
| I5 | The first touch on a dimmed or dark screen only wakes it | Required | Test | dimmed, tap where `action.flush` is: no command reaches the machine |

## Menu and settings

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| U1 | Wi-Fi and the machine address are reachable from Ready | Required | Test | from `screen.ready`, through tagged actions: `screen.wifi` and `screen.machine_address` |
| U2 | Scale page | Optional | Tag | `screen.scale` |
| U3 | Display settings (brightness, dim time) | Optional | Tag | `screen.display` |
| U4 | About: versions, uptime, why it last restarted, the machine's versions | Expected | Tag | `screen.about` |

## Wi-Fi and the machine's address

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| W1 | Wi-Fi list, join with a password typed on the screen | Required | Test | tap `row.wifi.<name>`, type into `input.password`, `action.join`: the core receives a join with that name and password |
| W2 | Network details; forget the network, with a confirmation | Expected | Test | one tap on `action.forget`: nothing forgotten; the confirmation: forgotten |
| W3 | Set the machine's address (name or IP) | Required | Test | type into `input.address`, `action.use`: the core receives the address |
| W4 | A refused password or failed join is shown, and the user can try again | Required | Test | the fake join fails: `info.wifi_error` visible, `input.password` or `row.wifi.*` reachable again |

## When the machine is not reachable

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| O1 | After 5 s offline: what the screen is doing; after 20 s: what is wrong | Required | Test | `screen.offline` within 6 s; after 20 s `value.offline_reason` differs for not found / Wi-Fi failed / no Wi-Fi set up |
| O2 | Never covers a running shot, steam, the review or the Wi-Fi pages | Required | Test | offline during each: that screen stays |
| O3 | A way out for each case: Wi-Fi settings, machine address, try again | Required | Test | not found: `action.machine_address` and `action.wifi_settings`; Wi-Fi failed: `action.retry` and `action.wifi_settings`; no Wi-Fi: `action.wifi_settings`; each leads to its screen |
| O4 | A short drop (under 5 s) goes unnoticed | Expected | Test | 3 s offline: `screen.offline` never shows |
| O5 | Back to Ready as soon as the machine answers | Required | Test | machine back: `screen.ready` within 2 s |

## Across screens

| ID | Feature | Level | Check | Checked |
|---|---|---|---|---|
| X1 | No command without a user action | Required | Test | a long scenario with no taps (warm-up, shot, steam, notices, offline and back): the core receives no command (reads of stored shots, such as the previous shot after a shot, do not count) |
| X2 | No dead ends: every screen can be left | Required | Test | from each tagged screen, `action.close` / `action.back` (or waiting 60 s) leads back to `screen.ready` |
| X3 | A shot or steam takes over whatever is open (menu, lists, editors, a review) | Required | Test | start a shot / steam with each open: `screen.brewing` / `screen.steam` |
| X4 | No crash and no memory left behind | Required | Test | every scenario runs to its end; opening and closing each screen 20 times does not grow the memory in use |
| X5 | No slow frames while live data is shown | Expected | Panel | longest `bd_ui_tick` during a shot and a steam session (Brewdeck's own UI: most frames under 40 ms, full redraws such as opening a screen ~50-150 ms) |
| X6 | Touch targets big enough for a wet finger | Expected | Panel | |

## How the checks work

**Scenarios.** A scenario is a script for the emulator, which already runs any UI against a fake machine and reads
commands on stdin (`tap`, `hold`, `key`, `button`, `wait`, `save`). It drives the fake machine (a recorded shot, steam,
going offline, a notice, a command the machine ignores) and checks two things that do not depend on the layout:

- **What reached the core**: every command the UI asked for, with its values. The core's own safety checks still
  apply, so these tests are about the UI asking at the right moments, not about what the machine would accept.
- **Tagged objects**: the UI names the objects that matter with standard tags. With LVGL 9 a tag is the object's
  name (`lv_obj_set_name_static`; needs `LV_USE_OBJ_NAME` in `lv_conf.h`), found with `lv_obj_find_by_name`. A UI
  that draws without LVGL is not covered for now; if one appears, a small hook (`bd_test_find(tag)`: area, visible, text) can answer the same questions, and the core would answer it from LVGL names for the others.

Emulator commands for the checks: `expect_cmd <text>` and `expect_no_cmd` exist (the emulator logs every command
the UI asks for; runner: `tests/ui/run.sh`, see [development.md](development.md)). Proposed: `tap_tag <tag>`,
`hold_tag <tag> <ms>`, `type <tag> <text>`, `expect_screen <tag> [within ms]`, `expect_visible` / `expect_hidden
<tag>`, `expect_text <tag> <pattern>`, `expect_backlight <op> <pct>`, `fast <factor>` (time fast-forward), `shot`
(screenshot for the contact sheet).

A missing tag is reported as **missing feature**, not as a failure, so the run fills in the UI's status table.
Required features that are missing are listed first. The screenshots of every scenario go on one contact sheet per UI.

**Tags (proposed).** The tag set is part of the public contract and versioned with the API.

| Kind | Tags |
|---|---|
| Screens | `screen.ready`, `screen.brewing`, `screen.review`, `screen.steam`, `screen.offline`, `screen.profiles`, `screen.keypad`, `screen.history`, `screen.notices`, `screen.wifi`, `screen.machine_address`, `screen.scale`, `screen.display`, `screen.about`, `screen.idle` |
| Values (a label, or an object whose first label child holds the text) | `value.state`, `value.boiler_temp`, `value.profile`, `value.pressure`, `value.scale`, `value.water`, `value.brew_temp`, `value.stop_weight`, `value.weight`, `value.shot_time`, `value.offline_reason` |
| Warnings (checked visible / hidden) | `warn.pressure`, `warn.no_scale`, `warn.refill`, `warn.release` |
| Information | `info.unofficial`, `info.notice`, `info.update`, `info.refusal`, `info.wifi_error` |
| Actions | `action.temp_up`, `action.temp_down`, `action.weight_up`, `action.weight_down`, `action.reset`, `action.restore`, `action.profiles`, `action.flush`, `action.close`, `action.back`, `action.join`, `action.forget`, `action.use`, `action.retry`, `action.wifi_settings`, `action.machine_address` |
| Rows and inputs | `row.profile.<id>`, `row.wifi.<name>`, `input.password`, `input.address` |

A screen counts as shown when its tagged object is visible and nothing opaque covers it. A UI that puts several
features on one object can give it one tag and the others through a hidden child, as long as it stays visible
together with what it stands for.

## Leaving features out

A UI does not have to do everything: a minimal UI, or one with a different idea of brewing, may leave features out
on purpose. That should read as a choice, not as unfinished work.

**The file.** Each UI keeps a `ui-features.txt` next to its code, listing only the exceptions, one per line:

```
# id  status    note
T3    omitted   - / + only, on purpose
I2    omitted   the idle clock stays still; it moves a few pixels against retention
B2    partial   no temperature line
V1    missing   planned
```

- **omitted**: left out on purpose; the note says why. Its scenarios are skipped.
- **partial**: there, but less than the feature describes; the note says what is missing. Its scenarios still run.
- **missing**: not there yet, but meant to be. Its scenarios are skipped.
- A feature not in the file is checked normally: its tests pass or fail, its tag is there or not.

**The rules.**
- **Required features cannot be omitted.** If a UI has a good reason to leave one out, the level in this list is
  wrong: change the list, not the UI's file. A Required feature marked missing is listed first in every report.
- A feature that is missing or omitted but whose tag is there anyway gets a warning, so the file does not go stale.
- A feature the platform cannot do (a tablet has no Wi-Fi setup of its own) is not something a UI omits: the list
  itself says where a feature applies.

**The report.** A run prints the UI's status table from the results and the file together (done / partial / missing
/ omitted per ID, with the notes) and writes it as `status.md`, so a UI needs no hand-kept feature table.

## Open questions

- Whether time fast-forward in the emulator is reliable enough for the 10 / 30 min standby tests, or those run with
  shorter settings.
