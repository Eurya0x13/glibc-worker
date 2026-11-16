"""Exercise cache deletion boundaries using disposable Git repositories."""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "clean_cache", Path(__file__).resolve().parents[1] / "clean-cache.py"
)
cleaner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cleaner)


class CacheCleanupTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.project = Path(self.temporary.name).resolve()
        subprocess.run(["git", "init", "-q", str(self.project)], check=True)
        (self.project / ".gitignore").write_text("core/work/\ncore/build/\n")
        self.work = self.project / "core/work"
        self.work.mkdir(parents=True)

    def tearDown(self):
        self.temporary.cleanup()

    def cache(self, kind, key):
        path = self.work / f"{kind}-x86_64-{key:016x}"
        (path / "src").mkdir(parents=True)
        (path / "obj").mkdir()
        (path / "patched").touch()
        (path / "src/Makefile").write_text("source\n")
        (path / "obj/Makefile").write_text("objects\n")
        return path

    def publish(self, kind, path):
        runtime = self.project / "core/build" / kind / "x86_64"
        if kind == "domain-runtime":
            runtime /= path.name.rsplit("-", 1)[1]
        runtime.mkdir(parents=True)
        (runtime / "build-root.txt").write_text(str(path) + "\n")
        (runtime / "manifest.json").write_text(json.dumps({"key": path.name}))

    def test_plan_keeps_base_and_latest_without_deleting(self):
        base = self.cache("glibc-build", 1)
        stale = self.cache("same-thread", 2)
        latest = self.cache("same-thread", 3)
        self.publish("glibc-runtime", base)
        self.publish("domain-runtime", latest)
        candidates, keep = cleaner.plan(self.project)
        self.assertEqual(candidates, [stale])
        self.assertIn(base, keep)
        self.assertIn(latest, keep)
        self.assertTrue(stale.is_dir())

    def test_explicit_keep_and_unrecognized_roots(self):
        retained = self.cache("same-thread", 4)
        incomplete = self.cache("same-thread", 5)
        (incomplete / "patched").unlink()
        candidates, keep = cleaner.plan(self.project, extra_keep=[retained])
        self.assertEqual(candidates, [])
        self.assertIn(retained, keep)
        self.assertIn(incomplete, keep)

    def test_retains_requested_published_generations(self):
        generations = [self.cache("same-thread", key) for key in (11, 12, 13)]
        for time, path in enumerate(generations, start=1):
            self.publish("domain-runtime", path)
            manifest = (
                self.project
                / "core/build/domain-runtime/x86_64"
                / path.name.rsplit("-", 1)[1]
                / "manifest.json"
            )
            os.utime(manifest, ns=(time, time))
        candidates, keep = cleaner.plan(self.project, keep_count=2)
        self.assertEqual(candidates, generations[:1])
        self.assertTrue(all(path in keep for path in generations[1:]))

    def test_cli_defaults_to_preview_from_another_directory(self):
        path = self.cache("same-thread", 14)
        script = self.project / "scripts/clean-cache.py"
        script.parent.mkdir()
        shutil.copyfile(cleaner.__file__, script)
        (self.project / "core/CMakeLists.txt").touch()
        report = self.project / "result.json"
        result = subprocess.run(
            [sys.executable, str(script), "--json", str(report)],
            cwd=self.work,
            capture_output=True,
            text=True,
            check=True,
        )
        data = json.loads(report.read_text())
        self.assertEqual(data["mode"], "preview")
        self.assertEqual(data["removed"], [])
        self.assertTrue(data["complete"])
        self.assertEqual(data["remove"][0]["path"], str(path))
        self.assertIn("Nothing deleted", result.stdout)
        self.assertTrue((path / "src/Makefile").is_file())

    def test_refuses_tracked_file(self):
        path = self.cache("same-thread", 6)
        subprocess.run(
            [
                "git",
                "-C",
                str(self.project),
                "add",
                "-f",
                str(path / "src/Makefile"),
            ],
            check=True,
        )
        with self.assertRaisesRegex(ValueError, "tracked"):
            cleaner.validate_candidate(self.project, path)
        self.assertTrue((path / "src/Makefile").is_file())

    def test_refuses_external_root_and_root_symlink(self):
        external = self.project / "external"
        external.mkdir()
        path = self.work / ("same-thread-x86_64-" + "a" * 16)
        path.symlink_to(external, target_is_directory=True)
        with self.assertRaises(ValueError):
            cleaner.validate_candidate(self.project, path)
        with self.assertRaises(ValueError):
            cleaner.validate_candidate(self.project, external)
        self.assertTrue(external.is_dir())

    def test_active_process_retains_cache(self):
        path = self.cache("same-thread", 7)
        child = subprocess.Popen(
            [
                sys.executable,
                "-c",
                "import time; print('ready', flush=True); time.sleep(30)",
            ],
            cwd=path,
            stdout=subprocess.PIPE,
            text=True,
        )
        try:
            self.assertEqual(child.stdout.readline().strip(), "ready")
            with self.assertRaisesRegex(ValueError, "Active processes"):
                cleaner.validate_candidate(self.project, path)
        finally:
            child.terminate()
            child.wait(timeout=5)
            child.stdout.close()
        self.assertTrue(path.is_dir())

    def test_delete_does_not_follow_internal_symlink(self):
        path = self.cache("same-thread", 8)
        sentinel = self.project / "sentinel"
        sentinel.write_text("keep\n")
        (path / "obj/external").symlink_to(sentinel)
        cleaner.remove(self.project, path)
        self.assertFalse(path.exists())
        self.assertEqual(sentinel.read_text(), "keep\n")


if __name__ == "__main__":
    unittest.main()
