#pragma once
#include <d3d11.h>
#include "generated/sheets.h"

struct XrPoseF3 { float qx, qy, qz, qw; float px, py, pz; bool valid; };

// Shared between the render thread (Present) and the game script thread.
struct VrState {
    XrPoseF3 head;              // headset pose in OpenXR stage space (Y up, -Z forward)
    XrPoseF3 eye[2];            // per-eye poses
    float    eyeFovDeg[2];      // vertical fov per eye
    XrPoseF3 pose[IN_COUNT];    // pose actions (grip/aim)
    float    value[IN_COUNT];   // float/bool actions
    int      renderEye;         // which eye the NEXT game frame must be rendered for (AER)
    bool     running;
};

namespace xr {
bool loadLoader();                                   // openxr_loader.dll next to GTA5.exe
bool startSession(ID3D11Device* dev);                // on first Present
void onPresent(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer); // submits this frame to its eye
VrState snapshot();                                  // copy for the script thread
void shutdown();
}
