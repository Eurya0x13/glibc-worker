#!/usr/bin/env python3
"""Summarize scheduling during the six eight-thread diagnostic benchmark trials.

Windows are inferred from each Host thread's long start-latch sleep and final
scheduler state, not explicit timing markers. They include thread cleanup and
must not be substituted for the unprofiled Java latency measurements.
"""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

METADATA = """
WITH target AS (
  SELECT t.utid,t.tid,t.upid,t.start_ts,
    (SELECT MAX(s.ts+MAX(s.dur,0)) FROM thread_state s WHERE s.utid=t.utid) AS end_ts,
    (SELECT s.ts+s.dur FROM thread_state s WHERE s.utid=t.utid AND s.state='S' AND s.dur>0 ORDER BY s.dur DESC LIMIT 1) AS release_ts,
    (SELECT MAX(s.dur) FROM thread_state s WHERE s.utid=t.utid AND s.state='S') AS latch_ns
  FROM thread t WHERE t.name GLOB 'bridge-bench-*'
), gaps AS (
  SELECT *,start_ts-LAG(start_ts) OVER (ORDER BY start_ts) AS gap FROM target
), clustered AS (
  SELECT *,SUM(CASE WHEN gap IS NULL OR gap>100000000 THEN 1 ELSE 0 END) OVER (ORDER BY start_ts) AS group_id FROM gaps
), groups AS (
  SELECT group_id,COUNT(*) AS threads,MIN(release_ts) AS ts,MAX(end_ts) AS end_ts,MIN(latch_ns) AS min_latch_ns FROM clustered GROUP BY group_id
)
SELECT json_object(
 'groups',(SELECT json_group_array(json_object('id',group_id,'threads',threads,'ts',ts,'end_ts',end_ts,'min_latch_ns',min_latch_ns)) FROM groups),
 'threads',(SELECT json_group_array(json_object('utid',utid,'tid',tid,'upid',upid,'group_id',group_id)) FROM clustered),
 'health',(SELECT json_group_array(json_object('name',name,'value',value)) FROM stats WHERE severity='error' AND value>0)
) AS payload;
"""


def query(processor, trace, sql, path):
    path.write_text(sql)
    run = subprocess.run(
        [
            str(processor),
            "query",
            "--format",
            "csv",
            "-f",
            str(path),
            str(trace),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=120,
    )
    path.with_suffix(".log").write_text(run.stderr)
    run.check_returncode()
    lines = run.stdout.splitlines()
    if len(lines) != 2 or lines[0] != '"payload"':
        raise ValueError("Expected one JSON payload from trace processor")
    # The CLI quotes TEXT cells but does not CSV-escape embedded JSON quotes.
    return json.loads(lines[1][1:-1])


def phase_sql(group, hosts):
    begin, end = int(group["ts"]), int(group["end_ts"])
    ids = ",".join(str(int(t["utid"])) for t in hosts)
    return f"""
WITH wakers AS (
 SELECT s.utid AS host,s.waker_utid,COUNT(*) AS wakes,
  ROW_NUMBER() OVER (PARTITION BY s.utid ORDER BY COUNT(*) DESC) AS rank
 FROM thread_state s JOIN thread h ON h.utid=s.utid JOIN thread p ON p.utid=s.waker_utid
 WHERE s.utid IN ({ids}) AND p.upid=h.upid AND s.state='R'
  AND s.ts>={begin} AND s.ts<{end} AND s.waker_utid NOT IN ({ids})
 GROUP BY s.utid,s.waker_utid
), participants AS (
 SELECT utid,'host' AS role FROM thread WHERE utid IN ({ids})
 UNION SELECT waker_utid,'peer' FROM wakers WHERE rank=1
), clipped AS (
 SELECT p.role,s.state,MAX(0,MIN(s.ts+s.dur,{end})-MAX(s.ts,{begin})) AS dur
 FROM participants p JOIN thread_state s USING(utid)
 WHERE s.dur>0 AND s.ts<{end} AND s.ts+s.dur>{begin}
), states AS (
 SELECT role,state,COUNT(*) AS intervals,SUM(dur) AS ns,MAX(dur) AS max_ns FROM clipped GROUP BY role,state
), cpus AS (
 SELECT s.cpu,SUM(MAX(0,MIN(s.ts+s.dur,{end})-MAX(s.ts,{begin}))) AS ns
 FROM sched s WHERE s.utid!=0 AND s.dur>0 AND s.ts<{end} AND s.ts+s.dur>{begin} GROUP BY s.cpu
), frequency AS (
 SELECT t.cpu,c.ts,c.value,LEAD(c.ts,1,{end}) OVER(PARTITION BY c.track_id ORDER BY c.ts) AS next_ts
 FROM counter c JOIN cpu_counter_track t ON t.id=c.track_id WHERE t.name='cpufreq'
), freq AS (
 SELECT cpu,SUM(value*MAX(0,MIN(next_ts,{end})-MAX(ts,{begin})))/SUM(MAX(0,MIN(next_ts,{end})-MAX(ts,{begin}))) AS khz
 FROM frequency WHERE ts<{end} AND next_ts>{begin} GROUP BY cpu
)
SELECT json_object(
 'peers',(SELECT json_group_array(json_object('host',host,'peer',waker_utid,'wakes',wakes)) FROM wakers WHERE rank=1),
 'states',(SELECT json_group_array(json_object('role',role,'state',state,'intervals',intervals,'ns',ns,'max_ns',max_ns)) FROM states),
 'cpu_busy_ns',(SELECT json_group_array(json_object('cpu',cpu,'ns',ns)) FROM cpus),
 'frequency_time_weighted_khz',(SELECT json_group_array(json_object('cpu',cpu,'khz',khz)) FROM freq)
) AS payload;
"""


def analyze(args, output):
    metadata = query(
        args.processor, args.trace, METADATA, output / "metadata.sql"
    )
    expected = [1] * 6 + [4] * 6 + [8] * 6
    if [g["threads"] for g in metadata["groups"]] != expected:
        raise ValueError(
            "Incomplete benchmark groups; inspect metadata.sql output"
        )
    phases = []
    for i, group in enumerate(metadata["groups"]):
        if group["threads"] != 8:
            continue
        if group["min_latch_ns"] < 500000000 or group["end_ts"] <= group["ts"]:
            raise ValueError("Unreliable inferred measurement window")
        hosts = [t for t in metadata["threads"] if t["group_id"] == group["id"]]
        values = query(
            args.processor,
            args.trace,
            phase_sql(group, hosts),
            output / f"phase-{i}.sql",
        )
        values.update(
            {"window": group, "callback": i >= 15, "sample": (i - 12) % 3}
        )
        values["peer_mapping_complete"] = (
            len(values["peers"])
            == len({p["peer"] for p in values["peers"]})
            == 8
        )
        calls = 16000
        values["state_us_per_call"] = {
            row["role"] + ":" + row["state"]: row["ns"] / calls / 1000
            for row in values["states"]
        }
        phases.append(values)
    return {
        "trace_sha256": hashlib.sha256(args.trace.read_bytes()).hexdigest(),
        "processor_version": subprocess.check_output(
            [str(args.processor), "--version"], text=True
        ).strip(),
        "health": metadata["health"],
        "phases": phases,
        "limits": [
            "Six inferred windows include native-thread cleanup; not exact Java timing intervals.",
            "Peers are inferred from each Host's most frequent same-process waker; peer_mapping_complete must hold before interpreting peer totals.",
            "Runnable time is measured from scheduler states, not wall time minus CPU. Values are means, not latency percentiles.",
            "All-process CPU busy includes recording, UI and other work; it is not solely external system load.",
            "Frequency is time-weighted over available counters per CPU, not weighted by app execution time.",
            "One profiled run per variant cannot identify the cause of the previous unprofiled run's variation.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--processor", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if any(output.iterdir()):
        parser.error("Choose a fresh output directory")
    result = analyze(args, output)
    (output / "summary.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Scheduling analysis: " + str(output / "summary.json"))


if __name__ == "__main__":
    main()
