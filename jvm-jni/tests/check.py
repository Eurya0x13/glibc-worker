"""Validate the smoke or functional JNI coverage report from a real Android run."""

import argparse
import json
import re
from pathlib import Path


def verify(
    report,
    group,
    native_jar=None,
    benchmark_jit=False,
    payload_benchmark=None,
    payload_trials=120,
):
    required = [
        "APP_RESULT=PASS",
        "JAVA_ART_RESUMED",
        "JNI_TARGET=Android-JRE-HotSpot",
        (
            "JRE_STAGE=JNI_CreateJavaVM checkjni=off interpreter=off benchmark_jit=1"
            if benchmark_jit
            else "JRE_STAGE=JNI_CreateJavaVM checkjni=on"
        ),
        "JRE_CREATE_RESULT=0",
        "distinct=1",
        "JRE_BRIDGE_RESULT=PASS",
        "JRE_JAVA_RESUMED",
        "JRE_DESTROY_RESULT=0",
        "JRE_VM_ENUM stage=destroyed rc=0 count=0",
        "GUEST_JNI_RESULT=PASS",
        "HOST_JNI_PROXY_RESULT=PASS",
        "Guest live parent/child errno+TLS isolation=1 Host allocator=1",
        "Guest raw thread exit observed=1",
        "SMOKE_TEST_RESULT=PASS group=" + group,
    ]

    if group == "coverage":
        required += [
            "JNI_TEST_SUMMARY checks=142 failures=0 unsupported=0",
            "LOADER_BACKEND_TEST_SUMMARY checks=26 failures=0 abi=1",
            "FULL_JNI_SUITE_RESULT=PASS",
            "LOAD_PROXY installed=1",
            "LOAD_PROXY restored=1",
            "MULTI_TEST sqlite_wal_pthreads_integrity_and_reopen=PASS",
            "MULTI_TEST stale_handle_generation_rejected=PASS",
            "MULTI_TEST register_then_reject_transaction=PASS",
            "MULTI_LIFECYCLE active=0",
            "SQLITE_SHUTDOWN rc=0",
            "ZSTD_TEST_SUMMARY checks=53 failures=0 cycles=2 jar_unmodified=true elf_unmodified=true",
            "GUEST_IMAGE_LIFECYCLE opens="
            + ("18 closes=18" if native_jar else "16 closes=16"),
            "LOADER_BACKEND stopped=1",
        ]
        version_match = re.search(r"JNI_VERSION_TEST (\{[^\n]+\})", report)
        if not version_match:
            raise ValueError("Missing JNI version capability evidence")
        version_check = json.loads(version_match[1])
        if version_check != {
            "capability": 0x000A0000,
            "getenv_current": True,
            "getenv_future_rejected": True,
            "attach_future_rejected": True,
        }:
            raise ValueError("JNI capability negotiation failed")

    if payload_benchmark:
        required.append(
            f"PAYLOAD_BENCH_CHECK rows={payload_trials} buffers=PASS domain=PASS"
        )
        if not re.search(
            r"PAYLOAD_BENCH_SUMMARY mode="
            + re.escape(payload_benchmark)
            + rf" round=\d+ samples={payload_trials} module_unloaded=PASS",
            report,
        ):
            raise ValueError("Missing complete payload benchmark lifecycle")
    elif benchmark_jit:
        required.append("BRIDGE_BENCH_SUMMARY samples=18 module_unloaded=PASS")

    for marker in required:
        if marker not in report:
            raise ValueError("Missing evidence: " + marker)

    for marker in (
        "WARNING in native method",
        "FATAL ERROR in native method",
        "JNI DETECTED ERROR",
        "=FAIL",
    ):
        if marker in report:
            raise ValueError("Runtime diagnostic: " + marker)

    uid = re.search(r"APP_CONTEXT pid=(\d+) uid=(\d+)", report)
    if uid is None or int(uid[2]) < 10000:
        raise ValueError("Missing ordinary application UID")

    slots = {
        int(i): {"name": name, "calls": int(calls)}
        for i, name, calls in re.findall(
            r"JNI_COVERAGE index=(\d+) name=(\S+) calls=(\d+)", report
        )
    }
    if group == "coverage" and len(slots) != 235:
        raise ValueError("Incomplete JNI/VM slot inventory")

    native_result = None
    if native_jar:
        marker = re.search(
            r"NATIVE_JAR_SUMMARY library=(\w+) checks=(\d+) failures=0 cycles=2 "
            r"jar_unmodified=true elf_unmodified=true result=PASS",
            report,
        )
        if not marker or marker[1] != native_jar:
            raise ValueError(
                "Missing selected original native JAR result: " + native_jar
            )
        assertions = re.findall(
            r"NATIVE_JAR_TEST library="
            + native_jar
            + r" name=(\S+) result=PASS",
            report,
        )
        if len(assertions) != int(marker[2]) or len(set(assertions)) != len(
            assertions
        ):
            raise ValueError(
                "Native JAR assertion count or uniqueness mismatch"
            )
        native_result = {
            "library": native_jar,
            "checks": int(marker[2]),
            "cycles": 2,
        }

    result = {
        "group": group,
        "result": "PASS",
        "pid": int(uid[1]),
        "uid": int(uid[2]),
        "java_checks": 142 if group == "coverage" else None,
        "backend_checks": 26 if group == "coverage" else None,
        "slots_touched": sum(item["calls"] > 0 for item in slots.values()),
        "slots_total": len(slots),
        "slot_inventory": slots,
        "coverage_kind": "functional JNI slot reach, not source line or branch coverage",
        "fatal_error_exercised": False,
        "jvm_mode": "benchmark-normal-jit"
        if benchmark_jit
        else "strict-checkjni-interpreter",
    }

    if native_result:
        result["native_jar"] = native_result
    if group == "coverage":
        result["jni_capabilities"] = version_check
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", choices=("smoke", "coverage"), required=True)
    parser.add_argument("--native-jar", choices=("lz4", "snappy", "rocksdb"))
    parser.add_argument("--benchmark-jit", action="store_true")
    parser.add_argument("--payload-benchmark", choices=("host", "guest"))
    parser.add_argument("report", type=Path)
    args = parser.parse_args()
    result = verify(
        args.report.read_text(),
        args.group,
        args.native_jar,
        args.benchmark_jit,
        args.payload_benchmark,
    )
    args.report.with_suffix(".json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    print(args.group + ": PASS")
