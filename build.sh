#!/bin/sh
# Preflight the sheets, regenerate code, cross-compile GTA5VR.asi, package the release zip.
set -e
cd "$(dirname "$0")"
python3 tools/preflight.py || { echo "PREFLIGHT NOT CLEAN - release build refused (use DEV=1 for a dev build)"; [ "$DEV" = 1 ] || exit 1; }
python3 tools/gen.py
x86_64-w64-mingw32-g++ -std=c++17 -O2 -shared -static -Wall -Wno-unused -o build/GTA5VR.asi src/*.cpp  -Isrc -Ithird_party/openxr/include -ld3d11 -ldxgi
rm -rf build/pkg && mkdir -p build/pkg/licenses
cp build/GTA5VR.asi third_party/openxr/openxr_loader.dll build/pkg/
cp third_party/openxr/LICENSE build/pkg/licenses/OpenXR-Loader-LICENSE.txt
cp GTA5VR-README.txt README_RU.txt build/pkg/
cp install/install.bat install/install.ps1 build/pkg/
(cd build/pkg && rm -f ../GTA5VR-0.2.0.zip && zip -qr ../GTA5VR-0.2.0.zip .)
echo built build/GTA5VR-0.2.0.zip
