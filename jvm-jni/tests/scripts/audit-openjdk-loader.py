#!/usr/bin/env python3
"""Audit pinned OpenJDK loader sources; this does not test runtime compatibility."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import urllib.parse
import urllib.request

PROJECT = Path(__file__).resolve().parents[2]
COMMENTS = re.compile(
    r"""("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|/\*.*?\*/|//[^\n]*""", re.S
)
JAVA_METHOD = re.compile(
    r"(?P<modifiers>(?:(?:private|public|protected|static|final)\s+)*)native\s+"
    r"(?P<return>\w+)\s+(?P<name>load|load0|unload|find|findEntry|findEntry0)"
    r"\s*\((?P<parameters>[^)]*)\)\s*;"
)
NATIVE_METHOD = re.compile(
    r"JNIEXPORT\s+(?P<return>\w+)\s+JNICALL\s+(?P<name>Java_\w+)"
    r"\s*\((?P<parameters>[^)]*)\)\s*\{"
)


def normalized(text):
    return " ".join(text.split())


def without_comments(text):
    return COMMENTS.sub(lambda match: match[1] or " ", text)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def source_path(output, record):
    return output / "cache" / record["repo"] / record["ref"] / record["path"]


def fetch_source(record, output, offline):
    path = source_path(output, record)
    if not path.is_file():
        if offline:
            raise ValueError("Missing cached source: " + str(path))
        url = (
            "https://raw.githubusercontent.com/"
            + record["repo"]
            + "/"
            + urllib.parse.quote(record["ref"], safe="")
            + "/"
            + record["path"]
        )
        with urllib.request.urlopen(url, timeout=45) as response:
            data = response.read()
        if digest(data) != record["sha256"]:
            raise ValueError("Downloaded source hash mismatch: " + url)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    data = path.read_bytes()
    if digest(data) != record["sha256"]:
        raise ValueError("Cached source hash mismatch: " + str(path))
    return record, without_comments(data.decode("utf-8"))


def body_hash(source, offset):
    # Ignore braces inside literals when locating this C function's end.
    # Mask only the suffix and preserve literal widths for brace scanning.
    suffix = source[offset:]
    masked = re.sub(
        r""""(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*' """,
        lambda m: " " * len(m[0]),
        suffix,
        flags=re.X,
    )
    depth = 1
    for index, char in enumerate(masked):
        depth += (char == "{") - (char == "}")
        if depth == 0:
            tokens = re.findall(r'"(?:\\.|[^"\\])*"|\w+|[^\s]', suffix[:index])
            return digest(json.dumps(tokens).encode())
    raise ValueError("Unterminated native function")


def java_methods(record, source):
    methods = []
    for match in JAVA_METHOD.finditer(source):
        methods.append(
            {
                "file": record["path"],
                **{
                    key: normalized(value)
                    for key, value in match.groupdict().items()
                },
            }
        )
    return methods


def native_methods(source):
    methods = []
    for match in NATIVE_METHOD.finditer(source):
        if not re.search(
            r"_(?:load|load0|unload|find|findEntry|findEntry0)$", match["name"]
        ):
            continue
        methods.append(
            {
                **{
                    key: normalized(value)
                    for key, value in match.groupdict().items()
                },
                "body_tokens_sha256": body_hash(source, match.end()),
            }
        )
    if len(methods) != 3:
        raise ValueError(
            "Expected exactly three load/unload/lookup entry points"
        )
    return methods


def jni_methods(source):
    table = re.search(
        r"struct JNINativeInterface_\s*\{(.*?)\n\};", source, re.S
    )
    if not table:
        raise ValueError("Missing public JNI function table")
    return [
        {
            "return": match[1],
            "name": match[2],
            "parameters": normalized(match[3]),
        }
        for match in re.finditer(
            r"(\w+)\s*\(JNICALL\s*\*(\w+)\)\s*\((.*?)\)\s*;",
            table[1],
            re.S,
        )
    ]


def inspect_snapshot(key, sources):
    baseline, major, repo, ref = key
    result = {
        "baseline": baseline,
        "major": major,
        "repo": repo,
        "ref": ref,
        "java_methods": [],
        "native_methods": [],
        "vm_api": [],
        "fields": [],
        "source_files": [],
    }
    for record, source in sources:
        result["source_files"].append(
            {"path": record["path"], "sha256": record["sha256"]}
        )
        if record["kind"] in ("library_java", "lookup_java"):
            result["java_methods"].extend(java_methods(record, source))
            fields = re.findall(
                r"^\s*(?:(?:private|public|protected|static|final|volatile)\s+)*(?:long\s+handle|int\s+jniVersion|Class<\?>\s+fromClass)\s*[;=]",
                source,
                re.M,
            )
            result["fields"].extend(normalized(field) for field in fields)
        elif record["kind"] == "native_c":
            result["native_methods"] = native_methods(source)
            result["has_jni_onload_and_onunload"] = all(
                name in source for name in ("JNI_OnLoad", "JNI_OnUnload")
            )
        elif record["kind"] == "vm_cpp":
            result["vm_api"] = [
                normalized(match)
                for match in re.findall(
                    r"JVM_(?:ENTRY_NO_ENV|LEAF)\([^\n]*JVM_(?:LoadLibrary|UnloadLibrary|FindLibraryEntry)\([^\n]+",
                    source,
                )
            ]
        elif record["kind"] == "runtime_java":
            result["runtime_has_native_access_gate"] = (
                "Reflection.ensureNativeAccess(caller, Runtime.class" in source
            )
        elif record["kind"] == "jni_header":
            result["jni_methods"] = jni_methods(source)
            result["jni_versions"] = dict(
                re.findall(r"#define\s+(JNI_VERSION_\w+)\s+(\S+)", source)
            )
        elif record["kind"] == "version_config":
            result["version_config"] = dict(
                re.findall(
                    r"^(DEFAULT_VERSION_FEATURE|DEFAULT_VERSION_DATE|DEFAULT_PROMOTED_VERSION_PRE)=(.*)$",
                    source,
                    re.M,
                )
            )
    if not result["native_methods"]:
        raise ValueError("Missing native loader source for " + str(key))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--manifest",
        type=Path,
        default=PROJECT / "tests/config/openjdk-loader-sources.json",
    )
    parser.add_argument(
        "--output", type=Path, default=PROJECT / "build/jre-loader-audit"
    )
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    records = json.loads(args.manifest.read_text())["files"]
    with ThreadPoolExecutor(max_workers=8) as pool:
        sources = list(
            pool.map(
                lambda record: fetch_source(record, args.output, args.offline),
                records,
            )
        )
    groups = {}
    for record, source in sources:
        key = (
            record["baseline"],
            record["major"],
            record["repo"],
            record["ref"],
        )
        groups.setdefault(key, []).append((record, source))
    snapshots = [
        inspect_snapshot(key, group) for key, group in sorted(groups.items())
    ]
    result = {
        "schema_version": 1,
        "kind": "source-audit-only",
        "runtime_tested": False,
        "verified_source_files": len(records),
        "snapshots": snapshots,
    }
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "loader-boundaries.json").write_text(
        json.dumps(result, indent=2) + "\n"
    )
    print(
        f"OpenJDK loader source audit: {len(records)} files, {len(snapshots)} snapshots; runtime_tested=false"
    )


if __name__ == "__main__":
    main()
