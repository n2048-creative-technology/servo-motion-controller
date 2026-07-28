# Wiring

```
                 +-------------------------+
   USB-C  ------>| XIAO ESP32C3            |
                 |                         |
                 |  D10 / GPIO10  o--------+----> Servo signal (orange/yellow)
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

- **Signal**: XIAO D10 (GPIO10) → servo signal wire. This pin is set in
  `firmware/include/Config.h` (`SERVO_PIN`) and mirrored in the default servo
  calibration shown in the Settings tab.
- **Power**: run the servo from a separate 5–6V supply sized for its stall
  current, not from the XIAO's own 3V3/5V rail, unless it's a small servo
  drawing well under ~500mA. Tie the servo supply's GND to the XIAO's GND
  (common ground) — without this the PWM signal has no reference and the
  servo will behave erratically or not move at all.
- **XIAO power**: USB-C only, as specified. The ESP32C3 itself draws well
  within USB power budgets; it's the servo that typically needs its own
  supply.

## Pin choice notes

GPIO10 was chosen because it's a general-purpose XIAO ESP32C3 pin with no
strapping-pin conflicts at boot (unlike GPIO2/8/9, which affect boot mode
selection). If you need to move the servo to a different pin, change
`SERVO_PIN` in `firmware/include/Config.h` and reflash.
