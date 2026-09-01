# Wiring

```
                 +-------------------------+
   USB-C  ------>| XIAO ESP32C3 / ESP32S3  |
                 |                         |
                 |  D10           o--------+----> Servo X / pan  signal
                 |  D3            o--------+----> Servo Y / tilt signal
                 |  D7            o--------+----> Relay module IN
                 |  GND           o--------+----> Servo GND (brown/black) ----+
                 |                         |      + relay module GND         |
                 +-------------------------+                                 |
                                                                              |
                 +-------------------------+                                 |
   5-6V supply-->| Servo power rail        |                                 |
                 |  V+  o------------------+----> Servo V+ (red)             |
                 |  GND o--------------------------------------------------->+
                 +-------------------------+
```

- **Signals**: XIAO pin **D10** → the pan (X) servo, **D3** → the tilt (Y)
  servo, on either board. These are the same silkscreen-labeled pins on both,
  but different GPIOs underneath — `SERVO_X_PIN` and `SERVO_Y_PIN` in
  `firmware/include/Config.h` select the right ones per board automatically
  (compiled in per-environment, nothing to configure). Each axis's pin is
  also shown next to its own calibration block in the Settings tab.
- **Power**: run the servos from a separate 5–6V supply sized for their stall
  current, not from the XIAO's own 3V3/5V rail. **Two servos roughly double
  the peak draw**, and both can move at once — a supply that just coped with
  one pan servo is the first thing to suspect if a Node starts resetting
  during fast moves (see docs/serial-protocol.md's brownout section). Tie the servo supply's GND to the XIAO's GND
  (common ground) — without this the PWM signal has no reference and the
  servo will behave erratically or not move at all.
- **Relay**: XIAO pin **D7** → the relay module's IN/signal pin, plus a shared
  GND. Power the module's coil side from its own 5V supply (the servo supply
  works), not the XIAO's 3V3 — a relay coil pulls tens of mA and switching it
  couples noise back into the rail the servo signal references. If your module
  switches on when IN goes *low* (most opto-isolated boards do), tick
  **Active low** in Settings → Relay / Light; nothing needs reflashing.
- **XIAO power**: USB-C only, as specified. Neither the C3 nor the S3 draws
  meaningfully within USB power budgets; it's the servos and the relay coil
  that need their own supply.

## Pin choice notes

| Board | D10 = | Why |
|---|---|---|
| XIAO ESP32C3 | GPIO10 | General-purpose pin, no strapping-pin conflicts at boot (unlike GPIO2/8/9 on this chip, which affect boot mode selection). |
| XIAO ESP32S3 | GPIO9 | General-purpose pin on this chip — the S3's boot-strapping pins are GPIO0/3/45/46, none of which GPIO9 shares. Confirmed booting cleanly on real hardware with the servo pin on GPIO9. |

| Board | D3 (tilt) = | Why |
|---|---|---|
| XIAO ESP32C3 | GPIO5 | General-purpose pin; not one of the C3's strapping pins (GPIO2/8/9) and not used by anything else in this firmware. |
| XIAO ESP32S3 | GPIO4 | General-purpose pin; not one of the S3's strapping pins (GPIO0/3/45/46). |

Both servos share LEDC timer 0 — they run at the same 50Hz, which is what a
shared timer supports, and each still gets its own channel (the C3 has 6).

| Board | D7 = | Why |
|---|---|---|
| XIAO ESP32C3 | GPIO20 | UART0's RX pin, and free to use as a plain output here: both board definitions build with `ARDUINO_USB_CDC_ON_BOOT=1`, so `Serial` — the boot log and the Master's PC bridge alike — runs over native USB and never touches UART0. Not a strapping pin. |
| XIAO ESP32S3 | GPIO44 | Same story on the S3: UART0 RX, unused because `Serial` is USB CDC. Not one of the S3's strapping pins (GPIO0/3/45/46). |

The one thing to know about D7: because it *is* UART0 RX, wiring a relay there
rules out ever driving these boards over the hardware UART instead of USB. If
you need that, move `RELAY_PIN` in `firmware/include/Config.h` to a free pin
(D1, D2, D4 and D5 are unused by this firmware — D3 is the tilt servo) and
reflash. The relay is driven low
before the pin is switched to an output at boot, so an active-low module
doesn't get a momentary "on" pulse during startup.

If you need to move a servo to a different pin, change `SERVO_X_PIN` or
`SERVO_Y_PIN` in `firmware/include/Config.h` (both are already conditional per
board via `ARDUINO_XIAO_ESP32S3` — keep that in mind if you add a pin
override) and reflash. Double-check your target chip's specific strapping-pin list in
Espressif's technical reference manual before picking a low-numbered GPIO —
it varies by chip, not just by board.
