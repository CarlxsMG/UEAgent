"""
check_copyright.py -- every C++ and C# file under Source/ starts with the copyright notice.

Usage:
  python Tools/check_copyright.py           # list the files without it; fails if there is one
  python Tools/check_copyright.py --write   # add it to them

Fab refuses a code plugin when a single source or header file lacks a notice with the publisher's
name and the year, so CI runs this check. The generators put NOTICE at the top of the files they
write; package_release.py --fab runs the same check on the archive it builds.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NOTICE = "// Copyright (c) 2026 CarlxsMG."
SUFFIXES = {".h", ".hpp", ".inl", ".c", ".cpp", ".cs"}
BOM = b"\xef\xbb\xbf"


def source_files(root: Path = ROOT) -> list[Path]:
    return sorted(p for p in (root / "Source").rglob("*") if p.suffix in SUFFIXES and p.is_file())


def has_notice(path: Path) -> bool:
    first_line = path.read_bytes().removeprefix(BOM).split(b"\n", 1)[0].rstrip(b"\r")
    return first_line == NOTICE.encode("ascii")


def add_notice(path: Path) -> None:
    """Puts NOTICE and a blank line above the first line, with the file's own line ending."""
    data = path.read_bytes()
    bom = BOM if data.startswith(BOM) else b""
    body = data[len(bom):]
    newline = body.find(b"\n")
    eol = b"\r\n" if newline > 0 and body[newline - 1:newline] == b"\r" else b"\n"
    path.write_bytes(bom + NOTICE.encode("ascii") + eol + eol + body)


def missing_notice(root: Path = ROOT) -> tuple[int, list[Path]]:
    """How many source files there are under root, and the ones without the notice."""
    files = source_files(root)
    return len(files), [f for f in files if not has_notice(f)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--write", action="store_true", help="add the notice to the files without it")
    args = parser.parse_args()

    total, missing = missing_notice()
    if args.write:
        for path in missing:
            add_notice(path)
        print(f"{total} source files: added the notice to {len(missing)}")
        return 0
    print(f"{total} source files")
    for path in missing:
        print(f"  MISSING {path.relative_to(ROOT).as_posix()}")
    if missing:
        print(f"FAIL: {len(missing)} files do not start with '{NOTICE}' "
              "(python Tools/check_copyright.py --write adds it)")
        return 1
    print("  every one starts with the copyright notice")
    return 0


if __name__ == "__main__":
    sys.exit(main())
