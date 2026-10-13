#!/usr/bin/env python3
"""Checks what the slicer needs to load a vendor bundle and orca_profile_tool's check does not: every
preset's parent exists, is not the preset itself, and is listed before it in <vendor>.json (the slicer
loads the lists in order, and one preset that fails to load stops the rest of the vendor, printers
included). With --fix, moves each parent ahead of its children in the lists.

Usage: scripts/shop_profiles/check_load_order.py [--fix] Qidi BBL Creality
"""
import json
import sys
from pathlib import Path

PROFILES = Path(__file__).resolve().parents[2] / "resources" / "profiles"
LISTS = ["machine_model_list", "process_list", "filament_list", "machine_list"]


def check(vendor, fix):
    index_path = PROFILES / f"{vendor}.json"
    raw = index_path.read_text(encoding="utf-8")
    index = json.loads(raw)
    problems = 0
    for key in LISTS:
        entries = index.get(key, [])
        if key == "machine_model_list":
            continue
        changed = True
        while changed:  # move parents ahead until stable (a parent can itself have a parent)
            changed = False
            pos = {e["name"]: i for i, e in enumerate(entries)}
            for i, e in enumerate(entries):
                data = json.loads((PROFILES / vendor / e["sub_path"]).read_text(encoding="utf-8"))
                parent = data.get("inherits")
                if not parent:
                    continue
                if parent == e["name"]:
                    print(f"{vendor}/{key}: {e['name']} inherits itself")
                    problems += 1
                elif parent in pos and pos[parent] > i:
                    print(f"{vendor}/{key}: {e['name']} (#{i}) is listed before its parent {parent} (#{pos[parent]})")
                    problems += 1
                    if fix:
                        entries.insert(i, entries.pop(pos[parent]))
                        changed = True
                        break
                elif parent not in pos and not any(parent in {x["name"] for x in index.get(k, [])} for k in LISTS):
                    print(f"{vendor}/{key}: {e['name']} inherits {parent}, which the vendor does not list")
                    problems += 1
    if fix:
        index_path.write_text(json.dumps(index, indent="\t" if "\t" in raw[:200] else 4, ensure_ascii=False) + "\n", encoding="utf-8")
    return problems


def main():
    fix = "--fix" in sys.argv
    vendors = [a for a in sys.argv[1:] if not a.startswith("--")] or ["Qidi", "BBL", "Creality"]
    total = sum(check(v, fix) for v in vendors)
    print(f"{total} load-order problem(s)" + (" (fixed what could be moved)" if fix and total else ""))
    return 1 if total and not fix else 0


if __name__ == "__main__":
    sys.exit(main())
