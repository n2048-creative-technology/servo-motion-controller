#!/usr/bin/env python3
"""Play a recorded CSV from the PC, through the Master, into the Nodes —
the command-line, no-GUI version of joystick_master_gui.py's Load CSV +
Play panel. Streams `{"node": <id>, "x": ..., "y": ..., "relay": ...}` move
commands in real time (resampled, same as the GUI) rather than uploading
anything to a Node's flash, so nothing is saved on the Nodes: unplug the PC
and the Nodes just hold their last position.

Usage:
    python3 play_recording.py combined.csv                  # once, no loop
    python3 play_recording.py combined.csv --loop            # repeat forever
    python3 play_recording.py combined.csv --loop --speed 0.5
    python3 play_recording.py combined.csv --port /dev/ttyACM0

Playback is NOT looped unless --loop is passed — one full run through the
file, then the script exits (the Nodes simply hold their final commanded
position, same as the GUI's non-loop Play). Needs only pyserial (no
Tkinter/pygame). Speaks ../docs/serial-protocol.md directly; see
remote_sequence.py for the (unrelated) "play a sequence already saved on a
Node's own flash" command.
"""

import argparse
import sys
import time

from sequence_csv import load_csv_rows, resample_rows
from remote_sequence import open_master

TICK_INTERVAL_MS = 40  # ~25 Hz, matches joystick_master_gui.py / the web UI
SPEED_MIN, SPEED_MAX = 0.1, 2.0


def send_row(master, xs, ys, lights, last_pos, relay_state):
    """Send one resampled row's commands, same fill-from-last-position logic
    as joystick_master_gui.py's _send_playback_row: a Node present in any
    channel this row gets a command; an axis the row doesn't carry for it
    holds wherever it last was."""
    node_ids = sorted(set(xs) | set(ys) | set(lights))
    for node_id in node_ids:
        pos = last_pos.setdefault(node_id, {"x": 135.0, "y": 90.0})
        x = xs.get(node_id, pos["x"])
        y = ys.get(node_id, pos["y"])
        relay_on = lights.get(node_id, relay_state.get(node_id, False))
        master.send({"node": node_id, "x": round(x, 1), "y": round(y, 1), "relay": relay_on})
        pos["x"], pos["y"] = x, y
        if node_id in lights:
            relay_state[node_id] = relay_on


def play(master, plan, loop, stop_check=None):
    """Streams `plan` (resample_rows output) once, or forever if loop. Sleeps
    between rows to land each one close to its t_ms offset in real time.
    stop_check: optional callable polled between rows; returning True ends
    playback early (hook for e.g. Ctrl-C handling by the caller)."""
    last_pos = {}
    relay_state = {}
    while True:
        start = time.monotonic()
        for i, (t_ms, xs, ys, lights) in enumerate(plan):
            if stop_check and stop_check():
                return
            target = start + t_ms / 1000.0
            delay = target - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            send_row(master, xs, ys, lights, last_pos, relay_state)
        if not loop:
            return
        print("-- loop --")


def main():
    parser = argparse.ArgumentParser(
        description="Play a recorded CSV from the PC through the Master into the Nodes.",
    )
    parser.add_argument("csv", help="recording to play (sequence_csv.py format)")
    parser.add_argument("--port", help="the Master's serial port (default: probe every port)")
    parser.add_argument(
        "--loop", action="store_true",
        help="repeat the recording forever instead of playing it once (default: play once)",
    )
    parser.add_argument(
        "--speed", type=float, default=1.0,
        help=f"playback speed multiplier, {SPEED_MIN}-{SPEED_MAX} (default 1.0)",
    )
    args = parser.parse_args()

    speed = min(SPEED_MAX, max(SPEED_MIN, args.speed))
    if speed != args.speed:
        print(f"-- speed clamped to {speed} (range {SPEED_MIN}-{SPEED_MAX}) --", file=sys.stderr)

    try:
        rows = load_csv_rows(args.csv)
    except (OSError, ValueError, IndexError) as exc:
        sys.exit(f"couldn't read {args.csv}: {exc}")
    if not rows:
        sys.exit(f"{args.csv} has no rows to play")

    plan = resample_rows(rows, TICK_INTERVAL_MS, speed)
    node_ids = sorted({nid for _t, xs, ys, lights in rows for nid in (*xs, *ys, *lights)})
    print(f"loaded {len(rows)} rows ({len(plan)} resampled @ {speed}x) for node(s) {node_ids}")

    master, port, _nodes = open_master(args.port)
    print(f"Master on {port}" + (" — looping until Ctrl-C" if args.loop else ""))
    try:
        play(master, plan, args.loop)
    except KeyboardInterrupt:
        print("\n-- stopped --")
    finally:
        master.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
