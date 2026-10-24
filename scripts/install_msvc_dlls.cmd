@REM SPDX-License-Identifier: GPL-3.0-or-later
@echo off
setlocal
if "%VCPKG_ROOT%"=="" set "VCPKG_ROOT=C:\vcpkg"
if not exist "%VCPKG_ROOT%\vcpkg.exe" (
  echo install_msvc_dlls.cmd: no %VCPKG_ROOT%\vcpkg.exe: install vcpkg, or set VCPKG_ROOT 1>&2
  exit /b 1
)
"%VCPKG_ROOT%\vcpkg.exe" install --x-manifest-root="%~dp0vcpkg" --x-install-root="%~dp0..\third_party\vcpkg_installed" --triplet x64-windows
exit /b %ERRORLEVEL%
