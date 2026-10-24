# Sigrok bridge driver for bitoscope

Driver for the USB logic analyzers supported by
[libsigrok](https://sigrok.org/wiki/Libsigrok), for use with the bitoscope host.

The driver is installed and updated independently of bitoscope: at start-up the application and the
driver negotiate the protocol version, so older and newer versions work together.

**The only tested logic analyzer is one of the FX2 logic analyzers. No other logic
analyzer supported by libsigrok is tested.**

## Features

- bitoscope can work with the USB logic analyzers that libsigrok supports.

### Supported devices

| libsigrok driver | USB ids (vendor:product) |
|---|---|
| `fx2lafw` | `08a9:0014`, `08a9:0015`, `08a9:0009`, `08a9:0005`, `0925:3881`, `04b4:8613`, `16d0:0498`, `1d50:608c`, `1d50:608d`, `1d50:608f` |
| `kingst-la2016` | `77a1:01a2` |
| `saleae-logic16` | `21a9:1001` |
| `saleae-logic-pro` | `21a9:1006` |
| `dreamsourcelab-dslogic` | `2a0e:0001`, `2a0e:0002`, `2a0e:0003`, `2a0e:0020`, `2a0e:0021` |
| `greatfet` | `1d50:60e6` |

## Prerequisites

Linux and Windows; macOS is not supported yet.

- CMake 3.24 or newer
- A C++23 compiler: GCC 13 or newer on Linux, MSVC from Visual Studio 2022 (or its Build Tools, with
  the "Desktop development with C++" workload) on Windows
- Git, to fetch libsigrok and Cap'n Proto
- glib, libzip and libusb-1.0

### Linux

Package names are Debian/Ubuntu ones:

```sh
sudo apt install build-essential cmake git autoconf automake libtool pkg-config libglib2.0-dev libzip-dev libusb-1.0-0-dev
```

### Windows

glib, libzip and libusb come from [vcpkg](https://github.com/microsoft/vcpkg), installed into `C:\vcpkg`;
the build installs the three itself. libsigrok is built with [MSYS2](https://www.msys2.org), installed
into `C:\msys64`, in its UCRT64 environment. The same in Command Prompt and PowerShell:

```bat
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat
C:\msys64\usr\bin\bash.exe -lc "pacman -S --needed --noconfirm base-devel mingw-w64-ucrt-x86_64-{gcc,autotools,pkgconf,tools-git,ntldd}"
```

## How to compile

### Linux

```sh
git clone https://github.com/megabyte1024/bitoscope-sigrok-bridge.git
cd bitoscope-sigrok-bridge
sh scripts/build_libsigrok_linux.sh
sh scripts/fetch_sigrok_firmware.sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(cmake -P cmake/BitoscopeBuildJobs.cmake)
```

### Windows

Command Prompt or PowerShell:

```bat
git clone https://github.com/megabyte1024/bitoscope-sigrok-bridge.git
cd bitoscope-sigrok-bridge
scripts\install_msvc_dlls.cmd
cmake -S . -B build
cmake --build build --config Release --target bridge_usb
C:\msys64\usr\bin\env.exe MSYSTEM=UCRT64 CHERE_INVOKING=1 /usr/bin/bash -lc "sh scripts/build_libsigrok.sh"
C:\msys64\usr\bin\env.exe MSYSTEM=UCRT64 CHERE_INVOKING=1 /usr/bin/bash -lc "sh scripts/fetch_sigrok_firmware.sh"
cmake -S . -B build
cmake --build build --config Release
```

## Install

bitoscope looks for drivers in the `drivers/` folder next to `bitoscope_gui`. Copy the whole content
of the driver folder (see Output) there, subfolders included.

Firmware is looked up in the folder named by `SIGROK_FIRMWARE_DIR`, then in `sigrok-firmware/`
beside the driver, then, on Linux, in `/usr/local/share/sigrok-firmware` and
`/usr/share/sigrok-firmware`. On Linux the device's USB node must be readable (udev rules, for
example those of the `libsigrok-dev` and `sigrok-firmware-fx2lafw` packages).

## Output

- `build/drivers/`: the driver folder (`build\drivers\Release\` on Windows)
  - `sigrok_bridge`: the driver executable (`sigrok_bridge.exe` on Windows)
  - `lib/libbridge_usb.so`, `lib/libsigrok.so.4`: the USB shim and libsigrok (on Windows their DLLs
    lie beside the driver)
  - `sigrok-firmware/`: the firmware, if fetched

## License

GPL-3.0-or-later, see `COPYING`; the driver links libsigrok, which is GPL-3.0-or-later. The bitoscope SDK sources in `sdk/` are MIT.
