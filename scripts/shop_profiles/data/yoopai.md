# YOOPAI 1.75 mm filament lineup - manufacturer print settings (retrieved 2026-10-11)

10 distinct YOOPAI-branded products. Primary sources, in order of preference: yoopai.com product pages (Shopify `products.json` descriptions plus the parameter images on those pages), then YOOPAI's Amazon listings (bullet text plus gallery images, read by OCR and spot-checked by eye), then a TDS. YOOPAI publishes no TDS and no density figures. yoopai.com lists only four YOOPAI filament products: PLA+ (Ingeo) 1 kg, PLA+ Matte 1 kg, PETG 1 kg (black and transparent pages), and 250 g PLA+/Matte/Silk/Silk Rainbow bundles. Every other product is on Amazon only. 3dfilamentprofiles.com returned HTTP 429 and was not used.

Conventions (same as sunlu): where YOOPAI gives a drying range, `drying_temp_c` records the LOWER bound and `drying_hours` the UPPER bound; the full range is in `notes`. When YOOPAI sources disagree, the yoopai.com value wins and the others go in `notes`; a field that yoopai.com does not publish is filled from the Amazon listing and the note says so. `abrasive`, `hardened_nozzle_required` and `enclosure_recommended` are false because YOOPAI never asks for any of them on these PLA/PETG grades. `spool_weight_g` is the 1 kg standard SKU; 250 g, 2 kg and 4 kg packs are noted per product. Several products appear on Amazon under one shared variation family, so one product per sub-line was recorded, never one per colour.

| Product | Material | Nozzle C | Bed C | Max speed mm/s | Density | Drying | Flags |
|---|---|---|---|---|---|---|---|
| YOOPAI PLA+ | PLA | 190-230 | 35-65 | 300 | - | 50C / 8h | Ingeo PLA |
| YOOPAI PLA+ Matte | PLA | 190-230 | 35-60 | 300 | - | 50C / 8h | text paraphrases SUNLU |
| YOOPAI Silk PLA | PLA | 210-240 | 30-60 | 200 | - | 55C / 8h | sources conflict (Amazon: 190-215C, 80 mm/s), speed bands |
| YOOPAI Silk Rainbow PLA | PLA | 190-230 | - | 250 | - | 55C / 8h | image-only settings, speed bands |
| YOOPAI Dual Color Silk PLA | PLA | 190-215 | 30-60 | 80 | - | 55C / 8h | speed bands |
| YOOPAI Silk Tri-Color PLA | PLA | 190-230 | - | 250 | - | - | image-only settings, speed bands |
| YOOPAI PETG | PETG | 220-250 | 70-80 | 300 | - | 4-6h (no temp) | text copied from Creality CR-PETG |
| YOOPAI PETG HF | PETG | 230-260 | 65-75 | 300 | - | 60C / 8h | 600 mm/s claim |
| YOOPAI PETG Transparent | PETG | 230-260 | 65-75 | 220 | - | 60C / 8h | gradient listing differs |
| YOOPAI PETG Multicolor | PETG | 220-270 | 65-75 | 150 | - | 60C / 8h | bed from image only |

## Notes and conflicts

- **Silk PLA is the biggest conflict.** yoopai.com sells silk only as 250 g bundles. Those pages carry YOOPAI's lineup comparison chart, whose PLA Silk row reads 210-240C / <200 mm/s / 55C 8h, and the preference rule records those values. Every Amazon silk listing (1 kg singles, 2 kg packs, the 250 g x8 bundle) says nozzle 190-215C, bed 30-60C, 50-80 mm/s instead. The Amazon gallery adds speed bands of 190-215C at 50-150 mm/s and 215-230C at 150-250 mm/s. Bed 30-60C comes from Amazon because yoopai.com gives no bed figure. Check this one before trusting a generated preset.
- **Lineup comparison chart** (image on the yoopai.com 250 g bundle pages and the Amazon 250 g bundles): PLA+ 190-230C <300 mm/s 50C 8h; PLA Matte 190-230C <300 50C 8h; PLA Silk 210-240C <200 55C 8h; PLA Silk Rainbow 190-230C <250 55C 8h. The chart also lists PLA Wood (190-230C, 50-200 mm/s, 55C 12h), PLA Marble, PLA Galaxy, PLA Clear Sparkle and PLA Chameleon. No YOOPAI listing for any of those five was found on yoopai.com or Amazon, so they get no entries.
- **Image-only settings:** Silk Rainbow PLA and Silk Tri-Color PLA have no settings in their listing text. Their values come from gallery images (speed/temperature table) and, for Silk Rainbow, the comparison chart. Neither product publishes a bed temperature, and Tri-Color publishes no drying figure.
- **YOOPAI PETG copies Creality.** Its yoopai.com and Amazon text is Creality's CR-PETG listing with "Creality" swapped for "Our", down to the same typo and "especially suitable for Creality 3D Printers!". It shares an Amazon variation family with the Official CREALITY PETG listings that YOOPAI resells, and one relisting (B0H45PG846) is titled "YOOPAI Official PETG ... CR PETG Filament". The recorded values are what YOOPAI publishes in text (220-250C / 70-80C). YOOPAI's own "Printing Setting" image on the same pages says 230-250C / 60-80C / <300 mm/s (the 300 mm/s max comes from that image), which matches the 230-250C on Creality's CR-PETG spool label. Drying is given only as "4-6 hours" with no temperature.
- **Text paraphrased from SUNLU:** the PLA+ Matte listing copy ("Elegant Muted Colors for Modern Designs", "Reduced Layer Lines", art/fashion/decor wording) closely tracks SUNLU's Matte PLA listing. The Silk PLA copy loosely follows SUNLU's Silk PLA+ copy. In both cases the numbers differ from SUNLU's, so they are recorded as YOOPAI publishes them.
- **SUNLU product under a YOOPAI title (excluded):** Amazon B0F6YGC23D, "YOOPAI Matte PLA Filament 1.75mm 2025 Upgrade Spool Compatible with Bambulab Refill Filament", copies SUNLU's title format. Its gallery shows "SUNLU PLA Matte" and the "SUNLU Reusable Filament Spool" with SUNLU's settings (205-215C @50-100 mm/s, 215-245C @100-230 mm/s), while its bullets are YOOPAI Silk PLA text. It is a SUNLU product, covered by sunlu.json (SUNLU PLA Matte), and gets no YOOPAI entry.
- **Wrong text on legacy listings:** the 1 kg Amazon listings titled "YOOPAI PLA ... PLA+ Filament" (Sakura Pink, Yellow) and "PLA Silk ... Milk White" sit in the silk variation family and carry Silk PLA bullets and images. They are noted under PLA+ and Silk PLA and not used as separate products.
- **PETG Transparent vs Translucent Gradient:** both belong to the same "B-1kg" high-clarity sub-line, so they share one entry. The plain Transparent listing (230-260C / 65-75C / <=220 mm/s) was used; the Blue-Purple gradient listing says 220-260C / 60-80C / 50-300 mm/s.
- **PETG HF** is named from the Black listing title ("PETG HF Black, 30-600mm/s"). The other solid "A-1kg" colours in that family share its gallery settings. "PETG HF" is also Bambu Lab's product name. The 600 mm/s figure is marketing; YOOPAI's recommended setting is <300 mm/s.
- **PETG Multicolor:** the text gives 220-270C (first layer 255C, then 230C) and <150 mm/s. The gallery image says 230-260C / 65-75C / <300 mm/s. The bed range comes from the image only.
- **Speed bands vs max speed:** for Silk PLA and Dual Color Silk the published speed bands run to 250 mm/s while the listing text caps speed at 50-80 mm/s (Dual Color) or the chart caps it at <200 mm/s (Silk). Both are recorded as published. The generator clamps band temperatures to the nozzle range.
- **Fan, density, shore, chamber:** YOOPAI publishes none of these for any product (all null). PETG HDT is 70C at 0.45 MPa (image). Diameter tolerance is +/-0.03 mm on most listings and +/-0.02 mm on the Matte PLA+ Amazon listings.
- **Resold SUNLU products:** yoopai.com also resells SUNLU filament (SUNLU PETG 1 kg/2 kg/250 g bundles, SUNLU Silk PLA+, SUNLU Matte PLA 10 kg, Elite PETG) and the SUNLU FilaDryer S4. These are covered by sunlu.json.

## Resold Creality products (covered by creality.json)

Sold on Amazon by "Creality YOOPAI Store" (several carry "YOOPAI" in Amazon's brand field, but titles and text are Creality's):

- Official CREALITY PETG (CR-PETG) 1 kg, many colours incl. ArmyGreen, ClearlightPurple/Blue/Brown, Blue Green / Blue White Gradient, Digital Blue (e.g. B0DPMR9YHJ, B0FFMXK5K5, B0FFMVXLWR, B0C8NMYKFM). Text: 220-250C nozzle, 70-80C bed; spool label 230-250C.
- Official Creality PETG 2 kg packs (B0D4PX28KK, B0D4Q6X22Q), 4 kg packs (B0D4Q1J5L3, B0D4QH824B) and 12 kg pack (B0GCMDXN9V).
- CREALITY PETG 1 kg (B0D72J483F) and 4 kg bundle (B0CZ98V9Q7).
- CREALITY 250 g PETG Filament Bundle, 8 spools (B0DZCC44QJ, B0DY17M9MM).
- Creality Rainbow PLA+ 1 kg (B0FNX5QG5F; YOOPAI Store, and its bullets actually describe Creality's Premium Matte PLA).
- Not found: no "Official Creality Ender PLA" listing sold by YOOPAI turned up. The Ender PLA / PLA Pro / Hyper PLA listings found are sold by "Creality 3D HYZ", "Creality CL Store" or "Creality Official Store". yoopai.com currently lists no Creality filament, only the Creality Raptor X scanner and Ender firmware downloads.

## Key sources

- https://www.yoopai.com/products.json (storefront feed: 56 products, YOOPAI/SUNLU/TopCube vendors)
- https://www.yoopai.com/products/ingeo-pla-plus-filament-1-75mm-us-sourced-pla-1kg-2-2lbs
- https://www.yoopai.com/products/yoopai-pla-matte-filament-1-75mm-made-with-usa-material-soft-matte-finish-reduced-layer-lines-1kg-2-2lbs
- https://www.yoopai.com/products/petg-filament-3d-printer-filament-1-75mm-1kg-2-2lbs-black and ...-transparent
- https://www.yoopai.com/products/yoopai-pla-silk-filament-1-75mm-250g-starter-bundle-8-rolls (comparison chart image)
- https://www.yoopai.com/products/yoopai-pla-silk-rainbow-filament-1-75mm-250g-starter-bundle-4-rolls (comparison chart image)
- Amazon (YOOPAI Store): B0FZB64BJK (PLA+ Matte), B0H1MXDY4K (PLA+ 250 g bundle), B0GTTM2977 / B0GTTM396W (Silk PLA), B0GT58FRB7 (Silk Rainbow), B0GTTL5SSM / B0GTTLMMFD (Dual Color Silk), B0GTTQMQ82 (Silk Tri-Color), B0C8NP63GD / B0H45PG846 (PETG), B0G2XY3BNJ (PETG HF), B0GK8CQSPX / B0GK8333QY (PETG Transparent / Translucent Gradient), B0GH5S9DSQ (PETG Multicolor), B0F6YGC23D (SUNLU-imaged listing, excluded)
- https://www.amazon.com/s?k=YOOPAI+filament (lineup discovery; plus searches for PLA, PETG, silk, matte, TPU, ABS, high speed, wood, marble, galaxy, sparkle, chameleon. YOOPAI-branded TPU, ABS or high-speed PLA was not found)
