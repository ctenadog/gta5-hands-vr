// Per-frame game logic, run on the Script Hook V script thread (sheet hooks.script_tick).
// One function per row of sheets/systems.json (in_v01 = true).
#include <cmath>
#include <type_traits>
#include "natives.h"
#include "xr.h"
#include "log.h"
#include "game.h"

namespace {
int  g_cam = 0;
bool g_enabled = false;   // VR starts OFF: press F8 in story mode
bool g_onlineBlocked = false;
float g_yawRef = 0.f; bool g_yawRefSet = false;   // head yaw at F8/F9 = "straight ahead"
int g_gen = 0;
const float kDeadzone = 0.25f;

struct V { float x, y, z; };
V qrot(const XrPoseF3& q, V v) {   // rotate v by quaternion q
    float ux = q.qx, uy = q.qy, uz = q.qz, s = q.qw;
    float d = ux*v.x + uy*v.y + uz*v.z, uu = ux*ux + uy*uy + uz*uz;
    float cx = uy*v.z - uz*v.y, cy = uz*v.x - ux*v.z, cz = ux*v.y - uy*v.x;
    return { 2*d*ux + (s*s-uu)*v.x + 2*s*cx, 2*d*uy + (s*s-uu)*v.y + 2*s*cy, 2*d*uz + (s*s-uu)*v.z + 2*s*cz };
}
// OpenXR stage (X right, Y up, -Z forward) -> GTA ped-local (X right, Y forward, Z up), then rotated by ped heading.
V xrToGta(V p, float headingDeg) {
    V l{p.x * kWorldScale, -p.z * kWorldScale, p.y * kWorldScale};
    float h = headingDeg * 0.0174533f, c = cosf(h), s = sinf(h);
    return { l.x*c - l.y*s, l.x*s + l.y*c, l.z };
}
V xrPos(const XrPoseF3& p) { return {p.px, p.py, p.pz}; }
V add(V a, V b) { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
V sub(V a, V b) { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
float len(V a) { return sqrtf(a.x*a.x + a.y*a.y + a.z*a.z); }
V boneCoords(int ped, int bone) { Vector3 r = natives::invokeV3(N_GET_PED_BONE_COORDS, ped, bone, 0.f, 0.f, 0.f); return {r.x, r.y, r.z}; }

float headYawGta(const XrPoseF3& h) { V f = qrot(h, {0,0,-1}); V g = xrToGta(f, 0.f); return atan2f(-g.x, g.y) * 57.29578f; }
// World-locked VR frame. g_baseYaw = world heading that "straight ahead in the room" maps to; it changes only on
// recenter (F8/F9) and right-stick turning - NOT when the character turns while walking (that made the view swing).
float g_baseYaw = 0.f; V g_headRef{0,0,0};
float frameYaw(int ped, const VrState& s) {
    if (!g_yawRefSet && s.head.valid) {
        g_yawRef = headYawGta(s.head); g_headRef = xrPos(s.head);
        g_baseYaw = natives::invoke<float>(N_GET_ENTITY_HEADING, ped);
        g_yawRefSet = true; vrlog::write("recentered (head yaw %.1f, character heading %.1f)", g_yawRef, g_baseYaw);
    }
    return g_baseYaw - g_yawRef;   // rotation applied to every OpenXR vector
}
// Stable eye anchor: the character's root + eye height (+ limited room-scale offset). The head bone bobs with the
// walk animation, and anchoring the camera to it made the view and the arms shake.
V eyeAnchor(int ped, const VrState& s, float yaw) {
    Vector3 r = natives::invokeV3(N_GET_ENTITY_COORDS, ped, 1);
    V off = xrToGta(sub(xrPos(s.head), g_headRef), yaw);
    float l = sqrtf(off.x*off.x + off.y*off.y); if (l > 0.4f) { off.x *= 0.4f / l; off.y *= 0.4f / l; }
    off.z = fmaxf(-0.6f, fminf(0.3f, off.z));
    return {r.x + off.x, r.y + off.y, r.z + kEyeHeight + off.z};
}

// systems.online_guard
bool onlineGuard() {
    bool online = natives::invoke<int>(N_NETWORK_IS_SESSION_STARTED) != 0;
    if (online && !g_onlineBlocked) vrlog::write("online session detected: mod switched off (story mode only)");
    g_onlineBlocked = online;
    return !online;
}

// systems.head_camera: scripted camera = the headset. Mono: head centre, same image to both eyes. Stereo: this frame's eye.
void headCamera(int ped, const VrState& s, float yaw, V anchor) {
    const XrPoseF3& e = s.stereo ? s.eye[s.renderEye] : s.head;
    V pos = s.stereo ? add(anchor, xrToGta(sub(xrPos(e), xrPos(s.head)), yaw)) : anchor;
    V fwd = qrot(e, {0,0,-1}), up = qrot(e, {0,1,0});
    V gf = xrToGta(fwd, yaw), gu = xrToGta(up, yaw);
    float camYaw = atan2f(-gf.x, gf.y) * 57.29578f;
    float pitch = asinf(fmaxf(-1.f, fminf(1.f, gf.z))) * 57.29578f;
    // roll: angle of the camera's up vector around the forward axis (0 when level)
    float cy = cosf(camYaw / 57.29578f), sy = sinf(camYaw / 57.29578f);
    V right{cy, sy, 0.f};   // GTA right vector for yaw camYaw (forward = (-sin, cos))
    float roll = atan2f(-(gu.x*right.x + gu.y*right.y), gu.z) * 57.29578f;
    float fov = fmaxf(30.f, fminf(120.f, s.fovDeg));
    if (!g_cam) {
        g_cam = natives::invoke<int>(N_CREATE_CAM_WITH_PARAMS, "DEFAULT_SCRIPTED_CAMERA", pos.x, pos.y, pos.z, pitch, roll, camYaw, fov, 1, 2);
        natives::invoke(N_SET_CAM_ACTIVE, g_cam, 1);
        natives::invoke(N_RENDER_SCRIPT_CAMS, 1, 0, 0, 1, 0, 0);
    }
    natives::invoke(N_SET_CAM_COORD, g_cam, pos.x, pos.y, pos.z);
    natives::invoke(N_SET_CAM_ROT, g_cam, pitch, roll, camYaw, 2);
    natives::invoke(N_SET_CAM_FOV, g_cam, fov);
    // GTA moves the character relative to the (hidden) gameplay camera: point it where the head looks,
    // so "stick forward" = walk where you look
    float pedH = natives::invoke<float>(N_GET_ENTITY_HEADING, ped);
    float rel = camYaw - pedH; while (rel > 180.f) rel -= 360.f; while (rel < -180.f) rel += 360.f;
    natives::invoke(N_SET_GAMEPLAY_CAM_RELATIVE_HEADING, rel);
}

// systems.arm_follow: one IK target per row of sheets/arms.json, in the same world-locked frame as the camera
void armFollow(int ped, const VrState& s, bool inVehicle, float yaw, V anchor) {
    for (const ArmRow& a : kArms) {
        if (inVehicle && !a.inVehicle) continue;
        const XrPoseF3& p = s.pose[a.pose];
        if (!p.valid) continue;
        V rel = xrToGta(sub(xrPos(p), xrPos(s.head)), yaw);
        if (len(rel) > a.maxReach) { float k = a.maxReach / len(rel); rel = {rel.x*k, rel.y*k, rel.z*k}; }
        V target = add(anchor, rel);
        natives::invoke(N_SET_IK_TARGET, ped, a.ikIndex, 0, 0, target.x, target.y, target.z, 0, a.blendIn, a.blendOut);
    }
}

// systems.weapon_aim: right controller's aim ray; trigger shoots at its end point
void weaponAim(int ped, const VrState& s, bool inVehicle, float yaw, V anchor) {
    if (inVehicle) return;
    const XrPoseF3& p = s.pose[IN_RIGHT_AIM_POSE];
    if (!p.valid || s.value[IN_FIRE] < kTrigger) return;
    V origin = add(anchor, xrToGta(sub(xrPos(p), xrPos(s.head)), yaw));
    V dir = xrToGta(qrot(p, {0,0,-1}), yaw);
    V t = add(origin, {dir.x*kAimRay, dir.y*kAimRay, dir.z*kAimRay});
    natives::invoke(N_SET_PED_SHOOTS_AT_COORD, ped, t.x, t.y, t.z, 1);
}

// right stick: snap turn 30 degrees
bool g_turnLatch = false;
void snapTurn(const VrState& s) {
    float x = s.value[IN_TURN_X];
    if (!g_turnLatch && fabsf(x) > 0.7f) { g_baseYaw -= (x > 0 ? 30.f : -30.f); g_turnLatch = true; }
    if (fabsf(x) < 0.3f) g_turnLatch = false;
}

// systems.controller_input: every non-pose row of sheets/inputs.json feeds its GTA control
void controllerInput(const VrState& s, bool inVehicle) {
    for (int i = 0; i < IN_COUNT; ++i) {
        const InputRow& r = kInputs[i];
        if (r.type == XrType::Pose) continue;
        if (i == IN_FIRE && !inVehicle) continue;            // on foot, fire goes through weapon_aim
        int ctl = inVehicle ? r.controlVehicle : r.control;
        if (ctl < 0) continue;
        float v = s.value[i];
        // nothing is sent while a stick is centred / a trigger is released.
        // Axes take -1..1 (MOVE_UD: -1 = forward, MOVE_LR: +1 = right). 0.4.x sent 0..1 with 0.5 as centre,
        // which pushed the character backwards/right all the time ("crooked" controls).
        if (i == IN_MOVE_X || i == IN_MOVE_Y) { if (fabsf(v) < kDeadzone) continue; v = v * r.axisSign; }
        else if (r.type == XrType::Float) { if (v < kTrigger) continue; }
        else if (v < 0.5f) continue;
        natives::invoke<int>(N_SET_CONTROL_VALUE_NEXT_FRAME, 0, ctl, v);
    }
}

void releaseCamera() {
    if (g_cam) { natives::invoke(N_RENDER_SCRIPT_CAMS, 0, 0, 0, 1, 0, 0); natives::invoke(N_SET_CAM_ACTIVE, g_cam, 0); natives::invoke(N_DESTROY_CAM, g_cam, 0); g_cam = 0; }
}
}

namespace game {
void toggle() { g_enabled = !g_enabled; if (g_enabled) { ++g_gen; g_yawRefSet = false; } vrlog::write("F8: VR %s", g_enabled ? "on" : "off"); }
void forceOff() { if (g_enabled) { g_enabled = false; vrlog::write("VR switched off automatically"); } }
int generation() { return g_gen; }
void recenter() { g_yawRefSet = false; vrlog::write("F9: recenter"); }
bool enabled() { return g_enabled; }

void tick() {
    if (!natives::ready()) return;
    if (!onlineGuard() || !g_enabled) { releaseCamera(); return; }
    if (natives::invoke<int>(N_GET_IS_LOADING_SCREEN_ACTIVE) || natives::invoke<int>(N_IS_PAUSE_MENU_ACTIVE)) { releaseCamera(); return; }
    VrState s = xr::snapshot();
    if (!s.running) { releaseCamera(); return; }
    int ped = natives::invoke<int>(N_PLAYER_PED_ID);
    bool inVehicle = natives::invoke<int>(N_IS_PED_IN_ANY_VEHICLE, ped, 0) != 0;
    snapTurn(s);
    float yaw = frameYaw(ped, s);
    V anchor = eyeAnchor(ped, s, yaw);
    headCamera(ped, s, yaw, anchor);
    armFollow(ped, s, inVehicle, yaw, anchor);
    weaponAim(ped, s, inVehicle, yaw, anchor);
    controllerInput(s, inVehicle);
}
}
