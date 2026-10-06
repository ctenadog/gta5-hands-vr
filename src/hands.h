#pragma once
#include "xr.h"
// Experimental wrist rotation (F12): first press searches the ped skeleton in memory, second press toggles rotation.
namespace hands {
void toggle();
// script thread, every frame while VR runs: grip poses (OpenXR stage space) for left/right, yaw = game.cpp frame yaw
void tick(int ped, const XrPoseF3* gripLeftRight[2], float yawDeg);
}
