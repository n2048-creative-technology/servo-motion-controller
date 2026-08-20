# Master serial protocol

A board in **Master** mode (Settings → Network) bridges a PC connected over
USB to every **Node** board in ESP-NOW range. It reads newline-delimited JSON
from `Serial` and turns positioning commands into ESP-NOW broadcasts; it
never drives a locally-attached servo itself.

- Port: the board's USB-CDC serial port (e.g. `/dev/ttyACM0` on Linux).
- Baud: `115200`.
- Framing: one JSON object per line, terminated by `\n`. `\r` is tolerated
  and stripped.

## Commands (PC → Master)

**Move a node**
```json
{"node": 3, "x": 120.5, "y": 90.0}
```
- `node`: target Node's id (`0`–`250`). `0` broadcasts to every Node.
- `x`: pan angle (D10) in degrees, clamped by that Node's own X calibration.
- `y`: tilt angle (D3) in degrees, clamped by that Node's own Y calibration.
- `angle`: accepted as a synonym for `x`, so scripts written for the
  single-servo firmware keep working.

**An axis you leave out holds its last commanded position** rather than
snapping to a default, so a tool that only drives pan never disturbs tilt.
Both axes travel in one packet: splitting them would let a Node act on half a
move, dog-legging its way to a diagonal — and recording that dog-leg if a
capture is running.
- `relay` *(optional, boolean)*: also switch that Node's relay/light output
  on D7. **Omit it to leave the light alone** — the Master remembers the last
  state sent to each target and re-sends that, so a tool that knows nothing
  about the relay can't switch one off by accident.

Relay state is tracked **per target**, so switching Node 3's light never
disturbs Node 5's — one button per Node on a gamepad works as you'd expect
(see `joystick_master_gui.py`). The one thing to watch: `node: 0` (broadcast)
is its own target entry, so a broadcast command carries *its* relay state to
every Node at once. Mix broadcast and per-node commands with that in mind —
if you're controlling lights individually, address Nodes individually.

The relay state travels on the move command rather than in a packet of its
own so it inherits the angle's resend and ordering guarantees — and so a Node
recording a Master-driven sequence captures the light exactly in step with
the motion, which a separate best-effort packet couldn't promise.

To switch a light without moving anything, re-send that Node's current angle
(from `{"cmd":"list"}`, below) with the new `relay` value.

**List known nodes**
```json
{"cmd": "list"}
```
Returns the Master's in-RAM table of Nodes it has heard a heartbeat from
(nodes broadcast one every ~1s while running — see `NET_HELLO_INTERVAL_MS` in
`firmware/include/Config.h`). Entries aren't actively expired; treat a large
`age_ms` as "probably offline".

**Upload a sequence to a Node** — remotely triggers a recording on that Node
(same effect as its own web UI's Record tab), so it ends up saved on the
Node's own flash under a name, playable standalone via that Node's Settings
→ Autostart with no PC/Master/controller involved afterward:
```json
{"cmd": "remote_record_start", "node": 3}
```
Then stream ordinary move commands (`{"node":3,"x":...,"y":...}`, above) at a
steady real-time pace — the Node is now in RECORDING mode, so every command
that reaches it both moves its servo *and* gets captured. When done:
```json
{"cmd": "remote_record_stop", "node": 3, "name": "dance1"}
```
`name` is sanitized to `[A-Za-z0-9_-]`, truncated to 23 characters. The Node
saves whatever it captured as `/seq/<name>.bin` — overwriting any existing
file of that name — and reports back asynchronously (see `upload_result`
below) — this can take a moment, since it travels Node → Master over ESP-NOW
after the save completes. A Node's recording buffer caps at `MAX_SEQ_POINTS ×
RECORD_INTERVAL_MS` (10 minutes by default); don't stream longer than that or
the excess is silently dropped. `scripts-tools/joystick_master_gui.py`'s
"Upload to Node…" button implements this whole flow, including only ever
sending one Node's own column from a multi-node recording (see its README).

**Clear a Node's saved recordings** — deletes every sequence file on that
Node, same effect as its own web UI's "Clear All" button in the Record tab:
```json
{"cmd": "remote_clear", "node": 3}
```
Unlike the upload commands above, this has no delivery confirmation (no
`upload_result`-style reply) — it's a one-shot housekeeping step, so the reply
`{"ok":true}` only means the Master queued the ESP-NOW send locally, not that
the Node actually received and finished it. `joystick_master_gui.py`'s Upload
dialog has a "Clear Node's saved recordings before uploading" checkbox that
sends this and waits a short settle time before starting the upload.

**Check a Node's free space** — an upload preflight check, asking how much
LittleFS space a Node has left before streaming a recording to it:
```json
{"cmd": "space_query", "node": 3}
```
Replies asynchronously (see `space_reply` below), same fire-and-forget
delivery characteristics as `remote_clear` — no guarantee it arrives or that
a reply comes back. `joystick_master_gui.py`'s Upload dialog sends this
automatically before every upload, estimates the recording's on-disk size
from its duration, and aborts that Node's upload with a clear error if the
reply shows insufficient space; if no reply arrives within ~800ms it uploads
anyway rather than blocking on an unreliable link.

## Replies (Master → PC)

- Move command: `{"ok":true}` or `{"ok":false,"error":"..."}`
- `list`: `{"type":"nodes","nodes":[{"id":3,"x":120.5,"y":90.0,"relay":true,"age_ms":840}, ...]}`
  — `x`, `y` and `relay` are that Node's *actual* state, reported in its own
  heartbeat, so they reflect the head however it was moved or switched (a
  Master command, the Node's own web UI, or its sequence playback).
- `remote_record_start`/`remote_record_stop`: `{"ok":true}` once the ESP-NOW
  send itself succeeded — *not* confirmation the Node actually saved
  anything, that's the separate asynchronous `upload_result` below.
- `upload_result` (asynchronous, arrives whenever the Node's own SEQ_ACK
  makes it back — could be well after the `remote_record_stop` reply):
  `{"type":"upload_result","node":3,"name":"dance1","ok":true,"points":842}`.
  `ok:false` means the Node received the stop request but failed to save;
  a `"reason"` string is included explaining why (no movement was ever
  captured — most often because the start request never arrived, or the
  Node lost power/reset partway through; an invalid name; or a write
  failure, e.g. out of space). No reply at all after a few seconds means the
  stop request itself likely didn't arrive — resend it.
- `space_reply` (asynchronous, reply to `space_query`):
  `{"type":"space_reply","node":3,"free_bytes":48200}`.
- Malformed line: `{"ok":false,"error":"bad_json"}`

Every reply is also one line, `\n`-terminated.

## Robustness against dropped packets

ESP-NOW has no delivery guarantee, and a Node's radio/CPU is shared with
whatever else it's doing — e.g. a phone joining that Node's own AP to view
its web UI competes for airtime/CPU with ESP-NOW on the same single core.
Two mechanisms make this self-healing instead of silently getting stuck:

- **Master**: remembers the last angle sent to each target (a specific Node
  id, or the broadcast target) and re-sends it at least every
  `NET_CMD_RESEND_INTERVAL_MS` (300 ms by default), even if nothing changed.
  A command that gets dropped in transit is corrected by the next periodic
  resend without needing a new movement to trigger it.
- **Node**: independently of receiving anything new, re-applies its last
  commanded angle to the servo at least every `SERVO_REAPPLY_INTERVAL_MS`
  (250 ms by default).

Net effect: whichever command — local jog, another app's command, or a
gamepad's stream — reached a Node *last* always wins and keeps winning, and
if the gamepad moves faster than packets can be delivered, intermediate
positions may be skipped but the Node reliably converges on the final
position once movement stops (see `firmware/include/Config.h` for both
constants).

### Ordering: a resend can't move the servo backward

The periodic resend above means more than one in-flight CMD packet for the
same target is now normal, not exceptional. Under a degraded/congested link
(the same phone-joins-the-AP scenario), packets can be delayed enough to
**arrive out of order** — without protection, a resend of an *older* angle
that gets delayed past a *newer* command would erratically yank the servo
back after it had already moved on.

Every CMD packet carries a per-boot `sessionId` (randomized when a Master's
ESP-NOW comes up) and a `seq` that increments on every send, resends
included. A Node tracks the highest `(sessionId, seq)` it's applied and
silently drops anything not newer — so a delayed/stale packet is a no-op,
never a step backward. A changed `sessionId` (the Master rebooted) always
wins, so a Node doesn't get stuck ignoring a Master that came back with its
counter reset to 0. Non-finite angles (a corrupted payload) are dropped the
same way, as cheap insurance against ever computing a garbage pulse width.

This is why **Master and every Node must run matching firmware** — a
A Node can hold **6min40s** of recording (8000 points at 50ms). That came
down from 10 minutes when each point gained its Y axis: the point grew 8 → 12
bytes, and the count was cut to keep the fixed buffer at the same 96KB rather
than spend another 48KB of the C3's RAM. See `MAX_SEQ_POINTS` in
`firmware/include/Config.h`.

`NET_PACKET_VERSION` mismatch (bumped whenever the packet layout changes,
as it did for this ordering fix) makes them silently ignore each other
rather than misinterpret each other's bytes.

### A Node can reset mid-upload on fast/large movements — this is a power issue, not a protocol one

Uploading a recording with frequent large, rapid swings (e.g. repeatedly
slewing close to a servo's full 0–270° range within a couple of ticks) can
brown out a Node and reset it mid-transfer, purely from the servo's own
current draw spiking faster than its power supply can source — confirmed by
reproducing it with real recorded joystick data, then ruling out every
software explanation: the *identical* recording succeeds after a fresh
reboot, and a gentle low-amplitude recording of the same duration succeeds
reliably on the exact same board and firmware every time, while the
high-motion one fails consistently. ESP-NOW/servo software can't distinguish
this from ordinary packet loss — it just sees the Node go silent and its
retry/timeout logic (and the `NoPointsCaptured` reason above) kick in the
same as any other dropped connection. If uploads fail specifically on
high-motion recordings: give the servo(s) their own adequately-sized supply
rather than sharing the ESP32's own rail, add bulk capacitance near the
servo, or — if that's not practical mid-show — favor recordings with gentler
transitions.

## Requirements for this to work

- Every board that should talk to each other (Master + all its Nodes) must
  share the same fixed AP WiFi channel — `AP_WIFI_CHANNEL` in
  `firmware/include/Config.h` (`1` by default, not currently exposed in the
  UI). ESP-NOW only reaches peers on the same channel; if you ever change
  that constant, rebuild and reflash every board in the group.
- Each Node needs a unique Node ID (1–250) assigned in its own Settings →
  Network tab. Two Nodes sharing an ID will both react to commands addressed
  to that ID.
- The Master needs no Node ID and no servo attached — it's a pure bridge.
- **Every board — Master and all Nodes — needs matching firmware.** Each CMD
  and HELLO packet carries a `NET_PACKET_VERSION`; a board running an older
  or newer build than the rest silently drops (and, since this version,
  logs) every packet from/to it rather than misinterpreting the bytes. A
  Node that "disappears" from the Master's known-node table despite being
  powered on and in range — while everything else works — usually means it
  missed the last firmware update round. Check the Master's serial console
  for `[NET] ignoring packet with protocol version...` and reflash whatever
  it names.

## Example: `pyserial`

```python
import json
import serial

port = serial.Serial("/dev/ttyACM0", 115200, timeout=1)

def move(node_id: int, x: float, y: float) -> dict:
    port.write((json.dumps({"node": node_id, "x": x, "y": y}) + "\n").encode())
    return json.loads(port.readline())

def list_nodes() -> dict:
    port.write(b'{"cmd":"list"}\n')
    return json.loads(port.readline())

print(move(3, 120.5))
print(list_nodes())
```
