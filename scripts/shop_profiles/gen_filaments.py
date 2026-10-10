#!/usr/bin/env python3
"""Generate per-printer filament profiles for the print shop.

Reads manufacturer catalog data (scripts/shop_profiles/data/<brand>.json) and
emits OrcaSlicer filament presets for each shop printer. Every generated preset
inherits the vendor bundle's existing printer-specific generic for that material
(so all printer tuning: fans, exhaust, pressure advance, volumetric limits comes
from upstream) and layers the manufacturer's identity, temperatures, density and
notes on top.

Usage:
    python3 scripts/shop_profiles/gen_filaments.py            # write files
    python3 scripts/shop_profiles/gen_filaments.py --dry-run  # report only

Afterwards run, per touched vendor:
    python3 scripts/orca_profile_tool.py normalize   --vendor <V>
    python3 scripts/orca_profile_tool.py update-index --vendor <V>
    python3 scripts/orca_profile_tool.py generate-id  --vendor <V>
    python3 scripts/orca_profile_tool.py check        --vendor <V>
"""

import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
PROFILES = ROOT / "resources" / "profiles"
DATA = pathlib.Path(__file__).resolve().parent / "data"

# Reuse the upstream id rule so BBL presets (which generate-id never rewrites)
# still get the byte-identical setting_id the app would compute.
sys.path.insert(0, str(ROOT / "scripts"))
from orca_profile_tool import generate_preset_setting_id, generate_filament_id  # noqa: E402

# ---------------------------------------------------------------------------
# Printers
# ---------------------------------------------------------------------------
# generic: material_class -> parent preset name pattern ({noz} = nozzle string)
# nozzles: nozzle strings that get their own file (None = one file for all)
PRINTERS = {
    "plus4": {
        "vendor": "Qidi",
        "tag": "Qidi X-Plus 4 {noz} nozzle",
        "nozzles": ["0.2", "0.4", "0.6", "0.8"],
        "compatible": lambda noz: [f"Qidi X-Plus 4 {noz} nozzle"],
        "max_nozzle_temp": 350,
        "enclosed": True,
        "hardened": True,
        "generic": {
            "PLA": "Generic PLA @Qidi X-Plus 4 {noz} nozzle",
            "PLA_HS": "Generic PLA High Speed @Qidi X-Plus 4 {noz} nozzle",
            "PLA_SILK": "Generic PLA Silk @Qidi X-Plus 4 {noz} nozzle",
            "PLA_PLUS": "Generic PLA+ @Qidi X-Plus 4 {noz} nozzle",
            "PLA-CF": "QIDI PLA-CF @Qidi X-Plus 4 {noz} nozzle",
            "PETG": "Generic PETG @Qidi X-Plus 4 {noz} nozzle",
            "PETG-CF": "QIDI PETG-CF @Qidi X-Plus 4 {noz} nozzle",
            "PET-CF": "QIDI PET-CF @Qidi X-Plus 4 {noz} nozzle",
            "ABS": "Generic ABS @Qidi X-Plus 4 {noz} nozzle",
            "ABS-GF": "QIDI ABS-GF @Qidi X-Plus 4 {noz} nozzle",
            "ASA": "Generic ASA @Qidi X-Plus 4 {noz} nozzle",
            "TPU": "Generic TPU @Qidi X-Plus 4 {noz} nozzle",
            "PA": "QIDI PA-Ultra @Qidi X-Plus 4 {noz} nozzle",
            "PA-CF": "QIDI PA12-CF @Qidi X-Plus 4 {noz} nozzle",
            "PA-GF": "QIDI PAHT-GF @Qidi X-Plus 4 {noz} nozzle",
            "PC": "Generic PC @Qidi X-Plus 4 {noz} nozzle",
            "PPS-CF": "QIDI PPS-CF @Qidi X-Plus 4 {noz} nozzle",
        },
        "plate_keys": ["hot_plate_temp", "textured_plate_temp", "eng_plate_temp"],
        "qidi_settings_id": True,
    },
    "ender3pro": {
        "vendor": "Creality",
        "tag": "Creality Ender-3 Pro",
        "nozzles": [None],
        "compatible": lambda noz: [f"Creality Ender-3 Pro {n} nozzle" for n in ("0.2", "0.4", "0.6", "0.8")],
        "max_nozzle_temp": 250,   # stock PTFE-lined hotend
        "enclosed": False,
        "hardened": False,
        "generic": {
            "PLA": "Generic PLA @Creality",
            "PLA_HS": "Generic PLA HF @Creality",
            "PLA_SILK": "Generic PLA @Creality",
            "PLA_PLUS": "Generic PLA @Creality",
            "PETG": "Generic PETG @Creality",
            "ABS": "Generic ABS @Creality",
            "ASA": "Generic ASA @Creality",
            "TPU": "Generic TPU @Creality",
        },
        "plate_keys": ["hot_plate_temp", "textured_plate_temp", "eng_plate_temp"],
    },
    "cr10smart": {
        "vendor": "Creality",
        "tag": "Creality CR-10 Smart",
        "nozzles": [None],
        "compatible": lambda noz: ["Creality CR-10 Smart 0.4 nozzle"],
        "max_nozzle_temp": 250,
        "enclosed": False,
        "hardened": False,
        "generic": {
            "PLA": "Generic PLA @CR-10 Smart-all",
            "PLA_HS": "Generic PLA @CR-10 Smart-all",
            "PLA_SILK": "Generic PLA @CR-10 Smart-all",
            "PLA_PLUS": "Generic PLA @CR-10 Smart-all",
            "PETG": "Generic PETG @CR-10 Smart-all",
            "ABS": "Generic ABS @CR-10 Smart-all",
            "ASA": "Generic ASA @Creality",
            "TPU": "Generic TPU @Creality",
        },
        "plate_keys": ["hot_plate_temp", "textured_plate_temp", "eng_plate_temp"],
    },
    "a1": {
        "vendor": "BBL",
        "tag": "BBL A1{noz_suffix}",
        "nozzles": ["0.4", "0.2"],   # 0.4 file covers 0.4/0.6/0.8 (Bambu convention)
        "compatible": lambda noz: (["Bambu Lab A1 0.2 nozzle"] if noz == "0.2"
                                   else [f"Bambu Lab A1 {n} nozzle" for n in ("0.4", "0.6", "0.8")]),
        "max_nozzle_temp": 300,
        "enclosed": False,
        "hardened": False,   # hardened nozzle is an optional upgrade; flagged in notes
        "generic": {
            "PLA": "Generic PLA @BBL A1{noz_suffix}",
            "PLA_HS": "Generic PLA High Speed @BBL A1{noz_suffix}",
            "PLA_SILK": "Generic PLA Silk @BBL A1",
            "PLA_PLUS": "Generic PLA @BBL A1{noz_suffix}",
            "PLA-CF": "Generic PLA-CF @BBL A1",
            "PETG": "Generic PETG @BBL A1{noz_suffix}",
            "PETG-CF": "Generic PETG-CF @BBL A1",
            "PET-CF": "Bambu PET-CF @BBL A1",
            "ABS": "Generic ABS @BBL A1{noz_suffix}",
            "ABS-GF": "Bambu ABS-GF @BBL A1",
            "ASA": "Generic ASA @BBL A1{noz_suffix}",
            "TPU": "Generic TPU @BBL A1",
            "PA": "Generic PA @BBL A1",
            "PA-CF": "Generic PA-CF @BBL A1",
            "PA-GF": "Bambu PA6-GF @BBL A1",
            "PC": "Generic PC @BBL A1{noz_suffix}",
            "PVA": "Generic PVA @BBL A1{noz_suffix}",
            "HIPS": "Generic HIPS @BBL A1{noz_suffix}",
        },
        "plate_keys": ["hot_plate_temp", "textured_plate_temp"],
    },
}

# material (from research) -> (material_class for parent lookup, filament_type value)
MATERIALS = {
    "PLA": ("PLA", "PLA"),
    "PLA-CF": ("PLA-CF", "PLA-CF"),
    "PETG": ("PETG", "PETG"),
    "PETG-CF": ("PETG-CF", "PETG-CF"),
    "PET-CF": ("PET-CF", "PET-CF"),
    "ABS": ("ABS", "ABS"),
    "ABS-GF": ("ABS-GF", "ABS-GF"),
    "ASA": ("ASA", "ASA"),
    "TPU": ("TPU", "TPU"),
    "PA": ("PA", "PA"),
    "PA-CF": ("PA-CF", "PA-CF"),
    "PA-GF": ("PA-GF", "PA-GF"),
    "PC": ("PC", "PC"),
    "PPS-CF": ("PPS-CF", "PPS-CF"),
    "PVA": ("PVA", "PVA"),
    "HIPS": ("HIPS", "HIPS"),
}

ABRASIVE_CLASSES = {"PLA-CF", "PETG-CF", "PET-CF", "ABS-GF", "PA-CF", "PA-GF", "PPS-CF"}
# Materials an open-frame, brass-nozzle, 250 C machine should offer.
OPEN_FRAME_OK = {"PLA", "PLA_HS", "PLA_SILK", "PLA_PLUS", "PETG", "TPU", "ABS", "ASA"}


def pla_subclass(product):
    """Refine PLA into PLA_HS / PLA_SILK / PLA_PLUS from the product name/variant."""
    text = f"{product.get('name','')} {product.get('variant') or ''}".lower()
    if any(w in text for w in ("silk", "rainbow", "dual-color", "dual color", "metal")):
        return "PLA_SILK"
    if any(w in text for w in ("high speed", "highspeed", "rapido", "hyper", "hf", "fast", "meta")):
        return "PLA_HS"
    if any(w in text for w in ("pla+", "pla plus", "pro", "tough", "super")):
        return "PLA_PLUS"
    return "PLA"


# Specific filament_type strings Orca already uses for filled grades; chosen from
# the product name so the slicer's material-aware defaults line up.
TYPE_TOKENS = [
    ("PA6-CF", "PA6-CF"), ("PA12-CF", "PA12-CF"), ("PA612-CF", "PA-CF"), ("PA6-GF", "PA-GF"),
    ("PAHT-CF", "PAHT-CF"), ("PAHT-GF", "PAHT-GF"), ("PET-GF", "PET-GF"), ("PET-CF", "PET-CF"),
    ("PPS-GF", "PPS-GF"), ("PPS-CF", "PPS-CF"), ("ASA-CF", "ASA-CF"), ("ABS-GF", "ABS-GF"),
    ("PETG-CF", "PETG-CF"), ("PETG-GF", "PETG-GF"), ("PLA-CF", "PLA-CF"), ("TPU-GF", "TPU-GF"),
]


def refine_type(product, base_type):
    text = f"{product.get('name','')} {product.get('variant') or ''}".upper().replace(" ", "")
    for token, ftype in TYPE_TOKENS:
        if token in text:
            return ftype
    if base_type == "ASA" and "CF" in text and product.get("abrasive"):
        return "ASA-CF"
    return base_type


def round5(x):
    return int(5 * round(float(x) / 5))


def slug_eq(a, b):
    norm = lambda s: re.sub(r"[^a-z0-9]+", "", s.lower())
    return norm(a) == norm(b)


def tokens(name):
    """Order-insensitive token set of a product name: 'QIDI PLA Matte Rapido' == 'QIDI PLA Rapido Matte'."""
    return frozenset(t for t in re.split(r"[^a-z0-9+]+", name.lower()) if t)


def find_existing(vendor_names, product_name, name_tag):
    """An upstream preset for the same product on the same printer tag, if any."""
    want = tokens(product_name)
    for existing in vendor_names:
        if "@" not in existing:
            continue
        prod, _, tag = existing.partition(" @")
        if tag == name_tag and tokens(prod) == want:
            return existing
    return None


def load_vendor_names(vendor):
    """All preset names defined in a vendor bundle (by file name, minus .json)."""
    names = {}
    for p in (PROFILES / vendor / "filament").rglob("*.json"):
        names[p.stem] = p
    return names


class Bundle:
    """Lazy reader for a vendor bundle's filament presets, with inherits resolution."""

    def __init__(self, vendor):
        self.vendor = vendor
        self.paths = load_vendor_names(vendor)
        self._cache = {}

    def get(self, name):
        if name not in self._cache:
            p = self.paths.get(name)
            self._cache[name] = json.loads(p.read_text()) if p else None
        return self._cache[name]

    def resolve(self, name, key, depth=0):
        d = self.get(name)
        if d is None or depth > 12:
            return None
        v = d.get(key)
        if isinstance(v, list) and v:
            return v[0]
        if isinstance(v, str) and v:
            return v
        parent = d.get("inherits")
        return self.resolve(parent, key, depth + 1) if parent else None


def upstream_triple(product_name, bundles):
    """(filament_vendor, filament_type) upstream already uses for this product, if any.

    Looks for "<name> @base" or "<name>" in the target vendor bundle first, then
    the shared OrcaFilamentLibrary, so our ids converge with upstream's.
    """
    want = tokens(product_name)
    for b in bundles:
        for cand in (f"{product_name} @base", product_name, f"{product_name} @System"):
            if cand in b.paths:
                vend, typ = b.resolve(cand, "filament_vendor"), b.resolve(cand, "filament_type")
                if vend and typ:
                    return vend, typ
        # Any preset of the same product (token match on the part before "@"),
        # e.g. upstream "QIDI ASA-Aero@X-Plus 5-Series".
        for existing in b.paths:
            prod = existing.split("@")[0].strip()
            if tokens(prod) == want:
                vend, typ = b.resolve(existing, "filament_vendor"), b.resolve(existing, "filament_type")
                if vend and typ:
                    return vend, typ
    return None


def build_preset(product, printer_key, printer, noz, parent, triple=None):
    name_tag = printer["tag"].format(noz=noz, noz_suffix=(f" {noz} nozzle" if noz == "0.2" else ""))
    name = f"{product['name']} @{name_tag}"
    mclass, ftype = MATERIALS[product["material"]]
    ftype = refine_type(product, ftype)
    vendor_name = product["brand"]
    if triple:
        vendor_name, ftype = triple
    preset = {
        "type": "filament",
        "name": name,
        "inherits": parent,
        "from": "system",
        "setting_id": generate_preset_setting_id(printer["vendor"], "filament", name),
        "filament_id": generate_filament_id(vendor_name, ftype, product["name"]),
        "instantiation": "true",
        "filament_vendor": [vendor_name],
        "filament_type": [ftype],
    }
    tmin, tmax = product.get("nozzle_temp_min_c"), product.get("nozzle_temp_max_c")
    if tmin is not None and tmax is not None:
        mid = min(round5((tmin + tmax) / 2), printer["max_nozzle_temp"])
        preset["nozzle_temperature"] = [str(mid)]
        preset["nozzle_temperature_initial_layer"] = [str(mid)]
        preset["nozzle_temperature_range_low"] = [str(int(tmin))]
        preset["nozzle_temperature_range_high"] = [str(int(tmax))]
    bmax = product.get("bed_temp_max_c")
    if bmax is not None:
        for k in printer["plate_keys"]:
            preset[k] = [str(int(bmax))]
            preset[k + "_initial_layer"] = [str(int(bmax))]
    if printer["enclosed"] and product.get("chamber_temp_c"):
        preset["activate_chamber_temp_control"] = ["1"]
        preset["chamber_temperature"] = [str(int(product["chamber_temp_c"]))]
    if product.get("density_g_cm3"):
        preset["filament_density"] = [str(product["density_g_cm3"])]
    if product.get("fan_pct") is not None:
        preset["fan_max_speed"] = [str(int(product["fan_pct"]))]
        preset["fan_min_speed"] = [str(int(product["fan_pct"]))]
    if mclass in ABRASIVE_CLASSES or product.get("abrasive"):
        preset["required_nozzle_HRC"] = ["55"]
    if printer.get("qidi_settings_id"):
        preset["filament_settings_id"] = [name]

    notes = [f"{product['brand']} manufacturer data, generated by scripts/shop_profiles/gen_filaments.py."]
    if product.get("drying_temp_c"):
        hrs = product.get("drying_hours")
        notes.append(f"Dry {product['drying_temp_c']} C" + (f" for {hrs} h." if hrs else "."))
    if product.get("print_speed_max_mm_s"):
        notes.append(f"Manufacturer max speed {product['print_speed_max_mm_s']} mm/s.")
    if (mclass in ABRASIVE_CLASSES or product.get("abrasive")) and not printer["hardened"]:
        notes.append("ABRASIVE: fit a hardened nozzle on this printer first.")
    if product.get("enclosure_recommended") and not printer["enclosed"]:
        notes.append("Manufacturer recommends an enclosure; this printer is open-frame.")
    if product.get("notes"):
        notes.append(product["notes"])
    if product.get("source_url"):
        notes.append(f"Source: {product['source_url']}")
    preset["filament_notes"] = ["\n".join(notes)]
    preset["compatible_printers"] = printer["compatible"](noz)
    return name, preset


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--brand", action="append", help="limit to brand file stem(s)")
    args = ap.parse_args()

    brand_files = sorted(DATA.glob("*.json"))
    if args.brand:
        brand_files = [f for f in brand_files if f.stem in args.brand]
    if not brand_files:
        print("no brand data files found in", DATA)
        return 1

    vendor_names = {v: load_vendor_names(v) for v in {p["vendor"] for p in PRINTERS.values()}}
    bundles = {v: Bundle(v) for v in vendor_names}
    ofl = Bundle("OrcaFilamentLibrary")
    written, skipped = [], []

    for bf in brand_files:
        data = json.loads(bf.read_text())
        brand = data["brand"]
        for product in data["products"]:
            product["brand"] = brand
            mat = product.get("material")
            if mat not in MATERIALS:
                skipped.append((product["name"], "*", f"material {mat} unsupported"))
                continue
            mclass, _ = MATERIALS[mat]
            if mclass == "PLA":
                mclass = pla_subclass(product)
            for pkey, printer in PRINTERS.items():
                vendor = printer["vendor"]
                tmin = product.get("nozzle_temp_min_c")
                if tmin is not None and tmin > printer["max_nozzle_temp"]:
                    skipped.append((product["name"], pkey, f"needs {tmin} C > printer max"))
                    continue
                abrasive = mclass in ABRASIVE_CLASSES or bool(product.get("abrasive"))
                if abrasive and not printer["hardened"] and pkey != "a1":
                    skipped.append((product["name"], pkey, "abrasive filament not offered on brass-nozzle printer"))
                    continue
                if not printer["enclosed"] and not printer["hardened"] and mclass not in OPEN_FRAME_OK \
                        and pkey != "a1":
                    skipped.append((product["name"], pkey, f"{mclass} not offered on open-frame brass-nozzle printer"))
                    continue
                parent_pat = printer["generic"].get(mclass) or printer["generic"].get("PLA" if mclass.startswith("PLA_") else None)
                if not parent_pat:
                    skipped.append((product["name"], pkey, f"no parent for {mclass}"))
                    continue
                for noz in printer["nozzles"]:
                    suffix = f" {noz} nozzle" if noz == "0.2" else ""
                    parent = parent_pat.format(noz=noz, noz_suffix=suffix)
                    if parent not in vendor_names[vendor]:
                        # fall back to the plain-PLA generic for PLA subclasses
                        alt = printer["generic"].get("PLA", "").format(noz=noz, noz_suffix=suffix) if mclass.startswith("PLA") else None
                        if alt and alt in vendor_names[vendor]:
                            parent = alt
                        else:
                            skipped.append((product["name"], f"{pkey}/{noz}", f"parent missing: {parent}"))
                            continue
                    # Prefer the product's own base when the bundle already ships one
                    # (Bambu convention: "X @BBL A1" inherits "X @base").
                    own_base = f"{product['name']} @base"
                    if own_base in vendor_names[vendor]:
                        parent = own_base
                    triple = upstream_triple(product["name"], [bundles[vendor], ofl])
                    name, preset = build_preset(product, pkey, printer, noz, parent, triple)
                    tag = name.partition(" @")[2]
                    existing = find_existing(vendor_names[vendor], product["name"], tag)
                    if existing:
                        skipped.append((name, pkey, f"already exists: {existing}"))
                        continue
                    out = PROFILES / vendor / "filament" / brand / (name.replace("/", "-") + ".json")
                    written.append(out)
                    if not args.dry_run:
                        out.parent.mkdir(parents=True, exist_ok=True)
                        out.write_text(json.dumps(preset, indent=4, ensure_ascii=False) + "\n")
                    vendor_names[vendor][name] = out

    verb = "would write" if args.dry_run else "wrote"
    print(f"{verb} {len(written)} presets")
    by_vendor = {}
    for p in written:
        by_vendor[p.relative_to(PROFILES).parts[0]] = by_vendor.get(p.relative_to(PROFILES).parts[0], 0) + 1
    for v, n in sorted(by_vendor.items()):
        print(f"  {n:4d}  {v}")
    print(f"skipped {len(skipped)}:")
    reasons = {}
    for _, _, r in skipped:
        key = r.split(":")[0]
        reasons[key] = reasons.get(key, 0) + 1
    for r, n in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print(f"  {n:4d}  {r}")
    log = DATA.parent / "last_run.log"
    log.write_text("\n".join(f"{a}\t{b}\t{c}" for a, b, c in skipped) + "\n")
    print("skip details ->", log)
    return 0


if __name__ == "__main__":
    sys.exit(main())
