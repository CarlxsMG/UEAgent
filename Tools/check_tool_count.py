"""
check_tool_count.py -- the tool counts the prose states agree with the catalog.

Usage:
  python Tools/check_tool_count.py          # fails if a stated count is out of range
  python Tools/check_tool_count.py --check  # the same (the name the generators use)

The prose keeps a rounded number, "more than 1,500" or "1,500+": the catalog's listed tools (retired
names excluded) rounded down to the hundred. A stated count fails when it is above the live count, which
claims tools that are not there, or more than one hundred below its rounding, which is a number nobody
updated. So a pull request that crosses a hundred corrects the prose in the same change.

The files are the ones that state the count by hand; docs/tools/README.md keeps the exact count and is
generated. A count is a number with a thousands separator (1,500, Spanish 1.500, or the badge's
URL-encoded 1%2C500) on a line that names tools (tool, herramienta).
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CATALOG = ROOT / "Resources" / "command_catalog.json"
FILES = [
    "README.md",
    "README.es.md",
    "docs/index.md",
    "docs/explorer.md",
    "docs/faq.md",
    "docs/troubleshooting.md",
    "UEAgent.uplugin",
]

COUNT_RE = re.compile(r"(?<![\d.,])(\d{1,2})(?:,|\.|%2C)(\d{3})(?![\d,]|\.\d)")
TOOLS_RE = re.compile(r"tool|herramienta", re.IGNORECASE)


def listed_count(catalog_path: Path = CATALOG) -> int:
    """The tools the server lists: every catalog command but the retired ones."""
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    return sum(1 for d in catalog["domains"] for c in d["commands"] if not c.get("retired"))


def stated_counts(path: Path) -> list[tuple[int, int]]:
    """(line number, count) for each tool count the file states."""
    found = []
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not TOOLS_RE.search(line):
            continue
        for match in COUNT_RE.finditer(line):
            found.append((number, int(match.group(1) + match.group(2))))
    return found


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    parser.add_argument("--check", action="store_true", help="check (the default; accepted for symmetry with the generators)")
    parser.parse_args()

    live = listed_count()
    rounded = live // 100 * 100
    lowest = rounded - 100
    print(f"{live} listed tools; a stated count must be between {lowest:,} and {live:,} (prose: {rounded:,})")

    wrong: list[str] = []
    seen = 0
    for name in FILES:
        path = ROOT / name
        if not path.is_file():
            wrong.append(f"{name}: no such file")
            continue
        for line, count in stated_counts(path):
            seen += 1
            if count > live:
                wrong.append(f"{name}:{line}: states {count:,}, above the {live:,} tools listed")
            elif count < lowest:
                wrong.append(f"{name}:{line}: states {count:,}, more than 100 below {rounded:,}")
    print(f"  {seen} stated counts in {len(FILES)} files")
    for line in wrong:
        print(f"  WRONG {line}")
    if wrong:
        print(f"FAIL: {len(wrong)} counts out of range; write {rounded:,} (\"more than {rounded:,}\", \"{rounded:,}+\")")
        return 1
    print("  every stated count is in range")
    return 0


if __name__ == "__main__":
    sys.exit(main())
