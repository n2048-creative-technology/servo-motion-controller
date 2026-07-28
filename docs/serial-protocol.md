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
