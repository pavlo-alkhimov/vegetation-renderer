#!/usr/bin/env bash
# Download the Poly Haven plant models used by the grass near field (CC0) into a local, git-ignored directory.
#
#   tools/fetch-polyhaven.sh [RES]      RES = texture resolution: 1k, 2k (default), 4k
#
# Options (environment):  VR_DATA=<dir>  data root (default: <repo>/data/external)
#
# Per asset: glTF + bin (geometry), diffuse, alpha (if the asset has one), OpenGL normal, AO/rough/metal (JPG).
# Licence: CC0 (https://polyhaven.com/license); no attribution required, the authors are listed per asset on
# polyhaven.com.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root="${VR_DATA:-$repo/data/external}/polyhaven"
res="${1:-2k}"
assets=(grass_medium_01 grass_medium_02 dandelion_01 nettle_plant weed_plant_02 celandine_01 periwinkle_plant fern_02)
base="https://dl.polyhaven.org/file/ph-assets/Models"

fetch() {  # $1 = url, $2 = destination, $3 = optional (1 = a 404 is fine)
    local url="$1" dst="$2" code
    [[ -s "$dst" ]] && return 0
    mkdir -p "$(dirname "$dst")"
    code="$(curl -sS -L --retry 3 --retry-delay 2 -m 300 -o "$dst.part" -w '%{http_code}' "$url" || echo 000)"
    if [[ "$code" == 200 ]]; then mv "$dst.part" "$dst"; echo "  $(basename "$dst")"; return 0; fi
    rm -f "$dst.part"
    if [[ "${3:-0}" == 1 && "$code" == 404 ]]; then echo "  $(basename "$dst"): none"; return 0; fi
    echo "error: HTTP $code for $url" >&2
    return 1
}

for a in "${assets[@]}"; do
    echo "$a"
    d="$root/$a"
    fetch "$base/gltf/$res/$a/${a}_$res.gltf" "$d/${a}_$res.gltf"
    # Geometry is shared by all resolutions and lives under varying paths; take its URL from the API.
    bin_url="$(curl -sS -L --retry 3 "https://api.polyhaven.com/files/$a" | grep -o "https://[^\"]*/$a\.bin" | head -n 1)"
    [[ -n "$bin_url" ]] || { echo "error: no .bin URL for $a" >&2; exit 1; }
    fetch "$bin_url" "$d/$a.bin"
    for m in diff nor_gl arm; do fetch "$base/jpg/$res/$a/${a}_${m}_$res.jpg" "$d/textures/${a}_${m}_$res.jpg"; done
    fetch "$base/jpg/$res/$a/${a}_alpha_$res.jpg" "$d/textures/${a}_alpha_$res.jpg" 1
done
printf 'Poly Haven assets (CC0, https://polyhaven.com/license), downloaded %s\n' "$(date -u +%Y-%m-%d)" > "$root/LICENSE.txt"
echo "done: $root"
