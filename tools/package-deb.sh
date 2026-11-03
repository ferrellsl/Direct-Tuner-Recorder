#!/bin/bash
# Builds dist/directtuner-recorder_<version>_amd64.deb and a .tar.gz of the program and its systemd unit.
# Run on Ubuntu 22.04 or later (or Debian 12) with cmake, ninja-build, g++ and libcurl4-openssl-dev.
# The package works on that release and newer ones.
set -e -o pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^project(DirectTunerRecorder .*VERSION \([0-9.]*\)).*/\1/p' "$ROOT/CMakeLists.txt")
B=${BUILD:-$HOME/directtuner-recorder-build}
cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$B"

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
install -Dm755 "$B/directtuner-recorder" "$STAGE/usr/bin/directtuner-recorder"
strip "$STAGE/usr/bin/directtuner-recorder"
install -Dm644 "$ROOT/packaging/linux/directtuner-recorder.service" \
  "$STAGE/lib/systemd/system/directtuner-recorder.service"
install -Dm644 "$ROOT/LICENSE.md" "$STAGE/usr/share/doc/directtuner-recorder/copyright"
install -Dm644 "$ROOT/README.md" "$STAGE/usr/share/doc/directtuner-recorder/README.md"
mkdir -p "$STAGE/DEBIAN"
for f in postinst prerm postrm; do install -m755 "$ROOT/packaging/linux/$f" "$STAGE/DEBIAN/$f"; done
cat > "$STAGE/DEBIAN/control" <<EOF
Package: directtuner-recorder
Version: $VERSION
Architecture: amd64
Maintainer: ferrellsl <ferrellsl@users.noreply.github.com>
Depends: libc6 (>= 2.35), libstdc++6 (>= 12), libcurl4 (>= 7.81), adduser
Section: video
Priority: optional
Homepage: https://github.com/ferrellsl/Direct-Tuner-Recorder
Description: Background recording for SiliconDust HDHomeRun tuners and Kodi
 Records from an HDHomeRun tuner whether or not Kodi is running, and shares
 one set of timers, recordings and tuners with every Kodi that uses the
 Direct Tuner PVR add-on. Install it on one computer only.
EOF

mkdir -p "$ROOT/dist"
DEB="$ROOT/dist/directtuner-recorder_${VERSION}_amd64.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$DEB" >/dev/null
echo "Built $DEB"

# The same program for other distributions: unpack and set up the service yourself.
TAR="$ROOT/dist/directtuner-recorder-${VERSION}-linux-x86_64.tar.gz"
tar -czf "$TAR" -C "$STAGE" usr/bin/directtuner-recorder lib/systemd/system/directtuner-recorder.service \
  usr/share/doc/directtuner-recorder
echo "Built $TAR"
