"""Check bilingual document pairs, local links and executable examples."""

import re
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
PROJECTS = (".", "core", "bionic-dlopen", "android-jni", "jvm-jni")
LINK = re.compile(r"(?<!!)\[[^\]\n]+\]\(([^)\n]+)\)")
FENCE = re.compile(r"```[^\n]*\n(.*?)```", re.S)


def anchors(text):
    result = set(re.findall(r'<a\s+id="([^"]+)"', text))
    counts = {}
    for heading in re.findall(r"^#{1,6}\s+(.+)$", text, re.M):
        slug = re.sub(r"[^\w\- ]", "", heading.lower()).replace(" ", "-")
        count = counts.get(slug, 0)
        counts[slug] = count + 1
        result.add(slug + (f"-{count}" if count else ""))
    return result


def examples(text):
    # Comments may be translated; commands, options and C initializer values
    # must agree. This is a consistency check, not a proof of prose equivalence.
    return [
        "\n".join(
            line
            for line in body.splitlines()
            if line.strip()
            and not line.lstrip().startswith(("#", "/*", "*", "//"))
        )
        for body in FENCE.findall(text)
    ]


def canonical_links(path):
    def replace(match):
        url = urlsplit(match.group(1))
        if url.scheme or url.netloc or url.path.startswith("/"):
            return match.group(0)
        target = (path.parent / unquote(url.path)).resolve()
        return str(target) + ("#" + url.fragment if url.fragment else "")

    return LINK.sub(replace, path.read_text())


def check_links(path, errors):
    text = path.read_text()
    for link in LINK.findall(FENCE.sub("", text)):
        url = urlsplit(link)
        if url.scheme or url.netloc or url.path.startswith("/"):
            continue
        target = path.parent / unquote(url.path) if url.path else path
        if not target.exists():
            errors.append(f"{path.relative_to(ROOT)}: missing {link}")
        elif url.fragment and target.suffix == ".md":
            if unquote(url.fragment) not in anchors(target.read_text()):
                errors.append(
                    f"{path.relative_to(ROOT)}: missing anchor {link}"
                )


def main():
    errors = []
    count = 0
    for project in PROJECTS:
        base = ROOT / project
        en = {p.name: p for p in (base / "docs/en").glob("*.md")}
        zh = {p.name: p for p in (base / "docs/zh").glob("*.md")}
        if not en or en.keys() != zh.keys():
            errors.append(
                f"{project}: unmatched English/Chinese document names"
            )
        for name in sorted(en.keys() & zh.keys()):
            count += 1
            if examples(en[name].read_text()) != examples(zh[name].read_text()):
                errors.append(f"{project}/{name}: executable examples differ")
            for lang, paths in (("en", en), ("zh", zh)):
                path = paths[name]
                text = path.read_text()
                for sibling in ("en", "zh"):
                    if f"../{sibling}/{name}" not in text:
                        errors.append(f"{path}: missing {sibling} navigation")
                check_links(path, errors)
        check_links(base / "README.md", errors)
        for directory in ("tests/same-thread", "tests/results/same-thread"):
            for path in (base / directory).rglob("*.md"):
                check_links(path, errors)
        if "README.md" in en and canonical_links(
            base / "README.md"
        ) != canonical_links(en["README.md"]):
            errors.append(
                f"{project}: default README must match the English version"
            )
    check_links(ROOT / "android-jni/tests/runtime-sdk/README.md", errors)
    if errors:
        raise SystemExit("\n".join(errors))
    print(f"Documentation check: {count} bilingual pairs; links/examples match")


if __name__ == "__main__":
    main()
