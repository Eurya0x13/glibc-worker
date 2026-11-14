"""Per-attempt result ownership; failed attempts cannot inherit an old PASS."""

import fcntl
import json
import os
from pathlib import Path
import shutil
import time
import uuid

ARTIFACTS = (
    "summary.json",
    "report.txt",
    "report.json",
    "ready.json",
    "scheduling.json",
    "benchmark.json",
    "payload.json",
    "payload-samples.bin",
    "payload-stages.bin",
    "timeout-report.txt",
    "launcher-probe.log",
    "normal.log",
    "checkjni.log",
    "deployment.log",
    "elf-route.json",
    "run-state.json",
    "failed-summary.json",
)


def atomic_json(path, value):
    temporary = path.with_name("." + path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n")
    os.replace(temporary, path)


class RunEvidence:
    def __init__(self, output, **metadata):
        self.output = Path(output)
        self.state = {
            **metadata,
            "run_id": uuid.uuid4().hex,
            "status": "RUNNING",
            "pid": os.getpid(),
            "started_unix_ns": time.time_ns(),
        }
        self.lock = None
        self.completed = False

    def __enter__(self):
        self.output.mkdir(parents=True, exist_ok=True)
        self.lock = (self.output / ".runner.lock").open("a")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            self.lock.close()
            raise RuntimeError(
                "Another runner owns output: " + str(self.output)
            ) from None
        try:
            prior = [
                self.output / name
                for name in ARTIFACTS
                if (self.output / name).exists()
            ]
            if prior:
                history = self.output / "history" / self.state["run_id"]
                history.mkdir(parents=True)
                for path in prior:
                    shutil.move(path, history / path.name)
                self.state["prior_artifacts"] = str(history)
            self.update()
            return self
        except BaseException:
            self.lock.close()
            raise

    def update(self, **metadata):
        self.state.update(metadata)
        atomic_json(self.output / "run-state.json", self.state)

    def finish(self, summary):
        if summary.get("result") != "PASS":
            raise ValueError("Only fully verified results can finish")
        summary = {**summary, "run_id": self.state["run_id"]}
        self.update(status="PASS", ended_unix_ns=time.time_ns())
        atomic_json(self.output / "summary.json", summary)
        self.completed = True

    def __exit__(self, kind, value, traceback):
        try:
            if kind is not None or not self.completed:
                summary = self.output / "summary.json"
                if summary.exists():
                    os.replace(summary, self.output / "failed-summary.json")
                self.update(
                    status="INTERRUPTED"
                    if kind is KeyboardInterrupt
                    else "FAILED",
                    error=str(value)
                    if kind
                    else "Runner returned without verified completion",
                    ended_unix_ns=time.time_ns(),
                )
                if kind is None:
                    raise RuntimeError(
                        "Runner returned without verified completion"
                    )
        finally:
            self.lock.close()
        return False
