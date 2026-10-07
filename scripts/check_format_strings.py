#!/usr/bin/env python3
"""Fail the build when a PRI macro is concatenated without a preceding %."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE_ROOTS = (ROOT / "src", ROOT / "components", ROOT / "tests")
EXTENSIONS = {".c", ".h", ".cc", ".cpp", ".hpp"}
CALL_RE = re.compile(
    r"\b(?:ESP_LOG[EWIDV]|printf|fprintf|snprintf|sprintf|vprintf|vsnprintf)\s*\("
)
PRI_RE = re.compile(r"\bPRI(?:[diouxX]|FAST|LEAST|MAX|PTR)[A-Za-z0-9_]*\b")
CONCAT_RE = re.compile(
    r'(?P<literal>"(?:\\.|[^"\\])*")\s*'
    r'(?P<macro>PRI(?:[diouxX]|FAST|LEAST|MAX|PTR)[A-Za-z0-9_]*)'
)


def line_number(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def main() -> int:
    errors: list[str] = []
    call_count = 0
    pri_count = 0
    files = sorted(
        p for root in SOURCE_ROOTS if root.exists()
        for p in root.rglob("*") if p.suffix.lower() in EXTENSIONS
    )
    for path in files:
        text = path.read_text(encoding="utf-8", errors="strict")
        call_count += len(CALL_RE.findall(text))
        pri_count += len(PRI_RE.findall(text))
        for match in CONCAT_RE.finditer(text):
            literal = match.group("literal")
            # A PRI macro supplies only the conversion letters (for example
            # "u"). The immediately preceding literal must supply '%'.
            if not literal[:-1].endswith("%"):
                rel = path.relative_to(ROOT)
                errors.append(
                    f"{rel}:{line_number(text, match.start())}: "
                    f"{match.group('macro')} is missing preceding '%'"
                )
    if errors:
        print("IoTSE format audit failed:", file=sys.stderr)
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"IoTSE format audit PASS: {call_count} format calls, {pri_count} PRI uses")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
