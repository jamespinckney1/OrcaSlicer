#!/bin/bash
# Deploys the failure check service to a Linux host over SSH: copies only what it needs (this folder and
# the comparison engine, src/slic3r/GUI/FailureCheck.*), then builds and installs it there with sudo.
#
# Usage: scripts/shop_checkd/deploy.sh <ssh host>     e.g. scripts/shop_checkd/deploy.sh slicer
set -euo pipefail

HOST="${1:?usage: deploy.sh <ssh host>}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/src/slic3r/GUI" "$STAGE/scripts/shop_checkd"
cp "$ROOT/src/slic3r/GUI/FailureCheck.cpp" "$ROOT/src/slic3r/GUI/FailureCheck.hpp" "$STAGE/src/slic3r/GUI/"
cp "$ROOT"/scripts/shop_checkd/{CMakeLists.txt,shop_check.cpp,shop_checkd.py,shop-checkd.service,install.sh} "$STAGE/scripts/shop_checkd/"

ssh "$HOST" 'rm -rf ~/shop-checkd-src && mkdir -p ~/shop-checkd-src'
tar -C "$STAGE" -cf - . | ssh "$HOST" 'tar -C ~/shop-checkd-src -xf -'
ssh "$HOST" 'sudo bash ~/shop-checkd-src/scripts/shop_checkd/install.sh ~/shop-checkd-src'
