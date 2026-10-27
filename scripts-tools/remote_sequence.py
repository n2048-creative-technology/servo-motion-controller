#!/usr/bin/env python3
"""Make every Node the Master currently knows about play (once) one of its own
saved sequences — the command-line version of the GUIs' "Play saved sequence on
Node(s)" panel (remote_play.py).

Usage:
    python3 remote_sequence.py <recording_name>
    python3 remote_sequence.py <recording_name> --port /dev/ttyACM0

Without --port, every serial port is probed with {"cmd": "list"} and the one
answering with a node table is used (a Node's own USB port answers
"not_master"). Speaks ../docs/serial-protocol.md:
  -> {"cmd": "list"}
  <- {"type": "nodes", "nodes": [{"id": 3, ...}, ...]}
  -> {"cmd": "remote_play", "node": <id>, "name": "<sequence>", "play_id": <n>}
  <- {"type": "play_result", "node": N, "name": "...", "ok": bool, "reason": "..."}

Each Node is addressed by its own id, not broadcast (node 0), so every one of
them gets its own play_result — and its own retries: a play is repeated every
400 ms until that Node answers, up to 4 times, same as the GUIs. All retries
share one play_id, so they don't restart a Node that's already playing; each
run of this script uses a new one, restarting the sequence from the top. Exits 0 only
if every Node reported it is playing.
"""

import argparse
import json
import queue
import random
import sys
import time

import serial
from serial.tools import list_ports

from serial_link import SerialLink

# Same as remote_play.py's (not imported from there: it pulls in tkinter).
RETRY_INTERVAL_MS = 400
ATTEMPTS = 4
PROBE_TIMEOUT_S = 1.5
# A just-opened Master may not have heard a heartbeat from every Node yet;
# keep asking for the table until it has someone in it, for up to this long.
DISCOVERY_TIMEOUT_S = 4.0
LIST_POLL_INTERVAL_S = 0.5


class Master:
    def __init__(self):
        self._lines = queue.Queue()
        self._error = None
        self.link = SerialLink(on_line=self._lines.put, on_error=self._on_error)

    def _on_error(self, msg):
        self._error = msg

    def connect(self, port):
        self.link.connect(port)

    def close(self):
        self.link.disconnect()

    def send(self, obj):
        self.link.send(obj)

    def messages(self, until):
        """Yield parsed JSON objects from the Master until the `until`
        (time.monotonic()) deadline. Non-JSON lines (boot log etc.) are skipped."""
        while True:
            if self._error:
                raise serial.SerialException(self._error)
            remaining = until - time.monotonic()
            if remaining <= 0:
                return
            try:
                text = self._lines.get(timeout=min(remaining, 0.05))
            except queue.Empty:
                continue
            try:
                msg = json.loads(text)
            except json.JSONDecodeError:
                continue
            if isinstance(msg, dict):
                yield msg

    def list_nodes(self, timeout=PROBE_TIMEOUT_S):
        """Returns the known-node id list, or None if no node table came back
        (not a Master, or not this firmware at all)."""
        self.send({"cmd": "list"})
        for msg in self.messages(time.monotonic() + timeout):
            if msg.get("type") == "nodes":
                return sorted(n["id"] for n in msg.get("nodes", []) if "id" in n)
            if msg.get("error") == "not_master":
                return None
        return None


def open_master(port):
    """Connect to `port`, or probe every serial port when it's None. Returns
    (Master, port, first node list)."""
    candidates = [port] if port else [p.device for p in list_ports.comports()]
    if not candidates:
        sys.exit("No serial ports found — is the Master plugged in?")
    for dev in candidates:
        master = Master()
        try:
            master.connect(dev)
            nodes = master.list_nodes()
        except (serial.SerialException, OSError) as exc:
            print(f"-- {dev}: {exc} --", file=sys.stderr)
            master.close()
            continue
        if nodes is not None:
            return master, dev, nodes
        master.close()
        if port:
            sys.exit(f"{dev} didn't answer like a Master (no node table in reply to 'list').")
    sys.exit("No Master found on " + ", ".join(candidates) + " (use --port to pick one).")


def discover_nodes(master, nodes):
    deadline = time.monotonic() + DISCOVERY_TIMEOUT_S
    while not nodes and time.monotonic() < deadline:
        time.sleep(LIST_POLL_INTERVAL_S)
        nodes = master.list_nodes() or []
    return nodes


def play_on_nodes(master, nodes, name):
    """Returns {node_id: (ok, reason)}; a Node that never answered is (None, ...)."""
    interval = RETRY_INTERVAL_MS / 1000.0
    results = {}
    attempts = {n: 0 for n in nodes}
    next_send = {n: 0.0 for n in nodes}  # monotonic time of each Node's next try
    play_id = random.randint(1, 0xFFFFFFFF)

    while len(results) < len(nodes):
        now = time.monotonic()
        for node in nodes:
            if node in results or now < next_send[node]:
                continue
            if attempts[node] >= ATTEMPTS:
                # Last attempt's reply got its interval to arrive; give up.
                results[node] = (None, "no reply (offline, or firmware older than 2.3.0?)")
                continue
            attempts[node] += 1
            master.send({"cmd": "remote_play", "node": node, "name": name, "play_id": play_id})
            next_send[node] = now + interval

        pending = [next_send[n] for n in nodes if n not in results]
        if not pending:
            break
        for msg in master.messages(min(pending)):
            if msg.get("type") != "play_result":
                continue
            node = msg.get("node")
            if node in attempts and node not in results:
                results[node] = (bool(msg.get("ok")), msg.get("reason", ""))
    return results


def main():
    parser = argparse.ArgumentParser(
        description="Make every Node the Master knows about play (once) a sequence saved on its own flash."
    )
    parser.add_argument("name", help="name of the recording saved on the Nodes")
    parser.add_argument("--port", help="the Master's serial port (default: probe every port)")
    args = parser.parse_args()

    master, port, nodes = open_master(args.port)
    try:
        print(f"Master on {port}")
        nodes = discover_nodes(master, nodes)
        if not nodes:
            sys.exit("The Master hasn't heard from any Node (are they powered and on its channel?).")
        print(f"Nodes: {', '.join(str(n) for n in nodes)}")

        results = play_on_nodes(master, nodes, args.name)
    finally:
        master.close()

    for node in nodes:
        ok, reason = results[node]
        if ok:
            print(f"  Node {node}: playing '{args.name}'")
        elif ok is None:
            print(f"  Node {node}: {reason}")
        else:
            print(f"  Node {node}: can't play '{args.name}' — {reason or 'failed'}")

    playing = sum(1 for ok, _ in results.values() if ok)
    print(f"{playing}/{len(nodes)} Nodes playing '{args.name}'")
    return 0 if playing == len(nodes) else 1


if __name__ == "__main__":
    sys.exit(main())
