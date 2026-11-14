"""Check artifact attestation failures against an otherwise valid Android report."""

import argparse
import json
from pathlib import Path
import re
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from check import verify

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--group", choices=("smoke", "coverage"), required=True)
parser.add_argument("report", type=Path)
args = parser.parse_args()
report = args.report.read_text()
verify(report, args.group)
cases = {}
prefixes = ["JRE_ARTIFACT", "JVM_ARTIFACT"]
if "JRE_EXEC_PROBE " in report:
    prefixes.append("JRE_EXEC_PROBE")
for prefix in prefixes:
    row = re.search(r"^" + prefix + r" (\{[^\n]+\})$", report, re.MULTILINE)
    assert row
    cases[prefix + "_missing"] = report[: row.start()] + report[row.end() :]
    cases[prefix + "_duplicate"] = report + "\n" + row[0] + "\n"
    original = json.loads(row[1])
    for field in original:
        changed = dict(original)
        value = original[field]
        changed[field] = (
            not value
            if isinstance(value, bool)
            else 999
            if isinstance(value, int)
            else "unknown"
        )
        cases[prefix + "_" + field] = (
            report[: row.start()]
            + prefix
            + " "
            + json.dumps(changed)
            + report[row.end() :]
        )
for name, invalid in cases.items():
    try:
        verify(invalid, args.group)
    except ValueError as error:
        assert "artifact" in str(error) or "selected-VM" in str(error)
    else:
        raise AssertionError("Invalid JRE evidence accepted: " + name)
baseline_cases = {}
if "JRE_LAUNCHER_BASELINE_BEGIN\n" in report:
    block = re.search(
        r"^JRE_LAUNCHER_BASELINE_BEGIN\n.*?\nJRE_LAUNCHER_BASELINE_END exit=0\n",
        report,
        re.MULTILINE | re.DOTALL,
    )
    assert block
    baseline_cases = {
        "missing": report[: block.start()] + report[block.end() :],
        "duplicate": report + block[0],
        "nested_begin": report.replace(
            "JRE_LAUNCHER_BASELINE_BEGIN\n",
            "JRE_LAUNCHER_BASELINE_BEGIN\nJRE_LAUNCHER_BASELINE_BEGIN\n",
        ),
        "failed_exit": report.replace(
            "BASELINE_END exit=0", "BASELINE_END exit=1"
        ),
        "missing_main": report.replace("JRE_LAUNCHER_BASELINE_MAIN\n", ""),
        "baseline_after_main": report.replace(
            "JRE_LAUNCHER_BASELINE_MAIN\n",
            "JRE_LAUNCHER_BASELINE_MAIN\nWARNING in native method: unexpected\n",
        ),
        "unmatched_launcher_warning": report.replace(
            "JRE_JLI_MAIN\n",
            "WARNING in native method: unexpected\nJRE_JLI_MAIN\n",
        ),
        "runtime_warning": report.replace(
            "JRE_JLI_MAIN\n",
            "JRE_JLI_MAIN\nWARNING in native method: unexpected\n",
        ),
        "fatal_in_baseline": report.replace(
            "JRE_LAUNCHER_BASELINE_BEGIN\n",
            "JRE_LAUNCHER_BASELINE_BEGIN\nFATAL ERROR in native method\n",
        ),
    }
    for name, invalid in baseline_cases.items():
        try:
            verify(invalid, args.group)
        except ValueError as error:
            assert any(
                word in str(error)
                for word in ("baseline", "CheckJNI", "Runtime diagnostic")
            )
        else:
            raise AssertionError("Invalid launcher baseline accepted: " + name)
module_cases = {}
if args.group == "coverage" and "JNI_MODULE_TEST " in report:
    pattern = r"^JNI_MODULE_TEST (\{[^\n]+\})$"
    modules = re.findall(pattern, report, re.MULTILINE)
    assert len(modules) == 1
    module_cases["missing"] = report.replace(
        "JNI_MODULE_TEST " + modules[0] + "\n", "", 1
    )
    for field in ("supported", "unsupported_exception"):
        original = json.loads(modules[0])
        original[field] = not original[field]
        module_cases[field] = report.replace(
            modules[0], json.dumps(original), 1
        )
    for name, invalid in module_cases.items():
        try:
            verify(invalid, args.group)
        except ValueError as error:
            assert "module" in str(error)
        else:
            raise AssertionError("Invalid module capability accepted: " + name)
print(
    json.dumps(
        {
            "result": "PASS",
            "negative_cases": list(cases),
            "baseline_negative_cases": list(baseline_cases),
            "module_negative_cases": list(module_cases),
        }
    )
)
