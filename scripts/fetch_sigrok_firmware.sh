#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
version=${1:-0.1.7}
name="sigrok-firmware-fx2lafw-bin-$version"
target="$here/third_party/sigrok-firmware"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -fsSL -o "$work/$name.tar.gz" "https://sigrok.org/download/binary/sigrok-firmware-fx2lafw/$name.tar.gz"
tar -xzf "$work/$name.tar.gz" -C "$work"
rm -rf "$target"
mkdir -p "$target"
cp "$work/$name"/* "$target"/
echo "fx2lafw firmware $version is in $target ($(ls "$target"/*.fw | wc -l) files)"
sha256sum "$work/$name.tar.gz"
