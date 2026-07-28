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
- **Record** — start/stop recording (captures the servo's position at a
  fixed 20 Hz while you jog it), live trace, save/discard, and play/stop the
  saved sequence on a loop.
- **Settings** — AP SSID/password, servo calibration, autostart
  enable/target (pattern or sequence) with its own parameter fields, and a
  factory-reset button.

## REST / WebSocket API

See [docs/api.md](docs/api.md) for the full route table and payload shapes.
WebSocket `/ws` pushes `{"type":"status", mode, angle, ...}` at ~10 Hz and
accepts `{"cmd":"jog","angle":123.4}` for low-latency manual moves.

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
- Flash is fairly full (69.5% of the app partition after this build) since
  `ESPAsyncWebServer` + `ArduinoJson` + `ESP32Servo` are meaningfully sized
  libraries on a 1.25MB app partition. If you add more features and hit the
  ceiling, look at a non-OTA partition table (`board_build.partitions`) to
  reclaim the unused second OTA app slot.
# servo-motion-controller
