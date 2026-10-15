#!/usr/bin/env python3
"""One command for the shop filament profile pipeline.

Runs the generator, then for each shop vendor: orca_profile_tool normalize, update-index,
generate-id and check, then check_load_order.py. Finally bumps the last component of
"version" in resources/profiles/<Vendor>.json for every vendor whose files changed since
the last commit, unless the version was already bumped. Without the bump, installs keep
the profiles they already have.

Usage:
    python3 scripts/shop_profiles/regen.py            # regenerate, validate, bump
    python3 scripts/shop_profiles/regen.py --prune    # also delete presets the generator no longer makes
    python3 scripts/shop_profiles/regen.py --check    # CI / pre-commit: fail if the committed profiles
                                                      # differ from what the data generates (changes nothing)
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
PROFILES = ROOT / "resources" / "profiles"
VENDORS = ["Qidi", "BBL", "Creality"]


def run(*cmd):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], cwd=ROOT).returncode


def git(*args):
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout


def changed(vendor):
    """True when files of this vendor differ from HEAD (tracked changes or new files)."""
    paths = [f"resources/profiles/{vendor}", f"resources/profiles/{vendor}.json"]
    return bool(git("status", "--porcelain", "--", *paths).strip())


def bump(vendor):
    index = PROFILES / f"{vendor}.json"
    text = index.read_text(encoding="utf-8")
    current = re.search(r'"version":\s*"([\d.]+)"', text).group(1)
    try:
        committed = json.loads(git("show", f"HEAD:resources/profiles/{vendor}.json"))["version"]
    except (json.JSONDecodeError, KeyError):
        committed = None
    if committed and current != committed:
        print(f"{vendor}: version already bumped ({committed} -> {current})")
        return
    parts = current.split(".")
    parts[-1] = str(int(parts[-1]) + 1).zfill(len(parts[-1]))
    new = ".".join(parts)
    index.write_text(text.replace(f'"version": "{current}"', f'"version": "{new}"', 1), encoding="utf-8")
    print(f"{vendor}: version {current} -> {new}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--prune", action="store_true", help="pass --prune to the generator")
    ap.add_argument("--check", action="store_true",
                    help="fail if regenerating would change the committed profiles; leaves the tree as it was")
    args = ap.parse_args()

    py = sys.executable
    if args.check:
        if any(changed(v) for v in VENDORS):
            print("profiles have uncommitted changes; commit or discard them before --check")
            return 2
    gen = [py, "-I", HERE / "gen_filaments.py"] + (["--prune"] if args.prune else [])
    if run(*gen):
        return 1
    failed = False
    for v in VENDORS:
        for step in ("normalize", "update-index", "generate-id"):
            failed |= bool(run(py, "-I", ROOT / "scripts" / "orca_profile_tool.py", step, "--vendor", v))
        failed |= bool(run(py, "-I", ROOT / "scripts" / "orca_profile_tool.py", "check", "--vendor", v))
        failed |= bool(run(py, "-I", HERE / "check_load_order.py", v))

    drifted = [v for v in VENDORS if changed(v)]
    if args.check:
        if drifted:
            print("committed profiles differ from the generated ones for: " + ", ".join(drifted))
            print(git("status", "--short", "--", *[f"resources/profiles/{v}" for v in drifted]))
            # Put the tree back the way it was.
            for v in drifted:
                git("checkout", "--", f"resources/profiles/{v}", f"resources/profiles/{v}.json")
                git("clean", "-fdq", "--", f"resources/profiles/{v}")
            return 1
        print("profiles are up to date")
        return 1 if failed else 0

    for v in drifted:
        bump(v)
    if failed:
        print("one or more validation steps FAILED; see above")
        return 1
    print("done" + (": changed " + ", ".join(drifted) if drifted else ": nothing changed"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
