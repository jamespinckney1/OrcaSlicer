# Creality 1.75 mm filament lineup - manufacturer print settings (retrieved 2026-10-11)

26 distinct products. Primary sources: the store.creality.com product pages (US store first; CA, EU, UK, AU and FR regional stores for items the US store does not carry). The settings on those pages live in a lazily loaded "Take a Closer Look" section served by Creality's CMS endpoint (`/front-api/exhibit/cms-product-details/shopify/detail?handle=...&siteCode=USA|CA|EU|UK|AU|FR`), partly as text and partly as parameter images; the images and the spool-label photos in each gallery were OCR'd (macOS Vision) and the important ones were checked by eye. The lineup was taken from the regional Shopify feeds (`<region>.myshopify.com/products.json`). Secondary sources: the Creality wiki drying guide, the K1 and K2 Plus "recommended filament parameters" spreadsheets (Creality Print profile values: one temperature, not a range), the CR-PLA and CR-PETG wiki guides and the CFS compatibility sheets. Creality does not publish downloadable TDS PDFs for its own filaments on store.creality.com, creality.com or the wiki, so none were used.

Conventions: where Creality gives a drying range, `drying_temp_c` records the LOWER bound and `drying_hours` the UPPER bound; the full range is in `notes`. When a product page gives no drying spec, the Creality wiki generic row for that material is used and the note says so. `fan_pct` is the published upper bound (product page or spool label first; otherwise the K2 Plus profile maximum, noted). A bed range written by Creality as "no heating - 60C" is recorded as min null / max 60 (shown as <=60 below). Where the product page spec block and the spool label disagree, the JSON records the product-page figure and the label figure goes in notes. RFID, spool-free refill and multi-kg pack listings of the same filament are folded into one entry. All spools are 1 kg net.

| Product | Material | Nozzle C | Bed C | Max speed mm/s | Density | Drying | Flags |
|---|---|---|---|---|---|---|---|
| Creality Hyper PLA | PLA | 190-230 | 25-60 | 600 | 1.25 | 55C / 12h |  |
| Creality Hyper PLA Stardust | PLA | 190-230 | 45-60 | 300 | 1.27 | 55C / 12h | abrasive? |
| Creality Hyper PLA Luminous | PLA | 190-230 | 50-60 | 100 | 1.25 | 55C / 12h | abrasive? |
| Creality Hyper PLA Lightweight | PLA | 210-270 | 45-60 | 100 | 1.2 | 60C / 6h |  |
| Creality Hyper Rainbow PLA | PLA | 190-230 | 25-60 | 300 | 1.25 | 55C / 12h |  |
| Creality Hyper PLA-CF | PLA-CF | 190-230 | 45 | 300 | - | 55C / 8h | abrasive |
| Creality Hyper PETG | PETG | 190-260 | 45 | 300 | - | 50C / 8h |  |
| Creality Hyper PETG-CF | PETG-CF | 240-260 | 70-90 | 300 | 1.24 | 60C / 12h | abrasive, hardened nozzle |
| Creality Hyper ABS | ABS | 230-270 | 75-95 | 350 | - | 65C / 12h | enclosure, chamber 50C |
| Creality HP-ASA | ASA | 200-300 | 50-100 | 300 | - | 65C / 8h | enclosure, chamber 50C |
| Creality Hyper PC | PC | 240-260 | 50-80 | 300 | 1.19 | 80C / 8h | enclosure |
| Creality PPA-CF (Hyper PPA-CF) | PA-CF | 280-300 | 90-105 | 100 | 1.06 | 80C / 12h | abrasive, enclosure |
| Creality HP-TPU | TPU | 210-240 | - | 120 | - | 70C / 12h |  |
| Creality CR-TPU | TPU | 210-240 | - | 120 | - | 70C / 12h |  |
| Creality CR-PLA | PLA | 190-230 | <=60 | 120 | 1.24 | 55C / 12h |  |
| Creality CR-PLA Matte | PLA | 190-230 | <=60 | 100 | - | 55C / 12h |  |
| Creality CR-Silk | PLA | 190-230 | <=60 | 100 | - | 55C / 12h |  |
| Creality CR-Wood | PLA | 190-230 | - | 90 | - | 55C / 12h | abrasive? |
| Creality CR-PLA Carbon | PLA | 195-230 | 25-60 | 80 | 1.25 | 55C / 12h | abrasive? |
| Creality CR-PETG | PETG | 230-250 | 60-100 | 80 | - | 60C / 6h |  |
| Creality Ender-PLA | PLA | 190-230 | - | 80 | 1.25 | 55C / 12h |  |
| Creality Ender-PLA+ | PLA | 190-230 | <=60 | 100 | - | 55C / 12h |  |
| Creality Ender Fast PLA | PLA | 190-230 | 45-60 | 300 | - | 55C / 12h |  |
| Creality Soleyin Ultra PLA | PLA | 190-230 | 45-60 | 300 | 1.25 | 55C / 12h |  |
| Creality Soleyin PLA Matte | PLA | 190-230 | 45-60 | 300 | 1.25 | 55C / 12h |  |
| Creality Soleyin Basic PETG | PETG | 210-250 | 60-80 | 200 | 1.23 | 60C / 12h |  |

## Notes and conflicts

- **Product page vs spool label**: Creality's product-page spec blocks and the printed spool labels shown in the same gallery often disagree. The biggest gap is **Hyper PETG**: page 190-260C, bed 45C, 50-300 mm/s, dry 50C/8h; label 220-260C, bed 60-80C, 30-600 mm/s, fan 50%. The label values look far more like a real PETG profile. Others: **Hyper PLA-CF** (page 190-230C / bed 45C / 300 mm/s; label 200-230C / 25-60C / 350 mm/s), **Hyper ABS** (page bed 75-95C / <350 mm/s; label bed 80-100C / 300 mm/s / fan 50-70%), **Ender Fast PLA** (page bed 45-60C; label 25-60C, fan 50-100%), **Soleyin Basic PETG** (spec image 210-250C / 200 mm/s; colour labels 230-250C / 100 mm/s).
- **Same product, different regional pages**: **HP-ASA** max speed is 300 mm/s in the US/AU/UK spec block but 200 mm/s in the headline and the EU/FR spec image. **Hyper PLA Luminous** US copy gives bed 50-60C at 50-100 mm/s; the FR spec block gives 45-60C at 30-300 mm/s. **CR-PLA Carbon** is 195-230C on the US page and 190-230C on the CA page. **Ender-PLA** gives 190-230C / 40-80 mm/s (US CMS) but a single 200C / 50 mm/s (CA page).
- **Single-value beds**: Hyper PLA-CF and Hyper PETG publish one heated-bed value (45C), recorded as min = max.
- **Drying**: only Hyper PETG (50C/8h), Hyper PLA-CF (55C/8h oven or 75C/12h on the heatbed), HP-ASA (65C/8h), PPA-CF (80C/8-12h), Hyper PLA Lightweight (60-80C/4-6h) and CR-PETG (wiki: 60-65C/4-6h) have product-specific drying advice. Everything else uses the wiki generic table: PLA family 55C/8-12h, PETG 60C/8-12h, ABS/ASA 65C/8-12h, TPU 70C/8-12h, PC 80-100C/8h (blast dryer only). Conflict: the wiki lists PPA-CF at 100-120C/8-12h but the PPA-CF page says 80C/8-12h (page value recorded). The Hyper PETG page's 50C is also below the wiki's 60C for PETG.
- **Chamber / enclosure**: Creality publishes no chamber temperatures on product pages. Hyper ABS and HP-ASA chamber 50C come from the K2 Plus profile table (the K1 table says 60C and 55C). The K2 Plus wiki says ABS/ASA/PC/PA need chamber heating (typically 50-60C) with the lid and door closed. That is the basis for `enclosure_recommended` on Hyper ABS and Hyper PC; HP-ASA's FAQ and PPA-CF's spec block ask for an enclosed printer directly. The 35C "chamber" value the K1/K2 tables give for PLA/PETG/TPU is an open-lid ceiling, not a target, so it is not recorded. CR-PETG's wiki guide says an enclosure is not mandatory.
- **Hardened nozzle / abrasive**: only Hyper PETG-CF names a hardened or wear-resistant nozzle. Hyper PLA-CF and PPA-CF are carbon-filled (`abrasive: true`), but Creality does not state a hardened-nozzle requirement for them (null). Fillers in Stardust (reflective flakes), Luminous (glow pigment), CR-Wood (wood powder) and CR-PLA Carbon have no abrasiveness statement (null). Creality's own K1/K2 tables class CR-PLA Carbon as PLA, not PLA-CF.
- **Density oddities**: PPA-CF's published 1.06 g/cm3 is unusually low for a 15% CF nylon but is recorded as published. Hyper PLA Lightweight is 1.2 g/cm3 raw; printed density falls from 1.2 (210C) to 0.54 (260C) as it foams, per Creality's foaming table.
- **No published values**: density is missing for Hyper PLA-CF, Hyper PETG, Hyper ABS, HP-ASA, both TPUs, CR-PLA Matte, CR-Silk, CR-Wood, CR-PETG, Ender-PLA+ and Ender Fast PLA. The bed is missing for HP-TPU, CR-TPU, CR-Wood and Ender-PLA. Shore hardness is not published for HP-TPU or CR-TPU. Creality publishes no temperature-by-speed guidance, so no product has `speed_temp_bands`.
- **CR-TPU** page copy is the HP-TPU text (120 mm/s), but Creality's K1 table gives CR-TPU 50 mm/s and 3.5 mm3/s. Treat 120 mm/s with caution.
- **Availability at retrieval**: CR-PLA, CR-PLA Matte and Ender-PLA are listed only on the CA store and are out of stock in every colour. The Ender-PLA US URL redirects to Ender Fast PLA. They are included because the Creality listings are still live. The 1 kg Ender Fast PLA listings (CA/UK) are out of stock, but the US 2 kg pack is in stock. CR-TPU and Ender-PLA+ are sold only on EU/UK/FR stores, Soleyin PLA Matte only on FR.
- **Excluded**: HP Ultra PLA (BASF co-developed): the CA listing now redirects to a 404, every variant is unavailable, and no settings appear anywhere except the K1/K2 profile tables (230C/45C or 220C/50C, 200 mm/s). Also excluded: CR-ABS, CR-Nylon, CR-PLA Fluo and Hyper PAHT-CF, which appear in Creality's wiki/CFS/slicer tables but on no current store; "Creality filament-W", a US listing with no stock or specs that looks like a test/wholesale SKU; and colour packs/bundles.

## Key sources

- https://store.creality.com/collections/filament (US store lineup; regional equivalents under /ca, /eu, /au, /fr)
- https://store.creality.com/front-api/exhibit/cms-product-details/shopify/detail?handle=hyper-series-pla-3d-printing-filament-1kg&siteCode=USA (example CMS payload behind a product page's spec section)
- https://crealityusa.myshopify.com/products.json, https://crealityeu.myshopify.com/products.json, https://crealityca.myshopify.com/products.json, https://crealityuk.myshopify.com/products.json, https://crealityau.myshopify.com/products.json (lineup and stock)
- https://store.creality.com/products/hyper-series-pla-3d-printing-filament-1kg
- https://store.creality.com/products/hyper-series-petg-3d-printing-filament-1kg
- https://store.creality.com/products/hyper-petg-cf-3d-printing-filament-1kg
- https://store.creality.com/products/hyper-pla-cf
- https://store.creality.com/products/hyper-pc-filament-1-75mm-1kg
- https://store.creality.com/products/ppa-cf-filament-1-75mm-1kg
- https://store.creality.com/products/creality-hp-asa-3d-printing-filament
- https://wiki.creality.com/en/filament-drying
- https://wiki.creality.com/en/k1-flagship-series/k1-series-general-documents/complete-filaments-recommended-parameters (xlsx download)
- https://wiki.creality.com/en/k2-flagship-series/k2-plus/filament-parameters (rar/xlsx download)
- https://wiki.creality.com/en/3d-printer-accessories/CR-PLA
- https://wiki.creality.com/en/3d-printer-accessories/CR-PETG
- https://wiki.creality.com/en/cfs/cfs-filament-compatibility and https://wiki.creality.com/en/cfs-c/filament-parameters
