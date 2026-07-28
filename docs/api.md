# REST / WebSocket API

Base URL: `http://192.168.4.1` while connected to the device's AP.

## REST routes

| Method | Route | Body | Response / notes |
|---|---|---|---|
| GET | `/api/status` | — | `{mode, angle, uptime_ms, free_heap, recording:{active,points}, sequence:{present,points,duration_ms}}` |
| GET | `/api/patterns` | — | `[{type, label, params: "comma,separated,keys"}]` — drives the UI's generated param forms |
| POST | `/api/pattern/start` | `{type, period_ms, amplitude_deg, offset_deg, duty_pct?, rise_pct?, hold_pct?, fall_pct?}` | mode → `pattern`, loops until stopped |
| POST | `/api/pattern/stop` | — | mode → `manual`, holds last angle |
| POST | `/api/manual/jog` | `{angle_deg}` | REST fallback for manual moves; prefer WS `jog` for latency |
| POST | `/api/record/start` | — | clears the in-RAM recording buffer, mode → `recording` |
| POST | `/api/record/stop` | — | mode → `manual`; buffer is kept until save/discard |
| POST | `/api/record/save` | — | writes the buffer to `/sequence.bin` |
| POST | `/api/record/discard` | — | clears the buffer without saving |
| GET | `/api/sequence` | — | `{present, points, duration_ms}` for the saved sequence |
| POST | `/api/sequence/play` | — | mode → `sequence`, loops the saved recording |
| POST | `/api/sequence/stop` | — | mode → `manual` |
| GET | `/api/settings` | — | `{ap:{ssid,has_password}, servo:{min_us,max_us,min_angle,max_angle,center_angle}, autostart:{enabled,target,pattern}}` — password is never echoed back |
| POST | `/api/settings` | any subset of the GET shape (`ap.password` only if changing it) | persists to NVS; servo calibration changes take effect immediately |
| POST | `/api/settings/reset` | — | restores factory defaults (only recovery path if AP credentials are forgotten) |
| POST | `/api/reboot` | — | applies pending AP credential changes via `ESP.restart()` |

`mode` is one of `idle`, `manual`, `recording`, `pattern`, `sequence`.

## WebSocket `/ws`

- **Client → server**: `{"cmd":"jog","angle":123.4}` — immediate manual
  move, same effect as `POST /api/manual/jog` but lower latency.
- **Server → client**, ~10 Hz: `{"type":"status", "mode":..., "angle":..., "uptime_ms":..., "free_heap":..., "recording":{...}, "sequence":{...}}` — same shape as `GET /api/status`.

## Pattern parameter keys

| Key | Applies to | Meaning |
|---|---|---|
| `period_ms` | all | one full cycle duration |
| `amplitude_deg` | all | peak deviation from `offset_deg` |
| `offset_deg` | all | center angle the pattern oscillates around |
| `duty_pct` | square | % of the period spent at the high value |
| `rise_pct` / `hold_pct` / `fall_pct` | trapezoid | % of the period for each ramp segment (remainder is held low) |

Angle produced = `offset_deg + amplitude_deg * shape(phase)`, where `shape`
is a normalized waveform in `[-1, 1]` — see
`firmware/src/PatternEngine.cpp`.
