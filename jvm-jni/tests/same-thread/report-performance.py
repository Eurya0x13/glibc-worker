"""Validate a complete paired capture and archive its raw evidence and report."""

import argparse
import gzip
import hashlib
import importlib.util
import json
import re
import statistics
from pathlib import Path

source = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "bridge_performance", source / "performance.py"
)
performance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(performance)
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("capture", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
capture, output = args.capture.resolve(), args.output.resolve()
manifest = json.loads((capture / "manifest.json").read_text())
summary = json.loads((capture / "summary.json").read_text())
runs = summary["runs"]
expected = {
    (t, p, b)
    for t in manifest["threads"]
    for p in range(1, manifest["pairs"] + 1)
    for b in ("worker", "domain")
}
actual = {
    (r["threads"], r["pair"], r["backend"])
    for r in runs
    if r["backend"] != "direct"
}
if expected != actual or len(runs) != len(expected) + 3 * len(
    manifest["threads"]
):
    parser.error("Incomplete capture: do not publish partial A/B as final")
comparisons = performance.analyze(runs)
if comparisons != summary["comparisons"]:
    parser.error("Summary does not reproduce from per-run metrics")
for r in runs:
    path = capture / f"t{r['threads']}-p{r['pair']:02d}-{r['backend']}.txt.gz"
    with gzip.open(path, "rt") as file:
        text = file.read()
    if (
        'SYSTEM_BENCH_RESULT {"result":"PASS"' not in text
        or "APP_DOMAIN_EXIT=0" not in text
    ):
        parser.error(f"Missing successful raw evidence: {path}")

output.mkdir(parents=True, exist_ok=True)
# Keep logs compact and omit unrelated device identifiers from the shareable
# archive. Unfiltered getprop remains only in the ignored local capture.
files = {}
properties = {
    "ro.product.model",
    "ro.product.manufacturer",
    "ro.build.fingerprint",
    "ro.build.version.sdk",
    "ro.product.cpu.abi",
}
for path in capture.iterdir():
    if not path.is_file():
        continue
    if path.suffix == ".gz":
        with gzip.open(path, "rt") as file:
            files[path.stem] = file.read()
    elif path.name == "device.txt":
        files[path.name] = (
            "\n".join(
                line
                for line in path.read_text().splitlines()
                if line.split("]: ", 1)[0].lstrip("[") in properties
            )
            + "\n"
        )
    else:
        files[path.name] = path.read_text()
with gzip.open(output / "capture.json.gz", "wt") as file:
    json.dump(files, file)
(output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
(output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

thermal = "\n".join(path.read_text() for path in capture.glob("*-device.txt"))
battery = [int(x) / 10 for x in re.findall(r"temperature: (\d+)", thermal)]
statuses = sorted(
    {int(x) for x in re.findall(r"Thermal Status: (\d+)", thermal)}
)
report = [
    "# Paired bridge performance results",
    "",
    "Android ARM64 Vivo V2239A / Android 13, pinned Android JRE17; ART, HotSpot, bionic and glibc in one application process.",
    f"Capture: `{capture.name}`. Same APK SHA256 `{manifest['apk_sha256']}`.",
    f"{manifest['pairs']} alternating worker/domain pairs per concurrency level, plus three ordinary bionic JNI reference runs each.",
    f"All {len(runs)} runs passed value/checksum, loader selection and unload/VM-destruction checks.",
    f"Battery temperature {min(battery):.1f}–{max(battery):.1f} °C; observed Android thermal statuses: {statuses}.",
    "Status 0 at boundaries does not prove fixed CPU frequencies or exclude transient scheduling noise.",
    "",
    "## Amortized native invocation time",
    "",
    "Medians of per-run batch medians, µs/call. Reduction is the median of paired ratios, so it need not equal the ratio of displayed medians.",
    "",
    "| Threads | Case | Worker | Same thread | Paired reduction | Faster pairs | Bionic reference |",
    "| ---: | --- | ---: | ---: | ---: | ---: | ---: |",
]
for row in comparisons:
    v = row["batch_median_ns_per_call"]
    direct = row["direct_reference"]["batch_median_ns_per_call"]
    report.append(
        f"| {row['threads']} | {row['case']} | {v['worker'] / 1000:.3f} | {v['domain'] / 1000:.3f} | {v['paired_median_reduction_pct']:.1f}% | {v['domain_lower_pairs']}/{len(row['pairs'])} | {direct / 1000:.3f} |"
    )
report += [
    "",
    "## CPU, observed long tails and scheduling",
    "",
    "Process CPU µs/completed call includes all Host/Guest/VM/ART threads during the concurrent batch window. P99 is measured in a separate individually clocked-call phase; it includes clock overhead and is not batch-mean P99.",
    "",
    "| Threads | Case | CPU worker → same thread | CPU reduction | P99 worker → same thread (µs) | Voluntary switches/call worker → same thread |",
    "| ---: | --- | ---: | ---: | ---: | ---: |",
]
for row in comparisons:
    cpu, tail = row["cpu_ns_per_call"], row["call_p99_ns"]
    switches = {
        b: statistics.median(
            r["cases"][row["case"]]["voluntary_per_call"]
            for r in runs
            if r["threads"] == row["threads"] and r["backend"] == b
        )
        for b in ("worker", "domain")
    }
    report.append(
        f"| {row['threads']} | {row['case']} | {cpu['worker'] / 1000:.3f} → {cpu['domain'] / 1000:.3f} | {cpu['paired_median_reduction_pct']:.1f}% | {tail['worker'] / 1000:.3f} → {tail['domain'] / 1000:.3f} | {switches['worker']:.3f} → {switches['domain']:.3f} |"
    )
tail_exceptions = [
    f"{row['threads']} thread(s), {row['case']}: "
    f"{row['call_p99_ns']['domain_lower_pairs']}/{len(row['pairs'])} lower-P99 pairs"
    for row in comparisons
    if row["call_p99_ns"]["domain_lower_pairs"] != len(row["pairs"])
]
report += [
    "",
    "Long-tail consistency: "
    + (
        "; ".join(tail_exceptions)
        + ". All other cases improved P99 in every pair. These exceptions are retained; their cause was not established."
        if tail_exceptions
        else "P99 improved in every pair for every case."
    ),
]
report += [
    "",
    "## Interpretation and limits",
    "",
    "The result applies to warmed synchronous System.load native calls and these exact artifacts. It does not measure Pojav/FPS, Guest-created asynchronous threads, startup cost, arbitrary JREs, or independent Host/Guest signal virtualization.",
    "The bionic reference uses the same C source with a different compiler/libc. It is a reference, not an exact subtraction isolating bridge instructions. Process CPU includes background work; it is not a sampled bridge-only CPU profile or a power measurement.",
    "No affinity, spin-budget or production-default changes were made. Fixed warmup, fresh processes, AB/BA ordering, original outliers, temperature snapshots, native/APK hashes and all raw samples are retained. The pilot is excluded from this formal result. capture.json.gz preserves unedited run output and temperature snapshots; general device properties are restricted to build/model/ABI metadata.",
    "See [PERFORMANCE.md](../../PERFORMANCE.md) for reproduction, JVM flags, timing boundaries and instrumentation limitations.",
    "",
]
(output / "REPORT.md").write_text("\n".join(report))
(output / "sha256.json").write_text(
    json.dumps(
        {
            str(path.relative_to(output)): hashlib.sha256(
                path.read_bytes()
            ).hexdigest()
            for path in sorted(output.rglob("*"))
            if path.is_file() and path.name != "sha256.json"
        },
        indent=2,
    )
    + "\n"
)
print(output / "REPORT.md")
