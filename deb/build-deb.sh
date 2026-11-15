#!/usr/bin/env bash
# Builds the .deb (Debian/Ubuntu counterpart of macos/build-pkg.sh).
# Usage: deb/build-deb.sh [version]   (run on Linux; needs gcc and debhelper)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:-$(cat "$root/VERSION")}"

# Keep debian/changelog in sync with the single source of truth (VERSION).
changelog="$root/deb/debian/changelog"
sed "1s/^lodge ([^)]*)/lodge ($version)/" "$changelog" > "$changelog.new"
mv "$changelog.new" "$changelog"

cd "$root/deb"
dpkg-buildpackage -us -uc -b
