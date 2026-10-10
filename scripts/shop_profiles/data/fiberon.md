# Fiberon (Polymaker) 1.75 mm lineup - manufacturer print settings

Retrieved 2026-10-09. 12 distinct products. Primary sources: Polymaker wiki product pages
(wiki.polymaker.com/polymaker-products/polymaker-filaments/fiberon-tm/...) and the official TDS PDFs
(cdn.shopify.com / fiberon.polymaker.com); lineup cross-checked against shop.polymaker.com/collections/fiberon
(Shopify products.json, 12 products) and the Polymaker product changelog (latest Fiberon entry: 2026-04-02,
1 kg spools for PET-CF17 and PETG-ESD; no new Fiberon material since ASA-CF08 in Aug 2025 and PET-GF15).
No retailer data was needed. Full data: `fiberon.json`.

| Product | Mapped material | Nozzle (C) | Bed (C) | Chamber | Max speed (mm/s) | Fan | Density | Drying | Anneal | Enclosure | Spool |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Fiberon PA6-CF20 | PA-CF | 280-300 | 40-50 | room temp | 300 | 0% | 1.17 | 100 C / 10 h | Required: 100 C for 16 h (TDS and wiki), then moisture-condi | no | 500 g |
| Fiberon PA6-GF25 | PA-GF | 280-300 | 40-50 | room temp | 300 | 0% | 1.2 | 100 C / 10 h | Required: 100 C for 16 h (TDS; shop page says 10-16 h), then | no | 500 g |
| Fiberon PA612-CF15 | PA-CF | 250-300 | 40-50 | room temp | 300 | 0% | 1.03 | 100 C / 10 h | Required: 100 C for 16 h, then moisture-condition | no | 500 g |
| Fiberon PA12-CF10 | PA-CF | 280-300 | 40-50 | room temp | 300 | 0% | 1.06 | 100 C / 10 h | Required: 100 C for 16 h, then moisture-condition | no | 500 g |
| Fiberon PA612-ESD | PA-CF | 280-300 | 40-50 | room temp | 300 | 0% | 1.1 | 100 C / 10 h | Optional/recommended: TDS 100 C for 16 h; wiki tips say 100  | no | 500 g |
| Fiberon PET-CF17 | PET-CF | 270-300 | 70-80 | room temp | 300 | 0% | 1.34 | 100 C / 10 h | Recommended: 120 C for 10 h (raises HDT from ~70 C to >100 C | no | 500 g |
| Fiberon PET-GF15 | PET-CF | 280-310 | 70-80 | room temp | 250 | 0% | 1.43 | 100 C / 10 h | Optional: 120 C for 16 h only if service temp >80 C (HDT to  | yes | 1000 g |
| Fiberon PETG-rCF08 | PETG-CF | 240-270 | 60-70 | room temp | 300 | 50% | 1.3 | 65 C / 3 h | None (TDS: N/A) | no | 500 g |
| Fiberon PETG-ESD | PETG | 250-290 | 70-80 | room temp | 500 | 0% | 1.24 | 65 C / 3 h | None (TDS: N/A) | no | 500 g |
| Fiberon PPS-CF10 | PPS-CF | 310-350 | 80-90 | room temp | 300 | 0% | 1.29 | 100 C / 10 h | Recommended: 125 C for 16 h for best layer adhesion and heat | no | 500 g |
| Fiberon PPS-GF20 | PPS-CF | 310-350 | 80-90 | room temp | 250 | 0% | 1.36 | 100 C / 10 h | Recommended: 130 C for 10 h (HDT 236 C @0 | no | 500 g |
| Fiberon ASA-CF08 | ASA | 260-280 | 90-100 | room temp | 350 | 0% | 1.09 | 90 C / 6 h | Not required (TDS: N/A); full performance as printed | yes | 500 g |

All TDS list "Chamber temperature: Room temp." (PPS wiki tables say ambient 25-80 C is acceptable), so
`chamber_temp_c` is null everywhere. All products are non-elastomeric, so `shore_hardness` is null.

## Things I was unsure about

- **Material mapping gaps**: PET-GF15 and PPS-GF20 are glass-fiber products but the allowed enum has no PET-GF / PPS-GF,
  so they are mapped to PET-CF and PPS-CF respectively (closest print behaviour). ASA-CF08 is mapped to ASA (no ASA-CF
  value). PA612-ESD (10% CF + CNT) is mapped to PA-CF. PETG-ESD (CNT only, no fiber) is mapped to PETG.
- **PETG-ESD abrasiveness**: the Fiberon FAQ says it is the only non-abrasive Fiberon product, but its wiki page boilerplate
  says "requirement of a hardened nozzle due to the abrasiveness". Recorded abrasive=false, hardened_nozzle_required=false
  with a note; a hardened nozzle is still advisable for CNT-filled material.
- **PETG-ESD speed/fan**: TDS V1.1 says up to 500 mm/s and fan OFF; wiki table says 30-300 mm/s and fan max 50%. JSON uses TDS values.
- **PA612-ESD annealing**: TDS says 100 C/16 h, wiki tips say 100 C/10 h ("can be annealed", not required).
- **PETG-rCF08 fan**: published as a 0-50% range; recorded fan_pct=50 (the max) with the range in notes.
- **Spool weights**: Fiberon ships mainly on 500 g spools (3 kg for all; 1 kg for PA6-CF20, PA612-CF15, PET-CF17, PETG-ESD;
  PET-GF15 is 1 kg only). spool_weight_g records the smallest/default size. Shopify "grams" values (860/1350/4260) are
  shipping weights, not net.
- **Enclosure for PET-GF15**: TDS says room-temp chamber, but the wiki FAQ recommends an enclosed/heated-chamber printer
  for best results; recorded enclosure_recommended=true.
- **No PEEK/PEKK/PPA/PC-CF Fiberon products exist** as of this date (checked wiki llms.txt index, shop collection, changelog,
  and web search). One search snippet mentioned a "PPS-CF20" but no product page, TDS or shop listing exists; treated as noise.
- fiberon.polymaker.com and polymaker.com block automated fetches (Cloudflare 403); the wiki, Shopify store and CDN PDFs were used instead.
