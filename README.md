# Servo Motion Controller (XIAO ESP32C3)

A self-contained servo motion sequencer for a Seeed XIAO ESP32C3, powered over
USB-C. On boot it creates its own WiFi access point and hosts a mobile-first
web app (open the AP's IP in a phone browser, WLED-style) for:

- Live manual jogging of the servo with a slider.
- Parametric motion patterns — sine, square, triangle, sawtooth, trapezoid —
  each with adjustable period/amplitude/offset (and duty or rise/hold/fall
  ratios where relevant), looped continuously.
- Recording a manual movement (drag the jog slider) as a timestamped
  sequence, then saving and looping it back.
- An **autostart** setting: on every power-up/reset, the configured pattern
  or saved sequence starts looping automatically with no user interaction,
  because it's applied *before* WiFi/the web server come up.

## Hardware

| | |
|---|---|
| Board | Seeed XIAO ESP32C3 |
| Power | USB-C (5V from host/charger) |
| Servo signal | **GPIO10 / pin D10** → servo signal wire |
| Servo power | **Do not power the servo from the XIAO's 3V3/5V pin if it draws more than ~500mA.** Use a separate 5–6V supply for the servo, with its ground tied to the XIAO's GND. |
| Servo pulse range | Default 500–2500 µs, calibratable in Settings (min/max pulse, min/max angle, center) |

The default calibration assumes a wide-range (~270°) servo, matching pulse
widths used in other projects in this workspace. Adjust min/max angle and
pulse width in the web UI's Settings tab to match your actual servo.

## Access point

- Default SSID: `ServoRig-XXXXXX` (last 3 MAC bytes, printed on the serial
  console at boot).
- Default password: `servo1234` (change it from Settings; leave it blank to
  run an open network — minimum 8 characters otherwise).
- Fixed AP IP: `192.168.4.1`. Visiting any other hostname while connected
  redirects there (best-effort captive portal — phone OS auto-popup
  detection isn't 100% reliable, so if it doesn't pop up, open
  `http://192.168.4.1/` manually).
- **There is no recovery WiFi network.** If you forget a custom AP password,
  the only way back in is `POST /api/settings/reset` — which you can't reach
  without a connection. In practice: flash a fresh build, or add a physical
  factory-reset trigger (e.g. a boot-time GPIO check) if you expect to need
  this in the field — not implemented here to keep scope minimal.

## Build & flash

Requires [PlatformIO](https://platformio.org/) (`pio` CLI).

```bash
cd firmware
pio run -e seeed_xiao_esp32c3              # compile firmware
pio run -e seeed_xiao_esp32c3 -t buildfs   # build the LittleFS web UI image

# with the board connected over USB-C:
pio run -e seeed_xiao_esp32c3 -t upload -t uploadfs
pio device monitor -e seeed_xiao_esp32c3 -b 115200
```

Both `firmware.bin` and the LittleFS image (`index.html`/`app.js`/`style.css`
in `firmware/data/`) must be flashed — `uploadfs` pushes the web UI, `upload`
pushes the firmware. Re-run `uploadfs` any time you edit files in `data/`.

**Native USB-CDC quirk on this board**: on this XIAO ESP32C3, `esptool`'s
default post-connect baud-rate change (and the RAM-stub handoff) failed
consistently over the native USB-CDC/JTAG port (`No serial data received`
after "Stub running..."). Fixed by pinning `upload_speed = 115200` and
`upload_flags = --no-stub` in `platformio.ini` (already set) — `--no-stub`
talks to the ROM bootloader directly instead of handing off to a RAM stub,
which is slower per-byte but reliable over this port. If you flash from a
different machine/OS and hit the same error, that's the first thing to try;
if your setup doesn't need it, it's harmless to leave in.

Verified end-to-end on real hardware: firmware and filesystem both flashed
successfully to a connected XIAO ESP32C3 (`/dev/ttyACM0`), and the serial
self-test log confirmed a clean boot — settings loaded, servo attached on
GPIO10, LittleFS mounted, WiFi AP up (`ServoRig-xxxxxx` @ `192.168.4.1`),
web server started, ~195KB free heap. **Not verified**: the web UI and API
themselves, since this environment has no WiFi adapter to join the AP and
exercise `/api/*` or the WebSocket over the air — connect a phone or laptop
to the AP and walk through [docs/self-test.md](docs/self-test.md)'s
functional checklist (jog, pattern loop, record/save, autostart-after-reset)
to finish verification.

## Web app

Three tabs, reachable from the bottom nav:

- **Manual** — jog slider (live, ~25 Hz over WebSocket) + pattern picker with
  generated parameter fields and Start/Stop.

  <img src="images/webui-manual.png" alt="Manual tab: jog slider and pattern picker" width="300">

- **Record** — start/stop recording (captures the servo's position at a
  fixed 20 Hz while you jog it), live trace, save/discard, and play/stop the
  saved sequence on a loop.

  <img src="images/webui-record.png" alt="Record tab: recording controls and saved sequence" width="300">

- **Settings** — AP SSID/password, servo calibration (including **Invert
  direction**, for a servo mounted mirrored/reversed relative to its
  calibrated min/max angle), autostart enable/target (pattern or sequence)
  with its own parameter fields, and a factory-reset button.

  <img src="images/webui-settings-calibration.png" alt="Settings tab: AP, servo calibration with invert checkbox" width="300">

## REST / WebSocket API

See [docs/api.md](docs/api.md) for the full route table and payload shapes.
WebSocket `/ws` pushes `{"type":"status", mode, angle, ...}` at ~10 Hz and
accepts `{"cmd":"jog","angle":123.4}` for low-latency manual moves.

## Multi-board Master/Node mode (v2)

Every board still runs the same firmware and, by default, is exactly the
self-contained single-servo rig described above (**Standalone** mode). On top
of that, a board can be switched (Settings → Network) into:

Settings → Network, all three modes:

<img src="images/webui-network-standalone.png" alt="Network settings: Standalone mode" width="260"> <img src="images/webui-network-node.png" alt="Network settings: Node mode with Node ID field" width="260"> <img src="images/webui-network-master.png" alt="Network settings: Master mode with known-nodes table" width="260">

- **Node** — everything Standalone does, plus a **Node ID** (1–250) and an
  ESP-NOW listener: it accepts wireless positioning commands from a Master in
  addition to local jog/pattern/sequence control, and reports its status as
  `mode: "network"` while being driven that way. A local jog on the same
  board's web UI still takes over immediately (last command wins — there's no
  arbitration between the two sources).
- **Master** — a bridge, not a servo controller: connect it to a PC over
  USB-C and it relays newline-delimited JSON commands from that serial port
  to every Node in ESP-NOW range. See [docs/serial-protocol.md](docs/serial-protocol.md)
  for the wire format (`{"node":3,"angle":120.5}`, `{"cmd":"list"}`, etc.)
  and a `pyserial` example. A Master needs no servo attached.

  A Master's **own web UI works too**: its Manual tab gets a "Target" card
  (Settings → Network → Master) listing known Nodes as checkboxes — select
  one, several, or check "All nodes" — and the existing Jog slider / Pattern
  controls drive that selection over ESP-NOW instead of a local servo. Handy
  for driving the rig by hand from a phone without any PC involved at all;
  the serial bridge above is for scripted/external control.

  <img src="images/webui-manual-master-target.png" alt="Master's Manual tab: Target card with node checkboxes" width="300">

  For PC-driven control there's also [scripts-tools/master_gui.py](scripts-tools/master_gui.py),
  a small Tkinter app that connects to a Master's serial port, shows its known
  Nodes, and lets you jog/send positions to a selection of them (or broadcast
  to all) without writing any code:

  <img src="images/pytool-master-gui.png" alt="master_gui.py: serial connection, known-nodes table, send controls" width="420">

  and [scripts-tools/joystick_master_gui.py](scripts-tools/joystick_master_gui.py),
  which links a physical joystick/gamepad's axes to Nodes (with a "learn the
  axis" calibration step), streams live movement to them, and can record a
  performance to CSV (Node ID + angle per timestamp) for standalone replay
  without the controller — see [scripts-tools/README.md](scripts-tools/README.md).

  <img src="images/pytool-joystick-gui.png" alt="joystick_master_gui.py: controller detected, axis readout, mapping table" width="420">

How it works: Master and Nodes talk over **ESP-NOW** (direct ESP32-to-ESP32
radio, no router involved), broadcasting on the same fixed AP WiFi channel
every board already uses (`AP_WIFI_CHANNEL` in `firmware/include/Config.h`).
That's what makes it a drop-in addition to the existing self-contained-AP
design — no board has to join anyone else's network. The only setup step per
board is picking its mode and, for Nodes, a unique Node ID, from its own
Settings → Network tab; changes take effect after a reboot.

Known limitation: like the rest of this project's networking, there's no
encryption or pairing beyond the shared channel + Node ID — anyone else's
ESP-NOW traffic on the same channel with a colliding Node ID could also drive
a Node. Fine for a single installation's private RF environment; don't rely
on it where that's not true.

## Storage

- Servo calibration, AP credentials, and autostart config live in NVS
  (`Preferences`, versioned blob — a magic/version mismatch falls back to
  factory defaults instead of crashing).
- The recorded sequence lives in LittleFS at `/sequence.bin` (12-byte header
  + up to 1200 points of `{t_ms, angle*10}`, i.e. up to 60s at the 20 Hz
  capture rate). It's only written on explicit **Save**, never per-sample, to
  avoid flash wear.

## Known limitations / risks

- Single RISC-V core: the 50 Hz servo tick shares the core with
  AsyncWebServer/WebSocket event handling. Route handlers are kept
  non-blocking and JSON payloads small to avoid starving the tick.
- Captive-portal auto-popup varies by phone OS; the DNS catch-all + redirect
  is best-effort, `192.168.4.1` is the documented fallback.
- No STA/recovery network — see the access point section above.
- Flash is fairly full (70.5% of the app partition as of v2) since
  `ESPAsyncWebServer` + `ArduinoJson` + `ESP32Servo` are meaningfully sized
  libraries on a 1.25MB app partition. If you add more features and hit the
  ceiling, look at a non-OTA partition table (`board_build.partitions`) to
  reclaim the unused second OTA app slot.
- Master/Node mode (v2) is build-verified (`pio run` / `-t buildfs` both
  succeed) and all 4 boards on hand have been flashed and boot-confirmed over
  serial (settings v3, LittleFS OK, AP up), but the actual ESP-NOW link
  between a Master and a Node (a real serial or web-UI command moving a
  *remote* servo) hasn't been hands-on verified end-to-end yet — this
  environment has no WiFi adapter to join a board's AP and drive its web UI
  over the air. Set one board to Master / another to Node with a chosen Node
  ID and confirm it responds before relying on this in the field. Note also
  that bumping `SETTINGS_VERSION` (for the new invert field) resets any
  previously-configured Master/Node role back to Standalone on reflash — a
  documented, intentional fallback, not a bug, but it means re-picking
  Mode/Node ID after this update.
- The web UI screenshots above were captured by serving `firmware/data/`
  through a local mock API (canned JSON standing in for the ESP32's
  responses) and driving a real headless Chromium over it — real rendering
  and JS logic, but not the actual ESP32 backend or WiFi AP.
- The `scripts-tools/` PC GUIs were actually launched on a real X display for
  these screenshots (not just syntax-checked) — which caught a real bug in
  `master_gui.py` (a callback referenced `live_jog_var` before it was created,
  crashing on the first slider draw; now fixed) and confirmed
  `joystick_master_gui.py` correctly detects a real controller and gates the
  streaming checkbox until a mapping exists. Still not exercised: an actual
  Master serial connection or live streaming/recording session with hardware
  end-to-end.
# servo-motion-controller
