# Boot self-test (serial monitor)

Connect at 115200 baud (`pio device monitor -e seeed_xiao_esp32c3 -b 115200`)
right after a power-cycle or reset. A healthy boot prints all of the
following `[SELFTEST]` lines within a couple of seconds, with no `[ERROR]`
lines and free heap comfortably above ~40KB:

```
[SELFTEST] booting servo-motion-controller
[SELFTEST] firmware version=2.2.0
[SELFTEST] settings loaded (version=6, autostart=0)
[SELFTEST] servos attached X=pin10 500-2500us, Y=pin5 500-2500us
[SELFTEST] relay pin=20 active_high, starting off
[SELFTEST] littlefs mount OK
[SELFTEST] sequences stored: 0
[SELFTEST] wifi AP up ssid=ServoRig-A1B2C3 ip=192.168.4.1
[NET] esp-now ready, role=standalone node_id=7
[SELFTEST] autostart disabled
[SELFTEST] webserver started, ws clients=0
[SELFTEST] free heap=185344 bytes, network mode=0 node_id=7
```

`network mode` is `0` standalone / `1` node / `2` master, matching
`OperatingMode` in `firmware/include/Config.h`. `node_id` starts from a value
derived from the board's own MAC (so two fresh boards usually differ rather
than colliding) until you set one in Settings → Network — yours won't be 7.

On a **Master** board there is no `autostart` line at all — autostart only
makes sense with servos attached locally, and a Master drives Nodes instead.

The two servo pins in `servos attached` are the compiled-in ones for that
board: `X=pin10, Y=pin5` on the C3, `X=pin9, Y=pin4` on the S3 — silkscreen
D10 and D3 either way.

A board that once ran the v1 single-file firmware prints one extra line the
first time it boots this version, as its old recording is migrated into the
named-sequence directory:

```
[SEQ] migrated legacy sequence file to /seq/local.bin
```

On a board with no saved sequences you'll also see one or two benign lines
from the LittleFS wrapper around the `sequences stored` line:

```
[   379][E][vfs_api.cpp:105] open(): /littlefs/seq/local.bin does not exist, no permits for creation
[   381][E][vfs_api.cpp:105] open(): /littlefs/sequence.bin does not exist, no permits for creation
```

That's `LittleFS.exists()` — called to decide whether there's a v1 recording
to migrate — logging at ESP-IDF's error level merely because the files aren't
there. Expected and harmless, not a real error.

What each line confirms:

| Line | Confirms |
|---|---|
| `firmware version` | Which build is actually on the board — compare it against the web UI's Settings → About, which reports the *filesystem* image's version; a mismatch means only one of `upload`/`uploadfs` took |
| `settings loaded` | NVS read succeeded (or factory defaults were written on first boot). `version` is `SETTINGS_VERSION`, currently 6 |
| `servos attached` | LEDC timer + pulse ranges initialized for **both** axes: pan on D10, tilt on D3, each with its own calibrated pulse range |
| `relay pin=...` | Relay output driven to its off level on D7 (GPIO20 on the C3, GPIO44 on the S3); `active_low` reflects the Settings → Relay / Light polarity |
| `littlefs mount OK` | Filesystem partition is readable (formats itself on first-ever boot) |
| `sequences stored: N` | How many saved sequences are in `/seq` — locally recorded or uploaded from a PC through a Master |
| `esp-now ready` | Radio link initialized for the configured role (printed by `NetworkLink`, not the self-test block) |
| `autostart ...` | Whether a pattern/sequence started looping automatically, and which mode. Absent on a Master |
| `wifi AP up` | SoftAP came up with the configured SSID at the fixed IP |
| `webserver started` | HTTP/WebSocket server and captive-portal DNS are live |
| `free heap` | Sanity check — if this trends down across boots/reflashes, investigate for leaks |

If `littlefs mount OK` fails, the web UI won't be served — re-run
`pio run -t uploadfs`. If `wifi AP up` fails, double check the configured
SSID isn't empty and the password (if set) is empty or ≥8 characters.

## Functional checks once connected to the AP

1. Open `http://192.168.4.1/` — the Manual tab should load with live X and Y
   readouts that update as you drag the trackpad.
2. Pick a pattern per axis (e.g. Sine on X, Triangle on Y), hit **Start
   Loop** — both readouts should oscillate on their own, independently.
3. Switch to Record, hit **Start Recording**, aim the head with the Manual
   trackpad for a few seconds, **Stop Recording**, give it a name and
   **Save** — it should appear in the Saved Sequences list, and in Settings →
   Autostart's sequence picker.
4. Enable Autostart with a target, power-cycle the board, and confirm the
   head starts moving on its own — both axes, and the light if the sequence
   captured one — before you reconnect to the AP.
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
9. Tap the saved recording's row in the list — the plot above should switch
   from the live trace to that recording, and while it plays a thin white
   playhead should sweep across it in time with the head. (A recording made
   before this firmware had a tilt axis plots its pan trace only, and says so
   next to the legend.)

These functional steps require real hardware and have not been run in this
environment — only the firmware and filesystem-image builds were verified.
