#!/usr/bin/env python3
"""Joystick/gamepad -> Master board bridge.

Reads one or more connected joysticks/gamepads (via pygame) and streams
positions to servo Nodes through a Master's USB serial port, using the same
line-based JSON protocol as master_gui.py (see ../docs/serial-protocol.md).
Lets you:

  - "Learn" which physical axis on which controller you just moved and link
    it to a Node, calibrating that axis's raw range to the Node's angle
    range. With several controllers connected, each learned mapping
    remembers its own controller (by a stable per-device GUID, not by
    plug-in order), so e.g. (controller 1, axis 2) -> Node 3 and
    (controller 2, axis 4) -> Node 1 coexist freely.
  - Stream live axis movement from every mapped controller to its linked
    Node(s) at once, once mapped.
  - Record the computed angle for every mapped Node to a CSV file (which
    controller/axis produced it is irrelevant to playback — only Node ID +
    angle are recorded), and replay that CSV later to reproduce the same
    motion without any physical controller connected at all.

Requires: pyserial, pygame (`pip install -r requirements.txt`). Tkinter
ships with most desktop Python installs; on Debian/Ubuntu install
`python3-tk` if it's missing.
"""

import bisect
import csv
import json
import queue
import time
import tkinter as tk
from tkinter import ttk, messagebox, filedialog

import pygame
import serial
from serial.tools import list_ports

from serial_link import SerialLink, BAUD_RATE

TICK_INTERVAL_MS = 40  # ~25 Hz, matches the web UI's jog slider throttle
NODE_POLL_INTERVAL_MS = 2000
LEARN_DURATION_MS = 4000
LEARN_POLL_INTERVAL_MS = 30
MIN_LEARN_RANGE = 0.15  # raw axis units; below this, "no clear movement" wins
ANGLE_SEND_EPSILON = 0.2  # degrees; skip a resend below this delta
DEFAULT_ANGLE_MIN = 0.0
DEFAULT_ANGLE_MAX = 270.0
MAX_LOG_LINES = 500
PLAYBACK_SPEED_MIN = 0.1
PLAYBACK_SPEED_MAX = 2.0


class AxisMapping:
    """One learned (controller, axis) -> Node link, with its raw and output
    angle ranges. `controller_guid` (SDL/pygame's Joystick.get_guid(), stable
    across reconnects and USB port/order changes) is what actually resolves
    to a live pygame.joystick.Joystick at runtime; `controller_name` is a
    display-only label carried alongside it for the UI and for reattaching a
    loaded mapping to a currently-connected controller by name when its guid
    isn't connected (see App._try_autobind)."""

    def __init__(
        self, controller_guid, controller_name, axis_index, raw_min, raw_max, node_id, angle_min, angle_max,
        invert=False,
    ):
        self.controller_guid = controller_guid
        self.controller_name = controller_name
        self.axis_index = axis_index
        self.raw_min = raw_min
        self.raw_max = raw_max
        self.node_id = node_id
        self.angle_min = angle_min
        self.angle_max = angle_max
        self.invert = invert
        self.last_sent_angle = None

    def compute_angle(self, raw_value):
        span = self.raw_max - self.raw_min
        t = 0.0 if span == 0 else (raw_value - self.raw_min) / span
        t = max(0.0, min(1.0, t))
        if self.invert:
            t = 1.0 - t
        return self.angle_min + t * (self.angle_max - self.angle_min)

    def label(self):
        controller = self.controller_name or "unknown controller"
        return f"{controller} axis {self.axis_index} -> Node {self.node_id} [{self.angle_min:.0f}-{self.angle_max:.0f}°]"

    def to_dict(self):
        return {
            "controller_guid": self.controller_guid,
            "controller_name": self.controller_name,
            "axis_index": self.axis_index,
            "raw_min": self.raw_min,
            "raw_max": self.raw_max,
            "node_id": self.node_id,
            "angle_min": self.angle_min,
            "angle_max": self.angle_max,
            "invert": self.invert,
        }

    @classmethod
    def from_dict(cls, d):
        return cls(
            controller_guid=d.get("controller_guid"),
            controller_name=d.get("controller_name"),
            axis_index=int(d["axis_index"]),
            raw_min=float(d["raw_min"]),
            raw_max=float(d["raw_max"]),
            node_id=int(d["node_id"]),
            angle_min=float(d["angle_min"]),
            angle_max=float(d["angle_max"]),
            invert=bool(d.get("invert", False)),
        )


def save_mapping_config(path, mappings):
    """Save learned mappings (each carrying its own controller identity) so
    they survive a restart without re-learning."""
    data = {"mappings": [m.to_dict() for m in mappings]}
    with open(path, "w") as f:
        json.dump(data, f, indent=2)


def load_mapping_config(path):
    """Returns [AxisMapping, ...]. Mappings saved by the older single-controller
    format (a file-level "controller_name", no per-mapping controller_guid)
    load with controller_guid=None and controller_name backfilled from that
    file-level field — App._try_autobind then reattaches them by name to
    whichever currently-connected controller matches, if any."""
    with open(path) as f:
        data = json.load(f)
    legacy_name = data.get("controller_name")
    mappings = [AxisMapping.from_dict(d) for d in data.get("mappings", [])]
    if legacy_name:
        for m in mappings:
            if m.controller_guid is None and m.controller_name is None:
                m.controller_name = legacy_name
    return mappings


def load_csv_rows(path):
    """Parse a recorded CSV back into [(t_ms, {node_id: angle}), ...]."""
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        node_ids = [int(col.split("_", 1)[1]) for col in header[1:]]
        rows = []
        for row in reader:
            t_ms = int(float(row[0]))
            sample = {nid: float(val) for nid, val in zip(node_ids, row[1:]) if val != ""}
            rows.append((t_ms, sample))
    return rows


def save_csv_rows(path, rows):
    """Write [(t_ms, {node_id: angle}), ...] out in the same shape load_csv_rows reads."""
    node_ids = sorted({nid for _, sample in rows for nid in sample})
    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["t_ms"] + [f"node_{nid}" for nid in node_ids])
        for t_ms, sample in rows:
            writer.writerow([t_ms] + [f"{sample[nid]:.1f}" if nid in sample else "" for nid in node_ids])


def _interp_channel(times, values, src_t):
    """Linear-interpolate one node's angle at src_t from its (sorted) recorded
    (time, value) points; holds the nearest edge value outside the recorded
    range instead of extrapolating."""
    if not times:
        return None
    if src_t <= times[0]:
        return values[0]
    if src_t >= times[-1]:
        return values[-1]
    i = bisect.bisect_left(times, src_t)
    t0, t1 = times[i - 1], times[i]
    if t1 == t0:
        return values[i]
    frac = (src_t - t0) / (t1 - t0)
    return values[i - 1] + frac * (values[i] - values[i - 1])


def resample_rows(rows, output_interval_ms, speed):
    """Resample recorded (t_ms, {node_id: angle}) rows onto a fixed-cadence
    output grid, so the actual send rate during playback stays close to
    `output_interval_ms` regardless of speed instead of just stretching or
    compressing the gaps between the original samples:

      - speed < 1 (slow motion): the source timeline is stretched, so more
        output points are generated than were recorded, each one linearly
        interpolated between whichever original samples straddle it —
        smoother motion instead of big gaps between infrequent updates.
      - speed > 1 (fast motion): fewer output points than were recorded,
        each interpolated from the compressed source timeline — caps the
        send rate instead of bursting every original sample as fast as
        possible.

    Always ends on the exact final recorded position, at every speed.
    """
    if not rows:
        return []

    node_ids = sorted({nid for _, sample in rows for nid in sample})
    timelines = {}
    for nid in node_ids:
        pts = [(t, s[nid]) for t, s in rows if nid in s]
        timelines[nid] = ([p[0] for p in pts], [p[1] for p in pts])

    duration_ms = rows[-1][0]
    output_duration_ms = max(0, int(duration_ms / speed))

    def sample_at(src_t):
        out = {}
        for nid in node_ids:
            times, values = timelines[nid]
            v = _interp_channel(times, values, src_t)
            if v is not None:
                out[nid] = v
        return out

    out_rows = []
    out_t = 0
    while out_t < output_duration_ms:
        out_rows.append((out_t, sample_at(out_t * speed)))
        out_t += output_interval_ms
    out_rows.append((output_duration_ms, sample_at(duration_ms)))  # exact final position, always
    return out_rows


class LearnDialog(tk.Toplevel):
    """Phase 1: watch every axis of every currently-connected controller for
    a few seconds, pick whichever (controller, axis) pair moved most — so
    with several controllers plugged in, you just move the stick you want to
    link and it's identified automatically, no need to pick a controller
    first. Phase 2: assign that winner to a Node + angle range, reusing the
    same window."""

    def __init__(self, parent, controllers, known_nodes_provider, on_saved):
        super().__init__(parent)
        self.title("Learn axis")
        self.resizable(False, False)
        self.controllers = controllers  # [(guid, name, Joystick), ...] snapshot
        self.known_nodes_provider = known_nodes_provider
        self.on_saved = on_saved
        self.mins = {}  # (guid, axis_index) -> min seen
        self.maxs = {}  # (guid, axis_index) -> max seen
        for guid, _name, js in self.controllers:
            for i in range(js.get_numaxes()):
                self.mins[(guid, i)] = float("inf")
                self.maxs[(guid, i)] = float("-inf")
        self.elapsed_ms = 0

        watching = ", ".join(name for _g, name, _j in self.controllers) or "no controllers connected"
        self.status_var = tk.StringVar(value=f"Move the axis you want to link now… (watching: {watching})")
        ttk.Label(self, textvariable=self.status_var, padding=12, wraplength=320).pack()
        self.progress = ttk.Progressbar(self, maximum=LEARN_DURATION_MS, length=280)
        self.progress.pack(padx=12, pady=(0, 12))
        self.cancel_btn = ttk.Button(self, text="Cancel", command=self.destroy)
        self.cancel_btn.pack(pady=(0, 12))

        self.protocol("WM_DELETE_WINDOW", self.destroy)
        self.after(LEARN_POLL_INTERVAL_MS, self._poll)

    def _poll(self):
        if not self.winfo_exists():
            return
        pygame.event.pump()
        for guid, _name, js in self.controllers:
            try:
                num_axes = js.get_numaxes()
                for i in range(num_axes):
                    v = js.get_axis(i)
                    key = (guid, i)
                    if v < self.mins[key]:
                        self.mins[key] = v
                    if v > self.maxs[key]:
                        self.maxs[key] = v
            except pygame.error:
                continue  # that controller was unplugged mid-learn; skip it this tick
        self.elapsed_ms += LEARN_POLL_INTERVAL_MS
        self.progress["value"] = min(self.elapsed_ms, LEARN_DURATION_MS)
        if self.elapsed_ms < LEARN_DURATION_MS:
            self.after(LEARN_POLL_INTERVAL_MS, self._poll)
        else:
            self._finish_learn()

    def _finish_learn(self):
        spans = [(self.maxs[k] - self.mins[k], k) for k in self.mins]
        spans.sort(reverse=True)
        best_span, best_key = spans[0] if spans else (0.0, None)
        if best_key is None or best_span < MIN_LEARN_RANGE:
            messagebox.showwarning(
                "No clear movement",
                "Didn't detect a clear axis movement. Try again and move one axis (on any connected "
                "controller) through its full range.",
                parent=self,
            )
            self.destroy()
            return
        guid, axis_index = best_key
        controller_name = next((name for g, name, _js in self.controllers if g == guid), "unknown controller")
        self._show_assign_form(guid, controller_name, axis_index, self.mins[best_key], self.maxs[best_key])

    def _show_assign_form(self, controller_guid, controller_name, axis_index, raw_min, raw_max):
        for child in self.winfo_children():
            child.destroy()

        ttk.Label(
            self,
            text=f"Detected {controller_name} axis {axis_index} (raw range {raw_min:.2f} to {raw_max:.2f})",
            padding=(12, 12, 12, 4),
            wraplength=320,
        ).grid(row=0, column=0, columnspan=2, sticky="w")

        form = ttk.Frame(self, padding=12)
        form.grid(row=1, column=0, columnspan=2, sticky="ew")

        ttk.Label(form, text="Node ID:").grid(row=0, column=0, sticky="w")
        node_var = tk.StringVar()
        node_combo = ttk.Combobox(form, textvariable=node_var, width=26)
        node_combo.grid(row=0, column=1, sticky="w", padx=(4, 0))
        node_display_to_id = {}

        def refresh_known_nodes():
            nonlocal node_display_to_id
            known = self.known_nodes_provider()
            node_display_to_id = {}
            values = []
            for nid in sorted(known):
                n = known[nid]
                age_s = n.get("age_ms", 0) / 1000.0
                display = f"{nid} ({n.get('angle', 0):.1f}°, {age_s:.1f}s ago)"
                node_display_to_id[display] = nid
                values.append(display)
            node_combo["values"] = values
            if values and not node_var.get():
                node_var.set(values[0])
            known_hint_var.set(f"{len(values)} known node(s)" if values else "no nodes heard from yet — type an ID")

        known_hint_var = tk.StringVar(value="")
        ttk.Button(form, text="Refresh", command=refresh_known_nodes, width=8).grid(row=0, column=2, sticky="w", padx=(4, 0))
        ttk.Label(form, textvariable=known_hint_var, foreground="#666").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(2, 6)
        )
        refresh_known_nodes()

        ttk.Label(form, text="Angle min (°):").grid(row=2, column=0, sticky="w", pady=(6, 0))
        angle_min_var = tk.DoubleVar(value=DEFAULT_ANGLE_MIN)
        ttk.Entry(form, textvariable=angle_min_var, width=10).grid(row=2, column=1, sticky="w", padx=(4, 0), pady=(6, 0))

        ttk.Label(form, text="Angle max (°):").grid(row=3, column=0, sticky="w", pady=(6, 0))
        angle_max_var = tk.DoubleVar(value=DEFAULT_ANGLE_MAX)
        ttk.Entry(form, textvariable=angle_max_var, width=10).grid(row=3, column=1, sticky="w", padx=(4, 0), pady=(6, 0))

        invert_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(form, text="Invert direction", variable=invert_var).grid(
            row=4, column=0, columnspan=2, sticky="w", pady=(6, 0)
        )

        def save():
            raw_node = node_var.get().strip()
            node_id = node_display_to_id.get(raw_node)
            if node_id is None:
                try:
                    node_id = int(raw_node.split()[0])
                except (ValueError, IndexError):
                    messagebox.showerror(
                        "Invalid input", "Pick a known node or type a Node ID (0-250).", parent=self
                    )
                    return
            try:
                angle_min = float(angle_min_var.get())
                angle_max = float(angle_max_var.get())
            except (tk.TclError, ValueError):
                messagebox.showerror("Invalid input", "Angle range must be numbers.", parent=self)
                return
            if not (0 <= node_id <= 250):
                messagebox.showerror("Invalid input", "Node ID must be 0-250.", parent=self)
                return
            mapping = AxisMapping(
                controller_guid=controller_guid,
                controller_name=controller_name,
                axis_index=axis_index,
                raw_min=raw_min,
                raw_max=raw_max,
                node_id=node_id,
                angle_min=angle_min,
                angle_max=angle_max,
                invert=invert_var.get(),
            )
            self.on_saved(mapping)
            self.destroy()

        btns = ttk.Frame(self, padding=(12, 0, 12, 12))
        btns.grid(row=2, column=0, columnspan=2, sticky="e")
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="left", padx=(0, 8))
        ttk.Button(btns, text="Save Mapping", command=save).pack(side="left")


class App:
    def __init__(self, root):
        self.root = root
        root.title("Servo Rig — Joystick Bridge")
        root.geometry("720x640")
        root.protocol("WM_DELETE_WINDOW", self.on_close)

        pygame.init()
        pygame.joystick.init()

        self.incoming = queue.Queue()
        self.link = SerialLink(on_line=self.incoming.put, on_error=self._on_link_error)
        self.joysticks = {}  # guid -> open pygame.joystick.Joystick, ALL connected controllers at once
        self.joystick_names = {}  # guid -> name, kept even after a controller disconnects (for display)
        self.mappings = []
        self.known_nodes = {}  # node_id -> {"angle":..., "age_ms":...}, from the Master's heartbeat table
        self._node_poll_job = None
        self._tick_job = None

        self.recording = False
        self.record_start_ms = None
        self.record_buffer = []
        self.playback_rows = []
        self._playback_plan = []  # resample_rows(playback_rows, ...) at the speed Play was pressed with
        self.playback_index = 0
        self._playback_job = None

        self._build_widgets()
        self._refresh_ports()
        self._refresh_controllers()
        self._set_connected_state(False)
        self.root.after(50, self._drain_incoming)
        self.root.after(TICK_INTERVAL_MS, self._tick)

    # ---------- UI construction ----------
    def _build_widgets(self):
        conn = ttk.LabelFrame(self.root, text="Master serial connection", padding=8)
        conn.pack(fill="x", padx=8, pady=8)

        ttk.Label(conn, text="Port:").grid(row=0, column=0, sticky="w")
        self.port_var = tk.StringVar()
        self.port_combo = ttk.Combobox(conn, textvariable=self.port_var, width=24, state="readonly")
        self.port_combo.grid(row=0, column=1, sticky="w", padx=(4, 4))
        ttk.Button(conn, text="Refresh", command=self._refresh_ports).grid(row=0, column=2, sticky="w")
        self.connect_btn = ttk.Button(conn, text="Connect", command=self._toggle_connect)
        self.connect_btn.grid(row=0, column=3, sticky="w", padx=(8, 0))
        self.status_var = tk.StringVar(value="disconnected")
        ttk.Label(conn, textvariable=self.status_var, foreground="#a33").grid(row=0, column=4, sticky="w", padx=(10, 0))

        joy = ttk.LabelFrame(self.root, text="Controllers (all connected ones are usable at once)", padding=8)
        joy.pack(fill="x", padx=8, pady=(0, 8))

        self.joy_tree = ttk.Treeview(joy, columns=("axes",), show="tree headings", height=3)
        self.joy_tree.heading("#0", text="Controller")
        self.joy_tree.column("#0", width=220)
        self.joy_tree.heading("axes", text="Live axis values")
        self.joy_tree.column("axes", width=380)
        self.joy_tree.grid(row=0, column=0, sticky="ew")
        joy.columnconfigure(0, weight=1)
        ttk.Button(joy, text="Refresh Controllers", command=self._refresh_controllers).grid(
            row=0, column=1, sticky="n", padx=(8, 0)
        )
        self.joy_hint_var = tk.StringVar(value="no controllers detected")
        ttk.Label(joy, textvariable=self.joy_hint_var, foreground="#666").grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(4, 0)
        )

        mapframe = ttk.LabelFrame(self.root, text="Axis -> Node mappings", padding=8)
        mapframe.pack(fill="both", expand=False, padx=8, pady=(0, 8))

        self.map_tree = ttk.Treeview(mapframe, columns=("mapping",), show="headings", height=5)
        self.map_tree.heading("mapping", text="Mapping")
        self.map_tree.column("mapping", width=420)
        self.map_tree.pack(side="left", fill="both", expand=True)

        map_btns = ttk.Frame(mapframe)
        map_btns.pack(side="left", padx=(8, 0))
        self.learn_btn = ttk.Button(map_btns, text="Learn New Mapping", command=self._start_learn)
        self.learn_btn.pack(fill="x")
        ttk.Button(map_btns, text="Remove Selected", command=self._remove_selected_mapping).pack(fill="x", pady=(4, 0))
        ttk.Separator(map_btns, orient="horizontal").pack(fill="x", pady=6)
        ttk.Button(map_btns, text="Save Mappings…", command=self._save_mappings).pack(fill="x")
        ttk.Button(map_btns, text="Load Mappings…", command=self._load_mappings).pack(fill="x", pady=(4, 0))

        stream = ttk.LabelFrame(self.root, text="Streaming", padding=8)
        stream.pack(fill="x", padx=8, pady=(0, 8))
        self.streaming_var = tk.BooleanVar(value=False)
        self.stream_check = ttk.Checkbutton(
            stream,
            text="Start streaming mapped axes to their Nodes (~25 Hz)",
            variable=self.streaming_var,
            state="disabled",
        )
        self.stream_check.pack(side="left")
        self.stream_hint_var = tk.StringVar(value="learn at least one axis -> Node mapping to enable")
        ttk.Label(stream, textvariable=self.stream_hint_var, foreground="#666").pack(side="left", padx=(10, 0))

        rec = ttk.LabelFrame(self.root, text="Recording / Playback (CSV)", padding=8)
        rec.pack(fill="x", padx=8, pady=(0, 8))
        self.record_btn = ttk.Button(rec, text="Start Recording", command=self._toggle_recording)
        self.record_btn.grid(row=0, column=0, sticky="w")
        self.record_status_var = tk.StringVar(value="not recording")
        ttk.Label(rec, textvariable=self.record_status_var, foreground="#666").grid(
            row=0, column=1, sticky="w", padx=(8, 0)
        )
        ttk.Button(rec, text="Save Recording As…", command=self._save_recording).grid(
            row=0, column=2, sticky="w", padx=(16, 0)
        )
        ttk.Button(rec, text="Load CSV…", command=self._load_recording).grid(row=1, column=0, sticky="w", pady=(6, 0))
        self.play_btn = ttk.Button(rec, text="Play", command=self._play_recording)
        self.play_btn.grid(row=1, column=1, sticky="w", padx=(8, 0), pady=(6, 0))
        ttk.Button(rec, text="Stop", command=self._stop_playback).grid(row=1, column=2, sticky="w", padx=(8, 0), pady=(6, 0))
        self.loop_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(rec, text="Loop playback", variable=self.loop_var).grid(
            row=1, column=3, sticky="w", padx=(16, 0), pady=(6, 0)
        )
        ttk.Label(rec, text="Speed:").grid(row=1, column=4, sticky="w", padx=(16, 0), pady=(6, 0))
        self.speed_var = tk.DoubleVar(value=1.0)
        ttk.Spinbox(
            rec, from_=PLAYBACK_SPEED_MIN, to=PLAYBACK_SPEED_MAX, increment=0.1,
            textvariable=self.speed_var, width=6,
        ).grid(row=1, column=5, sticky="w", padx=(4, 0), pady=(6, 0))
        self.playback_status_var = tk.StringVar(value="no CSV loaded")
        ttk.Label(rec, textvariable=self.playback_status_var, foreground="#666").grid(
            row=2, column=0, columnspan=6, sticky="w", pady=(6, 0)
        )

        log_frame = ttk.LabelFrame(self.root, text="Serial log", padding=8)
        log_frame.pack(fill="both", expand=True, padx=8, pady=(0, 8))
        self.log_text = tk.Text(log_frame, height=10, state="disabled", wrap="none")
        self.log_text.pack(side="left", fill="both", expand=True)
        scroll = ttk.Scrollbar(log_frame, command=self.log_text.yview)
        scroll.pack(side="right", fill="y")
        self.log_text.configure(yscrollcommand=scroll.set)

    # ---------- serial connection ----------
    def _refresh_ports(self):
        ports = [p.device for p in list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def _toggle_connect(self):
        if self.link.is_open:
            self.link.disconnect()
            self._set_connected_state(False)
            self._log("-- disconnected --")
            if self._node_poll_job is not None:
                self.root.after_cancel(self._node_poll_job)
                self._node_poll_job = None
            return
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
        self._schedule_node_poll()

    def _request_node_list(self):
        if not self.link.is_open:
            return
        try:
            self.link.send({"cmd": "list"})
        except (RuntimeError, serial.SerialException, OSError) as exc:
            self._log(f"-- send failed: {exc} --")

    def _schedule_node_poll(self):
        self._request_node_list()
        self._node_poll_job = self.root.after(NODE_POLL_INTERVAL_MS, self._schedule_node_poll)

    def _known_nodes_snapshot(self):
        return dict(self.known_nodes)

    def _on_link_error(self, message):
        self.incoming.put(f"__ERROR__{message}")

    def _set_connected_state(self, connected):
        self.connect_btn.configure(text="Disconnect" if connected else "Connect")
        self.status_var.set("connected" if connected else "disconnected")
        self.port_combo.configure(state="disabled" if connected else "readonly")

    # ---------- controller handling ----------
    def _refresh_controllers(self):
        # A full quit/init cycle is how pygame picks up newly (dis)connected
        # devices; it also invalidates any previously-opened Joystick
        # objects, so everything gets reopened fresh below.
        pygame.joystick.quit()
        pygame.joystick.init()

        self.joysticks = {}
        for i in range(pygame.joystick.get_count()):
            try:
                js = pygame.joystick.Joystick(i)
                js.init()
                guid = js.get_guid()
            except pygame.error:
                continue
            self.joysticks[guid] = js
            self.joystick_names[guid] = js.get_name()

        self.joy_tree.delete(*self.joy_tree.get_children())
        for guid, js in self.joysticks.items():
            self.joy_tree.insert("", "end", iid=guid, text=self.joystick_names[guid], values=("",))

        if self.joysticks:
            names = ", ".join(self.joystick_names[g] for g in self.joysticks)
            self.joy_hint_var.set(f"{len(self.joysticks)} controller(s) connected: {names}")
            self.learn_btn.configure(state="normal")
        else:
            self.joy_hint_var.set("no controllers detected")
            self.learn_btn.configure(state="disabled")

        # Mappings whose controller just (dis)appeared need their connectivity
        # label refreshed, and a reconnect is a good moment to try binding any
        # still-unresolved (e.g. loaded-from-an-old-file) mappings by name.
        for m in self.mappings:
            self._try_autobind(m)
        self._reload_mapping_tree()

    # ---------- mappings ----------
    def _start_learn(self):
        if not self.joysticks:
            messagebox.showwarning("No controller", "Connect a controller and click Refresh Controllers first.")
            return
        controllers = [(guid, self.joystick_names[guid], js) for guid, js in self.joysticks.items()]
        LearnDialog(
            self.root, controllers, known_nodes_provider=self._known_nodes_snapshot, on_saved=self._add_mapping
        )

    def _try_autobind(self, mapping):
        """If `mapping`'s controller isn't currently connected but exactly one
        connected controller shares its display name, adopt that
        controller's guid. Lets a file saved by the older single-controller
        format (or one made with a controller currently plugged into a
        different USB port) reattach automatically instead of sitting
        "not connected" forever for no real reason."""
        if mapping.controller_guid in self.joysticks:
            return
        if not mapping.controller_name:
            return
        candidates = [g for g, name in self.joystick_names.items() if name == mapping.controller_name and g in self.joysticks]
        if len(candidates) == 1:
            old_guid = mapping.controller_guid
            mapping.controller_guid = candidates[0]
            self._log(
                f"-- auto-bound mapping ({mapping.controller_name} axis {mapping.axis_index} -> "
                f"Node {mapping.node_id}) to connected controller (was {old_guid}) --"
            )

    def _mapping_label(self, mapping):
        label = mapping.label()
        if mapping.controller_guid not in self.joysticks:
            label += "  [controller not connected]"
        return label

    def _add_mapping(self, mapping):
        self.mappings.append(mapping)
        self.map_tree.insert("", "end", iid=str(len(self.mappings) - 1), values=(self._mapping_label(mapping),))
        self._log(f"-- mapping added: {mapping.label()} --")
        self._update_streaming_availability()

    def _remove_selected_mapping(self):
        for iid in self.map_tree.selection():
            idx = int(iid)
            self.mappings[idx] = None  # placeholder to keep indices stable, filtered out below
        self.mappings = [m for m in self.mappings if m is not None]
        self._reload_mapping_tree()

    def _reload_mapping_tree(self):
        self.map_tree.delete(*self.map_tree.get_children())
        for i, m in enumerate(self.mappings):
            self.map_tree.insert("", "end", iid=str(i), values=(self._mapping_label(m),))
        self._update_streaming_availability()

    def _save_mappings(self):
        if not self.mappings:
            messagebox.showinfo("Nothing to save", "Learn at least one mapping first.")
            return
        path = filedialog.asksaveasfilename(defaultextension=".json", filetypes=[("JSON files", "*.json")])
        if not path:
            return
        save_mapping_config(path, self.mappings)
        self._log(f"-- saved {len(self.mappings)} mapping(s) to {path} --")

    def _load_mappings(self):
        path = filedialog.askopenfilename(filetypes=[("JSON files", "*.json")])
        if not path:
            return
        try:
            mappings = load_mapping_config(path)
        except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
            messagebox.showerror("Load failed", str(exc))
            return
        if not mappings:
            messagebox.showwarning("Empty file", "No mappings found in that file.")
            return
        for m in mappings:
            self._try_autobind(m)
        self.mappings = mappings
        self._reload_mapping_tree()
        self._log(f"-- loaded {len(mappings)} mapping(s) from {path} --")
        missing = sorted({m.controller_name or "unknown" for m in mappings if m.controller_guid not in self.joysticks})
        if missing:
            messagebox.showwarning(
                "Controller(s) not connected",
                "These mappings reference controllers that aren't currently connected: "
                + ", ".join(missing)
                + ". Connect them and click Refresh Controllers — streaming/recording just skips a "
                "mapping while its controller is offline.",
            )

    def _update_streaming_availability(self):
        if self.mappings:
            self.stream_check.configure(state="normal")
            self.stream_hint_var.set(f"{len(self.mappings)} mapping(s) active")
        else:
            self.streaming_var.set(False)
            self.stream_check.configure(state="disabled")
            self.stream_hint_var.set("learn at least one axis -> Node mapping to enable")

    # ---------- main tick: display + streaming + recording ----------
    def _tick(self):
        if self.joysticks:
            pygame.event.pump()
            disconnected = []
            for guid, js in self.joysticks.items():
                try:
                    axes_preview = ", ".join(f"{i}:{js.get_axis(i):+.2f}" for i in range(js.get_numaxes()))
                    if self.joy_tree.exists(guid):
                        self.joy_tree.item(guid, values=(axes_preview,))
                except pygame.error:
                    disconnected.append(guid)
            if disconnected:
                for guid in disconnected:
                    del self.joysticks[guid]
                    if self.joy_tree.exists(guid):
                        self.joy_tree.item(guid, values=("(disconnected — click Refresh Controllers)",))
                self._log(f"-- controller disconnected: {', '.join(self.joystick_names[g] for g in disconnected)} --")
                self._reload_mapping_tree()  # mapping labels' "[not connected]" suffix needs updating

            if self.mappings:
                now_ms = int(time.monotonic() * 1000)
                sample = {}
                for m in self.mappings:
                    js = self.joysticks.get(m.controller_guid)
                    if js is None:
                        continue  # that mapping's controller isn't connected right now; skip it
                    try:
                        raw = js.get_axis(m.axis_index)
                    except pygame.error:
                        continue
                    angle = m.compute_angle(raw)
                    sample[m.node_id] = angle
                    if self.streaming_var.get() and self.link.is_open:
                        if m.last_sent_angle is None or abs(angle - m.last_sent_angle) >= ANGLE_SEND_EPSILON:
                            m.last_sent_angle = angle
                            try:
                                line = self.link.send({"node": m.node_id, "angle": round(angle, 1)})
                                self._log(f"-> {line.rstrip()}")
                            except (RuntimeError, serial.SerialException, OSError) as exc:
                                self._log(f"-- send failed: {exc} --")
                if self.recording:
                    elapsed = now_ms - self.record_start_ms
                    self.record_buffer.append((elapsed, sample))
                    self.record_status_var.set(f"recording… {len(self.record_buffer)} samples")

        self._tick_job = self.root.after(TICK_INTERVAL_MS, self._tick)

    # ---------- recording ----------
    def _toggle_recording(self):
        if self.recording:
            self.recording = False
            self.record_btn.configure(text="Start Recording")
            self.record_status_var.set(f"stopped — {len(self.record_buffer)} samples captured")
        else:
            if not self.mappings:
                messagebox.showwarning("No mappings", "Learn at least one axis mapping first.")
                return
            self.record_buffer = []
            self.record_start_ms = int(time.monotonic() * 1000)
            self.recording = True
            self.record_btn.configure(text="Stop Recording")
            self.record_status_var.set("recording… 0 samples")

    def _save_recording(self):
        if not self.record_buffer:
            messagebox.showinfo("Nothing to save", "Record something first.")
            return
        path = filedialog.asksaveasfilename(defaultextension=".csv", filetypes=[("CSV files", "*.csv")])
        if not path:
            return
        save_csv_rows(path, self.record_buffer)
        self._log(f"-- saved {len(self.record_buffer)} samples to {path} --")

    # ---------- playback ----------
    def _load_recording(self):
        path = filedialog.askopenfilename(filetypes=[("CSV files", "*.csv")])
        if not path:
            return
        try:
            self.playback_rows = load_csv_rows(path)
        except (OSError, ValueError, IndexError) as exc:
            messagebox.showerror("Load failed", str(exc))
            return
        self.playback_status_var.set(f"loaded {len(self.playback_rows)} rows from {path}")
        self._log(f"-- loaded {len(self.playback_rows)} rows from {path} --")

    def _play_recording(self):
        if not self.playback_rows:
            messagebox.showwarning("No recording loaded", "Load a CSV first.")
            return
        if not self.link.is_open:
            messagebox.showwarning("Not connected", "Connect to the Master's serial port first.")
            return
        self._stop_playback()
        self._resample_for_playback()
        self.playback_index = 0
        self._send_playback_row()

    def _resample_for_playback(self):
        speed = self._playback_speed()
        self._playback_plan = resample_rows(self.playback_rows, TICK_INTERVAL_MS, speed)
        self._log(
            f"-- resampled {len(self.playback_rows)} recorded sample(s) to "
            f"{len(self._playback_plan)} at {speed:.2f}x speed --"
        )

    def _send_playback_row(self):
        if self.playback_index >= len(self._playback_plan):
            return
        t_ms, sample = self._playback_plan[self.playback_index]
        for node_id, angle in sample.items():
            try:
                line = self.link.send({"node": node_id, "angle": round(angle, 1)})
                self._log(f"-> {line.rstrip()}")
            except (RuntimeError, serial.SerialException, OSError) as exc:
                self._log(f"-- playback send failed: {exc} --")
                self._stop_playback()
                return

        next_index = self.playback_index + 1
        if next_index < len(self._playback_plan):
            delay_ms = max(1, self._playback_plan[next_index][0] - t_ms)
            self.playback_index = next_index
            self._playback_job = self.root.after(delay_ms, self._send_playback_row)
        elif self.loop_var.get():
            self._resample_for_playback()  # pick up any speed change before looping again
            self.playback_index = 0
            self._playback_job = self.root.after(1, self._send_playback_row)
        else:
            self._log("-- playback finished --")

    def _playback_speed(self):
        try:
            speed = float(self.speed_var.get())
        except (tk.TclError, ValueError):
            speed = 1.0
        return min(PLAYBACK_SPEED_MAX, max(PLAYBACK_SPEED_MIN, speed))

    def _stop_playback(self):
        if self._playback_job is not None:
            self.root.after_cancel(self._playback_job)
            self._playback_job = None

    # ---------- incoming serial data ----------
    def _drain_incoming(self):
        try:
            while True:
                text = self.incoming.get_nowait()
                if text.startswith("__ERROR__"):
                    self._log(f"-- serial error: {text[len('__ERROR__'):]} --")
                    self._set_connected_state(False)
                else:
                    self._handle_incoming_line(text)
        except queue.Empty:
            pass
        self.root.after(50, self._drain_incoming)

    def _handle_incoming_line(self, text):
        self._log(f"<- {text}")
        try:
            msg = json.loads(text)
        except json.JSONDecodeError:
            return
        if isinstance(msg, dict) and msg.get("type") == "nodes":
            self.known_nodes = {n["id"]: n for n in msg.get("nodes", []) if "id" in n}

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
        if self._tick_job is not None:
            self.root.after_cancel(self._tick_job)
        if self._node_poll_job is not None:
            self.root.after_cancel(self._node_poll_job)
        self._stop_playback()
        self.link.disconnect()
        pygame.quit()
        self.root.destroy()


def main():
    root = tk.Tk()
    App(root)
    root.mainloop()


if __name__ == "__main__":
    main()
