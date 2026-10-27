#!/usr/bin/env python3
"""Combine several per-node recordings (one file recorded per Node) into one
CSV with every Node's columns side by side, ready for play_recording.py or
upload to the Nodes.

The intended workflow this exists for:
  1. Record each Node's own sequence separately (whatever's convenient at the
     time — master_gui.py/joystick_master_gui.py driving one Node at a time,
     or any CSV in the format sequence_csv.py reads).
  2. Combine them with this script into one CSV covering every Node.
  3. Play that combined CSV from the PC through the Master into all the
     Nodes at once with play_recording.py.

Usage:
    python3 combine_recordings.py node1.csv node2.csv node3.csv -o combined.csv
    python3 combine_recordings.py 3=pan.csv 3=light.csv 5=dance.csv -o combined.csv

A plain `file.csv` contributes whichever Node columns it already has (so you
can also use this to merge multi-node files, not just strictly one-node-each
recordings). An explicit `<node_id>=file.csv` instead *forces* every column
in that file onto the given Node id, overriding whatever node id the file's
own header says — the case this script is mainly for: a sequence recorded as
"Node 1" in one take so the joystick GUI's single physical controller could
drive it, destined for a different Node once combined (e.g. because you only
have one test rig and record every node's part on it in turn).

Each input keeps its own timeline (t_ms starts at 0 in each file); they are
NOT time-shifted or concatenated — row N of the output is "every node at the
time offset recorded in row N of its own source file". If two inputs
genuinely need to play as one continuous take rather than all starting at
t=0 together, splice them yourself first. A column already present in an
earlier input is NOT overwritten by a later one for the same node+channel —
duplicates are rejected with an error instead of silently picking one, since
a silent pick is exactly the kind of mistake that's invisible until a Node
doesn't move the way you expected on stage.
"""

import argparse
import sys

from sequence_csv import load_csv_rows, save_csv_rows


def parse_source(spec):
    """'file.csv' -> (None, 'file.csv'); '3=file.csv' -> (3, 'file.csv')."""
    if "=" in spec:
        node_str, path = spec.split("=", 1)
        try:
            return int(node_str), path
        except ValueError:
            sys.exit(f"bad source '{spec}': '{node_str}' isn't a node id")
    return None, spec


def remap_rows(rows, force_node):
    """If force_node is set, move every node's data in `rows` onto that one
    node id (only makes sense for a source that has exactly one node's data
    — refuses otherwise, since there'd be no single sensible target)."""
    if force_node is None:
        return rows
    node_ids = {nid for row in rows for ch in (1, 2, 3) for nid in row[ch]}
    if len(node_ids) > 1:
        sys.exit(
            f"can't remap to node {force_node}: source has data for multiple "
            f"nodes {sorted(node_ids)} — <id>=file only works on a single-node file"
        )
    remapped = []
    for t_ms, xs, ys, lights in rows:
        remapped.append((
            t_ms,
            {force_node: v for v in xs.values()},
            {force_node: v for v in ys.values()},
            {force_node: v for v in lights.values()},
        ))
    return remapped


def combine(sources):
    """sources: [(force_node_or_None, path), ...]. Returns combined rows.

    Each source's rows are kept on their own timeline (not merged row-by-row
    with other sources, since different takes are rarely the same length or
    sample rate) — they're concatenated as independent (t_ms, xs, ys, lights)
    tuples sharing one output list, each still only carrying its own node(s)'
    data per row. save_csv_rows then lays out one column per node+channel
    across the whole list, so a sparse row (one source's time slice) simply
    leaves other nodes' columns blank for that t_ms, exactly like any other
    node-without-that-channel row.
    """
    seen = set()  # (node_id, channel) already contributed by an earlier source
    combined = []
    for force_node, path in sources:
        try:
            rows = load_csv_rows(path)
        except (OSError, ValueError, IndexError) as exc:
            sys.exit(f"couldn't read {path}: {exc}")
        rows = remap_rows(rows, force_node)

        file_channels = set()
        for _t, xs, ys, lights in rows:
            file_channels.update((nid, "x") for nid in xs)
            file_channels.update((nid, "y") for nid in ys)
            file_channels.update((nid, "light") for nid in lights)
        dupes = file_channels & seen
        if dupes:
            sys.exit(
                f"{path} duplicates channel(s) already supplied by an earlier "
                f"source: {sorted(dupes)} — each node+channel must come from "
                "exactly one input file"
            )
        seen |= file_channels

        if not rows:
            print(f"-- warning: {path} contributed no rows --", file=sys.stderr)
            continue
        combined.extend(rows)
        node_ids = sorted({nid for _t, xs, ys, lights in rows for nid in (*xs, *ys, *lights)})
        print(f"{path}: {len(rows)} rows, node(s) {node_ids}" +
              (f" (forced to {force_node})" if force_node is not None else ""))

    combined.sort(key=lambda row: row[0])
    return combined


def main():
    parser = argparse.ArgumentParser(
        description="Combine per-node recordings into one multi-node CSV.",
    )
    parser.add_argument(
        "sources", nargs="+",
        help="input CSVs; plain 'file.csv' keeps its own node id(s), "
             "'<node_id>=file.csv' forces a single-node file onto that id",
    )
    parser.add_argument("-o", "--output", required=True, help="combined CSV to write")
    args = parser.parse_args()

    sources = [parse_source(s) for s in args.sources]
    rows = combine(sources)
    if not rows:
        sys.exit("nothing to write — every source was empty")

    save_csv_rows(args.output, rows)
    node_ids = sorted({nid for _t, xs, ys, lights in rows for nid in (*xs, *ys, *lights)})
    print(f"wrote {len(rows)} rows, {len(node_ids)} node(s) {node_ids} -> {args.output}")


if __name__ == "__main__":
    main()
