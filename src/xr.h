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
    int      renderEye;         // which eye the NEXT game frame must be rendered for (AER, stereo mode only)
    float    fovDeg;            // symmetric vertical fov the game camera must use (both modes)
    bool     stereo;            // false = mono: one camera at the head centre, same image to both eyes
    bool     running;
};

namespace xr {
bool loadLoader();                                   // on F8: picks SteamVR (GTA5VR.ini), loads openxr_loader.dll, creates the instance
bool startSession(ID3D11Device* dev, DXGI_FORMAT bbFormat); // on the first Present after F8
void setWanted(bool on);                             // F8 on/off: begin or end the OpenXR session
bool hasSession();
bool needsSession();                                // instance ok, no session (first F8 or after the last one ended)
bool failed();                                      // hung/broken: game must turn VR off
void poll();                                         // pump OpenXR events without submitting a frame
void onPresent(ID3D11DeviceContext* ctx, ID3D11Texture2D* backbuffer); // submits this frame to its eye
VrState snapshot();                                  // copy for the script thread
void shutdown();
void toggleTestPattern();                            // F10: colour test image instead of the game
}
