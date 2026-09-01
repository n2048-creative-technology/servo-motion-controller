#!/usr/bin/env python3
"""GUI for sending servo positioning commands to a Master board over USB serial.

Talks the line-based JSON protocol described in ../docs/serial-protocol.md:
  -> {"node": <0-250>, "x": <deg>, "y": <deg>, "relay": <bool>}
                                         aim a node's pan/tilt head (0 = all
                                         nodes), and optionally switch its
                                         relay/light. An omitted axis holds
                                         its last commanded position.
  -> {"cmd": "list"}                     ask for the Master's known-node table
  <- {"ok": true|false, "error": "..."}  ack/error for a move command
  <- {"type": "nodes", "nodes": [...]}   known-node table: id, x, y, light, age

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
PAD_SIZE = 180           # px; the XY pad is square so both axes read alike
PAD_ANGLE_MIN = 0.0      # the pad spans a servo's default 0-270 travel; the
PAD_ANGLE_MAX = 270.0    # firmware clamps to each node's real calibration
JOG_SEND_INTERVAL_S = 0.04  # ~25 Hz, matches the web UI's trackpad throttle
MAX_LOG_LINES = 500


class App:
    def __init__(self, root):
        self.root = root
        root.title("Servo Rig — Master Bridge")
        root.geometry("760x700")  # wide enough for the XY pad beside the entries
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        self.incoming = queue.Queue()
        self.link = SerialLink(on_line=self.incoming.put, on_error=self._on_link_error)
        self.nodes = {}  # node_id -> {"x": float, "y": float, "relay": bool, "age_ms": int}
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
        # Kept as an attribute so _set_connected_state can recolour it —
        # a red "connected" reads like an error at a glance.
        self.status_label = ttk.Label(conn, textvariable=self.status_var, foreground="#a33")
        self.status_label.pack(side="left", padx=(10, 0))

        nodes_frame = ttk.LabelFrame(
            self.root, text="Known nodes (ctrl/shift-click to select several as the send target)", padding=8
        )
        nodes_frame.pack(fill="both", expand=False, padx=8, pady=(0, 8))

        self.node_tree = ttk.Treeview(
            nodes_frame, columns=("x", "y", "light", "age"), show="tree headings", height=5,
            selectmode="extended",
        )
        self.node_tree.heading("#0", text="Node")
        self.node_tree.column("#0", width=70)
        self.node_tree.heading("x", text="X (pan)")
        self.node_tree.heading("y", text="Y (tilt)")
        self.node_tree.heading("light", text="Light")
        self.node_tree.heading("age", text="Last seen")
        self.node_tree.column("x", width=80, anchor="center")
        self.node_tree.column("y", width=80, anchor="center")
        self.node_tree.column("light", width=55, anchor="center")
        self.node_tree.column("age", width=100, anchor="center")
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

        ttk.Label(send, text="Fallback node ID:").grid(row=0, column=2, sticky="e", padx=(16, 0))
        self.node_var = tk.IntVar(value=0)
        ttk.Spinbox(send, from_=0, to=250, textvariable=self.node_var, width=6).grid(
            row=0, column=3, sticky="w", padx=(4, 0)
        )

        self.x_var = tk.DoubleVar(value=135.0)
        self.y_var = tk.DoubleVar(value=135.0)
        ttk.Label(send, text="X (deg):").grid(row=1, column=0, sticky="w", pady=(8, 0))
        x_entry = ttk.Entry(send, textvariable=self.x_var, width=8)
        x_entry.grid(row=1, column=1, sticky="w", pady=(8, 0))
        x_entry.bind("<Return>", lambda e: self._send_command())
        ttk.Label(send, text="Y (deg):").grid(row=2, column=0, sticky="w")
        y_entry = ttk.Entry(send, textvariable=self.y_var, width=8)
        y_entry.grid(row=2, column=1, sticky="w")
        y_entry.bind("<Return>", lambda e: self._send_command())

        # Created before the pad below, whose handlers read it.
        self.live_jog_var = tk.BooleanVar(value=False)

        # Square XY pad: click or drag to aim. Left/right is X, and *up* is
        # higher Y, matching the web UI's trackpad.
        self.pad = tk.Canvas(send, width=PAD_SIZE, height=PAD_SIZE, bg="#14161a",
                             highlightthickness=1, highlightbackground="#2b2f36", cursor="crosshair")
        self.pad.grid(row=1, column=2, rowspan=3, columnspan=3, sticky="w", padx=(16, 0), pady=(8, 0))
        self.pad.bind("<Button-1>", self._on_pad_drag)
        self.pad.bind("<B1-Motion>", self._on_pad_drag)
        self.pad.bind("<ButtonRelease-1>", self._on_pad_release)
        self._pad_dot = self.pad.create_oval(0, 0, 0, 0, fill="#4da3ff", outline="")
        self._render_pad()

        ttk.Checkbutton(
            send, text="Live jog while dragging (throttled ~25 Hz)", variable=self.live_jog_var
        ).grid(row=3, column=0, columnspan=2, sticky="w", pady=(8, 0))

        ttk.Button(send, text="Send", command=self._send_command).grid(row=4, column=1, sticky="w", pady=(8, 0))

        ttk.Separator(send, orient="horizontal").grid(row=5, column=0, columnspan=5, sticky="ew", pady=8)

        self.light_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            send, text="Light on (relay) — sent with every command below", variable=self.light_var
        ).grid(row=6, column=0, columnspan=3, sticky="w")
        ttk.Button(send, text="Apply Light Only", command=self._send_light_only).grid(
            row=6, column=4, sticky="e"
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
        self.status_label.configure(foreground="#2a2" if connected else "#a33")
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

    # ---------- XY pad ----------
    def _render_pad(self):
        """Draws the dot where the current X/Y entries point."""
        try:
            x, y = float(self.x_var.get()), float(self.y_var.get())
        except (tk.TclError, ValueError):
            return
        span = PAD_ANGLE_MAX - PAD_ANGLE_MIN
        fx = min(1.0, max(0.0, (x - PAD_ANGLE_MIN) / span))
        fy = min(1.0, max(0.0, (y - PAD_ANGLE_MIN) / span))
        cx = fx * PAD_SIZE
        cy = (1.0 - fy) * PAD_SIZE  # screen up = higher Y
        r = 7
        self.pad.coords(self._pad_dot, cx - r, cy - r, cx + r, cy + r)

    def _on_pad_drag(self, event):
        span = PAD_ANGLE_MAX - PAD_ANGLE_MIN
        fx = min(1.0, max(0.0, event.x / PAD_SIZE))
        fy = min(1.0, max(0.0, event.y / PAD_SIZE))
        self.x_var.set(round(PAD_ANGLE_MIN + fx * span, 1))
        self.y_var.set(round(PAD_ANGLE_MAX - fy * span, 1))
        self._render_pad()
        if self.live_jog_var.get() and self.link.is_open:
            now = time.monotonic()
            if now - self._jog_last_sent >= JOG_SEND_INTERVAL_S:
                self._jog_last_sent = now
                self._send_command(quiet=True)

    def _on_pad_release(self, _event):
        # The throttle can swallow the last position mid-drag; send it once
        # more so the head ends exactly where the pointer was let go.
        if self.live_jog_var.get() and self.link.is_open:
            self._jog_last_sent = 0.0
            self._send_command(quiet=True)

    def _send_command(self, quiet=False):
        if not self.link.is_open:
            if not quiet:
                messagebox.showwarning("Not connected", "Connect to the Master's serial port first.")
            return
        try:
            targets = self._resolve_targets()
            x = float(self.x_var.get())
            y = float(self.y_var.get())
        except (tk.TclError, ValueError):
            messagebox.showerror("Invalid input", "Node ID and angles must be numbers.")
            return
        # A multi-node selection is a client-side fan-out: one JSON line per
        # target, the wire protocol itself only ever addresses one id (or 0
        # for broadcast) per line.
        relay_on = bool(self.light_var.get())
        for node_id in targets:
            try:
                line = self.link.send({"node": node_id, "x": x, "y": y, "relay": relay_on})
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
            fallback_x = float(self.x_var.get())
            fallback_y = float(self.y_var.get())
        except (tk.TclError, ValueError):
            messagebox.showerror("Invalid input", "Node ID and angles must be numbers.")
            return
        relay_on = bool(self.light_var.get())
        for node_id in targets:
            known = self.nodes.get(node_id, {})
            # Broadcast (0) is never in the heartbeat table, and a node we
            # haven't heard from yet isn't either — fall back to the entries.
            x = known.get("x", fallback_x)
            y = known.get("y", fallback_y)
            try:
                line = self.link.send({"node": node_id, "x": round(x, 1), "y": round(y, 1),
                                       "relay": relay_on})
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
                    f"{n.get('x', 0):.1f}°",
                    f"{n.get('y', 0):.1f}°",
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
