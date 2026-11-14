#!/usr/bin/env python3
"""Check JNI name decoding against javac -h output, including DH's failure."""

from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
fixture = """package regression_names;
public class SignatureFixture {
    public static native void _open_utf8(byte[] path, int flags);
    public static native long with_under_score(long value);
    public static native int overloaded_(byte[] value);
    public static native long overloaded_(String value);
    public static native int _overloaded(int[] value);
    public static native long _overloaded(String[] value);
    public static native int unicode_值_(int value);
    public static class _Nested {
        public native int _call(String[][] value);
    }
}
"""
checks = """import dev.glibc.worker.JvmAdapter;
import regression_names.SignatureFixture;
import java.nio.file.*;
import java.util.Objects;
public class SignatureChecks {
    public static void main(String[] args) throws Exception {
        int count = 0;
        for (String row : Files.readAllLines(Paths.get(args[0]))) {
            String[] fields = row.split("\\t");
            String actual = JvmAdapter.nativeSignature(fields[0], SignatureFixture.class);
            if (!fields[1].equals(actual))
                throw new AssertionError(fields[0] + " expected=" + fields[1] + " actual=" + actual);
            count++;
        }
        for (String symbol : new String[] {
            "Java_regression_1names_SignatureFixture_overloaded_1",
            "Java_regression_1names_SignatureFixture__1overloaded",
            "not_a_JNI_symbol"
        }) {
            if (JvmAdapter.nativeSignature(symbol, SignatureFixture.class) != null)
                throw new AssertionError("Expected unresolved short name: " + symbol);
            count++;
        }
        System.out.println("JNI symbol regression: " + count + " cases passed against javac -h");
    }
}
"""
scratch = ROOT / "jvm-jni/build/test-work"
scratch.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="jni-symbols-", dir=scratch) as name:
    work = Path(name)
    source = work / "regression_names/SignatureFixture.java"
    source.parent.mkdir()
    source.write_text(fixture)
    (work / "SignatureChecks.java").write_text(checks)
    headers = work / "headers"
    headers.mkdir()
    classes = work / "classes"
    classes.mkdir()
    helpers = ROOT / "android-jni/src/java/dev/glibc/worker"
    subprocess.run(
        [
            "javac",
            "--release",
            "8",
            "-h",
            str(headers),
            "-d",
            str(classes),
            str(source),
            str(work / "SignatureChecks.java"),
            str(ROOT / "jvm-jni/src/java/dev/glibc/worker/JvmAdapter.java"),
            str(helpers / "GuestImages.java"),
            str(helpers / "BridgeTypes.java"),
        ],
        check=True,
    )
    expected = []
    for header in sorted(headers.glob("*.h")):
        expected += re.findall(
            r"Signature: (\S+)[\s\S]*?JNICALL\s+(Java_\w+)\s*\(",
            header.read_text(),
        )
    assert len(expected) == 8, expected
    table = work / "expected.tsv"
    table.write_text(
        "".join(
            symbol + "\t" + signature + "\n" for signature, symbol in expected
        )
    )
    subprocess.run(
        ["java", "-cp", str(classes), "SignatureChecks", str(table)], check=True
    )
