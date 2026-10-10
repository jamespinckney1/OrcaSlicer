# OVERTURE filament lineup - recommended settings (retrieved 2026-10-09)

Sources: overture3d.com product pages (spec tables) and the official TDS PDFs from
https://overture3d.com/pages/download. Amazon-only SKUs use retailer mirrors of the
Amazon listing text, marked (R). Full data: `overture.json` (23 products).

| Product | Material | Nozzle C | Bed C | Max speed mm/s | Fan | Dry | Density | Notes |
|---|---|---|---|---|---|---|---|---|
| PLA (2kg/3kg, Refill, 1kg on Amazon) | PLA | 190-220 | 25-60 | 70 | on | 50 C / 7 h | 1.21 | Gradient (rainbow) identical |
| High Speed PLA | PLA | 190-220 | 25-60 | 300 | on | 50 C / 7 h | - | no own TDS |
| Easy PLA | PLA | 190-220 | 25-60 | 60 (TDS 90) | on | 50 C / 7 h | 1.20 | |
| Matte PLA (+Dual Colors, Gradient) | PLA | 190-230 | 50-70 | 300 | on | 50 C / 7 h | 1.30/1.31 | |
| Silk PLA (+Dual, Gradient) | PLA | 200-220 | 50-60 | 70 | on | 50 C / 7 h | 1.19 | |
| Rock PLA (+Wood Series, Gradient) | PLA | 190-230 | 50-70 | 300 | on (full) | 50 C / 7 h | - | slightly abrasive, hardened 0.4 recommended |
| Shimmer PLA | PLA | 190-220 | 25-60 | 60 | on | 50 C / 7 h | - | table copied from Easy PLA |
| Glow PLA | PLA | 190-220 | 25-60 | 60 | on | 50 C / 7 h | - | TDS link = Easy PLA |
| PLA Professional (+Gradient) | PLA | 190-220 | 25-60 | 70 | on | 50 C / 7 h | 1.27 | |
| PLA+ (+Refill) | PLA | 190-230 | 25-60 | 300 | 100% | 55 C / 6 h (TDS) | 1.23 | page table bed "25-26" is a typo |
| Super PLA+ | PLA | 190-230 | 25-60 | 60 | on | 50 C / 7 h | 1.21 | |
| Turbo PLA (Amazon only) (R) | PLA | null | null | 600 | - | - | - | 40 mm3/s claim; no temps found |
| Air PLA (Amazon only) (R) | PLA | 190-210 | 25-60 | 50 | - | - | 0.9 | 0.8 kg spool, pre-foamed |
| High Speed PETG (+Refill, 2kg) | PETG | 230-260 | 65-70 | 300 | on | 60 C / 5 h | 1.3 | only PETG on overture3d.com |
| PETG (Amazon legacy) (R) | PETG | 230-250 | 80-90 | 50 | on | 65 C / >6 h | - | older OVPETG175 listing |
| TPU 95A (2.0) | TPU | 210-240 | 35-50 (TDS 25-60) | 250 (TDS 200) | on | 70 C / 7 h | ~1.18 | AMS incompatible |
| High Speed TPU 95A | TPU | 200-240 | 25-60 | 90 | on | 70 C / 7 h | 1.19 | direct drive recommended |
| Matte TPU 95A | TPU | 210-240 | 35-50 | 250 | 100% | 70 C / 7 h (TDS 6 h) | 1.17 | |
| ABS | ABS | 245-265 | 80-100 | 70 | off | 75 C / 7 h | 1.15 | enclosure required |
| ASA | ASA | 240-270 | 70-95 | 50 | off | 75 C / 7 h | 1.14 | enclosure required |
| Easy Nylon (PA6/66 copolymer) | PA | 245-260 | 50 | 70 | off | 95 C / 7 h | 1.12 | enclosure + hardened nozzle recommended; anneal |
| Nylon (Amazon legacy) (R) | PA | 250-270 | 25-50 | 50 | off | - | - | hardened nozzle recommended |
| PC Professional | PC | 250-270 | 90-105 | 50 | off | 75 C / 7 h | 1.21 | enclosure required |

## Not found in Overture's current lineup
PLA-CF, PETG-CF, PETG Professional, PA-CF/PA-GF, PVA or any support material, HIPS,
true wood-fill PLA (Overture's "Wood Series" is Rock PLA colors with marble dust),
PC blends, PPS. None appear on overture3d.com, the download page, or in Amazon store
search results.

## Uncertain / caveats
- Amazon storefront pages returned HTTP 503 and could not be read directly; Amazon-only
  SKUs (Turbo PLA, Air PLA, legacy PETG, legacy Nylon) come from retailer/deal-site
  mirrors of the listing text. Turbo PLA has no nozzle/bed table at all.
- Whether legacy PETG and Nylon are still actively sold on Amazon is unverified; some
  mirrors of the PETG listing now show the High Speed PETG table.
- Chamber temperature is never published; "Enclosed Printer Required" is recorded as
  enclosure_recommended=true with chamber_temp_c=null.
- fan_pct is null where Overture only says "Turned on"; 0 where "Turned off"; 100 only
  where a TDS states 100%.
- Several product pages contradict their own spec table (PLA+ bed "25-26", ABS copy
  230-250/90-110, PC copy bed 80-100, Silk copy "no heated bed", TPU copy 210-230).
  The spec table / TDS value was used; conflicts are in each product's notes.
- Shimmer PLA and Glow PLA pages reuse Easy PLA text and mechanical data, and the Glow
  TDS link points to the Easy PLA TDS, so their settings may be generic.
- Rock PLA, Shimmer, Glow, High Speed PLA have no TDS, so density is null.
- TPU 95A TDS density is garbled ("1.18 1.216"); 1.18 recorded.
- Gradient/rainbow, dual-color, refill and 2-3 kg SKUs were folded into their base
  product because their spec tables are identical.
