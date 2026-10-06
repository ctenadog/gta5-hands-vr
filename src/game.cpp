// Per-frame game logic, run on the Script Hook V script thread (sheet hooks.script_tick).
// One function per row of sheets/systems.json (in_v01 = true).
#include <windows.h>
#include <cmath>
#include <string>
#include <type_traits>
#include "natives.h"
#include "xr.h"
#include "log.h"
#include "game.h"
#include "hands.h"

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
int g_veh = 0; float g_vehYawOff = 0.f;
float frameYaw(int ped, const VrState& s) {
    if (!g_yawRefSet && s.head.valid) {
        g_yawRef = headYawGta(s.head); g_headRef = xrPos(s.head);
        g_baseYaw = natives::invoke<float>(N_GET_ENTITY_HEADING, ped);
        g_yawRefSet = true; g_veh = 0; vrlog::write("recentered (head yaw %.1f, character heading %.1f)", g_yawRef, g_baseYaw);
    }
    // 0.4.4: in a vehicle "straight ahead" turns with the vehicle (before, the view stayed world-locked and a turning car
    // left you looking out of the side window). Snap turn changes the offset to the vehicle.
    int veh = natives::invoke<int>(N_IS_PED_IN_ANY_VEHICLE, ped, 0) ? natives::invoke<int>(N_GET_VEHICLE_PED_IS_IN, ped, 0) : 0;
    if (veh) {
        float vh = natives::invoke<float>(N_GET_ENTITY_HEADING, veh);
        if (veh != g_veh) { g_vehYawOff = g_baseYaw - vh; g_veh = veh; vrlog::write("camera: in vehicle, view now turns with it"); }
        g_baseYaw = vh + g_vehYawOff;
    } else if (g_veh) { g_veh = 0; vrlog::write("camera: on foot, view world-locked"); }
    return g_baseYaw - g_yawRef;   // rotation applied to every OpenXR vector
}
// systems.online_guard
bool onlineGuard() {
    bool online = natives::invoke<int>(N_NETWORK_IS_SESSION_STARTED) != 0;
    if (online && !g_onlineBlocked) vrlog::write("online session detected: mod switched off (story mode only)");
    g_onlineBlocked = online;
    return !online;
}

// 0.4.5 camera settings (GTA5VR.ini [vr]): eye_up / eye_forward in cm above / in front of the neck bone,
// head_smooth = how much of the neck movement follows per frame in % (lower = calmer, more lag), car_hard_attach.
float g_eyeUp = 0.12f, g_eyeFwd = 0.12f, g_smooth = 0.25f; bool g_carHard = true; bool g_camIniRead = false;
void readCamIni() {
    if (g_camIniRead) return; g_camIniRead = true;
    char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string d = m; d = d.substr(0, d.find_last_of("\\/") + 1) + "GTA5VR.ini";
    auto def = [&](const char* k, const char* v) { if (GetPrivateProfileIntA("vr", k, -12345, d.c_str()) == -12345) WritePrivateProfileStringA("vr", k, v, d.c_str()); };
    def("eye_up", "12"); def("eye_forward", "12"); def("head_smooth", "25"); def("car_hard_attach", "1");
    g_eyeUp  = (int)GetPrivateProfileIntA("vr", "eye_up", 12, d.c_str()) / 100.f;
    g_eyeFwd = (int)GetPrivateProfileIntA("vr", "eye_forward", 12, d.c_str()) / 100.f;
    int sm = (int)GetPrivateProfileIntA("vr", "head_smooth", 25, d.c_str()); g_smooth = fmaxf(0.02f, fminf(1.f, sm / 100.f));
    g_carHard = GetPrivateProfileIntA("vr", "car_hard_attach", 1, d.c_str()) != 0;
    vrlog::write("camera: GTA5VR.ini eye_up=%.0f cm eye_forward=%.0f cm head_smooth=%.0f%% car_hard_attach=%d", g_eyeUp*100, g_eyeFwd*100, g_smooth*100, g_carHard ? 1 : 0);
}
// world -> ped-local (x right, y forward) for a horizontal vector, ped heading in degrees
V toLocal(V w, float hDeg) { float h = hDeg * 0.0174533f, c = cosf(h), s = sinf(h); return { w.x*c + w.y*s, -w.x*s + w.y*c, w.z }; }
V toWorld(V l, float hDeg) { float h = hDeg * 0.0174533f, c = cosf(h), s = sinf(h); return { l.x*c - l.y*s, l.x*s + l.y*c, l.z }; }
// Neck position in the character's own space, smoothed. 0.4.4 used root + fixed 0.65 m: the camera ended up beside /
// behind / under the real head (seated in cars especially) and the face was in view. The neck bone is used (not the
// head bone) because the head bones are collapsed by the head hiding.
int g_neckPed = 0, g_neckIdx = -1; V g_neckSm{0,0,0}; bool g_neckSet = false; int g_camMode = 0; ULONGLONG g_camDbg = 0;
V neckLocal(int ped) {
    if (ped != g_neckPed) { g_neckPed = ped; g_neckIdx = natives::invoke<int>(N_GET_PED_BONE_INDEX, ped, 39317 /*SKEL_Neck_1*/); g_neckSet = false; }
    V cur{0.f, 0.02f, 0.55f};   // fallback if the bone is missing
    if (g_neckIdx >= 0) {
        Vector3 w = natives::invokeV3(N_GET_WORLD_POSITION_OF_ENTITY_BONE, ped, g_neckIdx);
        Vector3 l = natives::invokeV3(N_GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS, ped, w.x, w.y, w.z);
        if (fabsf(l.x) < 1.5f && fabsf(l.y) < 1.5f && fabsf(l.z) < 2.f) cur = {l.x, l.y, l.z};
    }
    if (!g_neckSet) { g_neckSm = cur; g_neckSet = true; }
    else { g_neckSm.x += (cur.x - g_neckSm.x) * g_smooth; g_neckSm.y += (cur.y - g_neckSm.y) * g_smooth; g_neckSm.z += (cur.z - g_neckSm.z) * g_smooth; }
    return g_neckSm;
}

// systems.head_camera: scripted camera = the headset. Mono: head centre, same image to both eyes. Stereo: this frame's eye.
// Returns the eye position in world space (anchor for arms and aiming).
V headCamera(int ped, const VrState& s, float yaw, bool inVehicle) {
    readCamIni();
    XrPoseF3 e = xr::removeRoll(s.stereo ? s.eye[s.renderEye] : s.head);
    V gf = xrToGta(qrot(e, {0,0,-1}), yaw);
    float camYaw = atan2f(-gf.x, gf.y) * 57.29578f;
    float pitch = asinf(fmaxf(-1.f, fminf(1.f, gf.z))) * 57.29578f;
    float roll = 0.f;
    xr::reportCameraPose(e);
    float fov = fmaxf(30.f, fminf(120.f, s.fovDeg));
    float pedH = natives::invoke<float>(N_GET_ENTITY_HEADING, ped);
    // room-scale: real head movement since recenter (limited), plus the stereo eye offset
    V off = xrToGta(sub(xrPos(s.head), g_headRef), yaw);
    float l = sqrtf(off.x*off.x + off.y*off.y); if (l > 0.4f) { off.x *= 0.4f / l; off.y *= 0.4f / l; }
    off.z = fmaxf(-0.6f, fminf(0.3f, off.z));
    if (s.stereo) off = add(off, xrToGta(sub(xrPos(e), xrPos(s.head)), yaw));
    float h = camYaw * 0.0174533f;
    V fwd{-sinf(h) * g_eyeFwd, cosf(h) * g_eyeFwd, 0.f};
    V lo = add(add(neckLocal(ped), V{0.f, 0.f, g_eyeUp}), toLocal(add(off, fwd), pedH));
    if (!g_cam) {
        Vector3 r = natives::invokeV3(N_GET_ENTITY_COORDS, ped, 1);
        V p = add(V{r.x, r.y, r.z}, toWorld(lo, pedH));
        g_cam = natives::invoke<int>(N_CREATE_CAM_WITH_PARAMS, "DEFAULT_SCRIPTED_CAMERA", p.x, p.y, p.z, pitch, roll, camYaw, fov, 1, 2);
        natives::invoke(N_SET_CAM_ACTIVE, g_cam, 1);
        natives::invoke(N_RENDER_SCRIPT_CAMS, 1, 0, 0, 1, 0, 0);
        natives::invoke(N_SET_CAM_NEAR_CLIP, g_cam, 0.05f);
        g_camMode = 0;
        vrlog::write("camera: created, attached to the character's neck (bone index %d)", g_neckIdx);
    }
    // The offset is given in the character's own space and the engine places the camera AFTER moving the character.
    // In a vehicle (0.4.5) the rotation is attached too (HARD_ATTACH, relative to the character, who sits in the car):
    // 0.4.4 set a world rotation from the car heading read BEFORE the car moved, one frame late -> shaking in turns.
    int mode = (inVehicle && g_carHard) ? 2 : 1;
    if (mode != g_camMode) { natives::invoke(N_DETACH_CAM, g_cam); g_camMode = mode; }
    if (mode == 2) {
        float rel = camYaw - pedH; while (rel > 180.f) rel -= 360.f; while (rel < -180.f) rel += 360.f;
        natives::invoke(N_HARD_ATTACH_CAM_TO_ENTITY, g_cam, ped, pitch, 0.f, rel, lo.x, lo.y, lo.z, 1);
    } else {
        natives::invoke(N_ATTACH_CAM_TO_ENTITY, g_cam, ped, lo.x, lo.y, lo.z, 1);
        natives::invoke(N_SET_CAM_ROT, g_cam, pitch, roll, camYaw, 2);
    }
    natives::invoke(N_SET_CAM_FOV, g_cam, fov);
    ULONGLONG now = GetTickCount64();
    if (now - g_camDbg > 10000) { g_camDbg = now; vrlog::write("camera: %s, neck (%.2f %.2f %.2f), camera offset (%.2f %.2f %.2f) in character space", mode == 2 ? "vehicle (hard attach)" : "on foot", g_neckSm.x, g_neckSm.y, g_neckSm.z, lo.x, lo.y, lo.z); }
    // GTA moves the character relative to the (hidden) gameplay camera: point it where the head looks,
    // so "stick forward" = walk where you look
    float rel = camYaw - pedH; while (rel > 180.f) rel -= 360.f; while (rel < -180.f) rel += 360.f;
    natives::invoke(N_SET_GAMEPLAY_CAM_RELATIVE_HEADING, rel);
    Vector3 r = natives::invokeV3(N_GET_ENTITY_COORDS, ped, 1);
    return add(V{r.x, r.y, r.z}, toWorld(lo, pedH));
}

// systems.arm_follow: one IK target per row of sheets/arms.json, in the same world-locked frame as the camera
bool g_armsOn = true;
void armFollow(int ped, const VrState& s, bool inVehicle, float yaw, V anchor) {
    if (!g_armsOn) return;
    natives::invoke(N_SET_PED_CAN_ARM_IK, ped, 1);
    for (const ArmRow& a : kArms) {
        if (inVehicle && !a.inVehicle) continue;
        const XrPoseF3& p = s.pose[a.pose];
        if (!p.valid) continue;
        V rel = xrToGta(sub(xrPos(p), xrPos(s.head)), yaw);
        if (len(rel) > a.maxReach) { float k = a.maxReach / len(rel); rel = {rel.x*k, rel.y*k, rel.z*k}; }
        V target = add(anchor, rel);
        natives::invoke(N_SET_IK_TARGET, ped, a.ikIndex, 0, 0, target.x, target.y, target.z, 0, a.blendIn, a.blendOut);
        static int dbg = 0;
        if (dbg < 6) { ++dbg; vrlog::write("arm %s: controller rel (%.2f %.2f %.2f) target (%.1f %.1f %.1f) anchor z %.1f", a.side, rel.x, rel.y, rel.z, target.x, target.y, target.z, anchor.z); }
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
    if (!g_turnLatch && fabsf(x) > 0.7f) { float d = (x > 0 ? 30.f : -30.f); g_baseYaw -= d; g_vehYawOff -= d; g_turnLatch = true; }
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

// systems.head_camera, part 2: hide the character's head while VR is on, give it back when VR goes off.
// Hats / glasses / earpieces (props 0,1,2) are taken off and put back exactly as they were.
// The head itself is collapsed in the skeleton by hands.cpp (hands::setHideHead) - the game rebuilds it by itself
// as soon as the mod stops writing, so VR off = head back.
int g_propPed = 0; int g_prop[3], g_propTex[3]; bool g_propSaved = false;
int g_hideHeadIni = -1;
bool hideHeadEnabled() {
    if (g_hideHeadIni < 0) {
        char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string d = m; d = d.substr(0, d.find_last_of("\\/") + 1) + "GTA5VR.ini";
        if (GetPrivateProfileIntA("vr", "hide_head", -12345, d.c_str()) == -12345) WritePrivateProfileStringA("vr", "hide_head", "1", d.c_str());
        g_hideHeadIni = GetPrivateProfileIntA("vr", "hide_head", 1, d.c_str()) != 0 ? 1 : 0;
        vrlog::write("camera: GTA5VR.ini hide_head=%d", g_hideHeadIni);
    }
    return g_hideHeadIni == 1;
}
void restoreProps() {
    if (!g_propSaved) return;
    for (int i = 0; i < 3; ++i) if (g_prop[i] >= 0) natives::invoke(N_SET_PED_PROP_INDEX, g_propPed, i, g_prop[i], g_propTex[i], 1, 0);
    g_propSaved = false;
    vrlog::write("head: hat/glasses put back");
}
void hideHead(int ped) {
    if (!hideHeadEnabled()) return;
    if (g_propSaved && g_propPed != ped) restoreProps();   // switched character
    if (!g_propSaved) {
        g_propPed = ped;
        for (int i = 0; i < 3; ++i) {
            g_prop[i] = natives::invoke<int>(N_GET_PED_PROP_INDEX, ped, i, 0);
            g_propTex[i] = natives::invoke<int>(N_GET_PED_PROP_TEXTURE_INDEX, ped, i);
            if (g_prop[i] >= 0) natives::invoke(N_CLEAR_PED_PROP, ped, i, 0);
        }
        g_propSaved = true;
        vrlog::write("head: hidden (props hat %d glasses %d ears %d taken off)", g_prop[0], g_prop[1], g_prop[2]);
    }
    hands::setHideHead(true);
}
void showHead() { hands::setHideHead(false); restoreProps(); }

void releaseCamera() {
    showHead();
    g_neckPed = 0; g_camMode = 0;
    if (g_cam) { natives::invoke(N_DETACH_CAM, g_cam); natives::invoke(N_RENDER_SCRIPT_CAMS, 0, 0, 0, 1, 0, 0); natives::invoke(N_SET_CAM_ACTIVE, g_cam, 0); natives::invoke(N_DESTROY_CAM, g_cam, 0); g_cam = 0; }
}
}

namespace game {
void toggle() { g_enabled = !g_enabled; if (g_enabled) { ++g_gen; g_yawRefSet = false; } vrlog::write("F8: VR %s", g_enabled ? "on" : "off"); }
void forceOff() { if (g_enabled) { g_enabled = false; vrlog::write("VR switched off automatically"); } }
int generation() { return g_gen; }
void recenter() { g_yawRefSet = false; vrlog::write("F9: recenter"); }
bool enabled() { return g_enabled; }

void toggleArms() { g_armsOn = !g_armsOn; vrlog::write("F11: arm IK %s", g_armsOn ? "on" : "off"); }

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
    V anchor = headCamera(ped, s, yaw, inVehicle);
    armFollow(ped, s, inVehicle, yaw, anchor);
    { const XrPoseF3* g[2] = {&s.pose[IN_LEFT_GRIP_POSE], &s.pose[IN_RIGHT_GRIP_POSE]}; hands::tick(ped, g, yaw); }
    hideHead(ped);
    weaponAim(ped, s, inVehicle, yaw, anchor);
    controllerInput(s, inVehicle);
}
}
