# scripts-tools

PC-side tools for this project, all talking to a Master board over USB
serial using the protocol in [../docs/serial-protocol.md](../docs/serial-protocol.md).
`serial_link.py` is the shared connection code both GUIs below use — not a
standalone tool.

## `master_gui.py`

A small Tkinter app that connects to a Master board over USB serial and
sends servo positioning commands to its Nodes — a GUI front-end for the
protocol in [../docs/serial-protocol.md](../docs/serial-protocol.md), for
when you'd rather drag a slider than write a script.

Setup:

```bash
python3 -m venv .venv && source .venv/bin/activate   # optional but recommended
pip install -r requirements.txt
python3 master_gui.py
```

Tkinter ships with most desktop Python installs. On Debian/Ubuntu, if `import
tkinter` fails, install it separately: `sudo apt install python3-tk`.

What it does:

- Lists and connects to serial ports (115200 baud, matching the Master).
- Polls the Master's known-node table (`{"cmd":"list"}`) every 2s and shows
  it in a table — angle and how long ago each Node last heartbeat-ed.
- Select one or more rows in that table (ctrl/shift-click) to target them,
  or check **All nodes** to broadcast; a fallback node-ID field covers
  targeting a Node that hasn't reported in yet.
- A Send button for one-shot moves, plus an optional "live jog" mode that
  streams the angle slider at ~25 Hz while dragging (same throttle the web
  UI's jog slider uses).
- A scrolling log of every line sent/received, for debugging.

A multi-node selection is a client-side fan-out — the GUI just sends one
`{"node": <id>, "angle": ...}` line per selected Node in quick succession.
The wire protocol itself is unchanged; see the serial-protocol doc for the
full command/reply reference.

## `joystick_master_gui.py`

Drive Nodes from a physical joystick/gamepad instead of on-screen sliders,
using [pygame](https://www.pygame.org/) for controller input. Same setup as
above (`pip install -r requirements.txt` — this pulls in `pygame` too, then
`python3 joystick_master_gui.py`).

Workflow:

1. Connect to the Master's serial port (same as `master_gui.py`).
2. Pick your controller from the **Controller** dropdown and click
   **Select** — a live readout of every raw axis value appears, handy for
   telling axes apart before you commit to one.
3. Click **Learn New Mapping**, then move *only* the physical axis you want
   to link, for the ~4 seconds the dialog is watching. It picks whichever
   axis moved the most, shows you its observed raw range, and asks you to
   assign a **Node ID** — pick one from the dropdown of Nodes the Master has
   actually heard a heartbeat from (auto-refreshed every 2s in the
   background; hit the dialog's own Refresh if one just came online), or
   type an ID directly for a Node that hasn't reported in yet — and an
   **angle range** (defaults to 0–270°, the firmware's default servo
   calibration — adjust to match the target servo's actual calibrated range
   from its own Settings tab). An **Invert** checkbox flips direction
   without needing to redo the range. Repeat per axis you want to use; one
   axis can also be linked to more than one Node
   if you add it again. **Save Mappings…** writes the whole mapping list
   (axis index, calibrated raw range, Node ID, angle range, invert) to a
   JSON file, tagged with the controller's name; **Load Mappings…** restores
   them later without re-learning — you're warned (not blocked) if the
   currently-selected controller's name doesn't match the file's, since
   axis numbering can differ across controller models.
4. Check **Stream mapped axes to their Nodes** to start sending — each
   mapped axis's angle is recalculated ~25 Hz and only resent when it moves
   more than ~0.2°, to avoid flooding the link while idle.
5. **Start Recording** captures every mapped Node's computed angle (not the
   raw axis — the physical controller's identity is irrelevant on playback)
   at the same ~25 Hz, timestamped from recording start. **Save Recording
   As…** writes it to CSV: one `t_ms` column plus one `node_<id>` column per
   Node that was mapped, e.g.:

   ```csv
   t_ms,node_3,node_7
   0,135.0,90.0
   40,138.2,90.0
   80,141.0,91.5
   ```

6. **Load CSV…** + **Play** replays a recording by sending `{"node":
   <id>, "angle": ...}` at the original relative timing — no controller
   needed at all, so a captured performance can be replayed standalone.
   **Loop** repeats it; **Stop** cancels mid-playback.

Not hands-on verified in the sandbox this was built in (no display server,
no physical controller attached) — the axis-mapping math and CSV
round-trip are logic-tested, but the actual Tkinter/pygame window hasn't
been visually driven. Try a real controller before relying on it for a show.
