#!/usr/bin/env bash
# Download Bavarian open geodata (CC BY 4.0) into a local, git-ignored directory.
#
#   tools/fetch-bavaria.sh bbox E0 N0 E1 N1        1 km DGM1 tiles, EPSG:25832, kilometre indices of the
#                                                  south-west tile corners, inclusive (direct HTTPS, curl)
#   tools/fetch-bavaria.sh metalink CODE [PRODUCT] official bulk download via Metalink (aria2c)
#                                                  CODE: 09 = all Bavaria, 8-digit municipality key, ...
#                                                  PRODUCT: path below /odd/a/, default dgm/dgm1
#
# Options (environment):  VR_DATA=<dir>  data root (default: <repo>/data/external)
#                         JOBS=<n>       parallel downloads (default 8)
#                         DRY_RUN=1      print URLs only
#
# Example, 16 x 16 km around Grafenwoehr (Upper Palatinate), ~1 GB:
#   tools/fetch-bavaria.sh bbox 701 5503 716 5518
#
# Licence: CC BY 4.0. Required attribution:
#   "Datenquelle: Bayerische Vermessungsverwaltung – www.geodaten.bayern.de"
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
root="${VR_DATA:-$repo/data/external}/bavaria"
jobs="${JOBS:-8}"
attribution="Datenquelle: Bayerische Vermessungsverwaltung – www.geodaten.bayern.de (CC BY 4.0)"

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 1; }

write_attribution() {
    mkdir -p "$1"
    printf '%s\nDownloaded %s from %s\n' "$attribution" "$(date -u +%Y-%m-%d)" "$2" > "$1/ATTRIBUTION.txt"
}

fetch_tile() {  # $1 = url, $2 = destination; exit status 0 = ok/skip, 1 = missing, 2 = error
    local url="$1" dst="$2" tmp code
    [[ -s "$dst" ]] && return 0
    tmp="$dst.part"
    code="$(curl -sS -L --retry 3 --retry-delay 2 -m 300 -o "$tmp" -w '%{http_code}' "$url" || echo 000)"
    if [[ "$code" == 200 ]] && head -c 4 "$tmp" | od -An -tx1 | tr -d ' \n' | grep -qE '^(49492a00|4d4d002a|49492b00)$'; then
        mv "$tmp" "$dst"; return 0
    fi
    rm -f "$tmp"
    [[ "$code" == 404 ]] && { echo "missing $url" >&2; return 1; }
    echo "error $code $url" >&2; return 2
}
export -f fetch_tile

cmd_bbox() {
    [[ $# -eq 4 ]] || usage
    local e0="$1" n0="$2" e1="$3" n1="$4" dir="$root/dgm1" base="https://download1.bayernwolke.de/a/dgm/dgm1"
    (( e0 <= e1 && n0 <= n1 )) || { echo "bbox: need E0<=E1 and N0<=N1" >&2; exit 1; }
    local list; list="$(mktemp)"
    for ((e = e0; e <= e1; e++)); do
        for ((n = n0; n <= n1; n++)); do
            printf '%s/%d_%d.tif %s/%d_%d.tif\n' "$base" "$e" "$n" "$dir" "$e" "$n" >> "$list"
        done
    done
    echo "$(wc -l < "$list") tiles -> $dir"
    if [[ -n "${DRY_RUN:-}" ]]; then cut -d' ' -f1 "$list"; rm -f "$list"; return; fi
    mkdir -p "$dir"
    write_attribution "$dir" "$base"
    local rc=0
    xargs -P "$jobs" -L 1 bash -c 'fetch_tile "$0" "$1"' < "$list" || rc=$?
    rm -f "$list"
    echo "done: $(find "$dir" -name '*.tif' | wc -l) tiles present"
    # xargs returns 123 if any tile was missing or failed; missing tiles (e.g. inside military areas) are reported above.
    [[ $rc -eq 0 || $rc -eq 123 ]] || exit "$rc"
}

cmd_metalink() {
    [[ $# -ge 1 ]] || usage
    local code="$1" product="${2:-dgm/dgm1}"
    local url="https://geodaten.bayern.de/odd/a/$product/meta/metalink/$code.meta4"
    local dir="$root/${product//\//_}/$code"
    echo "$url -> $dir"
    [[ -n "${DRY_RUN:-}" ]] && return
    command -v aria2c >/dev/null || { echo "aria2c not found (apt install aria2 / dnf install aria2)" >&2; exit 1; }
    write_attribution "$dir" "$url"
    # -V verifies checksums; re-running fetches only new or changed files.
    aria2c -V --follow-metalink=mem --timeout=300 -j "$jobs" --dir="$dir" "$url"
}

case "${1:-}" in
    bbox)     shift; cmd_bbox "$@" ;;
    metalink) shift; cmd_metalink "$@" ;;
    *)        usage ;;
esac
