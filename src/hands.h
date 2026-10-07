#pragma once
#include "xr.h"
// Experimental wrist rotation (F12): first press searches the ped skeleton in memory, second press toggles rotation.
namespace hands {
void toggle();
void setHideHead(bool on);   // 0.4.4: VR on = head/face bones collapsed, off = game restores them
// script thread, every frame while VR runs: grip poses (OpenXR stage space) for left/right, yaw = game.cpp frame yaw
void tick(int ped, const XrPoseF3* gripLeftRight[2], float yawDeg);
}
