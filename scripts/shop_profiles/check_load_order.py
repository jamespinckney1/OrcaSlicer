#!/usr/bin/env python3
"""Checks what the slicer needs to load a vendor bundle and orca_profile_tool's check does not: every
preset's parent exists, is not the preset itself, and is listed before it in <vendor>.json (the slicer
loads the lists in order, and one preset that fails to load stops the rest of the vendor, printers
included). With --fix, moves each parent ahead of its children in the lists.

Usage: scripts/shop_profiles/check_load_order.py [--fix] [Qidi BBL Creality ...]
Exit status: 0 when nothing is left to fix, 1 otherwise (with --fix: problems it could not move).
"""
import argparse
import json
import sys
from pathlib import Path

PROFILES = Path(__file__).resolve().parents[2] / "resources" / "profiles"
LISTS = ["machine_model_list", "process_list", "filament_list", "machine_list"]


def check(vendor, fix):
    """Returns (problems found, problems left after --fix)."""
    index_path = PROFILES / f"{vendor}.json"
    raw = index_path.read_text(encoding="utf-8")
    index = json.loads(raw)
    all_names = {e["name"] for k in LISTS for e in index.get(k, [])}
    found = left = 0
    moved = False
    for key in LISTS:
        if key == "machine_model_list":
            continue
        entries = index.get(key, [])
        parent_of = {}
        for e in entries:
            data = json.loads((PROFILES / vendor / e["sub_path"]).read_text(encoding="utf-8"))
            parent_of[e["name"]] = data.get("inherits")

        for e in entries:
            parent = parent_of[e["name"]]
            if parent == e["name"]:
                print(f"{vendor}/{key}: {e['name']} inherits itself")
                found += 1
                left += 1
            elif parent and parent not in all_names:
                print(f"{vendor}/{key}: {e['name']} inherits {parent}, which the vendor does not list")
                found += 1
                left += 1

        def late_children():
            pos = {e["name"]: i for i, e in enumerate(entries)}
            return [(i, pos[p]) for i, e in enumerate(entries)
                    if (p := parent_of[e["name"]]) and p != e["name"] and p in pos and pos[p] > i]

        late = late_children()
        for i, j in late:
            print(f"{vendor}/{key}: {entries[i]['name']} (#{i}) is listed before its parent "
                  f"{entries[j]['name']} (#{j})")
        found += len(late)
        if not fix:
            left += len(late)
            continue

        # Move parents ahead until stable (a parent can itself have a parent). Each move settles one
        # child/parent pair, so needing more moves than there are pairs means an inheritance cycle.
        for _ in range(len(entries) ** 2 + 1):
            late = late_children()
            if not late:
                break
            i, j = late[0]
            entries.insert(i, entries.pop(j))
            moved = True
        else:
            print(f"{vendor}/{key}: inheritance cycle, cannot order the list")
            left += len(late_children())

    if fix and moved:
        indent = "\t" if "\t" in raw[:200] else 4
        index_path.write_text(json.dumps(index, indent=indent, ensure_ascii=False) + "\n", encoding="utf-8")
    return found, left


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--fix", action="store_true", help="move parents ahead of their children")
    ap.add_argument("vendors", nargs="*", default=["Qidi", "BBL", "Creality"])
    args = ap.parse_args()
    found = left = 0
    for v in args.vendors:
        f, l = check(v, args.fix)
        found += f
        left += l
    msg = f"{found} load-order problem(s)"
    if args.fix and found:
        msg += f", {found - left} fixed, {left} left"
    print(msg)
    return 1 if left else 0


if __name__ == "__main__":
    sys.exit(main())
