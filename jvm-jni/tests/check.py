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
    status = re.search(r"^APP_RESULT=(\w+)$", report, re.MULTILINE)
    if status and status[1] != "PASS":
        diagnostic = re.search(r"^BLOCKED SIGSYS[^\n]+", report, re.MULTILINE)
        raise ValueError(
            "Android probe result="
            + status[1]
            + (": " + diagnostic[0] if diagnostic else "")
        )
    launch_method = (
        "JLI_Launch" if "JRE_STAGE=JLI_Launch" in report else "JNI_CreateJavaVM"
    )
    required = [
        "APP_RESULT=PASS",
        "JAVA_ART_RESUMED",
        "JNI_TARGET=Android-JRE-HotSpot",
        (
            "JRE_STAGE="
            + launch_method
            + " checkjni=off interpreter=off benchmark_jit=1"
            if benchmark_jit
            else "JRE_STAGE=" + launch_method + " checkjni=on"
        ),
        "JRE_CREATE_RESULT=0",
        "distinct=1",
        "JRE_BRIDGE_RESULT=PASS",
        "LOADER_PROTOCOL version=5 matched=1",
        "JRE_JAVA_RESUMED",
        "JRE_DESTROY_RESULT=0",
        "JRE_VM_ENUM stage=destroyed rc=0 count=0",
        "GUEST_JNI_RESULT=PASS",
        "HOST_JNI_PROXY_RESULT=PASS",
        "Guest live parent/child errno+TLS isolation=1 Host allocator=1",
        "Guest raw thread exit observed=1",
        "SMOKE_TEST_RESULT=PASS group=" + group,
    ]

    artifact_rows = re.findall(
        r"^JRE_ARTIFACT (\{[^\n]+\})$", report, re.MULTILINE
    )
    vm_rows = re.findall(r"^JVM_ARTIFACT (\{[^\n]+\})$", report, re.MULTILINE)
    if len(artifact_rows) != 1 or len(vm_rows) != 1:
        raise ValueError("Missing unique Android JRE artifact identity")
    artifact = json.loads(artifact_rows[0])
    catalog = json.loads(
        (Path(__file__).parent / "config/android-jres.json").read_text()
    )
    pins = [p for p in catalog if p["id"] == artifact.get("id")]
    if len(pins) != 1:
        raise ValueError("Unknown Android JRE artifact identity")
    pin = pins[0]
    if launch_method != (
        "JLI_Launch" if pin["startup"] == "pojav-layout" else "JNI_CreateJavaVM"
    ):
        raise ValueError("Android JRE artifact startup method mismatch")
    if pin["startup"] == "pojav-layout":
        probes = re.findall(
            r"^JRE_EXEC_PROBE (\{[^\n]+\})$", report, re.MULTILINE
        )
        if len(probes) != 1:
            raise ValueError("Missing Android JRE artifact launcher probe")
        probe = json.loads(probes[0])
        direct = probe.get("direct_started")
        direct_exit = probe.get("direct_exit")
        valid_direct = (
            type(direct) is bool
            and type(direct_exit) is int
            and (
                (not direct and direct_exit == -1)
                or (direct and 0 <= direct_exit <= 255)
            )
        )
        if (
            not valid_direct
            or set(probe)
            != {
                "artifact",
                "mode",
                "direct_started",
                "direct_exit",
                "proxy_exit",
            }
            or type(probe.get("proxy_exit")) is not int
            or probe.get("artifact") != pin["id"]
            or probe.get("mode") != "linker64"
            or probe.get("proxy_exit") != 0
        ):
            raise ValueError("Android JRE artifact linker64 proxy failed")
    expected_artifact = {
        "id": pin["id"],
        "abi": pin["abi"],
        "major": pin["major"],
        "archive_sha256": pin["archive_sha256"],
        "libjava_sha256": pin["libjava"]["sha256"],
        "libjvm_sha256": pin["libjvm"]["sha256"],
        "family": pin["family"],
    }
    selected_vm = json.loads(vm_rows[0])
    if (
        any(type(artifact.get(k)) is not int for k in ("major", "family"))
        or type(selected_vm.get("family")) is not int
    ):
        raise ValueError("Android JRE artifact has invalid numeric fields")
    if any(
        type(selected_vm.get(k)) is not bool for k in ("resident", "vm_bound")
    ):
        raise ValueError("Android JRE artifact has invalid VM binding flags")
    if artifact != expected_artifact or selected_vm != {
        "family": pin["family"],
        "libjava_sha256": pin["libjava"]["sha256"],
        "libjvm_sha256": pin["libjvm"]["sha256"],
        "resident": True,
        "vm_bound": True,
    }:
        raise ValueError("Android JRE artifact or selected-VM mismatch")

    runtime_rows = re.findall(
        r"^RUNTIME_LIFECYCLE_TEST (\{[^\n]+\})$", report, re.MULTILINE
    )
    if len(runtime_rows) != 1:
        raise ValueError("Missing unique runtime lifecycle evidence")
    runtime_lifecycle = json.loads(runtime_rows[0])
    if runtime_lifecycle != {
        "result": "PASS",
        "singleton": True,
        "foreign_shutdown_refused": True,
        "stopped": True,
        "restart_refused": True,
    }:
        raise ValueError("Runtime lifecycle contract failed")

    mutex_rows = re.findall(
        r"^MUTEX_CONTRACT (\{[^\n]+\})$", report, re.MULTILINE
    )
    if len(mutex_rows) != 1:
        raise ValueError("Missing unique GNU mutex contract evidence")
    mutex_contract = json.loads(mutex_rows[0])
    expected_mutex = {
        "result": "PASS",
        "policy": "reject",
        "failures": 0,
        "robust_rejected": 12,
        "robust_accepted": 0,
        "owner_dead": 0,
        "owner_timed_out": 0,
    }
    if (
        any(mutex_contract.get(k) != v for k, v in expected_mutex.items())
        or mutex_contract.get("checks", 0) < 100
    ):
        raise ValueError("GNU mutex contract failed: " + str(mutex_contract))

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
            "capability": pin["jni_capability"],
            "getenv_current": True,
            "getenv_future_rejected": True,
            "attach_future_rejected": True,
        }:
            raise ValueError("JNI capability negotiation failed")

    lifecycle = None
    if group == "coverage":
        rows = re.findall(r"^FULL_LIFECYCLE ([^\n]+)$", report, re.MULTILINE)
        if len(rows) != 1:
            raise ValueError("Missing unique bridge lifecycle evidence")
        lifecycle = {
            key: int(value)
            for key, value in re.findall(r"(\w+)=(-?\d+)", rows[0])
        }
        if any(
            lifecycle.get(key) != 0
            for key in ("globals", "weak", "locals", "closures")
        ):
            raise ValueError("Bridge resource leak or error: " + str(lifecycle))
        if "loads" not in lifecycle or lifecycle["loads"] != lifecycle.get(
            "unloads"
        ):
            raise ValueError("Unbalanced module lifecycle: " + str(lifecycle))

    backend_match = re.search(r"BINDING_REGISTRY backend=(c|zig)", report)
    binding_backend = backend_match[1] if backend_match else "unreported"
    capacity_rows = [
        json.loads(row)
        for row in re.findall(r"BINDING_CAPACITY_TEST (\{[^\n]+\})", report)
    ]
    if group == "coverage" and binding_backend == "zig":
        if len(capacity_rows) != 3 or any(
            row["cycle"] != cycle
            or row["methods"] != 1024
            or row["live_closures"] < 1024
            for cycle, row in enumerate(capacity_rows)
        ):
            raise ValueError("Missing complete Zig binding capacity cycles")

    resource_rows = [
        json.loads(row)
        for row in re.findall(r"RESOURCE_CAPACITY_TEST (\{[^\n]+\})", report)
    ]
    if group == "coverage" and binding_backend == "zig":
        if resource_rows != [
            {"cycle": cycle, "classes": 320, "references": 20000}
            for cycle in range(3)
        ]:
            raise ValueError(
                "Missing complete Zig reference/class capacity cycles"
            )

    failure_rows = [
        json.loads(row)
        for row in re.findall(r"AUDIT_FAILURE_TEST (\{[^\n]+\})", report)
    ]
    scope_rows = [
        json.loads(row)
        for row in re.findall(
            r"^AUDIT_SCOPE_TEST (\{[^\n]+\})$", report, re.MULTILINE
        )
    ]
    if group == "coverage" and scope_rows != [
        {
            "result": "PASS",
            "cases": 4,
            "busy_refusals": 4,
            "exception_unwind": True,
            "resources_preserved": True,
        }
    ]:
        raise ValueError("Missing execution-scope and nested-close evidence")
    close_wait_rows = [
        json.loads(row)
        for row in re.findall(
            r"^AUDIT_CLOSE_WAIT_TEST (\{[^\n]+\})$", report, re.MULTILINE
        )
    ]
    if group == "coverage" and close_wait_rows != [
        {
            "result": "PASS",
            "threads": 2,
            "busy_refusals": 2,
            "resources_preserved": True,
        }
    ]:
        raise ValueError("Missing cross-thread close-wait refusal evidence")
    if group == "coverage":
        expected_rows = [
            {"cycle": cycle, "checks": 6, "expected_errors": 0}
            for cycle in range(3)
        ]
        if (
            failure_rows != expected_rows
            or lifecycle.get("errors") != 0
            or lifecycle.get("expected_errors") != 0
        ):
            raise ValueError(
                "Missing or unexpected bridge failure/recovery evidence"
            )

    dynamic_rows = [
        json.loads(row)
        for row in re.findall(
            r"^DYNAMIC_CAPACITY_TEST (\{[^\n]+\})$", report, re.MULTILINE
        )
    ]
    if group == "coverage" and (
        len(dynamic_rows) != 3
        or any(
            row.get("cycle") != cycle
            or row.get("depth") != 32
            or row.get("guest_threads") != 32
            or row.get("threads", 0) <= row.get("initial_contexts", 0)
            or row.get("contexts", 0) < row.get("threads", 0) + 33
            for cycle, row in enumerate(dynamic_rows)
        )
    ):
        raise ValueError("Missing dynamic context/frame growth evidence")
    ownership_rows = [
        json.loads(row)
        for row in re.findall(
            r"^AUDIT_OWNERSHIP_TEST (\{[^\n]+\})$", report, re.MULTILINE
        )
    ]
    if group == "coverage" and ownership_rows != [
        {
            "result": "PASS",
            "handoff": True,
            "dependency_context": True,
            "background_top": True,
            "ambiguous_dependency_rejected": True,
            "mixed_batch_rejected": True,
            "old_owner_unload_preserved_new": True,
            "final_owner_unload_unbound": True,
        }
    ]:
        raise ValueError(
            "Missing native ownership/registration handoff evidence"
        )

    diagnostics_rows = [
        json.loads(row)
        for row in re.findall(
            r"^AUDIT_DIAGNOSTICS_TEST (\{[^\n]+\})$", report, re.MULTILINE
        )
    ]
    if diagnostics_rows and (
        len(diagnostics_rows) != 1
        or diagnostics_rows[0].get("result") != "PASS"
        or diagnostics_rows[0].get("profile") is not True
        or not dynamic_rows
        or diagnostics_rows[0].get("contexts", 0) < dynamic_rows[-1]["contexts"]
    ):
        raise ValueError("Dynamic context diagnostic snapshots failed")

    descriptor_rows = [
        json.loads(row)
        for row in re.findall(r"AUDIT_DESCRIPTOR_TEST (\{[^\n]+\})", report)
    ]
    if group == "coverage" and binding_backend == "zig":
        if descriptor_rows != [
            {"cycle": cycle, "checks": 3} for cycle in range(3)
        ]:
            raise ValueError("Missing complete JNI descriptor boundary checks")

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
        "FATAL ERROR in native method",
        "JNI DETECTED ERROR",
        "=FAIL",
    ):
        if marker in report:
            raise ValueError("Runtime diagnostic: " + marker)

    launcher_jni_baseline = None
    module_capability = None
    module_rows = re.findall(
        r"^JNI_MODULE_TEST (\{[^\n]+\})$", report, re.MULTILINE
    )
    if group == "coverage":
        supported = pin["jni_capability"] >= 0x00090000
        module_capability = {
            "supported": supported,
            "unsupported_exception": not supported,
        }
        parsed = [json.loads(row) for row in module_rows]
        if len(parsed) != 1 or any(
            row != module_capability
            or any(type(value) is not bool for value in row.values())
            for row in parsed
        ):
            raise ValueError("JNI module capability evidence failed")
    diagnostic_report = report
    if pin.get("launcher_jni_baseline"):
        baselines = re.findall(
            r"^JRE_LAUNCHER_BASELINE_BEGIN\n(.*?)\nJRE_LAUNCHER_BASELINE_END exit=0$",
            report,
            re.MULTILINE | re.DOTALL,
        )
        if (
            len(baselines) != 1
            or report.count("JRE_LAUNCHER_BASELINE_BEGIN\n") != 1
            or report.count("JRE_LAUNCHER_BASELINE_END ") != 1
            or baselines[0].count("JRE_LAUNCHER_BASELINE_MAIN\n") != 1
        ):
            raise ValueError("Missing unique JRE launcher CheckJNI baseline")
        before, after = baselines[0].split("JRE_LAUNCHER_BASELINE_MAIN\n")
        if "WARNING in native method" in after:
            raise ValueError("CheckJNI diagnostic after baseline main")
        diagnostic_report = re.sub(
            r"^JRE_LAUNCHER_BASELINE_BEGIN\n.*?\nJRE_LAUNCHER_BASELINE_END exit=0\n",
            "",
            report,
            flags=re.MULTILINE | re.DOTALL,
        )
        if diagnostic_report.count("JRE_JLI_MAIN\n") != 1:
            raise ValueError("Missing unique JRE launcher main boundary")
        prefix, runtime = diagnostic_report.split("JRE_JLI_MAIN\n")
        warnings = r"^WARNING in native method:[^\n]*$"
        expected = re.findall(warnings, before, re.MULTILINE)
        observed = re.findall(warnings, prefix, re.MULTILINE)
        if observed != expected:
            raise ValueError(
                "CheckJNI launcher warnings differ from no-bridge baseline"
            )
        diagnostic_report = (
            re.sub(warnings, "", prefix, flags=re.MULTILINE) + runtime
        )
        launcher_jni_baseline = {
            "before_main_warnings": len(observed),
            "post_main_warnings": 0,
        }
    if "WARNING in native method" in diagnostic_report:
        raise ValueError("Runtime diagnostic: WARNING in native method")

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
        "binding_backend": binding_backend,
        "binding_capacity": capacity_rows,
        "resource_capacity": resource_rows,
        "bridge_lifecycle": lifecycle,
        "runtime_lifecycle": runtime_lifecycle,
        "jre_artifact": artifact,
        "launcher_jni_baseline": launcher_jni_baseline,
        "jni_module_capability": module_capability,
        "descriptor_boundaries": descriptor_rows,
        "failure_boundaries": failure_rows,
        "execution_scopes": scope_rows,
        "close_waits": close_wait_rows,
        "dynamic_capacity": dynamic_rows,
        "registration_ownership": ownership_rows,
        "diagnostics_growth": diagnostics_rows,
        "mutex_contract": mutex_contract,
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
