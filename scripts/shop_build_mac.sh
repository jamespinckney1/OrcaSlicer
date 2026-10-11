#!/bin/bash
# Print-shop: incremental macOS build, then install into /Applications (see shop_install_mac.sh).
#
# Builds against the active Xcode's own macOS SDK. After a macOS update the Command Line Tools can
# carry a newer SDK than the Xcode linker understands ("tapi error: malformed file ... unknown
# architecture"); pinning SDKROOT to Xcode's SDK keeps the linker and the SDK in step.
#
# Usage: scripts/shop_build_mac.sh            # build (-s -x -b) and install
#        scripts/shop_build_mac.sh --no-install
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
XCODE_SDK="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
DEV="$(xcode-select -p)"
if [ -z "$XCODE_SDK" ] || [[ "$XCODE_SDK" == /Library/Developer/CommandLineTools/* ]]; then
    XCODE_SDK="$(ls -d "$DEV"/Platforms/MacOSX.platform/Developer/SDKs/MacOSX[0-9]*.sdk 2>/dev/null | sort -V | tail -1)"
fi
if [ -z "$XCODE_SDK" ] || [ ! -d "$XCODE_SDK" ]; then
    echo "No macOS SDK found under $DEV" >&2
    exit 1
fi
export SDKROOT="$XCODE_SDK"
echo "SDKROOT=$SDKROOT"

cd "$ROOT"
./build_release_macos.sh -s -x -b -j 8
if [ "${1:-}" != "--no-install" ]; then
    "$ROOT/scripts/shop_install_mac.sh"
fi
