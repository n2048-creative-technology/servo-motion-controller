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
{"node": 3, "angle": 120.5}
```
- `node`: target Node's id (`0`–`250`). `0` broadcasts to every Node.
- `angle`: target angle in degrees, clamped by each Node's own servo
  calibration (Settings → Servo Calibration on that board).

**List known nodes**
```json
{"cmd": "list"}
```
Returns the Master's in-RAM table of Nodes it has heard a heartbeat from
(nodes broadcast one every ~1s while running — see `NET_HELLO_INTERVAL_MS` in
`firmware/include/Config.h`). Entries aren't actively expired; treat a large
`age_ms` as "probably offline".

## Replies (Master → PC)

- Move command: `{"ok":true}` or `{"ok":false,"error":"..."}`
- `list`: `{"type":"nodes","nodes":[{"id":3,"angle":120.5,"age_ms":840}, ...]}`
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
`NET_PACKET_VERSION` mismatch (bumped whenever the packet layout changes,
as it did for this ordering fix) makes them silently ignore each other
rather than misinterpret each other's bytes.

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

## Example: `pyserial`

```python
import json
import serial

port = serial.Serial("/dev/ttyACM0", 115200, timeout=1)

def move(node_id: int, angle: float) -> dict:
    port.write((json.dumps({"node": node_id, "angle": angle}) + "\n").encode())
    return json.loads(port.readline())

def list_nodes() -> dict:
    port.write(b'{"cmd":"list"}\n')
    return json.loads(port.readline())

print(move(3, 120.5))
print(list_nodes())
```
