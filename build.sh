#!/bin/sh
# Local build = the same script CI uses (downloads the OpenXR loader, builds GTA5VR.asi + GTA5VR_Host.exe, packs the zip).
exec sh "$(dirname "$0")/ci/build.sh"
