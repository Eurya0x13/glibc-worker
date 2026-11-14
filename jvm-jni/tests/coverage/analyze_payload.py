#!/usr/bin/env python3
"""Validate optional native phase samples and correlate fully traced calls."""

import argparse
import collections
import json
import re
import sqlite3
import struct
from pathlib import Path


def quantiles(values):
    values = sorted(values)
    return {
        f"p{p}_us": values[len(values) * p // 100] / 1000 for p in (50, 95, 99)
    } | {"max_us": values[-1] / 1000}


def analyze(directory, database=None):
    raw = (directory / "payload-stages.bin").read_bytes()
    if (
        len(raw) < 32
        or struct.unpack_from("<4Q", raw) != (0x47575331, 1, 64, 2400)
        or len(raw) != 32 + 64 * 2400
    ):
        raise ValueError("Incomplete native stages")

    keys = (
        "call",
        "start",
        "get",
        "end",
        "cpu_start",
        "cpu_get",
        "cpu_end",
        "tid",
    )
    rows = [
        dict(zip(keys, struct.unpack_from("<8Q", raw, offset), strict=True))
        for offset in range(32, len(raw), 64)
    ]

    if [r["call"] for r in rows] != list(range(1, 2401)) or len(
        {r["tid"] for r in rows}
    ) != 1:
        raise ValueError("Wrong call order or native caller")
    for row in rows:
        if not (
            row["start"] <= row["get"] <= row["end"]
            and row["cpu_start"] <= row["cpu_get"] <= row["cpu_end"]
        ):
            raise ValueError("Nonmonotonic diagnostic clocks")

    phases = [
        {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", line)}
        for line in (directory / "report.txt").read_text().splitlines()
        if line.startswith("PAYLOAD_PHASE")
        and "operation=4 bytes=4194304 " in line
    ]
    if (
        len(phases) != 2
        or [p["stage"] for p in phases] != [0, 1]
        or phases[0]["tid"] != phases[1]["tid"]
    ):
        raise ValueError("Incomplete Host phase snapshots")

    result = {
        "calls_including_warmup": len(rows),
        "get": quantiles([r["get"] - r["start"] for r in rows]),
        "release": quantiles([r["end"] - r["get"] for r in rows]),
        "host_phase": {
            key: phases[1][key] - phases[0][key]
            for key in ("mono_ns", "cpu_ns", "minflt", "majflt")
        },
        "host_minor_faults_per_call": (
            phases[1]["minflt"] - phases[0]["minflt"]
        )
        / len(rows),
        "scope": "Diagnostic samples include warmups and extra clocks. Use ordinary A/B runs for performance claims; phase CPU/faults cover the whole case including Java loop overhead.",
    }

    if database:
        connection = sqlite3.connect(database)
        offset = connection.execute(
            "select b.clock_value-m.clock_value from clock_snapshot b join clock_snapshot m using(snapshot_id,machine_id) where b.clock_id=6 and m.clock_id=3 limit 1"
        ).fetchone()[0]

        states = {}
        for name, tid in (
            ("host", phases[0]["tid"]),
            ("guest", rows[0]["tid"]),
        ):
            states[name] = connection.execute(
                "select s.ts,s.dur,s.state from thread_state s join thread t using(utid) where t.tid=? and s.dur>0 and s.ts+s.dur>? and s.ts<? order by s.ts",
                (
                    tid,
                    phases[0]["mono_ns"] + offset,
                    phases[1]["mono_ns"] + offset,
                ),
            ).fetchall()

        traced = []
        for row in sorted(
            rows, key=lambda r: r["end"] - r["start"], reverse=True
        ):
            begin, end = row["start"] + offset, row["end"] + offset
            item = {"call": row["call"], "wall_us": (end - begin) / 1000}
            for name, events in states.items():
                durations = collections.Counter()
                for ts, duration, state in events:
                    overlap = max(0, min(end, ts + duration) - max(begin, ts))
                    durations[state] += overlap
                if abs(sum(durations.values()) - (end - begin)) > 1000:
                    break  # Retain only complete intervals, never fill missing trace time.
                item[name + "_state_us"] = {
                    key: value / 1000
                    for key, value in durations.items()
                    if value
                }
            else:
                traced.append(item)
                if len(traced) == 10:
                    break

        if not traced:
            raise ValueError("No fully traced calls")
        result["slowest_fully_traced_calls"] = traced
        connection.close()

    (directory / "stages.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", type=Path, required=True)
    parser.add_argument("--trace-sqlite", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.run, args.trace_sqlite), indent=2))


if __name__ == "__main__":
    main()
