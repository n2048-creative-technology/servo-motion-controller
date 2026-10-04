"""The "Play saved sequence on Node(s)" panel, shared by master_gui.py and
joystick_master_gui.py.

Speaks the remote_play / remote_stop commands from ../docs/serial-protocol.md:
  -> {"cmd": "remote_play", "node": <0-250>, "name": "<sequence>"}
  -> {"cmd": "remote_stop", "node": <0-250>}
  <- {"type": "play_result", "node": N, "name": "...", "ok": bool, "reason": "..."}

Each Node loops a sequence saved on its *own* flash (recorded there, or
uploaded with joystick_master_gui.py's "Upload to Node…"). ESP-NOW broadcast
has no link-level ack, so a play is repeated until that Node's play_result
arrives (or we give up), and a stop is simply sent a few times. Both are safe
to repeat: a Node ignores a play of the sequence it's already looping.
"""

import re
import tkinter as tk
from tkinter import ttk

import serial

RETRY_INTERVAL_MS = 400
ATTEMPTS = 4


def parse_node_list(text):
    """Parses e.g. "3, 5 7" -> [3, 5, 7]; blank or "0" -> [0] (all Nodes). Raises
    ValueError on anything that isn't a node id in 0-250."""
    parts = [p for p in re.split(r"[,\s]+", text.strip()) if p]
    if not parts:
        return [0]
    ids = sorted({int(p) for p in parts})
    if any(i < 0 or i > 250 for i in ids):
        raise ValueError("node ids must be 0-250")
    return [0] if 0 in ids else ids


class RemotePlayPanel(ttk.LabelFrame):
    """Sequence name + Play / Stop for one or more Nodes.

    resolve_targets: callable returning the node id list to act on ([0] = all
        Nodes). If None, the panel shows its own "Nodes" entry instead.
    before_play: optional callable run before a play/stop goes out — e.g. to
        stop the PC tool's own CSV playback, which would otherwise pull the
        Nodes straight back under live control on its next row.
    """

    def __init__(self, parent, root, link, log, resolve_targets=None, before_play=None):
        super().__init__(parent, text="Play saved sequence on Node(s)", padding=8)
        self.root = root
        self.link = link
        self.log = log
        self.resolve_targets = resolve_targets
        self.before_play = before_play
        self._retries = {}  # node_id -> after() job id, while waiting for its play_result
        self._jobs = []     # broadcast play repeats and stop repeats, cancellable together
        self._names = set()

        col = 0
        if resolve_targets is None:
            ttk.Label(self, text="Nodes:").grid(row=0, column=col, sticky="w")
            self.nodes_var = tk.StringVar(value="0")
            ttk.Entry(self, textvariable=self.nodes_var, width=10).grid(row=0, column=col + 1, sticky="w", padx=(4, 12))
            col += 2

        ttk.Label(self, text="Sequence:").grid(row=0, column=col, sticky="w")
        self.name_var = tk.StringVar()
        self.name_combo = ttk.Combobox(self, textvariable=self.name_var, width=22)
        self.name_combo.grid(row=0, column=col + 1, sticky="w", padx=(4, 8))
        self.name_combo.bind("<Return>", lambda e: self.play())
        ttk.Button(self, text="Play on Node(s)", command=self.play).grid(row=0, column=col + 2, sticky="w")
        ttk.Button(self, text="Stop", command=self.stop).grid(row=0, column=col + 3, sticky="w", padx=(4, 0))

        hint = ("Nodes: ids separated by commas, 0 = all. " if resolve_targets is None
                else "Targets the node selection above. ")
        self.status_var = tk.StringVar(
            value=hint + "Any later move command to a Node takes it back under live control."
        )
        ttk.Label(self, textvariable=self.status_var, foreground="#666").grid(
            row=1, column=0, columnspan=col + 4, sticky="w", pady=(6, 0)
        )

    # ---------- names ----------
    def add_names(self, names):
        """Offer these in the dropdown (e.g. names just uploaded to a Node)."""
        new = {n for n in names if n} - self._names
        if new:
            self._names |= new
            self.name_combo["values"] = sorted(self._names)

    # ---------- actions ----------
    def _targets(self):
        if self.resolve_targets is not None:
            return self.resolve_targets()
        return parse_node_list(self.nodes_var.get())

    def _cancel_pending(self):
        for job in self._retries.values():
            self.root.after_cancel(job)
        self._retries.clear()
        for job in self._jobs:
            self.root.after_cancel(job)
        self._jobs.clear()

    def _send(self, obj):
        try:
            line = self.link.send(obj)
        except (RuntimeError, serial.SerialException, OSError) as exc:
            self.log(f"-- send failed: {exc} --")
            self._cancel_pending()
            return False
        self.log(f"-> {line.rstrip()}")
        return True

    def _describe(self, nodes):
        return "all Nodes" if nodes == [0] else ", ".join(f"Node {n}" for n in nodes)

    def play(self):
        if not self.link.is_open:
            self.status_var.set("not connected to the Master")
            return
        name = self.name_var.get().strip()
        if not name:
            self.status_var.set("enter the name of a sequence saved on the Node(s)")
            return
        try:
            nodes = self._targets()
        except (ValueError, tk.TclError) as exc:
            self.status_var.set(f"bad node list: {exc}")
            return
        if self.before_play:
            self.before_play()
        self._cancel_pending()
        self.add_names([name])
        self.status_var.set(f"asking {self._describe(nodes)} to play '{name}'…")
        for node in nodes:
            self._play_attempt(node, name, 1)

    def _play_attempt(self, node, name, attempt):
        self._retries.pop(node, None)
        if not self._send({"cmd": "remote_play", "node": node, "name": name}):
            return
        if node == 0:
            # "All" can't know when every Node has answered, so it just
            # repeats a fixed number of times; acks are still shown as they come.
            if attempt < ATTEMPTS:
                self._jobs.append(self.root.after(RETRY_INTERVAL_MS, self._play_attempt, 0, name, attempt + 1))
            return
        if attempt < ATTEMPTS:
            self._retries[node] = self.root.after(RETRY_INTERVAL_MS, self._play_attempt, node, name, attempt + 1)
        else:
            # Last attempt sent; give its reply one more interval to arrive.
            self._retries[node] = self.root.after(RETRY_INTERVAL_MS, self._give_up, node, name)

    def _give_up(self, node, name):
        self._retries.pop(node, None)
        msg = f"Node {node}: no reply to play '{name}' (offline, or firmware older than 2.3.0?)"
        self.status_var.set(msg)
        self.log(f"-- {msg} --")

    def stop(self):
        if not self.link.is_open:
            self.status_var.set("not connected to the Master")
            return
        try:
            nodes = self._targets()
        except (ValueError, tk.TclError) as exc:
            self.status_var.set(f"bad node list: {exc}")
            return
        if self.before_play:
            self.before_play()
        # Also drops any play still being retried — a late resend would
        # otherwise start the Node again right after it stopped.
        self._cancel_pending()
        # remote_stop has no ack, so it's just sent a few times.
        for i in range(ATTEMPTS):
            self._jobs.append(self.root.after(i * RETRY_INTERVAL_MS, self._stop_once, nodes))
        self.status_var.set(f"stopping {self._describe(nodes)}")

    def _stop_once(self, nodes):
        for node in nodes:
            if not self._send({"cmd": "remote_stop", "node": node}):
                return

    # ---------- incoming ----------
    def handle_message(self, msg):
        """Feed every parsed JSON line from the Master. Returns True if it
        was a play_result (already handled here)."""
        if msg.get("type") == "upload_result" and msg.get("ok"):
            self.add_names([msg.get("name")])
            return False
        if msg.get("type") != "play_result":
            return False
        node = msg.get("node")
        job = self._retries.pop(node, None)
        if job is not None:
            self.root.after_cancel(job)
        name = msg.get("name", "")
        if msg.get("ok"):
            self.add_names([name])
            self.status_var.set(f"Node {node}: playing '{name}'")
        else:
            self.status_var.set(f"Node {node}: can't play '{name}' — {msg.get('reason', 'failed')}")
        return True

    def cancel(self):
        """Call on disconnect/close so no retry fires into a closed port."""
        self._cancel_pending()
