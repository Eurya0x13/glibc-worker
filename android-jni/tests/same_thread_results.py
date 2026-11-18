"""Summarize compatibility evidence without treating stale runs as current."""

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SUITES = [
    "core/build/tests/guest-async",
    "core/build/tests/guest-async-android",
    "android-jni/build/tests/guest-signal-unwind",
    "android-jni/build/tests/guest-signal-unwind-android",
    *(
        f"core/build/tests/signal-{kind}{platform}"
        for kind in ("aliases", "deferred")
        for platform in ("", "-android")
    ),
    *(
        f"android-jni/build/tests/runtime-sdk{platform}{mode}"
        for platform in ("", "-android")
        for mode in (
            "",
            "-nonlocal-recovery-pending",
            "-nonlocal-recovery-host-mask",
            "-recovery-worker-signals",
            "-recovery-adopted-signals",
        )
    ),
    "android-jni/build/tests/runtime-sdk-android-late",
    "android-jni/build/tests/runtime-sdk-android-late-nonlocal-recovery-pending",
    "jvm-jni/build/tests/signal-profile-android",
]
REMAINING = [
    "Guest signalfd readiness, job control and remaining I/O interruption semantics",
    "Continuation lifetimes beyond live signal scopes and additional Guest JNI targets",
    "Reviewed live VM/ART quiescence, VM signal protocols and late integration acceptance",
]


def inspect_suite(relative):
    directory = ROOT / relative
    summary, manifest = directory / "summary.json", directory / "manifest.json"
    row = {
        "suite": relative,
        "summary": str(summary),
        "manifest": str(manifest),
    }
    if not summary.is_file() or not manifest.is_file():
        return {**row, "result": "MISSING"}

    try:
        result = json.loads(summary.read_text())
        inputs = json.loads(manifest.read_text())["sha256"]
        if not inputs:
            raise ValueError("manifest has no input hashes")

        changed = []
        for name, expected in inputs.items():
            path = Path(name)
            if not path.is_absolute():
                path = ROOT / path
            if not path.is_file():
                changed.append({"path": name, "reason": "missing"})
            else:
                with path.open("rb") as stream:
                    observed = hashlib.file_digest(stream, "sha256").hexdigest()
                if observed != expected:
                    changed.append({"path": name, "reason": "changed"})

        state = "STALE" if changed else "PASS"
        # Some SDK summaries are a list of individually checked cases.
        cases = (
            result
            if isinstance(result, list)
            else result.get("cases", result.get("runs", [result]))
        )
        if (
            (isinstance(result, dict) and result.get("result") != "PASS")
            or not cases
            or any(
                not isinstance(r, dict) or r.get("result") != "PASS"
                for r in cases
            )
        ):
            state = "FAIL"

        return {
            **row,
            "result": state,
            "verified_inputs": len(inputs),
            "changed_inputs": changed,
        }
    except (OSError, ValueError, TypeError, KeyError) as error:
        return {**row, "result": "FAIL", "error": str(error)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=ROOT / "android-jni/build/tests/same-thread-compatibility.json",
    )
    parser.add_argument("--require-current", action="store_true")
    args = parser.parse_args()

    cases = [inspect_suite(path) for path in SUITES]
    report = {
        "full_compatibility": "INCOMPLETE",
        "execution": "native Linux and Android ARM64 shell JRE17; no APK/ART claim",
        "evidence_current": all(row["result"] == "PASS" for row in cases),
        "remaining_acceptance": REMAINING,
        "cases": cases,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(
        json.dumps(
            {
                "report": str(args.output),
                "full_compatibility": "INCOMPLETE",
                "suites": {r["suite"]: r["result"] for r in cases},
            }
        )
    )

    if args.require_current and not report["evidence_current"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
