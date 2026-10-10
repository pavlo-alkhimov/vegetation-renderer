# 13 — Reference maps and source data

Goal: large (≥ ~100 km²) terrains of the target biome — mixed forest, fields, meadows, villages, rolling hills — whose
files can be downloaded and legally used in this project. Excluded: desert, rainforest, pure high mountains, small
maps. State: October 2026; licences must be re-checked on the provider's page before download.

## Arma Reforger maps and mods: visual reference only

| Map | Size | Character |
|---|---|---|
| Everon (official) | 12.8 × 12.8 km | Cold War island, Central European look |
| Arland (official) | ~6 × 6 km | Czech-style heath and pine groves |
| Community "Eastern Europe" terrains (e.g. Serhiivka, Darkgru East Europe, Nemesis Ukraine) | mostly town-sized or undocumented | Ukrainian/Eastern European villages and fields |

**Their files cannot be used here.** Bohemia's samples and most Workshop mods are under the **Arma Public License
(APL / APL-SA / APL-ND)**, whose summary forbids commercial use and forbids converting or adapting the material
"for use in games other than Arma". Everything produced with the **Arma Reforger Tools** falls under the Tools EULA,
which limits use to non-commercial content for Bohemia games. Some mods add stricter custom terms (no extraction of
models or textures). The same applies to Arma 3 terrains (Krayina 14×14 km, Bulakhi 20×20 km). Playing them and
taking screenshots for look-dev comparison is fine; extracting heightmaps, masks or models is not.

## What can be used: open national geodata

Real places give large, believable terrains with the right biome, and the data is better than any game map:
LiDAR terrain at 1 m, orthophotos, forest masks and even per-stand tree species.

| Source | Content | Licence | Notes |
|---|---|---|---|
| ČÚZK (Czechia) DMR 5G | LiDAR terrain model (TIN/LAZ), whole country | **CC BY 4.0**, free | 0.18 m mean height error in the open, 0.3 m in forest; ATOM download per SM5 sheet |
| ČÚZK Ortofoto ČR | colour orthophoto (2024–2025), 2.5 × 2 km JPEG tiles | **CC BY 4.0**, free (open since July 2023) | bulk download reportedly throttled; check the metadata wording |
| GUGiK (Poland) NMT / NMPT / point clouds | 1 m DTM and DSM, classified LAS point clouds, orthophoto | free, "any purpose" (Geodetic Law amendment 2020); no named licence | download ≤ 10 km² per request or via WCS |
| Lasy Państwowe BDL (Poland) | forest stands: geometry + species composition in 10 % steps, age | public, free; must cite source and dates | the only per-stand species source found; ideal for placement rules |
| Bayerische Vermessungsverwaltung DGM1 | 1 m DTM, 1 km GeoTIFF tiles | **CC BY 4.0** | fixed attribution line required |
| ÚGKK (Slovakia) DMR 5.0 | 1 m DTM, point cloud | free; derived products allowed, raw redistribution reportedly not (v6.0 listed as CC BY 4.0) | licence to confirm |
| Copernicus HRL Tree Cover Density / Dominant Leaf Type | 10 m canopy density, broadleaf/conifer | free, open (Copernicus terms) | Europe-wide |
| ForestPaths European tree genus map | 10 m, 8 classes (Larix, Picea, Pinus, Fagus, Quercus, …) | open, early access (check record) | genus-level placement |
| ESA WorldCover | 10 m land cover | **CC BY 4.0** | fields, grassland, built-up, water |
| ETH canopy height (10 m) / Meta-WRI canopy height (1 m) | tree height | **CC BY 4.0** | tree-height distributions where LiDAR DSM is missing |
| Copernicus DEM GLO-30 | 30 m DEM, horizon/far field | free, Copernicus licence with notices | for the 5–10 km vista around the 1 m core |
| OpenStreetMap | roads, villages, water, field boundaries | ODbL (share-alike for the database) | |

## Candidate regions (16 × 16 km core + 30 m far field)

| Region | Why | Data |
|---|---|---|
| **Brdy, Central Bohemia (CZ)** — former military training area, Protected Landscape Area since 2016 | rolling forested hills up to ~860 m (not alpine), spruce/beech/oak, meadows, ponds, villages at the edges; fits the mil-sim theme | DMR 5G + Ortofoto (CC BY 4.0), Copernicus/ESA layers |
| **Białowieża Forest and surrounding farmland (PL)** | lowland primeval oak–hornbeam–lime, alder carr, spruce, pine; fields and villages around it; the reference for "natural" Central European forest | GUGiK NMT + point clouds + orthophoto, **BDL stand species** |
| **Bohemian-Moravian Highlands, e.g. Žďárské vrchy foothills (CZ)** | farmland, spruce forests, fish ponds, villages; classic Central European mosaic | DMR 5G + Ortofoto |
| **Upper Palatinate / Bavarian Forest foothills (DE)** | mixed forest, fields, villages; large training areas nearby (Grafenwöhr, Hohenfels) for a mil-sim flavour — check whether these are covered | DGM1 (CC BY 4.0) |

The first two cover the biome extremes (managed hill forest, natural lowland forest); one of them is enough for M1–M3.

## Downloading the Bavarian data (local only)

Data goes to `data/external/` (git-ignored) or to `$VR_DATA`. Script: [`tools/fetch-bavaria.sh`](../tools/fetch-bavaria.sh).

```sh
# 16 x 16 km DGM1 core around Grafenwöhr (Upper Palatinate): 256 tiles of 1 km, GeoTIFF, EPSG:25832, ~1 GB
tools/fetch-bavaria.sh bbox 701 5503 716 5518

# Official bulk route: Metalink per municipality (8-digit key) or all of Bavaria (09, ~240 GB), needs aria2c
tools/fetch-bavaria.sh metalink 09277140
# Other products by path below /odd/a/, e.g. 40 cm orthophotos per administrative region (091..097, ~1.2 TB total)
tools/fetch-bavaria.sh metalink 093 dop40

DRY_RUN=1 tools/fetch-bavaria.sh bbox 701 5503 716 5518   # list URLs only
```

- **bbox** fetches tiles directly from `https://download1.bayernwolke.de/a/dgm/dgm1/{E_km}_{N_km}.tif` (south-west
  corner in km, UTM zone 32 for the whole state). This URL scheme comes from a third-party library (geokachel),
  not from LDBV documentation; it is cross-checked by the tile of Munich's Marienplatz (`691_5334`).
- **metalink** uses the officially documented route (`https://geodaten.bayern.de/odd/a/<product>/meta/metalink/<code>.meta4`,
  `aria2c -V`); re-running fetches only changed files. Municipality keys and other products are listed on
  geodaten.bayern.de/opengeodata.
- The script validates every tile (HTTP 200 + TIFF magic), skips tiles already present, reports missing ones (tiles
  inside military training areas may be absent) and writes the required attribution into `ATTRIBUTION.txt`.
- **Not verified end to end:** the cloud environment this was written in blocks the Bavarian servers; the logic was
  tested against a local HTTP server. First real run: start with a single tile (`bbox 709 5511 709 5511`).

To get kilometre indices for a place: UTM zone 32 (EPSG:25832) easting/northing of the point, divided by 1000 and
rounded down (e.g. `gdaltransform -s_srs EPSG:4326 -t_srs EPSG:25832` with lon lat input).

## How it feeds the cooker

1. **Terrain:** DTM → 257² `u16` tiles per 256 m cell, base altitude per cell; Copernicus DEM for the far ring
   ([12](12-terrain.md)).
2. **Trees:** LiDAR point clouds (vegetation classes) or DSM − DTM → canopy height model → individual tree detection
   → `TreeInstance` positions and scale; species from BDL stands (PL) or ForestPaths genus + Dominant Leaf Type (CZ,
   DE) ([04](04-data-representation.md)).
3. **Ground cover and layers:** orthophoto + WorldCover → terrain layer weights and grass/crop density maps;
   OSM → roads, paths, field boundaries.
4. **Look-dev:** orthophotos and Reforger/Arma screenshots as visual targets only.

## Vegetation and material assets

| Source | Licence | Use |
|---|---|---|
| Poly Haven, ambientCG | CC0 | ground, bark and rock materials; a few plants |
| Fab (incl. Megascans plants) | Fab Standard License: any engine, products may ship them, **raw assets may not be redistributed** | local use only — never committed to this public repository |
| TreeScanPL10K (Polish TLS scans, 10k trees, 2026) | data licence to confirm on Zenodo (article CC BY-NC-ND) | species structure reference for rigs and LOD |
| Own procedural trees (Blender geometry nodes / Houdini) | ours | the assets that ship with the repository |

## Repository rule

This repository is MIT-licensed and public. Only CC0/CC BY data (with attribution in `ATTRIBUTION.md`) and our own
assets may be committed; everything else stays in a local data directory referenced by the cooker. Attribution lines
required: "© ČÚZK, CC BY 4.0", "Datenquelle: Bayerische Vermessungsverwaltung – www.geodaten.bayern.de",
"ESA WorldCover project 2021 / Contains modified Copernicus Sentinel data", OSM "© OpenStreetMap contributors",
"produced using Copernicus WorldDEM-30 © DLR e.V.", BDL source and acquisition date.
