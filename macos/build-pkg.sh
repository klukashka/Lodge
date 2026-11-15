#!/usr/bin/env bash
# Builds a macOS installer package (lodge-<version>.pkg) — the macOS counterpart of the .deb in deb/.
# Usage: macos/build-pkg.sh [version]   (run from the repository root, or via `make pkg`)
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
version="${1:-${VERSION:-0.0.0}}"
out="$root/lodge-$version.pkg"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

make -C "$root" clean all VERSION="$version" >/dev/null
make -C "$root" install DESTDIR="$stage/root" PREFIX=/usr/local >/dev/null

pkgbuild \
  --root "$stage/root" \
  --identifier com.github.klukashka.lodge \
  --version "$version" \
  --install-location / \
  "$out"

echo "Built: $out"
