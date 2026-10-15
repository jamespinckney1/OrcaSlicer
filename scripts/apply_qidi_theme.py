#!/usr/bin/env python3
"""Recolor the OrcaSlicer UI from Orca teal to the Qidi Studio blue palette.

Idempotent: once a tree has been recolored there is nothing left to match, so
the script can be re-run safely after merging upstream to pick up new icons or
new hard-coded colors.

Usage:
    python3 scripts/apply_qidi_theme.py            # apply
    python3 scripts/apply_qidi_theme.py --dry-run  # report only
"""

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Orca teal family -> Qidi blue family (hex, matched case-insensitively).
HEX_MAP = {
    "009688": "4479FB",  # primary accent
    "26A69A": "4F8CFF",  # hover (lighter)
    "52C7B8": "6A96FC",  # hover + checked (lighter still)
    "4DB6AC": "6A96FC",  # light accent variant in a few icons
    "00897B": "3461D1",  # pressed (darker)
    "00675B": "2F5BD9",  # dark-mode accent
}

# Decimal RGB triples used by wxColour(...) and CSS rgb()/rgba().
RGB_MAP = {
    (0, 150, 136): (68, 121, 251),  # 009688 -> 4479FB
    (0, 137, 123): (52, 97, 209),   # 00897B -> 3461D1
    (0, 103, 91): (47, 91, 217),    # 00675B -> 2F5BD9
}

# Normalized ImGui float triples (ImVec4(r, g, b, a)).
IMVEC_MAP = {
    ("0.00", "0.59", "0.53"): ("0.27", "0.47", "0.98"),
}

# Literal nanosvg ABGR packed color in BitmapCache.cpp (0xAABBGGRR).
LITERAL_MAP = {
    "0xFF889600": "0xFFFB7944",
}

INCLUDE_DIRS = [
    ROOT / "src" / "slic3r",
    ROOT / "resources" / "images",
    ROOT / "resources" / "web",
    ROOT / "resources" / "data",
]
TEXT_SUFFIXES = {".cpp", ".hpp", ".h", ".c", ".mm", ".svg", ".css", ".html",
                 ".htm", ".js", ".json", ".ini", ".txt", ".xml"}

HEX_RE = re.compile(
    r"(?P<prefix>#|0x)(?P<hex>" + "|".join(HEX_MAP) + r")(?![0-9A-Fa-f])",
    re.IGNORECASE,
)
# wxColour(r, g, b), rgb()/rgba(), ImColor(r, g, b, a), IM_COL32(r, g, b, a)
# and brace-initialised wxColour {r, g, b}.
RGB_RE = re.compile(
    r"(?P<fn>wxColou?r\(|rgba?\(|ImColor\(|IM_COL32\(|\{)\s*(?P<r>\d{1,3})\s*,\s*(?P<g>\d{1,3})\s*,\s*(?P<b>\d{1,3})\s*(?P<rest>[,)}])"
)
IMVEC_RE = re.compile(
    r"ImVec4\(\s*(?P<r>\d\.\d+)f?\s*,\s*(?P<g>\d\.\d+)f?\s*,\s*(?P<b>\d\.\d+)f?\s*,"
)


def norm2(s: str) -> str:
    return f"{float(s):.2f}"


def recolor(text: str) -> str:
    def hex_sub(m):
        new = HEX_MAP[m.group("hex").upper()]
        return m.group("prefix") + new

    def rgb_sub(m):
        key = (int(m.group("r")), int(m.group("g")), int(m.group("b")))
        if key not in RGB_MAP:
            return m.group(0)
        r, g, b = RGB_MAP[key]
        return f"{m.group('fn')}{r}, {g}, {b}{m.group('rest')}"

    def imvec_sub(m):
        key = (norm2(m.group("r")), norm2(m.group("g")), norm2(m.group("b")))
        if key not in IMVEC_MAP:
            return m.group(0)
        r, g, b = IMVEC_MAP[key]
        return f"ImVec4({r}f, {g}f, {b}f,"

    text = HEX_RE.sub(hex_sub, text)
    text = RGB_RE.sub(rgb_sub, text)
    text = IMVEC_RE.sub(imvec_sub, text)
    for old, new in LITERAL_MAP.items():
        text = text.replace(old, new)
    return text


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    changed = []
    for base in INCLUDE_DIRS:
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in TEXT_SUFFIXES:
                continue
            try:
                # newline="" keeps CRLF files CRLF, so recolouring never rewrites every line.
                with open(path, encoding="utf-8", newline="") as f:
                    original = f.read()
            except UnicodeDecodeError:
                continue
            updated = recolor(original)
            if updated != original:
                changed.append(path)
                if not args.dry_run:
                    with open(path, "w", encoding="utf-8", newline="") as f:
                        f.write(updated)

    verb = "would change" if args.dry_run else "changed"
    print(f"{verb} {len(changed)} files")
    by_dir = {}
    for p in changed:
        by_dir.setdefault(str(p.relative_to(ROOT).parent), 0)
        by_dir[str(p.relative_to(ROOT).parent)] += 1
    for d, n in sorted(by_dir.items(), key=lambda kv: -kv[1]):
        print(f"  {n:5d}  {d}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
