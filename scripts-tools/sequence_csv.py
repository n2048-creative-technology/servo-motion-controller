"""Shared recording CSV format + resampling, used by every PC-side tool that
reads or writes a servo-motion-controller recording (joystick_master_gui.py,
combine_recordings.py, play_recording.py).

A recording is a list of rows: [(t_ms, xs, ys, lights), ...] where xs/ys/
lights are dicts keyed by node id. A node only appears in the channels it
actually has data for (see load_csv_rows/save_csv_rows below) — this module
is the single place that format is read and written, so every tool stays in
sync automatically instead of re-implementing (and risking drifting from)
the same parsing/writing logic.

Extracted out of joystick_master_gui.py, which now imports from here instead
of defining its own copies.
"""

import bisect
import csv

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


def channel_timelines(rows):
    """rows -> {1: {node_id: (times, values)}, 2: {...}, 3: {...}} for the
    x/y/light channels (indices 1/2/3), each node's points sorted by time.
    Shared building block for resample_rows and combine_recordings.py."""
    timelines = {}
    for idx in (1, 2, 3):
        node_ids = sorted({nid for row in rows for nid in row[idx]})
        channel = {}
        for nid in node_ids:
            pts = [(row[0], row[idx][nid]) for row in rows if nid in row[idx]]
            channel[nid] = ([p[0] for p in pts], [p[1] for p in pts])
        timelines[idx] = channel
    return timelines


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

    timelines = channel_timelines(rows)

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
