# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Sean Snaider

"""Adds (or checks) the licence header on every source file BellyDSP owns.

    uv run --no-project python tools/spdx_headers.py           add the header where it's missing
    uv run --no-project python tools/spdx_headers.py --check   list files without it; exit 1 if any

The header is two lines, in the file's own comment syntax, as the very first lines (after a shebang,
which has to stay first for the OS to run the script):

    SPDX-License-Identifier: AGPL-3.0-or-later
    Copyright (C) 2026 Sean Snaider

SPDX identifiers (https://spdx.dev/learn/handling-license-info/) are the standard machine-readable way
to say which licence a file is under; the full text is LICENSE at the repo root. Only files we wrote get
it: never third_party/ (the submodules), build-deps/ (downloads), generated files, or data (WAVs, JSON,
CSV, fonts, the content/ licences). Files are taken from `git ls-files`, so build output never shows up.
"""
import argparse
import pathlib
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[1]
SPDX = "SPDX-License-Identifier: AGPL-3.0-or-later"
COPYRIGHT = "Copyright (C) 2026 Sean Snaider"

# Comment prefix per file type. Everything else in the repo is data or prose and is left alone.
PREFIX = {
    ".cpp": "//", ".h": "//", ".mm": "//",
    ".py": "#", ".sh": "#", ".cmake": "#", ".yml": "#",
    ".iss": ";",
}
NAMED = {"CMakeLists.txt": "#"}
# Top-level folders whose files we own (third_party/, build-deps/, content/, presets/, resources/,
# docs/ are not code we write, or are data).
OWNED_ROOTS = ("src/", "tests/", "tools/", "prototypes/", "installer/", ".github/")
SKIP = ("tests/fixtures/",)


def owned_files():
    listed = subprocess.run(["git", "ls-files"], cwd=REPO, check=True, capture_output=True, text=True).stdout.splitlines()
    for rel in listed:
        if rel in NAMED:
            yield rel, NAMED[rel]
            continue
        if not rel.startswith(OWNED_ROOTS) or rel.startswith(SKIP):
            continue
        prefix = PREFIX.get(pathlib.PurePosixPath(rel).suffix)
        if prefix:
            yield rel, prefix


def has_header(lines):
    return any(SPDX in line for line in lines[:4])


def with_header(text, prefix):
    lines = text.splitlines(keepends=True)
    header = [f"{prefix} {SPDX}\n", f"{prefix} {COPYRIGHT}\n"]
    shebang = []
    if lines and lines[0].startswith("#!"):
        shebang, lines = lines[:1], lines[1:]
    # A blank line (or, inside a block of shell/CMake comments that continues, an empty comment line)
    # separates the header from what the file already said.
    if lines and lines[0].startswith(prefix) and shebang:
        header.append(f"{prefix}\n")
    elif lines and lines[0].strip():
        header.append("\n")
    return "".join(shebang + header + lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="only report files without the header")
    args = parser.parse_args()

    missing, added = [], []
    for rel, prefix in owned_files():
        path = REPO / rel
        text = path.read_text(encoding="utf-8")
        if has_header(text.splitlines()):
            continue
        if args.check:
            missing.append(rel)
        else:
            path.write_text(with_header(text, prefix), encoding="utf-8")
            added.append(rel)

    if args.check:
        for rel in missing:
            print(f"no licence header: {rel}")
        print(f"{len(missing)} file(s) without the header")
        return 1 if missing else 0
    for rel in added:
        print(f"added: {rel}")
    print(f"added the header to {len(added)} file(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
