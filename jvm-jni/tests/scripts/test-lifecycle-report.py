"""Reject missing, duplicated or failed lifecycle evidence in a real report."""

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
pattern = r"^RUNTIME_LIFECYCLE_TEST (\{[^\n]+\})$"
row = re.search(pattern, report, re.MULTILINE)
assert row
cases = {
    "missing": report[: row.start()] + report[row.end() :],
    "duplicate": report + "\n" + row[0] + "\n",
}
for field in (
    "singleton",
    "foreign_shutdown_refused",
    "stopped",
    "restart_refused",
):
    data = json.loads(row[1])
    data[field] = False
    cases[field] = (
        report[: row.start()]
        + "RUNTIME_LIFECYCLE_TEST "
        + json.dumps(data)
        + report[row.end() :]
    )
for name, changed in cases.items():
    try:
        verify(changed, args.group)
    except ValueError as error:
        assert "lifecycle" in str(error)
    else:
        raise AssertionError("Invalid runtime lifecycle accepted: " + name)
print(json.dumps({"result": "PASS", "negative_cases": list(cases)}))
