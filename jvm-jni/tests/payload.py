"""Validate recorded payload timings against complete per-call binary samples."""

import re
import struct

SIZES = (0, 64, 1024, 16384, 65536, 262144, 1048576, 4194304)
OPERATIONS = (
    "pointer",
    "direct_address",
    "memcpy",
    "array_region",
    "array_elements",
)
THRESHOLD_SIZES = (
    0,
    64,
    1048576,
    1572864,
    2093056,
    2097152,
    2101248,
    2621440,
    3145728,
    4194304,
)


def payload_points(profile):
    if profile == "threshold":
        return THRESHOLD_SIZES, ("array_elements",)
    if profile == "default":
        return SIZES, OPERATIONS
    raise ValueError("Unknown payload profile")


def verify_payload_phases(report, sizes):
    """Untimed case boundaries include all 300+500 calls in each of 3 trials."""
    phases = [
        {key: int(value) for key, value in re.findall(r"(\w+)=(\d+)", line)}
        for line in report.splitlines()
        if line.startswith("PAYLOAD_PHASE ")
    ]

    if len(phases) != 2 * len(sizes):
        raise ValueError("Incomplete threshold phase snapshots")

    result = {}
    for begin, end in zip(phases[::2], phases[1::2], strict=True):
        size = begin["bytes"]
        if (
            size not in sizes
            or size in result
            or end["bytes"] != size
            or begin["stage"] != 0
            or end["stage"] != 1
            or begin["operation"] != 4
            or end["operation"] != 4
            or begin["tid"] != end["tid"]
        ):
            raise ValueError("Invalid threshold phase pairing")

        deltas = {
            key: end[key] - begin[key]
            for key in ("mono_ns", "cpu_ns", "minflt", "majflt")
        }
        if (
            any(value < 0 for value in deltas.values())
            or deltas["mono_ns"] == 0
        ):
            raise ValueError("Nonmonotonic threshold phase snapshots")

        result[size] = {
            **deltas,
            "host_tid": begin["tid"],
            "calls_including_warmup": 2400,
            "minor_faults_per_call": deltas["minflt"] / 2400,
        }

    if set(result) != set(sizes):
        raise ValueError("Missing threshold phase sizes")
    return result


def verify_payload(report, raw, mode, round_index, profile="default"):
    sizes, operations = payload_points(profile)
    rows = []

    for line in report.splitlines():
        if line.startswith("PAYLOAD_BENCH "):
            fields = dict(token.split("=", 1) for token in line.split()[1:])
            rows.append(
                {
                    key: value if key in ("mode", "operation") else int(value)
                    for key, value in fields.items()
                }
            )

    expected = {
        (operation, size, trial)
        for operation in operations
        for size in sizes
        for trial in range(3)
    }

    if (
        len(rows) != len(expected)
        or {(row["operation"], row["bytes"], row["trial"]) for row in rows}
        != expected
        or any(
            row["mode"] != mode
            or row["point"]
            != OPERATIONS.index(row["operation"]) * len(sizes)
            + sizes.index(row["bytes"])
            or row["round"] != round_index
            or row["calls"] != 500
            or row["warmup"] != 300
            or row["validated_calls"] != 800 * (row["trial"] + 1)
            or not 0 <= row["elements_copy_calls"] <= row["validated_calls"]
            or row["wall_ns"] <= 0
            or row["cpu_ns"] <= 0
            or not 0
            <= row["p50_ns"]
            <= row["p95_ns"]
            <= row["p99_ns"]
            <= row["max_ns"]
            or row["max_ns"] == 0
            for row in rows
        )
    ):
        raise ValueError("Incomplete/invalid payload benchmark")

    if raw[:4] != b"GWP1":
        raise ValueError("Invalid payload sample header")

    position = 4
    for row in rows:
        operation, size, trial, count = struct.unpack_from(
            ">iiii", raw, position
        )
        position += 16
        if not 0 <= operation < len(OPERATIONS) or count != 500:
            raise ValueError("Invalid raw payload sample metadata")

        values = struct.unpack_from(">" + "q" * count, raw, position)
        position += count * 8
        ordered = sorted(values)
        # Equal readings are valid for calls below the timer's resolution.
        # Keep them in the distribution and explicitly count them.
        row["zero_ns_calls"] = sum(value == 0 for value in values)

        if (
            OPERATIONS[operation] != row["operation"]
            or size != row["bytes"]
            or trial != row["trial"]
            or count != row["calls"]
            or min(values) < 0
            or any(
                ordered[count * p // 100] != row[f"p{p}_ns"]
                for p in (50, 95, 99)
            )
            or max(values) != row["max_ns"]
            or sum(value >= 1000000 for value in values) != row["over_1ms"]
            or sum(value >= 5000000 for value in values) != row["over_5ms"]
        ):
            raise ValueError("Raw payload samples disagree with report")

    if position != len(raw):
        raise ValueError("Unexpected trailing payload samples")
    return rows
