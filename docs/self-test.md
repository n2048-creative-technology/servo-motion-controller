# Boot self-test (serial monitor)

Connect at 115200 baud (`pio device monitor -e seeed_xiao_esp32c3 -b 115200`)
right after a power-cycle or reset. A healthy boot prints all of the
following `[SELFTEST]` lines within a couple of seconds, with no `[ERROR]`
lines and free heap comfortably above ~40KB:

```
[SELFTEST] booting servo-motion-controller
[SELFTEST] settings loaded (version=1, autostart=0)
[SELFTEST] servos attached X=pin10 500-2500us, Y=pin5 500-2500us
[SELFTEST] relay pin=20 active_high, starting off
[SELFTEST] littlefs mount OK
[SELFTEST] sequence file: none
[SELFTEST] autostart disabled
[SELFTEST] wifi AP up ssid=ServoRig-A1B2C3 ip=192.168.4.1
[SELFTEST] webserver started, ws clients=0
[SELFTEST] free heap=185344 bytes
```

On a first boot (or any boot before a sequence has ever been saved) you'll
also see one benign line from the LittleFS wrapper right before
`sequence file: none`:

```
[   379][E][vfs_api.cpp:105] open(): /littlefs/sequence.bin does not exist, no permits for creation
```

This is `LittleFS.exists()` logging at ESP-IDF's error level merely because
the file isn't there yet — expected and harmless, not a real error. It goes
away once you save a recorded sequence.

What each line confirms:

| Line | Confirms |
|---|---|
| `settings loaded` | NVS read succeeded (or factory defaults were written on first boot) |
| `servos attached` | LEDC timer + pulse ranges initialized for both axes: pan on D10, tilt on D3 |
| `relay pin=...` | Relay output driven to its off level on D7 (GPIO20 on the C3, GPIO44 on the S3); `active_low` reflects the Settings → Relay / Light polarity |
| `littlefs mount OK` | Filesystem partition is readable (formats itself on first-ever boot) |
| `sequence file: ...` | Whether a previously saved `/sequence.bin` was found and loaded |
| `autostart ...` | Whether a pattern/sequence started looping automatically, and which mode |
| `wifi AP up` | SoftAP came up with the configured SSID at the fixed IP |
| `webserver started` | HTTP/WebSocket server and captive-portal DNS are live |
| `free heap` | Sanity check — if this trends down across boots/reflashes, investigate for leaks |

If `littlefs mount OK` fails, the web UI won't be served — re-run
`pio run -t uploadfs`. If `wifi AP up` fails, double check the configured
SSID isn't empty and the password (if set) is empty or ≥8 characters.

## Functional checks once connected to the AP

1. Open `http://192.168.4.1/` — the Manual tab should load with a live angle
   readout that updates as you move the jog slider.
2. Pick a pattern (e.g. Sine), hit **Start Loop** — the angle readout should
   oscillate on its own.
3. Switch to Record, hit **Start Recording**, drag the Manual jog slider for
   a few seconds, **Stop Recording**, then **Save** — Settings → Autostart
   target "Recorded sequence" should then show points/duration.
4. Enable Autostart with a target, power-cycle the board, and confirm the
   servo starts moving on its own before you reconnect to the AP.
5. Flip the **Light** toggle beside the trackpad — the relay should click and
   stay switched. If it's on when the toggle is off (and vice versa), tick
   **Active low** in Settings → Relay / Light.
6. Drag around the **XY trackpad** — pan should follow left/right and tilt
   up/down, both staying inside their own calibrated limits.
7. Pick the **Random** pattern for *both* axes, set a short interval range,
   hit **Start Loop** — the head should look at a new point at irregular
   intervals, easing into and out of each move rather than snapping, with
   both axes arriving together. Nothing should ever take either axis past
   its Min/Max angle; lower **Max speed** if the moves look faster than the
   servos can comfortably track.
8. Record while moving both axes and toggling the Light partway through,
   save, then play it back — pan, tilt and light should all replay together.

These functional steps require real hardware and have not been run in this
environment — only the firmware and filesystem-image builds were verified.
