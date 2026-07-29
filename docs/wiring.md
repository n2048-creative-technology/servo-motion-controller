# Wiring

```
                 +-------------------------+
   USB-C  ------>| XIAO ESP32C3 / ESP32S3  |
                 |                         |
                 |  D10           o--------+----> Servo signal (orange/yellow)
                 |  GND           o--------+----> Servo GND (brown/black) ----+
                 |                         |                                 |
                 +-------------------------+                                 |
                                                                              |
                 +-------------------------+                                 |
   5-6V supply-->| Servo power rail        |                                 |
                 |  V+  o------------------+----> Servo V+ (red)             |
                 |  GND o--------------------------------------------------->+
                 +-------------------------+
```

- **Signal**: XIAO pin **D10** → servo signal wire, on either board. This is
  the same silkscreen-labeled pin on both, but a different GPIO underneath —
  `SERVO_PIN` in `firmware/include/Config.h` selects the right one per board
  automatically (compiled in per-environment, nothing to configure). It's
  also mirrored in the default servo calibration shown in the Settings tab.
- **Power**: run the servo from a separate 5–6V supply sized for its stall
  current, not from the XIAO's own 3V3/5V rail, unless it's a small servo
  drawing well under ~500mA. Tie the servo supply's GND to the XIAO's GND
  (common ground) — without this the PWM signal has no reference and the
  servo will behave erratically or not move at all.
- **XIAO power**: USB-C only, as specified. Neither the C3 nor the S3 draws
  meaningfully within USB power budgets; it's the servo that typically needs
  its own supply.

## Pin choice notes

| Board | D10 = | Why |
|---|---|---|
| XIAO ESP32C3 | GPIO10 | General-purpose pin, no strapping-pin conflicts at boot (unlike GPIO2/8/9 on this chip, which affect boot mode selection). |
| XIAO ESP32S3 | GPIO9 | General-purpose pin on this chip — the S3's boot-strapping pins are GPIO0/3/45/46, none of which GPIO9 shares. Confirmed booting cleanly on real hardware with the servo pin on GPIO9. |

If you need to move the servo to a different pin, change `SERVO_PIN` in
`firmware/include/Config.h` (it's already conditional per board via
`ARDUINO_XIAO_ESP32S3` — keep that in mind if you add a pin override) and
reflash. Double-check your target chip's specific strapping-pin list in
Espressif's technical reference manual before picking a low-numbered GPIO —
it varies by chip, not just by board.
