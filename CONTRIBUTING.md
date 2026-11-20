# Contributing to Brewdeck

Thanks for your interest. Brewdeck is a small personal project: an unofficial Wi-Fi screen for Gaggiuino machines.
Pull requests are welcome; please read this first.

## Safety first

The machine is a hot, pressurised appliance. Brewdeck mostly reads, and sends only the few commands listed in the
README ("What Brewdeck sends to the machine"), only while the machine is idle.

- **Do not add new machine commands** (pump, valve, heater, steam, brewing, descaling, calibration, deleting
  anything, settings) in a pull request. If you think one is needed, open an issue first to discuss it.
- Changes to the core (`firmware/src/core/`, especially `core_state.cpp` with the command rules, `ws.cpp`, `pb.cpp`
  and `net.cpp`) or anything that sends to the machine get a careful review and are tested on real hardware before
  they are merged.

## Before you open a pull request

You do not need the hardware to work on most of Brewdeck:

```sh
tests/run.sh                                               # unit tests
sim/build-render.sh && sim/render sim/out && python3 sim/topng.py sim/out   # renders every screen to PNG
sim/build-emu.sh && sim/emu                                # the real UI in a desktop window, fake machine
```

Prerequisites, the other tools and the debug builds: [docs/development.md](docs/development.md). For a UI change, look
at the renders or the emulator and include a screenshot in the pull request; if the user manual's animations change,
`python3 tools/gen_manual.py` re-records them.

Every pull request runs the unit tests, the renders, the release and debug firmware builds for both boards, the
example UI (firmware and emulator) and the P4 C6 updater automatically; the renders are attached to the run.

## Hardware

Brewdeck has been tested on one screen of each supported model, the Sunton ESP32-S3 and the Guition ESP32-P4 (see the
README). Merged changes are confirmed on those screens before a release; please say in your pull request what you
tested and how (emulator, sim, your own hardware).

## Style

- Keep changes small and focused; one topic per pull request.
- Comments explain *why* something is done, in plain language, and stay short. A file starts with a comment saying
  what it is; section banners are `// ---- name ----` padded to 120 columns. No dates or "was ..." history: that is
  what the commit log is for.
- C / C++: 2-space indent, lines up to 120 columns (see `.editorconfig`).
- The UI follows the design system in [docs/design-system.md](docs/design-system.md): take fonts, colours and
  radii from `firmware/src/ui/theme.h` by name, never as raw values.
- Never commit Wi-Fi credentials (`firmware/include/wifi_config.h` is git-ignored) or a backup of a screen's flash.

## Commits

Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/):

```
<type>(<scope>): <summary>

<body: what changed and why, wrapped at 72 columns>
```

- **type**: `feat` (something new for users or UI authors), `fix` (a bug), `perf`, `refactor` (no change in
  behaviour), `docs`, `test`, `build` (build scripts, PlatformIO, toolchains), `ci`, `chore` (anything else).
- **scope** (optional): the part it touches: `core`, `ui`, `board`, `s3`, `p4`, `sim`, `web`, `tools`, `docs`.
- **summary**: imperative, lower case, no full stop, up to about 72 characters ("fix(core): find the previous shot
  after a WebSocket gap").
- A change that breaks the UI API (`brewdeck_core.h`) or a saved setting: `!` after the type or scope and a
  `BREAKING CHANGE:` line in the body.

By contributing you agree that your contribution is licensed under this project's license (the PolyForm
Noncommercial License 1.0.0; MIT for the example UI in `firmware/src/ui_minimal`), and that the project's author may
also license it under other terms (for example a commercial license). If you do not agree, please open an issue
instead of a pull request.
