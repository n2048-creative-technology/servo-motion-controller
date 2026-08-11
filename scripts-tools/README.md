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

**Multiple controllers work at once** — plug in as many as you like (e.g. two
gamepads for two people, or a pile of specialized controllers), and freely mix
which one drives which Node: (controller 1, axis 2) → Node 3 and
(controller 2, axis 4) → Node 1 can coexist as two ordinary mappings. Each
mapping remembers its own controller by a stable per-device GUID (not by
plug-in order, which can change across reconnects), so it stays correctly
attached to the right physical device no matter what order things get
plugged in.

Workflow:

1. Connect to the Master's serial port (same as `master_gui.py`).
2. Connect your controller(s), then click **Refresh Controllers** — every
   detected controller is opened at once and listed with a live readout of
   its raw axis values, handy for telling axes apart. There's no "select a
   controller" step; all connected ones are available for mapping
   simultaneously.
3. Click **Learn New Mapping**, then move *only* the physical axis you want
   to link — on *any* connected controller — for the ~4 seconds the dialog
   is watching. It watches every axis of every controller at once and picks
   whichever one moved the most, identifying both the controller and the
   axis automatically, shows you its observed raw range, and asks you to
   assign a **Node ID** — pick one from the dropdown of Nodes the Master has
   actually heard a heartbeat from (auto-refreshed every 2s in the
   background; hit the dialog's own Refresh if one just came online), or
   type an ID directly for a Node that hasn't reported in yet — and an
   **angle range** (defaults to 0–270°, the firmware's default servo
   calibration — adjust to match the target servo's actual calibrated range
   from its own Settings tab). An **Invert** checkbox flips direction
   without needing to redo the range. Repeat per axis you want to use, on
   whichever controller it's on; one axis can also be linked to more than
   one Node if you add it again. **Save Mappings…** writes the whole mapping
   list (each with its own controller GUID + name, axis index, calibrated
   raw range, Node ID, angle range, invert) to a JSON file; **Load
   Mappings…** restores them later without re-learning. A mapping whose
   controller isn't currently connected shows `[controller not connected]`
   in the list and is skipped by streaming/recording until you plug it back
   in and click Refresh Controllers — it isn't lost, and reattaches
   automatically once that controller (matched by GUID, or by name as a
   fallback for files saved by an older version of this tool) is back.
   **Remove All** clears every mapping at once (with a confirmation prompt)
   instead of selecting them one by one.
4. Check **Stream mapped axes to their Nodes** to start sending — every
   mapped axis, across all connected controllers, is recalculated ~25 Hz and
   only resent when it moves more than ~0.2°, to avoid flooding the link
   while idle.
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
   <id>, "angle": ...}` — no controller needed at all, so a captured
   performance can be replayed standalone. **Loop** repeats it; **Stop**
   cancels mid-playback. **Speed** (0.1–2.0, default 1.0) doesn't just scale
   the delay between the original rows — pressing Play resamples the whole
   recording onto a fresh ~25 Hz grid at that speed (the CSV file itself is
   untouched): below 1.0 it interpolates *extra* in-between points so slow
   motion stays smooth instead of getting choppy from stretched-out gaps;
   above 1.0 it interpolates *down* to ~25 Hz instead of bursting every
   original row as fast as possible. Either way playback always lands
   exactly on the final recorded position.
7. **Upload to Node…** pushes a recording onto a Node's own flash instead of
   just replaying it live: pick a source (the loaded CSV or your last live
   recording), a Node ID present in it (or check **Upload to all Nodes
   present in source** to push every Node's own column to its Node at
   once), and a name. Each Node only ever receives its own column — never
   another Node's data — streamed through the Master at real-time pace (via
   `remote_record_start`/`remote_record_stop`, see
   [../docs/serial-protocol.md](../docs/serial-protocol.md)). Multiple
   Nodes' uploads run concurrently rather than queued one after another —
   each Node's point stream overlaps almost completely with the others —
   with only a small (~150ms per Node) stagger to their start, so their
   remote_record_start/stop control packets and SEQ_ACK replies don't all
   land in the same instant and collide (real hardware testing without this
   staggering showed every Node's ack getting lost when four uploads landed
   at once). The status line/log track each Node's own progress plus a
   combined summary once every Node in the batch has finished. Each Node
   saves its sequence and can then loop it on every
   boot via its own Settings → Autostart, completely standalone — no PC,
   Master, or controller needed afterward. Long recordings (over 10
   minutes, a Node's own capacity) are truncated with a warning per Node.
   The start request is sent redundantly (a lost one is otherwise invisible
   — the Node still moves on every point of the stream that follows
   regardless of whether it actually started recording) and a failed/lost
   stop-and-save confirmation is retried once automatically. Re-uploading a
   name that already exists on a Node overwrites it. Check **Clear Node's
   saved recordings before uploading** to have each target Node delete every
   sequence it's currently holding (`remote_clear`, see
   [../docs/serial-protocol.md](../docs/serial-protocol.md)) right before its
   own upload starts — a clean slate, e.g. to keep a Node's flash from
   accumulating a pile of one-off names over many test uploads. Before
   streaming anything, each Node's upload also asks it how much flash space
   it has free (`space_query`) and estimates the recording's on-disk size
   from its duration — if the Node's own reply shows there isn't enough
   room, that Node's upload is aborted immediately with a clear error
   instead of streaming for the full duration only to fail at the very end;
   a Node that doesn't answer within ~800ms (a dropped packet, not
   necessarily a real problem) just gets uploaded to anyway. A failure's log
   line and status text explain *why* where possible — a Node that captured
   no movement at all most often means either its start request never
   arrived or **it lost power and reset partway through the transfer**:
   recordings with frequent large, fast swings can brown out a Node from the
   servo's own current draw and reset it mid-upload — this is a power-supply
   characteristic, not a firmware bug (see
   [../docs/serial-protocol.md](../docs/serial-protocol.md) for how this was
   isolated). If uploads fail specifically on your highest-motion
   recordings, that's the first thing to check.
8. **Discard Recording** clears the in-memory recording buffer (the one
   **Save Recording As…**/**Upload to Node…** would use) without writing it
   anywhere — a confirmation prompt guards against losing an unsaved take.
   To clear what's already saved *on a Node's own flash*, either check
   **Clear Node's saved recordings before uploading** in the Upload dialog,
   or use the **Clear All** button in that Node's own web UI (Record tab).

The axis-mapping math, CSV round-trip, and upload data-extraction/resampling
logic are all unit-tested, and the window itself has been launched and
visually confirmed on a real display with two real controllers connected
simultaneously (an Xbox pad and a Logitech joystick) — but the Upload to
Node… flow's actual PC → Master → Node round trip hasn't been exercised
against real hardware in this sandbox (no WiFi adapter here to complete
that loop). Try a real upload before relying on it for a show.
