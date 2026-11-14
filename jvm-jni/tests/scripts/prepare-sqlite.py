"""Fixed upstream SQLite source, independently verified by the release's C hash."""

import hashlib, json, urllib.request, zipfile
from pathlib import Path

root = Path(__file__).resolve().parents[3]
url = "https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip"
archive = root / "jvm-jni/vendor/sqlite-amalgamation-3500400.zip"
sha = "1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032"
c_sha3 = "9145255e83da6529e70121ee4d7a4c88fe83ca4511da0c9ed13d10842df36782"
archive.parent.mkdir(exist_ok=True)
if not archive.exists():
    urllib.request.urlretrieve(url, str(archive) + ".tmp")
    Path(str(archive) + ".tmp").rename(archive)
assert hashlib.sha256(archive.read_bytes()).hexdigest() == sha, (
    "SQLite archive hash mismatch"
)
out = root / "jvm-jni/build/sqlite"
out.mkdir(parents=True, exist_ok=True)
with zipfile.ZipFile(archive) as z:
    for name in ["sqlite3.c", "sqlite3.h", "sqlite3ext.h"]:
        data = z.read("sqlite-amalgamation-3500400/" + name)
        if name == "sqlite3.c":
            assert hashlib.sha3_256(data).hexdigest() == c_sha3, (
                "SQLite source hash mismatch"
            )
        (out / name).write_bytes(data)
manifest = dict(
    version="3.50.4",
    url=url,
    archive_sha256=sha,
    sqlite3_c_sha3_256=c_sha3,
    source_hash_reference="https://sqlite.org/changes.html#version_3_50_4",
    defines=["SQLITE_THREADSAFE=1"],
    source_modified=False,
)
(out / "source.json").write_text(json.dumps(manifest, indent=2) + "\n")
notice = (
    "SQLite 3.50.4: upstream source unmodified.\n"
    + url
    + "\n\n"
    + (out / "sqlite3.c").read_text().split("*/", 1)[0]
    + "*/\n"
)
(
    root / "jvm-jni/build/tests/android/assets/runtime/sqlite-NOTICE.txt"
).write_text(notice)
print("Verified SQLite 3.50.4 amalgamation")
