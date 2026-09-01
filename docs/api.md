# REST / WebSocket API

Base URL: `http://192.168.4.1` while connected to the device's AP.

## REST routes

| Method | Route | Body | Response / notes |
|---|---|---|---|
| GET | `/api/status` | — | `{mode, x, y, angle, relay_on, uptime_ms, free_heap, firmware_version, recording:{active,points}, sequence:{present,name,points,duration_ms,playing,position_ms}}` — `x`/`y` are the two servo axes (`angle` is a deprecated alias for `x`); `sequence.position_ms` is how far into the loop playback has got, for a playhead |
| GET | `/api/patterns` | — | `[{type, label, params: "comma,separated,keys"}]` — drives the UI's generated param forms |
| POST | `/api/pattern/start` | `{x:{type, period_ms, …}, y:{…}}` | mode → `pattern`, loops until stopped. Either axis may be omitted to leave it as-is; a flat body with no `x`/`y` is applied to X |
| POST | `/api/pattern/stop` | — | mode → `manual`, holds last angle |
| POST | `/api/manual/jog` | `{x_deg?, y_deg?}` | REST fallback for manual moves; prefer WS `jog` for latency. An omitted axis holds its position; `angle_deg` is still accepted as X |
| POST | `/api/relay` | `{on}` | switches the relay/light on D7; doesn't change `mode` (see below) |
| GET | `/api/network/targets` | — | `{broadcast_all, node_ids:[...]}` — Master only: which Node(s) `manual/jog`, WS `jog`, and `pattern/start` currently drive |
| POST | `/api/network/targets` | `{broadcast_all, node_ids:[...]}` | Master only: retarget jog/pattern output — broadcast to all Nodes, or an explicit id list (client-side fan-out, one ESP-NOW CMD per id); ephemeral, resets to broadcast-all on reboot |
| POST | `/api/record/start` | — | clears the in-RAM recording buffer, mode → `recording` |
| POST | `/api/record/stop` | — | mode → `manual`; buffer is kept until save/discard |
| POST | `/api/record/save` | `{name}` | writes the buffer to `/seq/<name>.bin` (sanitized to `[A-Za-z0-9_-]`, ≤23 chars) |
| POST | `/api/record/discard` | — | clears the buffer without saving |
| GET | `/api/sequences` | — | `[{name, points, duration_ms}, ...]` — every sequence saved on this board, local or uploaded via a Master |
| GET | `/api/sequence/data?name=…&points=…` | — | `{name, duration_ms, has_y, points:[[t_ms,x,y,light], ...]}` — a downsampled view of one saved sequence, for plotting it. `points` caps at 300 (the plot is only ~600px wide); reading never disturbs whatever is currently playing |
| POST | `/api/sequence/play` | `{name}` | loads and plays that sequence on loop; mode → `sequence` |
| POST | `/api/sequence/stop` | — | mode → `manual` |
| POST | `/api/sequence/delete` | `{name}` | removes that sequence file; clears it as "active" first if it was playing |
| POST | `/api/sequences/clear` | — | removes every saved sequence on this board; `{ok:true, removed:N}` |
| GET | `/api/settings` | — | `{ap:{ssid,has_password}, servo:{x:{pin,min_us,max_us,min_angle,max_angle,center_angle,invert}, y:{…}}, relay:{pin,active_low,on}, autostart:{enabled,target,pattern:{x:{…},y:{…}},sequence_name}, network:{mode,node_id}}` — password is never echoed back |
| POST | `/api/settings` | any subset of the GET shape (`ap.password` only if changing it) | persists to NVS; servo calibration changes take effect immediately, `network.*` changes need a reboot (see `/api/reboot`) |
| POST | `/api/settings/reset` | — | restores factory defaults (only recovery path if AP credentials are forgotten) |
| POST | `/api/reboot` | — | applies pending AP credential / network-mode changes via `ESP.restart()` |
| GET | `/api/network/nodes` | — | `{nodes:[{id,x,y,relay,age_ms}, ...]}` — a Master's in-RAM table of Nodes it has heard an ESP-NOW heartbeat from (`x`/`y`/`relay` are each Node's actual state, however it was set); empty list on Standalone/Node boards |

`mode` is one of `idle`, `manual`, `recording`, `pattern`, `sequence`, `network`
(`network` = last moved by a wireless command from a Master, Node boards only).

## Two axes

A node drives a pan/tilt head: **X (pan) on D10** and **Y (tilt) on D3**. The
two calibrate independently (`servo.x` / `servo.y`), run their own pattern
each, and are both captured into recordings.

Both axes always travel together in one command — one `jog`, one CMD packet,
one recorded point. Splitting them would let a node act on half a move,
visibly dog-legging its way to a diagonal and recording that dog-leg if a
capture is running.

`/api/sequence/data` reports `has_y: false` for a sequence recorded before
there was a tilt axis — its `y` column is padding, not data, and shouldn't be
plotted as a flat line at zero.

Sequences recorded before this firmware had a Y axis still load and play:
their X and light tracks replay as before, and the tilt axis is left where it
is rather than being driven somewhere the recording never described.

`relay.active_low` inverts the pin level the "on" state drives, for relay
boards that switch closed when their IN pin is pulled low. It changes only the
electrical level: `relay_on`, `POST /api/relay`, and what a recording stores
all mean the same logical on/off either way. `relay.pin` is read-only
(compiled in per board — GPIO20 on the C3, GPIO44 on the S3, silkscreen D7 on
both); `relay.on` is the live state and is ignored on POST — switch it with
`POST /api/relay` instead.

`servo.<axis>.invert` flips which physical end of the pulse range a given
angle drives (for a servo mounted mirrored/reversed relative to how min/max
angle were calibrated) — it doesn't change what an angle *means* to the rest of
the API: jog/pattern/sequence/network commands, recordings, and `GET
/api/status`'s `angle` are unaffected, only the resulting pulse direction is.

`network.mode` is one of `standalone` (default), `node`, `master` — see
[serial-protocol.md](serial-protocol.md) for the Master's PC-facing protocol
and the ESP-NOW design behind Master/Node mode.

When `autostart.target` is `"sequence"`, `autostart.sequence_name` picks
*which* saved sequence (from `GET /api/sequences`) loops on boot — a board
can hold several (locally recorded, or remotely uploaded by a Master; see
serial-protocol.md's `remote_record_start`/`remote_record_stop`), not just
one fixed recording like early versions of this firmware.

On a **Master** board the Light toggle behaves like the trackpad: it
switches the relay on whichever Node(s) `/api/network/targets` selects, not
the Master's own D7 pin. Relay state is tracked per target, so a Node's light
is only changed by a command actually addressed to it.

On a **Master** board, `/api/manual/jog`, the WS `jog` command, and
`/api/pattern/*` don't drive a local servo at all — they drive whichever
Node(s) `/api/network/targets` currently selects, over ESP-NOW. `GET
/api/status`'s `angle`/`mode` then reflect the last commanded network angle,
not a physical servo position.

## WebSocket `/ws`

- **Client → server**: `{"cmd":"jog","x":123.4,"y":90.0}` — immediate manual
  move, same effect as `POST /api/manual/jog` but lower latency. Either axis
  may be omitted (it holds); `angle` is still read as X.
- **Client → server**: `{"cmd":"relay","on":true}` — same effect as
  `POST /api/relay`.
- **Server → client**, ~10 Hz: `{"type":"status", "mode":..., "x":..., "y":..., "relay_on":..., "uptime_ms":..., "free_heap":..., "recording":{...}, "sequence":{...}}` — same shape as `GET /api/status`.

## Pattern parameter keys

| Key | Applies to | Meaning |
|---|---|---|
| `period_ms` | all | one full cycle duration |
| `amplitude_deg` | all | peak deviation from `offset_deg` |
| `offset_deg` | all | center angle the pattern oscillates around |
| `duty_pct` | square | % of the period spent at the high value |
| `rise_pct` / `hold_pct` / `fall_pct` | trapezoid | % of the period for each ramp segment (remainder is held low) |
| `interval_min_ms` / `interval_max_ms` | random | bounds of the randomly-drawn interval from one move to the next |
| `max_speed_dps` | random | speed ceiling for each move, in degrees per second (5–400) |

Angle produced = `offset_deg + amplitude_deg * shape(phase)`, where `shape`
is a normalized waveform in `[-1, 1]` — see
`firmware/src/PatternEngine.cpp`.

### `random`

The one shape that isn't a waveform, and the one that ignores `period_ms` /
`amplitude_deg` / `offset_deg` entirely. Each cycle it picks a new target
uniformly inside that axis's **calibrated travel** (`servo.min_angle` …
`servo.max_angle`, so a servo configured for 180° never gets sent past 180°),
then waits a fresh interval drawn uniformly from `[interval_min_ms,
interval_max_ms]` before the next one.

Getting there is deliberately not a jump. Each move is eased with a smoothstep
profile — zero velocity at both ends — stretched over however long it takes to
keep peak speed at or below `max_speed_dps`, so what reaches the servo is a
ramp it can track instead of a step that would slam the gear train and spike
the current draw. Two consequences worth knowing:

- The interval covers the **whole cycle** (move + hold), not just the pause.
  If a long move wouldn't fit in the drawn interval, the interval is extended
  so the move always completes plus a short settle — a large range with a low
  speed cap therefore moves less often than the interval bounds alone suggest.
- Changing servo calibration re-ranges a running random pattern immediately;
  no restart needed.
- Set **both** axes to `random` and they share one schedule: the head picks a
  whole new (X, Y) point each interval and travels there as a single eased
  move, with the duration set by whichever axis needs longer so neither
  breaks its own speed cap. Set only one to `random` and it runs on its own
  schedule alongside whatever waveform the other axis is doing.
