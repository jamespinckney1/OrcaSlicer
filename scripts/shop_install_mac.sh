#!/bin/bash
# Print-shop: install the locally built OrcaSlicer into /Applications with a stable code signature.
#
# A build is only ad-hoc signed, and it runs from inside ~/Documents, so macOS asks for Documents access
# on every launch and forgets the answer after every rebuild (privacy grants are tied to the signature).
# Installed in /Applications and signed with a real identity, the app keeps its grants across rebuilds
# and appears in System Settings > Privacy & Security.
#
# Usage: scripts/shop_install_mac.sh            # signs with the first "Apple Development" identity
#        SIGN_IDENTITY="..." scripts/shop_install_mac.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/build/arm64/OrcaSlicer/OrcaSlicer.app"
DEST="/Applications/OrcaSlicer.app"

if [ ! -d "$SRC" ]; then
    echo "No build at $SRC" >&2
    exit 1
fi
if pgrep -f "$DEST/Contents/MacOS/OrcaSlicer" >/dev/null; then
    echo "OrcaSlicer is running from $DEST; quit it first." >&2
    exit 1
fi

IDENTITY="${SIGN_IDENTITY:-$(security find-identity -v -p codesigning | sed -n 's/.*"\(Apple Development: [^"]*\)".*/\1/p' | head -1)}"
if [ -z "$IDENTITY" ]; then
    echo "No code-signing identity found; set SIGN_IDENTITY." >&2
    exit 1
fi

STAGE_DIR="$(mktemp -d)"
STAGE="$STAGE_DIR/OrcaSlicer.app"
PREV="$DEST.previous-$$"
NEXT="$DEST.new-$$"
# On any failure: drop the staging copy and, if the old app was already moved aside, put it back,
# so a failed install never leaves /Applications without OrcaSlicer.
cleanup() {
    rm -rf "$STAGE_DIR" "$NEXT"
    if [ -d "$PREV" ] && [ ! -d "$DEST" ]; then
        mv "$PREV" "$DEST"
        echo "Install failed; restored the previous $DEST" >&2
    fi
}
trap cleanup EXIT

ditto "$SRC" "$STAGE"
xattr -cr "$STAGE"
codesign --force --deep --timestamp=none --sign "$IDENTITY" "$STAGE"
codesign --verify --deep --strict "$STAGE"

# Copy beside the destination first, then swap by rename: the copy is the slow step that can fail,
# and the old app stays in place until it has succeeded.
ditto "$STAGE" "$NEXT"
if [ -d "$DEST" ]; then
    mv "$DEST" "$PREV"
fi
mv "$NEXT" "$DEST"
rm -rf "$PREV"

echo "Installed $DEST, signed by: $IDENTITY"
codesign -dv "$DEST" 2>&1 | grep -E 'Identifier=|TeamIdentifier='
