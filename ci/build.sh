#!/bin/sh
# CI / Linux build: cross-compiles GTA5VR.asi with mingw-w64 and packages the release zip.
# Downloads the official Khronos OpenXR loader (Apache-2.0) instead of keeping binaries in git.
set -e
cd "$(dirname "$0")/.."
VER=0.6.5
OXR=1.1.63
mkdir -p build
if [ ! -f build/oxr/include/openxr/openxr.h ]; then
  curl -sSL -o build/oxr.zip "https://github.com/KhronosGroup/OpenXR-SDK-Source/releases/download/release-$OXR/openxr_loader_windows-$OXR.zip"
  rm -rf build/oxr && mkdir -p build/oxr && unzip -q build/oxr.zip -d build/oxr
fi
python3 tools/preflight.py
python3 tools/gen.py
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -shared -static -Wall -Wno-unused -o build/GTA5VR.asi src/*.cpp \
  -Isrc -Ibuild/oxr/include -ld3d11 -ldxgi -luuid
x86_64-w64-mingw32-g++ -std=c++17 -O2 -s -static -mwindows -Wall -Wno-unused -o build/GTA5VR_Host.exe tools/host/host.cpp -Isrc -Ibuild/oxr/include -ld3d11 -ldxgi -luuid
rm -rf build/pkg && mkdir -p build/pkg/licenses
cp build/GTA5VR.asi build/GTA5VR_Host.exe build/oxr/x64/bin/openxr_loader.dll build/pkg/
cp build/oxr/share/doc/openxr/LICENSE build/pkg/licenses/OpenXR-Loader-LICENSE.txt
cp README_RU.txt build/pkg/
cp install/setup.bat install/update.bat install/uninstall.bat build/pkg/
(cd build/pkg && rm -f ../GTA5VR-$VER.zip && zip -qr ../GTA5VR-$VER.zip .)
echo "built build/GTA5VR-$VER.zip"
