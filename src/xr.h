#pragma once
#include <d3d11.h>
#include "generated/sheets.h"

struct XrPoseF3 { float qx, qy, qz, qw; float px, py, pz; bool valid; };

// Shared between the helper process (OpenXR), the render thread (Present) and the game script thread.
struct VrState {
    XrPoseF3 head;              // headset pose in OpenXR local space (Y up, -Z forward)
    XrPoseF3 eye[2];            // per-eye poses
    float    eyeFovDeg[2];      // vertical fov per eye
    XrPoseF3 pose[IN_COUNT];    // pose actions (grip/aim)
    float    value[IN_COUNT];   // float/bool actions
    int      renderEye;         // which eye the NEXT game frame must be rendered for (AER, stereo mode only)
    float    fovDeg;            // symmetric vertical fov the game camera must use (both modes)
    bool     stereo;            // false = mono: one camera at the head centre, same image to both eyes
    bool     running;
};

// Since 0.4.0 OpenXR does not run inside GTA5.exe: SteamVR never displayed frames submitted from the game process
// (0.2.x-0.3.x), while the same code in a separate process (xrtest.exe) worked. GTA5VR_Host.exe owns the OpenXR
// session; the game side only hands each frame over (shared texture or shared memory) and reads poses back.
namespace xr {
void setWanted(bool on);                             // F8 on/off: starts / stops GTA5VR_Host.exe
bool failed();                                       // the helper failed or died: game must turn VR off
void onPresent(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer); // hands this frame over
VrState snapshot();                                  // copy for the script thread
void shutdown();
void toggleTestPattern();                            // F10: colour test image instead of the game
void reportCameraPose(const XrPoseF3& usedPose);       // script thread: the (roll-free) pose the game camera was just set to
XrPoseF3 removeRoll(const XrPoseF3& p);                 // same orientation without head tilt (GTA camera renders roll-free)
}
