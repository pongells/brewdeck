# Security

Brewdeck is a screen on your home network that talks to an espresso machine, so security problems matter. Please
report them privately: use **Report a vulnerability** on the repository's
[Security tab](https://github.com/pongells/brewdeck/security/advisories/new), not a public issue. You will get an
answer as soon as possible; this is a one-person hobby project, so there is no fixed response time.

Things worth reporting: a way to make the screen send commands to the machine that it should not send (see "What
Brewdeck sends to the machine" in the [README](README.md)), to read the Wi-Fi password or other stored data, or to
run code on the screen over the network.

Known and by design:
- **Debug builds** (`ui-debug`, `ui-p4-debug`) run a small web server with no password
  ([routes](docs/development.md#debug-web-server)): anyone on the network can see the screen, press it, change the
  backlight or start a Wi-Fi roaming check. Use them only for development, on a network you trust. Release builds
  leave the server out.
- **Gaggiuino's own interfaces** (REST, WebSocket) have no authentication. That is the machine's design, outside
  Brewdeck: keep the machine on a trusted network. Problems in the machine's firmware belong to the
  [Gaggiuino project](https://github.com/Zer0-bit/gaggiuino).
- The Wi-Fi password is stored on the screen (NVS). It is encrypted only on a screen with flash encryption, such as a
  Gaggiuino Gen 3 screen; on a plain Sunton board anyone with the board and a USB cable can read it.

Only the latest version on `main` is supported.
