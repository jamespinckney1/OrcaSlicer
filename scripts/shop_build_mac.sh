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

usage() { sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; }
INSTALL=1
for arg in "$@"; do
    case "$arg" in
        --no-install) INSTALL=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

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
# -b skips CMake's configure step, but the cache holds absolute SDK paths: after Xcode replaces its SDK
# (26.5 -> 27.0 on 2026-10-13) every one of them is stale. Then drop the cache and configure afresh.
BUILD_ARGS=(-s -x -j "$(sysctl -n hw.ncpu)")
CACHE="$ROOT/build/arm64/CMakeCache.txt"
if [ -f "$CACHE" ]; then
    CACHED_SDK="$(sed -n 's/^CMAKE_OSX_SYSROOT:PATH=//p' "$CACHE")"
    if [ ! -d "$CACHED_SDK" ] || [ "$(cd "$CACHED_SDK" && pwd -P)" != "$(cd "$SDKROOT" && pwd -P)" ]; then
        echo "SDK changed since the last configure ($CACHED_SDK); reconfiguring from scratch"
        rm -f "$CACHE"
    else
        BUILD_ARGS+=(-b)
    fi
fi
./build_release_macos.sh "${BUILD_ARGS[@]}"
if [ "$INSTALL" = 1 ]; then
    "$ROOT/scripts/shop_install_mac.sh"
fi
