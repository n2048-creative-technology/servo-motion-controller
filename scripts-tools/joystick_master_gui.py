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
  - "Learn" a button the same way and link it to a Node's relay/light, so a
    button press toggles that one Node's light (or holds it on while held,
    if you pick momentary). Each Node's light is switched independently —
    the Master tracks relay state per target, so Node 3's button never
    disturbs Node 5's light.
  - Record the computed angle *and* the light state for every mapped Node to
    a CSV file (which controller/axis/button produced it is irrelevant to
    playback — only Node ID + angle + light are recorded), and replay that
    CSV later to reproduce the same motion and lighting without any physical
    controller connected at all.

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
# For a two-axis stick to be learned as a pair, the second axis must have moved
# at least this fraction of the first. Loose enough that a rolled stick counts,
# tight enough that cross-talk while sweeping one axis doesn't.
STICK_PAIR_MIN_RATIO = 0.4
ANGLE_SEND_EPSILON = 0.2  # degrees; skip a resend below this delta

# A Node's light needs an angle to travel with (the firmware carries relay
# state on ordinary move commands — see ../docs/serial-protocol.md), so a
# light-only change re-sends the Node's last known angle. This is the
# fallback when we've never seen one: the firmware clamps it to that Node's
# own calibration anyway.
DEFAULT_HOLD_ANGLE = 135.0
DEFAULT_ANGLE_MIN = 0.0
DEFAULT_ANGLE_MAX = 270.0
MAX_LOG_LINES = 500
PLAYBACK_SPEED_MIN = 0.1
PLAYBACK_SPEED_MAX = 2.0

# Must match the firmware's Config.h: MAX_SEQ_POINTS * RECORD_INTERVAL_MS —
# how long a Node's own recording buffer can hold (400s/6min40s at 8000
# points/50ms; the count came down when each point gained its Y axis).
UPLOAD_MAX_DURATION_MS = 400000
UPLOAD_ACK_TIMEOUT_MS = 4000  # how long to wait for a SEQ_ACK before retrying the stop-and-save once

# SEQ_START has no delivery guarantee and, unlike an ordinary move command,
# a lost one is invisible: the Node still moves on every point of the
# stream that follows (PlaybackEngine::onNetworkCommand writes the servo
# unconditionally, whether or not recording actually started), so nothing
# *looks* wrong until the save fails at the very end with zero points
# captured. Resending the start request a few times, like the firmware
# already does for ordinary move commands, costs nothing — the Node's
# SequenceStore::startRecording() just resets its buffer each time — and
# makes a single dropped packet far less likely to lose an entire upload.
UPLOAD_START_RESENDS = 5
UPLOAD_START_RESEND_INTERVAL_MS = 100

# "Clear before uploading" fires remote_clear and waits this long before
# starting the actual remote_record_start — SEQ_CLEAR has no ack (see
# firmware NetworkLink.h), so this is just giving it a moment to land and
# the Node's own directory-scan-and-delete loop a moment to finish before
# the upload's own control packets start competing for the same link.
UPLOAD_CLEAR_SETTLE_MS = 300

# When uploading to several Nodes at once, each Node's own upload still runs
# concurrently end to end (their point streams fully overlap — that's the
# whole point), but kicking every Node's remote_record_start off in the very
# same instant means their remote_record_stop requests (and the SEQ_ACKs
# racing back from N different Nodes) also land in the same few
# milliseconds, since real "upload to all Nodes" sources are almost always
# same-duration multi-Node recordings. Verified against real hardware: doing
# that produced a reproducible 0/4 success rate (every ack lost) across
# repeated runs, versus reliable success staggered like this by even a
# small, mostly-imperceptible offset per Node.
UPLOAD_BATCH_STAGGER_MS = 150

# Must match firmware's SequenceStore.cpp FileHeader (12 bytes) and
# SequencePoint (12 bytes: uint32_t t_ms + int16_t x + int16_t y + flags) —
# used to estimate a recording's on-Node file size for the free-space
# preflight check below. RECORD_INTERVAL_MS must match Config.h: a Node
# captures at that cadence regardless of how densely we stream points to it,
# so the estimate is based on wall-clock duration, not our own point count.
SEQ_FILE_HEADER_BYTES = 12
SEQ_POINT_BYTES = 12
NODE_RECORD_INTERVAL_MS = 50

# How long to wait for a space_reply before giving up and uploading anyway
# (best-effort, like every other ESP-NOW round trip in this tool — a Node
# that doesn't answer isn't necessarily out of space, it might just be a
# dropped packet, so this doesn't block the upload, only informs it).
SPACE_QUERY_TIMEOUT_MS = 800


def estimate_sequence_file_bytes(duration_ms):
    """How many bytes a recording of this wall-clock duration will occupy on
    a Node's flash once captured at its own fixed cadence — see
    NODE_RECORD_INTERVAL_MS."""
    points = int(duration_ms / NODE_RECORD_INTERVAL_MS) + 1
    return SEQ_FILE_HEADER_BYTES + points * SEQ_POINT_BYTES


# Must match firmware's Config.h SEQ_NAME_MAX_LEN and SequenceStore::sanitizeName.
SEQ_NAME_MAX_LEN = 23
_SEQ_NAME_ALLOWED_CHARS = frozenset(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-"
)


def sanitize_sequence_name(name):
    """Mirrors the firmware's SequenceStore::sanitizeName exactly. The Node
    saves an uploaded sequence under this sanitized form and echoes it back
    verbatim in its SEQ_ACK — sending anything else (e.g. a name over 23
    chars, or containing a space) makes the ack's name never match what we
    sent, so a successful upload looks stuck/failed until the ack timeout
    (see App._on_upload_ack). Sanitizing client-side before sending avoids
    that mismatch entirely."""
    return "".join(c for c in name if c in _SEQ_NAME_ALLOWED_CHARS)[:SEQ_NAME_MAX_LEN]


class AxisMapping:
    """One learned (controller, axis) -> (Node, servo axis) link, with its raw
    and output angle ranges. `target_axis` is "x" (pan, D10) or "y" (tilt, D3):
    a node is a pan/tilt head, so a stick's two axes usually become two of
    these pointed at the same node. `controller_guid` (SDL/pygame's Joystick.get_guid(), stable
    across reconnects and USB port/order changes) is what actually resolves
    to a live pygame.joystick.Joystick at runtime; `controller_name` is a
    display-only label carried alongside it for the UI and for reattaching a
    loaded mapping to a currently-connected controller by name when its guid
    isn't connected (see App._try_autobind)."""

    def __init__(
        self, controller_guid, controller_name, axis_index, raw_min, raw_max, node_id, angle_min, angle_max,
        invert=False, target_axis="x",
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
        self.target_axis = target_axis if target_axis in ("x", "y") else "x"
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
        return (f"{controller} axis {self.axis_index} -> Node {self.node_id} "
                f"{self.target_axis.upper()} [{self.angle_min:.0f}-{self.angle_max:.0f}°]")

    def to_dict(self):
        return {
            "kind": "axis",
            "controller_guid": self.controller_guid,
            "controller_name": self.controller_name,
            "axis_index": self.axis_index,
            "raw_min": self.raw_min,
            "raw_max": self.raw_max,
            "node_id": self.node_id,
            "angle_min": self.angle_min,
            "angle_max": self.angle_max,
            "invert": self.invert,
            "target_axis": self.target_axis,
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
            # Mappings saved before nodes had two axes drove the one servo
            # there was, which is now X.
            target_axis=d.get("target_axis", "x"),
        )


class ButtonMapping:
    """One learned (controller, button) -> Node relay/light link.

    Two behaviours, because both are useful for a light:
      - "toggle" (default): each press flips that Node's light. What a wall
        switch does, and what you want for a light left on across a take.
      - "momentary": light follows the button — on while held, off on
        release. For flashes/strobing by hand.

    Node targeting is per-Node by design: the Master keeps relay state
    separately for each target, so one button switching Node 3's light
    leaves every other Node's alone (see NetworkLink::sendCommand in the
    firmware)."""

    def __init__(self, controller_guid, controller_name, button_index, node_id, mode="toggle"):
        self.controller_guid = controller_guid
        self.controller_name = controller_name
        self.button_index = button_index
        self.node_id = node_id
        self.mode = mode if mode in ("toggle", "momentary") else "toggle"
        self.was_pressed = False  # edge detection state, not persisted

    def next_state(self, pressed, current):
        """The light state this button wants, given whether it's held right
        now and what the light is currently doing. Edge detection lives here
        (not in the caller's tick loop) so it's exercisable on its own."""
        if self.mode == "momentary":
            new_state = pressed
        elif pressed and not self.was_pressed:
            new_state = not current  # toggle on the press edge only, not while held
        else:
            new_state = current
        self.was_pressed = pressed
        return new_state

    def label(self):
        controller = self.controller_name or "unknown controller"
        return f"{controller} button {self.button_index} -> Node {self.node_id} light [{self.mode}]"

    def to_dict(self):
        return {
            "kind": "button",
            "controller_guid": self.controller_guid,
            "controller_name": self.controller_name,
            "button_index": self.button_index,
            "node_id": self.node_id,
            "mode": self.mode,
        }

    @classmethod
    def from_dict(cls, d):
        return cls(
            controller_guid=d.get("controller_guid"),
            controller_name=d.get("controller_name"),
            button_index=int(d["button_index"]),
            node_id=int(d["node_id"]),
            mode=d.get("mode", "toggle"),
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
    mappings = [
        # No "kind" at all means a mapping file written before button/light
        # mappings existed — those were all axis mappings.
        ButtonMapping.from_dict(d) if d.get("kind") == "button" else AxisMapping.from_dict(d)
        for d in data.get("mappings", [])
    ]
    if legacy_name:
        for m in mappings:
            if m.controller_guid is None and m.controller_name is None:
                m.controller_name = legacy_name
    return mappings


# Recorded rows are (t_ms, xs, ys, lights) — three dicts keyed by node id.
# They're kept as separate channels rather than one record per node because a
# node can legitimately have any subset: an axis mapped to pan only, a button
# but no stick (light-only), and so on. A channel a node has no mapping for is
# simply absent, and stays absent through save/load/resample/playback.
CSV_LIGHT_SUFFIX = "_light"
CSV_X_SUFFIX = "_x"
CSV_Y_SUFFIX = "_y"


def load_csv_rows(path):
    """Parse a recorded CSV into [(t_ms, xs, ys, lights), ...].

    Reads the current format (`node_N_x`, `node_N_y`, `node_N_light`), the
    previous one (`node_N` angle + optional `node_N_light`), and the original
    angle-only files. A bare `node_N` column is read as that node's X (pan),
    which is the servo those recordings were made with."""
    with open(path, newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
        x_cols, y_cols, light_cols = {}, {}, {}
        for idx, col in enumerate(header[1:], start=1):
            if not col.startswith("node_"):
                continue
            rest = col[len("node_"):]
            if rest.endswith(CSV_LIGHT_SUFFIX):
                light_cols[idx] = int(rest[: -len(CSV_LIGHT_SUFFIX)])
            elif rest.endswith(CSV_Y_SUFFIX):
                y_cols[idx] = int(rest[: -len(CSV_Y_SUFFIX)])
            elif rest.endswith(CSV_X_SUFFIX):
                x_cols[idx] = int(rest[: -len(CSV_X_SUFFIX)])
            else:
                x_cols[idx] = int(rest)  # pre-pan/tilt file: the one axis is X
        rows = []
        for row in reader:
            t_ms = int(float(row[0]))
            def floats(cols):
                return {nid: float(row[i]) for i, nid in cols.items() if i < len(row) and row[i] != ""}
            lights = {
                nid: row[i].strip().lower() in ("1", "true", "on")
                for i, nid in light_cols.items()
                if i < len(row) and row[i] != ""
            }
            rows.append((t_ms, floats(x_cols), floats(y_cols), lights))
    return rows


def save_csv_rows(path, rows):
    """Write [(t_ms, xs, ys, lights), ...] in the shape load_csv_rows reads.

    A node only gets the columns it actually has data for, so a pan-only
    recording doesn't carry a column of blanks for tilt."""
    channels = [(CSV_X_SUFFIX, 1), (CSV_Y_SUFFIX, 2), (CSV_LIGHT_SUFFIX, 3)]
    node_ids = sorted({nid for row in rows for ch in (1, 2, 3) for nid in row[ch]})

    columns = []  # (node_id, row_index, suffix)
    for nid in node_ids:
        for suffix, idx in channels:
            if any(nid in row[idx] for row in rows):
                columns.append((nid, idx, suffix))

    with open(path, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["t_ms"] + [f"node_{nid}{suffix}" for nid, _idx, suffix in columns])
        for row in rows:
            out = [row[0]]
            for nid, idx, suffix in columns:
                value = row[idx].get(nid)
                if value is None:
                    out.append("")
                elif suffix == CSV_LIGHT_SUFFIX:
                    out.append("1" if value else "0")
                else:
                    out.append(f"{value:.1f}")
            writer.writerow(out)


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


def _light_at(times, values, src_t):
    """A light's state at src_t: held from the most recent sample at or before
    it (a relay is on or off — interpolating between the two would mean
    chattering it at the sample rate). Mirrors the firmware's
    SequenceStore::relayAtTime."""
    if not times:
        return None
    if src_t <= times[0]:
        return values[0]
    i = bisect.bisect_right(times, src_t) - 1
    return values[max(0, i)]


def resample_rows(rows, output_interval_ms, speed):
    """Resample recorded (t_ms, xs, ys, lights) rows onto a fixed-cadence
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

    Angles interpolate; lights are held (stepped), mirroring the firmware.
    Always ends on the exact final recorded position, at every speed.
    """
    if not rows:
        return []

    # channel index -> {node_id: (times, values)}
    timelines = {}
    for idx in (1, 2, 3):
        node_ids = sorted({nid for row in rows for nid in row[idx]})
        channel = {}
        for nid in node_ids:
            pts = [(row[0], row[idx][nid]) for row in rows if nid in row[idx]]
            channel[nid] = ([p[0] for p in pts], [p[1] for p in pts])
        timelines[idx] = channel

    duration_ms = rows[-1][0]
    output_duration_ms = max(0, int(duration_ms / speed))

    def sample_at(src_t):
        out = []
        for idx in (1, 2, 3):
            picker = _light_at if idx == 3 else _interp_channel
            values = {}
            for nid, (times, vals) in timelines[idx].items():
                v = picker(times, vals, src_t)
                if v is not None:
                    values[nid] = v
            out.append(values)
        return out

    out_rows = []
    out_t = 0
    while out_t < output_duration_ms:
        xs, ys, lights = sample_at(out_t * speed)
        out_rows.append((out_t, xs, ys, lights))
        out_t += output_interval_ms
    xs, ys, lights = sample_at(duration_ms)
    out_rows.append((output_duration_ms, xs, ys, lights))  # exact final position, always
    return out_rows


class LearnDialog(tk.Toplevel):
    """Phase 1: watch every axis *and* button of every currently-connected
    controller for a few seconds, and pick whichever one you actually used —
    so with several controllers plugged in, you just move the stick (or press
    the button) you want to link and it's identified automatically, no need
    to pick a controller first. Phase 2: assign that winner to a Node,
    reusing the same window: an axis gets an angle range, a button gets that
    Node's light.

    A button press wins over any axis movement in the same window: axes idle
    with noise and drift, so "a button went down" is the unambiguous signal
    of intent, whereas a big axis span could just be a stick being released."""

    def __init__(self, parent, controllers, known_nodes_provider, on_saved):
        super().__init__(parent)
        self.title("Learn axis or button")
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
        self.pressed_button = None  # (guid, button_index) of the first button seen going down
        self.elapsed_ms = 0

        watching = ", ".join(name for _g, name, _j in self.controllers) or "no controllers connected"
        self.status_var = tk.StringVar(
            value=f"Move the axis, or press the button, you want to link now… (watching: {watching})"
        )
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
                if self.pressed_button is None:
                    for b in range(js.get_numbuttons()):
                        if js.get_button(b):
                            self.pressed_button = (guid, b)
                            break
            except pygame.error:
                continue  # that controller was unplugged mid-learn; skip it this tick
        self.elapsed_ms += LEARN_POLL_INTERVAL_MS
        self.progress["value"] = min(self.elapsed_ms, LEARN_DURATION_MS)
        # A pressed button is unambiguous, so don't make the user wait out the
        # rest of the window for it.
        if self.pressed_button is not None:
            self._finish_learn()
        elif self.elapsed_ms < LEARN_DURATION_MS:
            self.after(LEARN_POLL_INTERVAL_MS, self._poll)
        else:
            self._finish_learn()

    def _stick_candidate(self):
        """Two axes on the *same* controller that both moved clearly: almost
        always the two halves of one physical stick, which for a pan/tilt head
        is what you actually want to map. Returns (guid, axisA, axisB) with the
        pair in axis-index order, or None.

        Requires the second axis to have moved a decent fraction of the first
        (not just past the noise floor), so a deliberate one-axis sweep with a
        little cross-talk still learns as a single axis."""
        spans = sorted(((self.maxs[k] - self.mins[k], k) for k in self.mins), reverse=True)
        if len(spans) < 2:
            return None
        best_span, (best_guid, best_axis) = spans[0]
        if best_span < MIN_LEARN_RANGE:
            return None
        for span, (guid, axis) in spans[1:]:
            if guid != best_guid or axis == best_axis:
                continue
            if span >= MIN_LEARN_RANGE and span >= best_span * STICK_PAIR_MIN_RATIO:
                lo, hi = sorted((best_axis, axis))
                return (best_guid, lo, hi)
            break  # spans are sorted; nothing after this can qualify either
        return None

    def _finish_learn(self):
        if self.pressed_button is not None:
            guid, button_index = self.pressed_button
            controller_name = next(
                (name for g, name, _js in self.controllers if g == guid), "unknown controller"
            )
            self._show_button_form(guid, controller_name, button_index)
            return

        stick = self._stick_candidate()
        if stick is not None:
            guid, axis_a, axis_b = stick
            controller_name = next(
                (name for g, name, _js in self.controllers if g == guid), "unknown controller"
            )
            self._show_stick_form(guid, controller_name, axis_a, axis_b)
            return

        spans = [(self.maxs[k] - self.mins[k], k) for k in self.mins]
        spans.sort(reverse=True)
        best_span, best_key = spans[0] if spans else (0.0, None)
        if best_key is None or best_span < MIN_LEARN_RANGE:
            messagebox.showwarning(
                "No clear input",
                "Didn't detect a clear axis movement or button press. Try again and either move one "
                "axis (on any connected controller) through its full range, or press the button you "
                "want to link.",
                parent=self,
            )
            self.destroy()
            return
        guid, axis_index = best_key
        controller_name = next((name for g, name, _js in self.controllers if g == guid), "unknown controller")
        self._show_assign_form(guid, controller_name, axis_index, self.mins[best_key], self.maxs[best_key])

    def _build_node_picker(self, form):
        """Node ID combobox + Refresh, filled from the Master's known-node
        table. Returns a resolver that yields the chosen id (or None, having
        already shown the error, if what's typed isn't usable) — shared by
        the axis and button assign forms."""
        ttk.Label(form, text="Node ID:").grid(row=0, column=0, sticky="w")
        node_var = tk.StringVar()
        node_combo = ttk.Combobox(form, textvariable=node_var, width=26)
        node_combo.grid(row=0, column=1, sticky="w", padx=(4, 0))
        node_display_to_id = {}
        known_hint_var = tk.StringVar(value="")

        def refresh_known_nodes():
            nonlocal node_display_to_id
            known = self.known_nodes_provider()
            node_display_to_id = {}
            values = []
            for nid in sorted(known):
                n = known[nid]
                age_s = n.get("age_ms", 0) / 1000.0
                light = "light on" if n.get("relay") else "light off"
                display = f"{nid} ({n.get('angle', 0):.1f}°, {light}, {age_s:.1f}s ago)"
                node_display_to_id[display] = nid
                values.append(display)
            node_combo["values"] = values
            if values and not node_var.get():
                node_var.set(values[0])
            known_hint_var.set(f"{len(values)} known node(s)" if values else "no nodes heard from yet — type an ID")

        ttk.Button(form, text="Refresh", command=refresh_known_nodes, width=8).grid(
            row=0, column=2, sticky="w", padx=(4, 0)
        )
        ttk.Label(form, textvariable=known_hint_var, foreground="#666").grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(2, 6)
        )
        refresh_known_nodes()

        def resolve():
            raw_node = node_var.get().strip()
            node_id = node_display_to_id.get(raw_node)
            if node_id is None:
                try:
                    node_id = int(raw_node.split()[0])
                except (ValueError, IndexError):
                    messagebox.showerror(
                        "Invalid input", "Pick a known node or type a Node ID (0-250).", parent=self
                    )
                    return None
            if not (0 <= node_id <= 250):
                messagebox.showerror("Invalid input", "Node ID must be 0-250.", parent=self)
                return None
            return node_id

        return resolve

    def _show_stick_form(self, controller_guid, controller_name, axis_a, axis_b):
        """Both axes of one stick -> one node's X and Y, in a single step."""
        for child in self.winfo_children():
            child.destroy()

        ttk.Label(
            self,
            text=(f"Detected {controller_name} axes {axis_a} + {axis_b} — a stick. "
                  "Link both to one pan/tilt node:"),
            padding=(12, 12, 12, 4),
            wraplength=340,
        ).grid(row=0, column=0, columnspan=2, sticky="w")

        form = ttk.Frame(self, padding=12)
        form.grid(row=1, column=0, columnspan=2, sticky="ew")
        resolve_node_id = self._build_node_picker(form)

        # Which half of the stick drives pan. The lower axis index is X on
        # essentially every gamepad, but a flight stick or an odd HID mapping
        # can disagree, so it's one checkbox to swap rather than a re-learn.
        swap_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(form, text=f"Swap (use axis {axis_b} for X, axis {axis_a} for Y)",
                        variable=swap_var).grid(row=2, column=0, columnspan=3, sticky="w", pady=(6, 0))

        invert_x = tk.BooleanVar(value=False)
        invert_y = tk.BooleanVar(value=True)  # screen/stick down usually means "tilt down"
        ttk.Checkbutton(form, text="Invert X", variable=invert_x).grid(row=3, column=0, sticky="w", pady=(6, 0))
        ttk.Checkbutton(form, text="Invert Y", variable=invert_y).grid(row=3, column=1, sticky="w", pady=(6, 0))

        ttk.Label(form, text="X angle range (°):").grid(row=4, column=0, sticky="w", pady=(6, 0))
        x_min = tk.DoubleVar(value=DEFAULT_ANGLE_MIN)
        x_max = tk.DoubleVar(value=DEFAULT_ANGLE_MAX)
        ttk.Entry(form, textvariable=x_min, width=8).grid(row=4, column=1, sticky="w", pady=(6, 0))
        ttk.Entry(form, textvariable=x_max, width=8).grid(row=4, column=2, sticky="w", pady=(6, 0))

        ttk.Label(form, text="Y angle range (°):").grid(row=5, column=0, sticky="w", pady=(6, 0))
        y_min = tk.DoubleVar(value=DEFAULT_ANGLE_MIN)
        y_max = tk.DoubleVar(value=DEFAULT_ANGLE_MAX)
        ttk.Entry(form, textvariable=y_min, width=8).grid(row=5, column=1, sticky="w", pady=(6, 0))
        ttk.Entry(form, textvariable=y_max, width=8).grid(row=5, column=2, sticky="w", pady=(6, 0))

        def save():
            node_id = resolve_node_id()
            if node_id is None:
                return
            try:
                ranges = {
                    "x": (float(x_min.get()), float(x_max.get())),
                    "y": (float(y_min.get()), float(y_max.get())),
                }
            except (tk.TclError, ValueError):
                messagebox.showerror("Invalid input", "Angle ranges must be numbers.", parent=self)
                return

            pan_axis, tilt_axis = (axis_b, axis_a) if swap_var.get() else (axis_a, axis_b)
            for axis_index, target, invert in ((pan_axis, "x", invert_x.get()),
                                               (tilt_axis, "y", invert_y.get())):
                lo, hi = ranges[target]
                self.on_saved(AxisMapping(
                    controller_guid=controller_guid,
                    controller_name=controller_name,
                    axis_index=axis_index,
                    raw_min=self.mins[(controller_guid, axis_index)],
                    raw_max=self.maxs[(controller_guid, axis_index)],
                    node_id=node_id,
                    angle_min=lo,
                    angle_max=hi,
                    invert=invert,
                    target_axis=target,
                ))
            self.destroy()

        btns = ttk.Frame(self, padding=(12, 0, 12, 12))
        btns.grid(row=2, column=0, columnspan=2, sticky="e")
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="left", padx=(0, 8))
        ttk.Button(btns, text="Save Both Mappings", command=save).pack(side="left")

    def _show_button_form(self, controller_guid, controller_name, button_index):
        for child in self.winfo_children():
            child.destroy()

        ttk.Label(
            self,
            text=f"Detected {controller_name} button {button_index} — link it to a Node's light",
            padding=(12, 12, 12, 4),
            wraplength=320,
        ).grid(row=0, column=0, columnspan=2, sticky="w")

        form = ttk.Frame(self, padding=12)
        form.grid(row=1, column=0, columnspan=2, sticky="ew")

        resolve_node_id = self._build_node_picker(form)

        mode_var = tk.StringVar(value="toggle")
        ttk.Radiobutton(
            form, text="Toggle: each press flips the light", variable=mode_var, value="toggle"
        ).grid(row=2, column=0, columnspan=3, sticky="w", pady=(6, 0))
        ttk.Radiobutton(
            form, text="Momentary: light on only while held", variable=mode_var, value="momentary"
        ).grid(row=3, column=0, columnspan=3, sticky="w")

        def save():
            node_id = resolve_node_id()
            if node_id is None:
                return
            self.on_saved(
                ButtonMapping(
                    controller_guid=controller_guid,
                    controller_name=controller_name,
                    button_index=button_index,
                    node_id=node_id,
                    mode=mode_var.get(),
                )
            )
            self.destroy()

        btns = ttk.Frame(self, padding=(12, 0, 12, 12))
        btns.grid(row=2, column=0, columnspan=2, sticky="e")
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="left", padx=(0, 8))
        ttk.Button(btns, text="Save Mapping", command=save).pack(side="left")

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

        resolve_node_id = self._build_node_picker(form)

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

        ttk.Label(form, text="Drives:").grid(row=5, column=0, sticky="w", pady=(6, 0))
        target_var = tk.StringVar(value="x")
        axis_row = ttk.Frame(form)
        axis_row.grid(row=5, column=1, columnspan=2, sticky="w", pady=(6, 0))
        ttk.Radiobutton(axis_row, text="X (pan)", variable=target_var, value="x").pack(side="left")
        ttk.Radiobutton(axis_row, text="Y (tilt)", variable=target_var, value="y").pack(side="left", padx=(10, 0))

        def save():
            node_id = resolve_node_id()
            if node_id is None:
                return
            try:
                angle_min = float(angle_min_var.get())
                angle_max = float(angle_max_var.get())
            except (tk.TclError, ValueError):
                messagebox.showerror("Invalid input", "Angle range must be numbers.", parent=self)
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
                target_axis=target_var.get(),
            )
            self.on_saved(mapping)
            self.destroy()

        btns = ttk.Frame(self, padding=(12, 0, 12, 12))
        btns.grid(row=2, column=0, columnspan=2, sticky="e")
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="left", padx=(0, 8))
        ttk.Button(btns, text="Save Mapping", command=save).pack(side="left")


class UploadDialog(tk.Toplevel):
    """Picks a source (loaded CSV or the last live recording), a Node ID (or
    every Node present in the source) and a name — then hands that off to
    the caller, which does the actual streaming. Only each target Node's own
    column is ever sent to it (see App._begin_upload), matching the "each
    Node only gets its own data" requirement."""

    def __init__(self, parent, csv_rows, record_rows, on_confirm):
        super().__init__(parent)
        self.title("Upload sequence to a Node")
        self.resizable(False, False)
        self.csv_rows = csv_rows
        self.record_rows = record_rows
        self.on_confirm = on_confirm

        form = ttk.Frame(self, padding=12)
        form.pack()

        ttk.Label(form, text="Source:").grid(row=0, column=0, sticky="w")
        self.source_var = tk.StringVar()
        source_options = []
        if self.record_rows:
            source_options.append(f"Last recording ({len(self.record_rows)} samples)")
        if self.csv_rows:
            source_options.append(f"Loaded CSV ({len(self.csv_rows)} rows)")
        self.source_combo = ttk.Combobox(form, textvariable=self.source_var, values=source_options, state="readonly", width=28)
        self.source_combo.grid(row=0, column=1, sticky="w", padx=(4, 0))
        self.source_combo.bind("<<ComboboxSelected>>", lambda e: self._refresh_node_choices())

        ttk.Label(form, text="Node ID:").grid(row=1, column=0, sticky="w", pady=(6, 0))
        self.node_var = tk.StringVar()
        self.node_combo = ttk.Combobox(form, textvariable=self.node_var, state="readonly", width=28)
        self.node_combo.grid(row=1, column=1, sticky="w", padx=(4, 0), pady=(6, 0))

        self.all_nodes_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            form, text="Upload to all Nodes present in source", variable=self.all_nodes_var,
            command=self._refresh_node_choices,
        ).grid(row=2, column=0, columnspan=2, sticky="w", pady=(2, 0))

        self.clear_first_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(
            form, text="Clear Node's saved recordings before uploading", variable=self.clear_first_var,
        ).grid(row=3, column=0, columnspan=2, sticky="w", pady=(2, 0))

        ttk.Label(form, text="Sequence name:").grid(row=4, column=0, sticky="w", pady=(6, 0))
        self.name_var = tk.StringVar()
        ttk.Entry(form, textvariable=self.name_var, width=30).grid(row=4, column=1, sticky="w", padx=(4, 0), pady=(6, 0))

        self.hint_var = tk.StringVar(value="")
        ttk.Label(form, textvariable=self.hint_var, foreground="#666", wraplength=320).grid(
            row=5, column=0, columnspan=2, sticky="w", pady=(6, 0)
        )

        btns = ttk.Frame(form)
        btns.grid(row=6, column=0, columnspan=2, sticky="e", pady=(10, 0))
        ttk.Button(btns, text="Cancel", command=self.destroy).pack(side="left", padx=(0, 8))
        ttk.Button(btns, text="Start Upload", command=self._confirm).pack(side="left")

        if source_options:
            self.source_var.set(source_options[0])
            self._refresh_node_choices()
        else:
            self.hint_var.set("Nothing to upload — record something or load a CSV first.")

    def _current_rows(self):
        if self.source_var.get().startswith("Last recording"):
            return self.record_rows
        if self.source_var.get().startswith("Loaded CSV"):
            return self.csv_rows
        return []

    def _refresh_node_choices(self):
        rows = self._current_rows()
        node_ids = sorted({nid for row in rows for ch in (1, 2, 3) for nid in row[ch]})
        self.node_combo["values"] = [str(n) for n in node_ids]
        if node_ids and not self.node_var.get():
            self.node_var.set(str(node_ids[0]))
        self.node_combo.configure(state="disabled" if self.all_nodes_var.get() else "readonly")

        duration_s = (rows[-1][0] / 1000.0) if rows else 0.0
        over = duration_s * 1000 > UPLOAD_MAX_DURATION_MS
        if self.all_nodes_var.get():
            target_desc = (
                f" Will upload to all {len(node_ids)} Node(s) in the source at once, "
                f"each getting only its own column." if node_ids else ""
            )
        else:
            target_desc = " Only the selected Node's own column is sent — the others are never transmitted to it." if node_ids else ""
        light_ids = sorted({nid for row in rows for nid in row[3]})
        light_desc = f" Light track included for Node(s) {', '.join(str(n) for n in light_ids)}." if light_ids else ""
        self.hint_var.set(
            f"{duration_s:.1f}s of motion."
            + light_desc
            + target_desc
            + (f" Longer than the {UPLOAD_MAX_DURATION_MS/1000:.0f}s a Node can hold; it'll be truncated."
               if over else "")
        )

    def _confirm(self):
        rows = self._current_rows()
        if not rows:
            messagebox.showerror("No source", "Pick a source with data first.", parent=self)
            return

        if self.all_nodes_var.get():
            node_ids = sorted({nid for row in rows for ch in (1, 2, 3) for nid in row[ch]})
            if not node_ids:
                messagebox.showerror("No Nodes", "No Node data found in the selected source.", parent=self)
                return
        else:
            try:
                node_ids = [int(self.node_var.get())]
            except (ValueError, TypeError):
                messagebox.showerror("No Node", "Pick a Node ID first.", parent=self)
                return

        name = sanitize_sequence_name(self.name_var.get().strip())
        if not name:
            messagebox.showerror(
                "No name",
                "Enter a sequence name using letters, numbers, '_' or '-' (max 23 characters).",
                parent=self,
            )
            return
        self.on_confirm(rows, node_ids, name, self.clear_first_var.get())
        self.destroy()


class App:
    def __init__(self, root):
        self.root = root
        root.title("Servo Rig — Joystick Bridge")
        root.geometry("720x760")
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

        # node_id -> current light state as this tool believes it. The Master
        # tracks it per target too (see NetworkLink::lastRelayFor), so these
        # stay in step as long as nothing else is switching the same Node.
        self.relay_state = {}
        self.relay_sent = {}   # node_id -> light state actually put on the wire
        self.last_sent = {}  # node_id -> {"x","y"} last commanded, for partial updates

        self.recording = False
        self.record_start_ms = None
        self.record_buffer = []
        self.playback_rows = []
        self._playback_plan = []  # resample_rows(playback_rows, ...) at the speed Play was pressed with
        self.playback_index = 0
        self._playback_job = None

        self._uploads = {}  # node_id -> in-progress upload state dict, concurrently — see _begin_upload
        self._upload_batch = None  # {"total","done","ok","failed"} while a multi-Node upload run is active
        self.space_replies = {}  # node_id -> last-seen free_bytes, from space_reply messages

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

        mapframe = ttk.LabelFrame(self.root, text="Axis / button -> Node mappings", padding=8)
        mapframe.pack(fill="both", expand=False, padx=8, pady=(0, 8))

        self.map_tree = ttk.Treeview(mapframe, columns=("mapping",), show="headings", height=5)
        self.map_tree.heading("mapping", text="Mapping")
        self.map_tree.column("mapping", width=420)
        self.map_tree.pack(side="left", fill="both", expand=True)

        map_btns = ttk.Frame(mapframe)
        map_btns.pack(side="left", padx=(8, 0))
        self.learn_btn = ttk.Button(map_btns, text="Learn Axis / Button…", command=self._start_learn)
        self.learn_btn.pack(fill="x")
        ttk.Button(map_btns, text="Remove Selected", command=self._remove_selected_mapping).pack(fill="x", pady=(4, 0))
        ttk.Button(map_btns, text="Remove All", command=self._remove_all_mappings).pack(fill="x", pady=(4, 0))
        ttk.Separator(map_btns, orient="horizontal").pack(fill="x", pady=6)
        ttk.Button(map_btns, text="Save Mappings…", command=self._save_mappings).pack(fill="x")
        ttk.Button(map_btns, text="Load Mappings…", command=self._load_mappings).pack(fill="x", pady=(4, 0))

        stream = ttk.LabelFrame(self.root, text="Streaming", padding=8)
        stream.pack(fill="x", padx=8, pady=(0, 8))
        self.streaming_var = tk.BooleanVar(value=False)
        self.stream_check = ttk.Checkbutton(
            stream,
            text="Start streaming mapped axes/buttons to their Nodes (~25 Hz)",
            variable=self.streaming_var,
            state="disabled",
        )
        self.stream_check.pack(side="left")
        self.stream_hint_var = tk.StringVar(value="learn at least one axis/button -> Node mapping to enable")
        ttk.Label(stream, textvariable=self.stream_hint_var, foreground="#666").pack(side="left", padx=(10, 0))

        rec = ttk.LabelFrame(self.root, text="Recording / Playback (CSV — angles + lights)", padding=8)
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
        ttk.Button(rec, text="Discard Recording", command=self._discard_recording).grid(
            row=0, column=3, sticky="w", padx=(8, 0)
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

        ttk.Separator(rec, orient="horizontal").grid(row=3, column=0, columnspan=6, sticky="ew", pady=8)
        ttk.Button(rec, text="Upload to Node…", command=self._open_upload_dialog).grid(
            row=4, column=0, columnspan=2, sticky="w"
        )
        self.upload_status_var = tk.StringVar(value="")
        ttk.Label(rec, textvariable=self.upload_status_var, foreground="#666").grid(
            row=4, column=2, columnspan=4, sticky="w"
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
                f"-- auto-bound mapping ({mapping.label()}) to connected controller (was {old_guid}) --"
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

    def _remove_all_mappings(self):
        if not self.mappings:
            messagebox.showinfo("Nothing to remove", "There are no mappings to remove.")
            return
        if not messagebox.askyesno(
            "Remove all mappings",
            f"Remove all {len(self.mappings)} mapping(s)? This can't be undone unless you already "
            "saved them with Save Mappings…",
        ):
            return
        self.mappings = []
        self._reload_mapping_tree()
        self._log("-- all mappings removed --")

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

    def _axis_mappings(self):
        return [m for m in self.mappings if isinstance(m, AxisMapping)]

    def _button_mappings(self):
        return [m for m in self.mappings if isinstance(m, ButtonMapping)]

    def _send_node(self, node_id, x, y, relay_on):
        """One move command carrying both servo axes and that Node's light.

        Everything travels together deliberately: the firmware carries relay
        state on move commands, and both axes in one packet so a Node never
        acts on half a move (see ../docs/serial-protocol.md). Sending X and Y
        separately would make the head dog-leg to every diagonal — and record
        that dog-leg, if the Node is capturing."""
        line = self.link.send({
            "node": node_id,
            "x": round(x, 1),
            "y": round(y, 1),
            "relay": bool(relay_on),
        })
        self.last_sent[node_id] = {"x": x, "y": y}
        return line

    def _last_pos(self, node_id):
        """Where we last put this Node, for filling in an axis we have no
        fresh value for. Falls back to what the Node itself reports, then to
        the centre of a default range."""
        known = self.known_nodes.get(node_id, {})
        remembered = self.last_sent.get(node_id, {})
        return {
            "x": remembered.get("x", known.get("x", DEFAULT_HOLD_ANGLE)),
            "y": remembered.get("y", known.get("y", DEFAULT_HOLD_ANGLE)),
        }

    def _update_streaming_availability(self):
        if self.mappings:
            self.stream_check.configure(state="normal")
            self.stream_hint_var.set(f"{len(self.mappings)} mapping(s) active")
        else:
            self.streaming_var.set(False)
            self.stream_check.configure(state="disabled")
            self.stream_hint_var.set("learn at least one axis/button -> Node mapping to enable")

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
                streaming = self.streaming_var.get() and self.link.is_open

                # Buttons first, so a light switched on this tick goes out with
                # the same command as the motion below rather than a tick later.
                lights = {}
                for m in self._button_mappings():
                    js = self.joysticks.get(m.controller_guid)
                    pressed = None
                    if js is not None:
                        try:
                            pressed = bool(js.get_button(m.button_index))
                        except pygame.error:
                            pressed = None
                    if pressed is None:
                        lights[m.node_id] = self.relay_state.get(m.node_id, False)
                        continue
                    current = self.relay_state.get(m.node_id, False)
                    desired = m.next_state(pressed, current)
                    self.relay_state[m.node_id] = desired
                    lights[m.node_id] = desired

                # Then every mapped stick axis, collected per node so each one
                # gets a single command with both of its axes.
                xs, ys = {}, {}
                moved = set()
                for m in self._axis_mappings():
                    js = self.joysticks.get(m.controller_guid)
                    if js is None:
                        continue  # that mapping's controller isn't connected right now; skip it
                    try:
                        raw = js.get_axis(m.axis_index)
                    except pygame.error:
                        continue
                    angle = m.compute_angle(raw)
                    (xs if m.target_axis == "x" else ys)[m.node_id] = angle
                    if m.last_sent_angle is None or abs(angle - m.last_sent_angle) >= ANGLE_SEND_EPSILON:
                        m.last_sent_angle = angle
                        moved.add(m.node_id)

                if streaming:
                    # A node is (re)sent when one of its axes moved past the
                    # epsilon, or when its light just changed.
                    due = set(moved)
                    for node_id, state in lights.items():
                        if state != self.relay_sent.get(node_id):
                            due.add(node_id)
                    for node_id in sorted(due):
                        pos = self._last_pos(node_id)
                        x = xs.get(node_id, pos["x"])
                        y = ys.get(node_id, pos["y"])
                        relay_on = lights.get(node_id, self.relay_state.get(node_id, False))
                        try:
                            line = self._send_node(node_id, x, y, relay_on)
                            self.relay_sent[node_id] = relay_on
                            self._log(f"-> {line.rstrip()}")
                        except (RuntimeError, serial.SerialException, OSError) as exc:
                            self._log(f"-- send failed: {exc} --")

                if self.recording:
                    elapsed = now_ms - self.record_start_ms
                    self.record_buffer.append((elapsed, dict(xs), dict(ys), dict(lights)))
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
                messagebox.showwarning("No mappings", "Learn at least one axis or button mapping first.")
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

    def _discard_recording(self):
        if self.recording:
            messagebox.showwarning("Recording in progress", "Stop recording before discarding it.")
            return
        if not self.record_buffer:
            messagebox.showinfo("Nothing to discard", "There's no recorded sequence in memory.")
            return
        if not messagebox.askyesno(
            "Discard recording",
            f"Discard the {len(self.record_buffer)}-sample recording in memory? This can't be undone "
            "unless you already saved it with Save Recording As….",
        ):
            return
        self.record_buffer = []
        self.record_status_var.set("not recording")
        self._log("-- recording discarded --")

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
        t_ms, xs, ys, lights = self._playback_plan[self.playback_index]
        # A Node present in any channel gets a command; channels the recording
        # doesn't have for it are filled from where we last put it, so a
        # pan-only or light-only recording holds everything else still.
        node_ids = sorted(set(xs) | set(ys) | set(lights))
        for node_id in node_ids:
            pos = self._last_pos(node_id)
            x = xs.get(node_id, pos["x"])
            y = ys.get(node_id, pos["y"])
            relay_on = lights.get(node_id, self.relay_state.get(node_id, False))
            try:
                line = self._send_node(node_id, x, y, relay_on)
                self._log(f"-> {line.rstrip()}")
            except (RuntimeError, serial.SerialException, OSError) as exc:
                self._log(f"-- playback send failed: {exc} --")
                self._stop_playback()
                return
            if node_id in lights:
                self.relay_state[node_id] = relay_on

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

    # ---------- upload to Node ----------
    def _open_upload_dialog(self):
        if not self.link.is_open:
            messagebox.showwarning("Not connected", "Connect to the Master's serial port first.")
            return
        if self._uploads:
            messagebox.showwarning("Upload in progress", "Wait for the current upload to finish first.")
            return
        if not self.playback_rows and not self.record_buffer:
            messagebox.showwarning("Nothing to upload", "Record something or load a CSV first.")
            return
        UploadDialog(self.root, self.playback_rows, self.record_buffer, self._begin_upload_batch)

    def _begin_upload_batch(self, rows, node_ids, name, clear_first=False):
        # A Node only reacts to ESP-NOW packets addressed to its own id (or a
        # broadcast), so several Nodes' point streams run fully concurrently
        # without interfering with each other. Each Node's own start is
        # staggered by a small offset (see UPLOAD_BATCH_STAGGER_MS) purely so
        # their remote_record_start/stop control packets — and the SEQ_ACKs
        # racing back — don't all land in the very same instant; the
        # streaming itself still overlaps almost completely for any
        # recording longer than a couple of seconds.
        self._upload_batch = {"total": len(node_ids), "done": 0, "ok": 0, "failed": 0}
        for i, node_id in enumerate(node_ids):
            delay_ms = i * UPLOAD_BATCH_STAGGER_MS if len(node_ids) > 1 else 0
            if delay_ms > 0:
                self.root.after(delay_ms, self._begin_upload, rows, node_id, name, clear_first)
            else:
                self._begin_upload(rows, node_id, name, clear_first)

    def _finish_node_upload(self, node_id, ok):
        """Call exactly once when a given Node's upload is fully resolved
        (saved, explicitly failed, or gave up after the ack retry) — removes
        it from the in-flight set and, once every Node in a multi-Node batch
        has resolved, logs/shows a summary line."""
        self._uploads.pop(node_id, None)
        if self._upload_batch is not None:
            self._upload_batch["done"] += 1
            self._upload_batch["ok" if ok else "failed"] += 1
            if self._upload_batch["done"] >= self._upload_batch["total"]:
                total = self._upload_batch["total"]
                ok_count = self._upload_batch["ok"]
                failed_count = self._upload_batch["failed"]
                if total > 1:
                    fail_note = f", {failed_count} failed" if failed_count else ""
                    self._log(f"-- upload batch finished: {ok_count}/{total} Node(s) saved successfully{fail_note} --")
                    self.upload_status_var.set(f"batch upload finished: {ok_count}/{total} succeeded{fail_note}")
                self._upload_batch = None
                return
        self._refresh_upload_status()

    def _set_solo_status(self, node_id, message):
        """If `node_id` is the only upload currently in flight, show
        `message` directly — otherwise it'd get silently replaced by nothing
        once _finish_node_upload removes it from self._uploads, instead of
        the aggregate status reflecting what actually happened."""
        if node_id in self._uploads and len(self._uploads) == 1:
            self.upload_status_var.set(message)

    def _refresh_upload_status(self):
        if not self._uploads:
            return
        if len(self._uploads) == 1:
            (node_id, st), = self._uploads.items()
            if st["phase"] == "waiting_ack":
                self.upload_status_var.set(f"'{st['name']}' sent to Node {node_id} — waiting for confirmation…")
            else:
                self.upload_status_var.set(
                    f"uploading '{st['name']}' to Node {node_id}… {st['index']}/{len(st['plan'])}"
                )
            return
        waiting = sum(1 for st in self._uploads.values() if st["phase"] == "waiting_ack")
        sent = sum(st["index"] for st in self._uploads.values())
        total_points = sum(len(st["plan"]) for st in self._uploads.values())
        names = {st["name"] for st in self._uploads.values()}
        name = next(iter(names)) if len(names) == 1 else "sequences"
        done_note = f", {self._upload_batch['done']}/{self._upload_batch['total']} Node(s) finished" if self._upload_batch else ""
        self.upload_status_var.set(
            f"uploading '{name}' to {len(self._uploads)} Node(s){done_note} — "
            f"{sent}/{total_points} points sent, {waiting} awaiting confirmation"
        )

    def _begin_upload(self, rows, node_id, name, clear_first=False):
        # This Node's own channels: X, Y and light. Any of them can be absent —
        # a stick mapped to pan only, a button with no stick — but if all three
        # are, there's nothing to upload.
        def present(row):
            return node_id in row[1] or node_id in row[2] or node_id in row[3]

        points = [
            (row[0], row[1].get(node_id), row[2].get(node_id), row[3].get(node_id))
            for row in rows
            if present(row)
        ]
        if not points:
            messagebox.showerror("Upload failed", f"No data for Node {node_id} in the selected source.")
            self._finish_node_upload(node_id, ok=False)
            return
        if points[-1][0] > UPLOAD_MAX_DURATION_MS:
            points = [p for p in points if p[0] <= UPLOAD_MAX_DURATION_MS]
            self._log(f"-- upload to Node {node_id} truncated to {UPLOAD_MAX_DURATION_MS / 1000:.0f}s (Node capacity) --")

        # Whatever this Node is holding right now, to fill in any channel the
        # recording doesn't cover for it.
        hold = self._last_pos(node_id)

        # Resample to a dense, single-node point list at real-time pace —
        # the same mechanism CSV playback uses, just targeting one Node and
        # never touching the other columns (only this Node's data is ever
        # transmitted to it).
        single_node_rows = [
            (t,
             {} if x is None else {node_id: x},
             {} if y is None else {node_id: y},
             {} if l is None else {node_id: l})
            for t, x, y, l in points
        ]
        plan = resample_rows(single_node_rows, TICK_INTERVAL_MS, 1.0)

        needed_bytes = estimate_sequence_file_bytes(points[-1][0])
        self._uploads[node_id] = {
            "name": name, "plan": plan, "index": 0, "retried": False, "job": None, "phase": "start",
            "needed_bytes": needed_bytes, "hold": hold,
        }
        self._refresh_upload_status()

        self.space_replies.pop(node_id, None)
        self._log(f"-- checking free space on Node {node_id} (recording needs ~{needed_bytes} bytes) --")
        try:
            self.link.send({"cmd": "space_query", "node": node_id})
        except (RuntimeError, serial.SerialException, OSError) as exc:
            self._log(f"-- space query failed (Node {node_id}): {exc} --")
        self._uploads[node_id]["job"] = self.root.after(
            SPACE_QUERY_TIMEOUT_MS, self._check_space_reply, node_id, clear_first
        )

    def _check_space_reply(self, node_id, clear_first):
        state = self._uploads.get(node_id)
        if state is None:
            return
        needed_bytes = state["needed_bytes"]
        free_bytes = self.space_replies.get(node_id)

        if free_bytes is not None and free_bytes < needed_bytes:
            self._log(
                f"-- upload to Node {node_id} aborted: only {free_bytes} bytes free, recording "
                f"needs ~{needed_bytes} — delete old sequences on it (or check 'Clear Node's saved "
                f"recordings before uploading') and try again --"
            )
            self._set_solo_status(
                node_id, f"Node {node_id}: not enough space ({free_bytes} free, needs ~{needed_bytes})"
            )
            self._finish_node_upload(node_id, ok=False)
            return
        if free_bytes is not None:
            self._log(f"-- Node {node_id} has {free_bytes} bytes free, proceeding --")
        else:
            self._log(
                f"-- no space reply from Node {node_id} within {SPACE_QUERY_TIMEOUT_MS}ms "
                "(dropped packet, most likely) — uploading anyway --"
            )

        if clear_first:
            self._log(f"-- clearing Node {node_id}'s saved recordings before upload --")
            try:
                self.link.send({"cmd": "remote_clear", "node": node_id})
            except (RuntimeError, serial.SerialException, OSError) as exc:
                self._log(f"-- clear request failed (Node {node_id}): {exc} --")
            state["job"] = self.root.after(UPLOAD_CLEAR_SETTLE_MS, self._send_upload_start, node_id)
        else:
            self._send_upload_start(node_id)

    def _send_upload_start(self, node_id):
        state = self._uploads.get(node_id)
        if state is None:
            return
        self._log(f"-- upload starting: Node {node_id} as '{state['name']}' ({len(state['plan'])} points) --")
        try:
            self.link.send({"cmd": "remote_record_start", "node": node_id})
        except (RuntimeError, serial.SerialException, OSError) as exc:
            self._log(f"-- upload to Node {node_id} failed to start: {exc} --")
            self._set_solo_status(node_id, f"upload to Node {node_id} failed to start (send error)")
            self._finish_node_upload(node_id, ok=False)
            return
        state["job"] = self.root.after(
            UPLOAD_START_RESEND_INTERVAL_MS, self._resend_upload_start, node_id, UPLOAD_START_RESENDS - 1
        )

    def _resend_upload_start(self, node_id, remaining):
        state = self._uploads.get(node_id)
        if state is None:
            return
        if remaining > 0:
            try:
                self.link.send({"cmd": "remote_record_start", "node": node_id})
            except (RuntimeError, serial.SerialException, OSError) as exc:
                self._log(f"-- upload start resend failed (Node {node_id}): {exc} --")
            state["job"] = self.root.after(
                UPLOAD_START_RESEND_INTERVAL_MS, self._resend_upload_start, node_id, remaining - 1
            )
        else:
            state["phase"] = "streaming"
            state["job"] = self.root.after(
                UPLOAD_START_RESEND_INTERVAL_MS, self._upload_send_next_point, node_id
            )

    def _upload_send_next_point(self, node_id):
        state = self._uploads.get(node_id)
        if state is None:
            return
        plan = state["plan"]
        idx = state["index"]
        if idx >= len(plan):
            self._finish_upload_stream(node_id)
            return

        t_ms, xs, ys, lights = plan[idx]
        # Same fallback as playback: a channel this recording doesn't have for
        # the Node holds where it already is while the rest is streamed into
        # the Node's own recording.
        x = xs.get(node_id, state["hold"]["x"])
        y = ys.get(node_id, state["hold"]["y"])
        relay_on = lights.get(node_id, False)
        try:
            self._send_node(node_id, x, y, relay_on)
        except (RuntimeError, serial.SerialException, OSError) as exc:
            self._log(f"-- upload send failed, aborting (Node {node_id}): {exc} --")
            self._set_solo_status(node_id, f"upload to Node {node_id} failed (send error)")
            self._finish_node_upload(node_id, ok=False)
            return

        state["index"] = idx + 1
        self._refresh_upload_status()

        next_idx = idx + 1
        delay_ms = max(1, plan[next_idx][0] - t_ms) if next_idx < len(plan) else 1
        state["job"] = self.root.after(delay_ms, self._upload_send_next_point, node_id)

    def _finish_upload_stream(self, node_id):
        state = self._uploads.get(node_id)
        if state is None:
            return
        self._log(f"-- upload stream sent, asking Node {node_id} to save as '{state['name']}' --")
        try:
            self.link.send({"cmd": "remote_record_stop", "node": node_id, "name": state["name"]})
        except (RuntimeError, serial.SerialException, OSError) as exc:
            self._log(f"-- upload save request failed (Node {node_id}): {exc} --")
            self._set_solo_status(node_id, f"upload to Node {node_id} failed (send error)")
            self._finish_node_upload(node_id, ok=False)
            return
        state["phase"] = "waiting_ack"
        self._refresh_upload_status()
        state["job"] = self.root.after(UPLOAD_ACK_TIMEOUT_MS, self._on_upload_ack_timeout, node_id)

    def _on_upload_ack_timeout(self, node_id):
        state = self._uploads.get(node_id)
        if state is None:
            return
        if state["retried"]:
            self._log(
                f"-- no upload confirmation received from Node {node_id} (retried once) — it "
                "may have lost power/reset, moved out of range, or its stop request/reply was "
                "dropped twice in a row; it may not have saved --"
            )
            self._set_solo_status(node_id, f"Node {node_id}: no confirmation received — it may not have saved")
            self._finish_node_upload(node_id, ok=False)
            return
        self._log(f"-- no upload confirmation yet from Node {node_id}, retrying the save request once --")
        state["retried"] = True
        try:
            self.link.send({"cmd": "remote_record_stop", "node": node_id, "name": state["name"]})
        except (RuntimeError, serial.SerialException, OSError):
            pass
        state["job"] = self.root.after(UPLOAD_ACK_TIMEOUT_MS, self._on_upload_ack_timeout, node_id)

    def _on_upload_ack(self, msg):
        node_id = msg.get("node")
        state = self._uploads.get(node_id)
        if state is None or msg.get("name") != state["name"]:
            return  # stray/unrelated ack — ignore rather than clobber an unrelated in-progress upload
        if state["job"] is not None:
            self.root.after_cancel(state["job"])
            state["job"] = None
        ok = bool(msg.get("ok"))
        points = msg.get("points", 0)
        name = msg.get("name")
        reason = msg.get("reason", "")
        if ok:
            self._log(f"-- upload confirmed: Node {node_id} saved '{name}' ({points} points) --")
        else:
            reason_note = f" — {reason}" if reason else ""
            self._log(f"-- upload failed on Node {node_id}: could not save '{name}'{reason_note} --")
        if len(self._uploads) == 1:
            # The only one in flight — its result is the whole picture, so
            # show it directly instead of the multi-Node aggregate.
            self.upload_status_var.set(
                f"'{name}' saved on Node {node_id} ({points} points)" if ok
                else f"Node {node_id} failed to save '{name}'" + (f": {reason}" if reason else "")
            )
        self._finish_node_upload(node_id, ok=ok)

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
        elif isinstance(msg, dict) and msg.get("type") == "upload_result":
            self._on_upload_ack(msg)
        elif isinstance(msg, dict) and msg.get("type") == "space_reply":
            node_id = msg.get("node")
            if node_id is not None and "free_bytes" in msg:
                self.space_replies[node_id] = msg["free_bytes"]

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
        for state in self._uploads.values():
            if state["job"] is not None:
                self.root.after_cancel(state["job"])
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
