# scripts-tools

PC-side tools for this project. Currently just the Master serial GUI.

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
