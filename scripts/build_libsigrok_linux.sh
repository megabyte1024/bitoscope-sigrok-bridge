#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later

set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
commit=${COMMIT:-0bc2487}
default_source="$here/third_party/libsigrok"
source_dir=${1:-$default_source}
build_dir="$here/third_party/build-libsigrok-linux"
install_dir="$here/third_party/install-libsigrok-linux"

for tool in git autoreconf libtoolize pkg-config make cc cmake; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "build_libsigrok_linux.sh: $tool is not installed" >&2
    exit 1
  fi
done
for module in glib-2.0 libzip libusb-1.0; do
  if ! pkg-config --exists "$module"; then
    echo "build_libsigrok_linux.sh: pkg-config does not find $module (libglib2.0-dev, libzip-dev, libusb-1.0-0-dev)" >&2
    exit 1
  fi
done

if [ ! -f "$source_dir/configure.ac" ]; then
  if [ "$source_dir" != "$default_source" ]; then
    echo "build_libsigrok_linux.sh: no libsigrok source in $source_dir" >&2
    exit 1
  fi
  mkdir -p "$here/third_party"
  git -c core.autocrlf=false clone https://github.com/sigrokproject/libsigrok.git "$source_dir"
  git -C "$source_dir" checkout "$commit"
fi

if [ "$source_dir" = "$default_source" ]; then
  have=$(git -C "$source_dir" rev-parse HEAD)
  want=$(git -C "$source_dir" rev-parse "$commit^{commit}")
  if [ "$have" != "$want" ]; then
    echo "build_libsigrok_linux.sh: $source_dir is at $have, not at $commit; check it out or name another source" >&2
    exit 1
  fi
fi

if [ ! -x "$source_dir/configure" ]; then
  (cd "$source_dir" && ./autogen.sh)
fi

mkdir -p "$build_dir"
if [ ! -f "$build_dir/Makefile" ]; then
  (cd "$build_dir" && "$source_dir/configure" --prefix="$install_dir" --enable-shared --disable-static --disable-bindings)
fi
make -C "$build_dir" -j"$(cmake -P "$here/cmake/BitoscopeBuildJobs.cmake")"
make -C "$build_dir" install

changes=0
commit_built=unknown
if [ -d "$source_dir/.git" ] || git -C "$source_dir" rev-parse --git-dir >/dev/null 2>&1; then
  commit_built=$(git -C "$source_dir" rev-parse --short HEAD)
  changes=$(git -C "$source_dir" status --porcelain --untracked-files=no | wc -l | tr -d ' ')
fi
{
  echo "$source_dir"
  echo "commit $commit_built, $changes local changes"
} > "$install_dir/libsigrok-source.txt"

echo "libsigrok $(PKG_CONFIG_PATH="$install_dir/lib/pkgconfig" pkg-config --modversion libsigrok) is in $install_dir"
echo "(commit $commit_built, $changes local changes)"
