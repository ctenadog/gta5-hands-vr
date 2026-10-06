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
float g_yawRef = 0.f; bool g_yawRefSet = false;

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

// systems.online_guard
bool onlineGuard() {
    bool online = natives::invoke<int>(N_NETWORK_IS_SESSION_STARTED) != 0;
    if (online && !g_onlineBlocked) vrlog::write("online session detected: mod switched off (story mode only)");
    g_onlineBlocked = online;
    return !online;
}

// systems.head_camera: scripted camera at the head bone, rotated by the eye this frame renders.
void headCamera(int ped, const VrState& s) {
    float heading = natives::invoke<float>(N_GET_ENTITY_HEADING, ped);
    const XrPoseF3& e = s.eye[s.renderEye];
    if (!g_yawRefSet && s.head.valid) { g_yawRef = 0.f; g_yawRefSet = true; }
    V head = boneCoords(ped, kHeadBone);
    V eyeOff = xrToGta(sub(xrPos(e), xrPos(s.head)), heading);   // IPD offset only; body stays where GTA puts it
    V pos = add(head, eyeOff);
    // rotation from quaternion, GTA order 2 (x pitch, y roll, z yaw), degrees
    V fwd = qrot(e, {0,0,-1}), up = qrot(e, {0,1,0});
    V gf = xrToGta(fwd, heading), gu = xrToGta(up, heading);
    float yaw = atan2f(-gf.x, gf.y) * 57.29578f;
    float pitch = asinf(fmaxf(-1.f, fminf(1.f, gf.z))) * 57.29578f;
    float roll = atan2f(-(gu.x*cosf(yaw/57.29578f) + gu.y*sinf(yaw/57.29578f)), gu.z) * 57.29578f;   // TODO verify sign in game
    if (!g_cam) {
        g_cam = natives::invoke<int>(N_CREATE_CAM_WITH_PARAMS, "DEFAULT_SCRIPTED_CAMERA", pos.x, pos.y, pos.z, pitch, roll, yaw, s.eyeFovDeg[s.renderEye], 1, 2);
        natives::invoke(N_SET_CAM_ACTIVE, g_cam, 1);
        natives::invoke(N_RENDER_SCRIPT_CAMS, 1, 0, 0, 1, 0, 0);
    }
    natives::invoke(N_SET_CAM_COORD, g_cam, pos.x, pos.y, pos.z);
    natives::invoke(N_SET_CAM_ROT, g_cam, pitch, roll, yaw, 2);
    natives::invoke(N_SET_CAM_FOV, g_cam, s.eyeFovDeg[s.renderEye]);
}

// systems.arm_follow: one IK target per row of sheets/arms.json
void armFollow(int ped, const VrState& s, bool inVehicle) {
    float heading = natives::invoke<float>(N_GET_ENTITY_HEADING, ped);
    V head = boneCoords(ped, kHeadBone);
    for (const ArmRow& a : kArms) {
        if (inVehicle && !a.inVehicle) continue;
        const XrPoseF3& p = s.pose[a.pose];
        if (!p.valid) continue;
        V rel = xrToGta(sub(xrPos(p), xrPos(s.head)), heading);
        if (len(rel) > a.maxReach * 1.6f) { float k = a.maxReach * 1.6f / len(rel); rel = {rel.x*k, rel.y*k, rel.z*k}; }
        V target = add(head, rel);
        natives::invoke(N_SET_IK_TARGET, ped, a.ikIndex, 0, 0, target.x, target.y, target.z, 0, a.blendIn, a.blendOut);
    }
}

// systems.weapon_aim: right controller's aim ray; trigger shoots at its end point
void weaponAim(int ped, const VrState& s, bool inVehicle) {
    if (inVehicle) return;
    const XrPoseF3& p = s.pose[IN_RIGHT_AIM_POSE];
    if (!p.valid || s.value[IN_FIRE] < kTrigger) return;
    float heading = natives::invoke<float>(N_GET_ENTITY_HEADING, ped);
    V head = boneCoords(ped, kHeadBone);
    V origin = add(head, xrToGta(sub(xrPos(p), xrPos(s.head)), heading));
    V dir = xrToGta(qrot(p, {0,0,-1}), heading);
    V t = add(origin, {dir.x*kAimRay, dir.y*kAimRay, dir.z*kAimRay});
    natives::invoke(N_SET_PED_SHOOTS_AT_COORD, ped, t.x, t.y, t.z, 1);
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
        if (r.type == XrType::Float && (i == IN_MOVE_X || i == IN_MOVE_Y)) v = 0.5f + 0.5f * v * r.axisSign;   // axis: 0..1, 0.5 = centre
        else if (r.type == XrType::Bool && v < 0.5f) continue;
        natives::invoke<int>(N_SET_CONTROL_VALUE_NEXT_FRAME, 0, ctl, v);
    }
}

void releaseCamera() {
    if (g_cam) { natives::invoke(N_RENDER_SCRIPT_CAMS, 0, 0, 0, 1, 0, 0); natives::invoke(N_SET_CAM_ACTIVE, g_cam, 0); g_cam = 0; }
}
}

namespace game {
void toggle() { g_enabled = !g_enabled; vrlog::write("F8: VR %s", g_enabled ? "on" : "off"); }
bool enabled() { return g_enabled; }

void tick() {
    if (!natives::ready()) return;
    if (!onlineGuard() || !g_enabled) { releaseCamera(); return; }
    VrState s = xr::snapshot();
    if (!s.running) { releaseCamera(); return; }
    int ped = natives::invoke<int>(N_PLAYER_PED_ID);
    bool inVehicle = natives::invoke<int>(N_IS_PED_IN_ANY_VEHICLE, ped, 0) != 0;
    headCamera(ped, s);
    armFollow(ped, s, inVehicle);
    weaponAim(ped, s, inVehicle);
    controllerInput(s, inVehicle);
}
}
