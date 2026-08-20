#!/usr/bin/env python3
"""GUI for sending servo positioning commands to a Master board over USB serial.

Talks the line-based JSON protocol described in ../docs/serial-protocol.md:
  -> {"node": <0-250>, "angle": <deg>, "relay": <bool>}
                                         move a node (0 = all nodes), and
                                         optionally switch its relay/light
  -> {"cmd": "list"}                     ask for the Master's known-node table
  <- {"ok": true|false, "error": "..."}  ack/error for a move command
  <- {"type": "nodes", "nodes": [...]}   known-node table, incl. each node's light

The Master tracks relay state per target, so switching one node's light
never disturbs another's.

Requires: pyserial (`pip install pyserial`). Tkinter ships with most desktop
Python installs; on Debian/Ubuntu install `python3-tk` if it's missing.
"""

import json
import queue
import time
import tkinter as tk
from tkinter import ttk, messagebox

import serial
from serial.tools import list_ports

from serial_link import SerialLink, BAUD_RATE

NODE_POLL_INTERVAL_MS = 2000
JOG_SEND_INTERVAL_S = 0.04  # ~25 Hz, matches the web UI's jog slider throttle
MAX_LOG_LINES = 500


class App:
    def __init__(self, root):
        self.root = root
        root.title("Servo Rig — Master Bridge")
        root.geometry("640x560")
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.incoming = queue.Queue()
        self.link = SerialLink(on_line=self.incoming.put, on_error=self._on_link_error)
        self.nodes = {}  # node_id -> {"angle": float, "age_ms": int}
        self._jog_last_sent = 0.0
        self._poll_job = None

        self._build_widgets()
        self._refresh_ports()
        self._set_connected_state(False)
        self.root.after(50, self._drain_incoming)

    # ---------- UI construction ----------
    def _build_widgets(self):
        conn = ttk.Frame(self.root, padding=8)
        conn.pack(fill="x")

        ttk.Label(conn, text="Port:").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(conn, textvariable=self.port_var, width=28, state="readonly")
        self.port_combo.pack(side="left", padx=(4, 4))
        ttk.Button(conn, text="Refresh", command=self._refresh_ports).pack(side="left")

        self.connect_btn = ttk.Button(conn, text="Connect", command=self._toggle_connect)
        self.connect_btn.pack(side="left", padx=(8, 0))

        self.status_var = tk.StringVar(value="disconnected")
        ttk.Label(conn, textvariable=self.status_var, foreground="#a33").pack(side="left", padx=(10, 0))

        nodes_frame = ttk.LabelFrame(
            self.root, text="Known nodes (ctrl/shift-click to select several as the send target)", padding=8
        )
        nodes_frame.pack(fill="both", expand=False, padx=8, pady=(0, 8))

        self.node_tree = ttk.Treeview(
            nodes_frame, columns=("angle", "light", "age"), show="tree headings", height=5,
            selectmode="extended",
        )
        self.node_tree.heading("#0", text="Node ID")
        self.node_tree.column("#0", width=90)
        self.node_tree.heading("angle", text="Angle")
        self.node_tree.heading("light", text="Light")
        self.node_tree.heading("age", text="Last seen")
        self.node_tree.column("angle", width=90, anchor="center")
        self.node_tree.column("light", width=60, anchor="center")
        self.node_tree.column("age", width=110, anchor="center")
        self.node_tree.pack(side="left", fill="x", expand=True)

        btns = ttk.Frame(nodes_frame)
        btns.pack(side="left", padx=(8, 0))
        ttk.Button(btns, text="Refresh now", command=self._request_node_list).pack(fill="x")
        self.autopoll_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(btns, text="Auto-refresh (2s)", variable=self.autopoll_var).pack(fill="x", pady=(4, 0))

        send = ttk.LabelFrame(self.root, text="Send position command", padding=8)
        send.pack(fill="x", padx=8, pady=(0, 8))

        self.all_nodes_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(
            send, text="All nodes (broadcast) — overrides the selection above", variable=self.all_nodes_var
        ).grid(row=0, column=0, columnspan=2, sticky="w")

        ttk.Label(send, text="Fallback node ID (used if nothing is selected above):").grid(
            row=0, column=2, columnspan=2, sticky="w"
        )
        self.node_var = tk.IntVar(value=0)
        ttk.Spinbox(send, from_=0, to=250, textvariable=self.node_var, width=6).grid(
            row=0, column=4, sticky="w", padx=(4, 0)
        )

        ttk.Label(send, text="Angle (deg):").grid(row=1, column=0, sticky="w", pady=(8, 0))
        self.angle_var = tk.DoubleVar(value=135.0)
        self.angle_entry = ttk.Entry(send, textvariable=self.angle_var, width=8)
        self.angle_entry.grid(row=1, column=1, sticky="w", pady=(8, 0))
        self.angle_entry.bind("<Return>", lambda e: self._send_command())

        # Created before the Scale below: setting its initial value fires
        # `command` immediately, and _on_scale_move reads live_jog_var.
        self.live_jog_var = tk.BooleanVar(value=False)

        self.angle_scale = ttk.Scale(
            send, from_=0, to=270, orient="horizontal", command=self._on_scale_move
        )
        self.angle_scale.set(135.0)
        self.angle_scale.grid(row=1, column=2, columnspan=3, sticky="ew", pady=(8, 0))
        send.columnconfigure(4, weight=1)

        ttk.Checkbutton(
            send, text="Live jog while dragging (throttled ~25 Hz)", variable=self.live_jog_var
        ).grid(row=2, column=0, columnspan=3, sticky="w", pady=(8, 0))

        ttk.Button(send, text="Send", command=self._send_command).grid(row=2, column=4, sticky="e", pady=(8, 0))

        ttk.Separator(send, orient="horizontal").grid(row=3, column=0, columnspan=5, sticky="ew", pady=8)

        self.light_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            send, text="Light on (relay) — sent with every command below", variable=self.light_var
        ).grid(row=4, column=0, columnspan=3, sticky="w")
        ttk.Button(send, text="Apply Light Only", command=self._send_light_only).grid(
            row=4, column=4, sticky="e"
        )

        log_frame = ttk.LabelFrame(self.root, text="Serial log", padding=8)
        log_frame.pack(fill="both", expand=True, padx=8, pady=(0, 8))

        self.log_text = tk.Text(log_frame, height=10, state="disabled", wrap="none")
        self.log_text.pack(side="left", fill="both", expand=True)
        scroll = ttk.Scrollbar(log_frame, command=self.log_text.yview)
        scroll.pack(side="right", fill="y")
        self.log_text.configure(yscrollcommand=scroll.set)

    # ---------- port / connection handling ----------
    def _refresh_ports(self):
        ports = [p.device for p in list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _toggle_connect(self):
        if self.link.is_open:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        port = self.port_var.get()
        if not port:
            messagebox.showwarning("No port selected", "Choose a serial port first.")
            return
        try:
            self.link.connect(port)
        except (serial.SerialException, OSError) as exc:
            messagebox.showerror("Connection failed", str(exc))
            return
        self._set_connected_state(True)
        self._log(f"-- connected to {port} @ {BAUD_RATE} baud --")
        self._request_node_list()
        self._schedule_node_poll()

    def _disconnect(self):
        self.link.disconnect()
        self._set_connected_state(False)
        self._log("-- disconnected --")
        if self._poll_job is not None:
            self.root.after_cancel(self._poll_job)
            self._poll_job = None

    def _on_link_error(self, message):
        self.incoming.put(f"__ERROR__{message}")

    def _set_connected_state(self, connected):
        self.connect_btn.configure(text="Disconnect" if connected else "Connect")
        self.status_var.set("connected" if connected else "disconnected")
        self.status_var_color = "#2a2" if connected else "#a33"
        self.port_combo.configure(state="disabled" if connected else "readonly")

    # ---------- sending ----------
    def _resolve_targets(self):
        """Which node id(s) a Send/jog action should reach right now.

        All-nodes checkbox wins if checked; otherwise the Treeview's
        (possibly multi-) selection; otherwise the fallback spinbox, for
        targeting a node that hasn't sent a heartbeat yet.
        """
        if self.all_nodes_var.get():
            return [0]
        selected = self.node_tree.selection()
        if selected:
            return [int(iid) for iid in selected]
        return [int(self.node_var.get())]

    def _on_scale_move(self, value_str):
        angle = round(float(value_str), 1)
        self.angle_var.set(angle)
        if self.live_jog_var.get() and self.link.is_open:
            now = time.monotonic()
            if now - self._jog_last_sent >= JOG_SEND_INTERVAL_S:
                self._jog_last_sent = now
                self._send_command(quiet=True)

    def _send_command(self, quiet=False):
        if not self.link.is_open:
            if not quiet:
                messagebox.showwarning("Not connected", "Connect to the Master's serial port first.")
            return
        try:
            targets = self._resolve_targets()
            angle = float(self.angle_var.get())
        except (tk.TclError, ValueError):
            messagebox.showerror("Invalid input", "Node ID and angle must be numbers.")
            return
        # A multi-node selection is a client-side fan-out: one JSON line per
        # target, the wire protocol itself only ever addresses one id (or 0
        # for broadcast) per line.
        relay_on = bool(self.light_var.get())
        for node_id in targets:
            try:
                line = self.link.send({"node": node_id, "angle": angle, "relay": relay_on})
            except (RuntimeError, serial.SerialException, OSError) as exc:
                messagebox.showerror("Send failed", str(exc))
                return
            self._log(f"-> {line.rstrip()}")

    def _send_light_only(self):
        """Switch the target(s)' light without moving anything.

        The firmware carries relay state on ordinary move commands, so a
        light-only change is really "re-send where this node already is, with
        the new light state" — hence each target's own last-reported angle
        from the heartbeat table, rather than the angle box (which would yank
        every selected node to one shared position)."""
        if not self.link.is_open:
            messagebox.showwarning("Not connected", "Connect to the Master's serial port first.")
            return
        try:
            targets = self._resolve_targets()
            fallback_angle = float(self.angle_var.get())
        except (tk.TclError, ValueError):
            messagebox.showerror("Invalid input", "Node ID and angle must be numbers.")
            return
        relay_on = bool(self.light_var.get())
        for node_id in targets:
            known = self.nodes.get(node_id, {})
            # Broadcast (0) is never in the heartbeat table, and a node we
            # haven't heard from yet isn't either — fall back to the angle box.
            angle = known.get("angle", fallback_angle)
            try:
                line = self.link.send({"node": node_id, "angle": round(angle, 1), "relay": relay_on})
            except (RuntimeError, serial.SerialException, OSError) as exc:
                messagebox.showerror("Send failed", str(exc))
                return
            self._log(f"-> {line.rstrip()}")

    def _request_node_list(self):
        if not self.link.is_open:
            return
        try:
            line = self.link.send({"cmd": "list"})
            self._log(f"-> {line.rstrip()}")
        except (RuntimeError, serial.SerialException, OSError) as exc:
            self._log(f"-- send failed: {exc} --")

    def _schedule_node_poll(self):
        if self.autopoll_var.get() and self.link.is_open:
            self._request_node_list()
        self._poll_job = self.root.after(NODE_POLL_INTERVAL_MS, self._schedule_node_poll)

    # ---------- incoming data ----------
    def _drain_incoming(self):
        try:
            while True:
                text = self.incoming.get_nowait()
                if text.startswith("__ERROR__"):
                    self._handle_link_error(text[len("__ERROR__"):])
                else:
                    self._handle_incoming_line(text)
        except queue.Empty:
            pass
        self.root.after(50, self._drain_incoming)

    def _handle_link_error(self, message):
        self._log(f"-- serial error: {message} --")
        self._set_connected_state(False)

    def _handle_incoming_line(self, text):
        self._log(f"<- {text}")
        try:
            msg = json.loads(text)
        except json.JSONDecodeError:
            return
        if isinstance(msg, dict) and msg.get("type") == "nodes":
            self.nodes = {n["id"]: n for n in msg.get("nodes", []) if "id" in n}
            self._refresh_node_table()

    def _refresh_node_table(self):
        previously_selected = set(self.node_tree.selection())
        self.node_tree.delete(*self.node_tree.get_children())
        for node_id in sorted(self.nodes):
            n = self.nodes[node_id]
            age_s = n.get("age_ms", 0) / 1000.0
            self.node_tree.insert(
                "", "end", iid=str(node_id), text=str(node_id),
                values=(
                    f"{n.get('angle', 0):.1f}°",
                    "on" if n.get("relay") else "off",
                    f"{age_s:.1f}s ago",
                ),
            )
        # Re-apply the selection across the rebuild (Treeview.delete forgets it).
        still_present = [iid for iid in previously_selected if self.node_tree.exists(iid)]
        if still_present:
            self.node_tree.selection_set(still_present)

    def _log(self, line):
        self.log_text.configure(state="normal")
        self.log_text.insert("end", line + "\n")
        num_lines = int(self.log_text.index("end-1c").split(".")[0])
        if num_lines > MAX_LOG_LINES:
            self.log_text.delete("1.0", f"{num_lines - MAX_LOG_LINES}.0")
        self.log_text.see("end")
        self.log_text.configure(state="disabled")

    # ---------- shutdown ----------
    def on_close(self):
        if self._poll_job is not None:
            self.root.after_cancel(self._poll_job)
        self.link.disconnect()
        self.root.destroy()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
