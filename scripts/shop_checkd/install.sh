#!/bin/bash
# Builds and installs the failure check service on a Debian/Ubuntu host. Run as root from a staged copy
# (deploy.sh does both): install.sh <staged source root>
#
# Needs: build-essential cmake pkg-config libboost-filesystem-dev libeigen3-dev nlohmann-json3-dev
#        libturbojpeg0-dev python3 curl
set -euo pipefail

SRC="${1:?usage: install.sh <staged source root>}"
BUILD="$(mktemp -d)" # root's own: the staged copy belongs to the deploying user, who replaces it next time
trap 'rm -rf "$BUILD"' EXIT

cmake -S "$SRC/scripts/shop_checkd" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" -j"$(nproc)"

id shop-checkd >/dev/null 2>&1 || useradd --system --home-dir /var/lib/shop-checkd --no-create-home --shell /usr/sbin/nologin shop-checkd
install -d -m 755 /opt/shop-checkd
install -m 755 "$BUILD/shop_check" /opt/shop-checkd/shop_check
install -m 644 "$SRC/scripts/shop_checkd/shop_checkd.py" /opt/shop-checkd/shop_checkd.py
install -m 644 "$SRC/scripts/shop_checkd/shop-checkd.service" /etc/systemd/system/shop-checkd.service
systemctl daemon-reload
systemctl enable --quiet shop-checkd
systemctl restart shop-checkd

for _ in $(seq 1 10); do
    if curl -fsS http://127.0.0.1:8765/health; then
        echo
        echo "shop-checkd is running (data in /var/lib/shop-checkd)."
        for _ in $(seq 1 10); do [ -s /var/lib/shop-checkd/token ] && break; sleep 1; done
        echo "Slicer: Preferences > Failure check service token: $(cat /var/lib/shop-checkd/token)"
        exit 0
    fi
    sleep 1
done
echo "shop-checkd did not answer; see: journalctl -u shop-checkd -n 50" >&2
exit 1
