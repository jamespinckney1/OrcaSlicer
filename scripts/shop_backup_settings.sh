#!/bin/bash
# Print-shop: back up (or restore) the slicer settings a rebuild from source does not carry.
#
# The source holds no personal values; they live in the app data folder: the app config (printer
# connections, ntfy topic, Box slot choices), user presets and physical printers, and the shop/ folder
# (camera calibration, printer list, preset backups). Failure-check job folders (G-code and photos,
# hundreds of MB) are left out unless --with-jobs is given.
#
# Usage: scripts/shop_backup_settings.sh [--with-jobs] <backup folder>   # copy settings into the folder
#        scripts/shop_backup_settings.sh --restore <backup folder>       # copy them back (slicer closed)
#
# Keep the backup somewhere private (iCloud Drive, an external disk); it contains the ntfy topic and
# any printer access codes.
set -euo pipefail

DATA="$HOME/Library/Application Support/OrcaSlicer"
ITEMS=(OrcaSlicer.conf user printers shop)

usage() { sed -n '9,10p' "$0" | sed 's/^# //'; exit 2; }

restore=0
with_jobs=0
while [ $# -gt 0 ]; do
    case "$1" in
        --restore) restore=1 ;;
        --with-jobs) with_jobs=1 ;;
        -h|--help) usage ;;
        -*) echo "Unknown option: $1" >&2; usage ;;
        *) break ;;
    esac
    shift
done
[ $# -eq 1 ] || usage
DEST="$1"

if [ "$restore" -eq 1 ]; then
    if pgrep -x OrcaSlicer >/dev/null; then
        echo "Close OrcaSlicer first: it rewrites its config when it quits." >&2
        exit 1
    fi
    [ -f "$DEST/OrcaSlicer.conf" ] || { echo "No backup found in $DEST" >&2; exit 1; }
    mkdir -p "$DATA"
    for item in "${ITEMS[@]}"; do
        [ -e "$DEST/$item" ] || continue
        rsync -a "$DEST/$item" "$DATA/"
        echo "restored $item"
    done
    exit 0
fi

mkdir -p "$DEST"
excludes=()
[ "$with_jobs" -eq 1 ] || excludes=(--exclude 'shop/failure_check/jobs/')
for item in "${ITEMS[@]}"; do
    [ -e "$DATA/$item" ] || continue
    rsync -a --delete ${excludes[@]+"${excludes[@]}"} "$DATA/$item" "$DEST/"
done
date '+%Y-%m-%d %H:%M:%S' > "$DEST/BACKED_UP_AT"
echo "Backed up $(du -sh "$DEST" | cut -f1) to $DEST"
