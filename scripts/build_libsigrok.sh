#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later

set -e

if [ "$MSYSTEM" != "UCRT64" ]; then
  echo "build_libsigrok.sh: run this in the MSYS2 UCRT64 shell (MSYSTEM is '$MSYSTEM')" >&2
  exit 1
fi

root=$(cd "$(dirname "$0")/.." && pwd)
wanted_commit=${COMMIT:-0bc2487}
third_party="$root/third_party"
default_source="$third_party/libsigrok"
out="${SIGROK_MSVC_OUT:-$third_party/sigrok-msvc}"
vcpkg="$third_party/vcpkg_installed/x64-windows"
shim_pc="$third_party/build-usbshim-pc"
shim_dir="${BRIDGE_BUILD_DIR:-$root/build}/usbshim/${SHIM_CONFIG:-Release}"

if [ $# -gt 1 ]; then
  echo "usage: build_libsigrok.sh [libsigrok source directory]" >&2
  exit 2
fi

git=
for candidate in git "/c/Program Files/Git/cmd/git.exe" "/c/Program Files/Git/bin/git.exe"; do
  if [ -z "$git" ] && command -v "$candidate" >/dev/null 2>&1; then
    git=$candidate
  fi
done

for needed in "$vcpkg/lib/glib-2.0.lib" "$vcpkg/lib/zip.lib" "$vcpkg/lib/pkgconfig/glib-2.0.pc"; do
  if [ ! -f "$needed" ]; then
    echo "build_libsigrok.sh: no $needed: run scripts\\install_msvc_dlls.cmd first" >&2
    exit 1
  fi
done
for needed in "$shim_dir/bridge_usb.lib" "$shim_dir/bridge_usb.dll"; do
  if [ ! -f "$needed" ]; then
    echo "build_libsigrok.sh: no $needed: cmake --build build --config ${SHIM_CONFIG:-Release} --target bridge_usb" >&2
    exit 1
  fi
done
if [ ! -f "${1:-$default_source}/configure.ac" ]; then
  if [ $# -eq 1 ]; then
    echo "build_libsigrok.sh: no libsigrok source in $1" >&2
    exit 1
  fi
  if [ -z "$git" ]; then
    echo "build_libsigrok.sh: git is not installed (pacman -S git, or Git for Windows)" >&2
    exit 1
  fi
  mkdir -p "$third_party"
  # WHY: LF line endings: autogen.sh and configure do not run with CRLF.
  (cd "$third_party" && "$git" -c core.autocrlf=false clone https://github.com/sigrokproject/libsigrok.git libsigrok)
  (cd "$default_source" && "$git" checkout "$wanted_commit")
fi
source_dir=$(cd "${1:-$default_source}" && pwd)
name=$(basename "$source_dir")
build_dir="$third_party/build-$name-vcpkg"
install_dir="$third_party/install-$name-vcpkg"

if [ -n "$git" ] && commit=$(cd "$source_dir" && "$git" rev-parse HEAD 2>/dev/null); then
  changes=$(cd "$source_dir" && "$git" status --porcelain --untracked-files=no | wc -l | tr -d ' ')
  if [ "$changes" = 0 ]; then
    state="no local changes"
  else
    state="$changes files with local changes"
  fi
else
  commit="unknown"
  changes=unknown
  state="unknown (no git found, or not a git clone)"
fi

if [ "$source_dir" = "$default_source" ]; then
  if [ "$changes" = unknown ]; then
    echo "build_libsigrok.sh: WARNING: cannot check that $source_dir is unchanged (no git found)" >&2
  elif [ "$changes" != 0 ]; then
    echo "build_libsigrok.sh: $source_dir has local changes; the bridge is built against unchanged libsigrok" >&2
    exit 1
  else
    want=$(cd "$source_dir" && "$git" rev-parse "$wanted_commit^{commit}")
    if [ "$commit" != "$want" ]; then
      echo "build_libsigrok.sh: $source_dir is at $commit, not at $wanted_commit; check it out or name another source" >&2
      exit 1
    fi
  fi
fi

mkdir -p "$shim_pc"
(cd "$shim_pc" && gendef "$shim_dir/bridge_usb.dll" &&
  dlltool -d bridge_usb.def -D bridge_usb.dll -l libbridge_usb.dll.a)
libusb_version=$(PKG_CONFIG_LIBDIR="$vcpkg/lib/pkgconfig" pkg-config --modversion libusb-1.0)
cat > "$shim_pc/libusb-1.0.pc" <<EOF
Name: libusb-1.0
Description: the bitoscope sigrok bridge's USB shim, which forwards to libusb-1.0 ($libusb_version)
Version: $libusb_version
Libs: -L$(cygpath -m "$shim_pc") -lbridge_usb
Cflags: -I$(cygpath -m "$vcpkg")/include/libusb-1.0
EOF

if [ ! -x "$source_dir/configure" ]; then
  (cd "$source_dir" && ./autogen.sh)
fi
mkdir -p "$build_dir"
cd "$build_dir"
if [ ! -f Makefile ]; then
  PKG_CONFIG_LIBDIR="$shim_pc:$vcpkg/lib/pkgconfig:$vcpkg/share/pkgconfig" PKG_CONFIG_PATH= \
    "$source_dir/configure" --prefix="$install_dir" --enable-shared --disable-static --disable-bindings
fi
physical_cores=$(grep -E '^(physical id|core id)' /proc/cpuinfo | paste - - | sort -u | wc -l)
[ "$physical_cores" -gt 0 ] || physical_cores=1
make -j"$physical_cores"
make install

imports=$(objdump -p "$install_dir/bin/libsigrok-4.dll" | grep 'DLL Name:')
for wanted in bridge_usb.dll glib-2.0-0.dll zip.dll; do
  if ! echo "$imports" | grep -q "DLL Name: $wanted"; then
    echo "build_libsigrok.sh: libsigrok-4.dll must import $wanted; it imports:" >&2
    echo "$imports" >&2
    echo "build_libsigrok.sh: remove $build_dir and run this again" >&2
    exit 1
  fi
done
if echo "$imports" | grep -qi 'DLL Name: \(libusb\|libglib\|libzip\)'; then
  echo "build_libsigrok.sh: libsigrok-4.dll imports a MinGW library of libusb, glib or libzip:" >&2
  echo "$imports" >&2
  echo "build_libsigrok.sh: remove $build_dir and run this again" >&2
  exit 1
fi

rm -rf "$out"
mkdir -p "$out/include" "$out/lib" "$out/bin"

{
  cygpath -m "$source_dir"
  echo "commit: $commit"
  echo "state: $state"
  echo "built: $(date '+%Y-%m-%d %H:%M:%S')"
  echo "usb: libsigrok-4.dll imports bridge_usb.dll (the bridge's USB shim, MSVC), which forwards to libusb-1.0.dll (vcpkg, MSVC)"
  echo "libraries: glib $(PKG_CONFIG_LIBDIR="$vcpkg/lib/pkgconfig" pkg-config --modversion glib-2.0) and libzip $(PKG_CONFIG_LIBDIR="$vcpkg/lib/pkgconfig" pkg-config --modversion libzip) of vcpkg (MSVC)"
} > "$out/libsigrok-source.txt"

cp -r "$install_dir/include/libsigrok" "$out/include/"

cp "$install_dir/bin/libsigrok-4.dll" "$out/bin/"
ntldd -R "$out/bin/libsigrok-4.dll" | tr '\' '/' | grep -i '/ucrt64/bin/' | awk '{print $3}' | sort -u |
while read -r dll; do
  cp "$(cygpath -u "$dll")" "$out/bin/"
done

cd "$out/lib"
gendef "$out/bin/libsigrok-4.dll"

echo "build_libsigrok.sh: done, $out holds:"
cat "$out/libsigrok-source.txt"
ls "$out/bin" "$out/lib"
