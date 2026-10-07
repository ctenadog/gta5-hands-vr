// Per-frame game logic, run on the Script Hook V script thread (sheet hooks.script_tick).
// One function per row of sheets/systems.json (in_v01 = true).
#include <windows.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>
#include <type_traits>
#include <atomic>
#include "natives.h"
#include "xr.h"
#include "log.h"
#include "game.h"
#include "hands.h"
#include "blit.h"

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
float g_lastCamYaw = 0.f; bool g_camYawSet = false;   // 0.4.9: view yaw of the last frame (body follow)
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
// 0.5.3 comfort: head_bob=0 (default) keeps the on-foot camera steady instead of following every step / sprint lean of
// the neck bone (the 0.5.1 log: neck forward offset jumped 0 -> 0.28 m while running = the view rocks back and forth).
int g_headBob = 0; bool g_onFoot = true;
void readCamIni() {
    if (g_camIniRead) return; g_camIniRead = true;
    char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string d = m; d = d.substr(0, d.find_last_of("\\/") + 1) + "GTA5VR.ini";
    g_headBob = GetPrivateProfileIntA("vr", "head_bob", 0, d.c_str()) != 0;
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
    float k = g_smooth;
    if (g_onFoot && !g_headBob) {
        // steady on-foot camera: sideways sway ignored, forward lean limited, height follows only slowly (crouch, stairs)
        cur.x = 0.f; cur.y = fmaxf(-0.04f, fminf(0.10f, cur.y)); k = 0.03f;
    }
    if (!g_neckSet) { g_neckSm = cur; g_neckSet = true; }
    else { g_neckSm.x += (cur.x - g_neckSm.x) * k; g_neckSm.y += (cur.y - g_neckSm.y) * k; g_neckSm.z += (cur.z - g_neckSm.z) * k; }
    return g_neckSm;
}

// systems.head_camera: scripted camera = the headset. Mono: head centre, same image to both eyes. Stereo: this frame's eye.
// Returns the eye position in world space (anchor for arms and aiming).
V headCamera(int ped, const VrState& s, float yaw, bool inVehicle) {
    readCamIni();
    g_onFoot = !inVehicle;
    XrPoseF3 e = xr::removeRoll(s.stereo ? s.eye[s.renderEye] : s.head);
    V gf = xrToGta(qrot(e, {0,0,-1}), yaw);
    float camYaw = atan2f(-gf.x, gf.y) * 57.29578f;
    g_lastCamYaw = camYaw; g_camYawSet = true;
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
// 0.4.6: GTA5VR.ini arm_scale (% of the real controller distance, default 120) and arm_forward (cm pushed forward
// along the view, default 12): 0.4.5 put the hands exactly where the controllers are relative to the eyes, which with
// GTA's bigger body looked glued to the chest. Reach limit 0.95 m (was 0.7).
float g_armScale = 1.2f, g_armFwd = 0.12f; bool g_armIni = false;
void readArmIni() {
    if (g_armIni) return; g_armIni = true;
    char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string d = m; d = d.substr(0, d.find_last_of("\\/") + 1) + "GTA5VR.ini";
    g_armScale = fmaxf(0.5f, fminf(2.f, (int)GetPrivateProfileIntA("vr", "arm_scale", 120, d.c_str()) / 100.f));
    g_armFwd = fmaxf(-0.3f, fminf(0.5f, (int)GetPrivateProfileIntA("vr", "arm_forward", 12, d.c_str()) / 100.f));
    vrlog::write("arms: GTA5VR.ini arm_scale=%.0f%% arm_forward=%.0f cm", g_armScale * 100, g_armFwd * 100);
}
void armFollow(int ped, const VrState& s, bool inVehicle, float yaw, V anchor) {
    if (!g_armsOn) return;
    readArmIni();
    natives::invoke(N_SET_PED_CAN_ARM_IK, ped, 1);
    V hf = xrToGta(qrot(xr::removeRoll(s.head), {0,0,-1}), yaw); hf.z = 0.f;
    { float l = len(hf); if (l > 0.01f) { hf.x /= l; hf.y /= l; } }
    for (const ArmRow& a : kArms) {
        if (inVehicle && !a.inVehicle) continue;
        const XrPoseF3& p = s.pose[a.pose];
        if (!p.valid) continue;
        V rel = xrToGta(sub(xrPos(p), xrPos(s.head)), yaw);
        rel = {rel.x * g_armScale + hf.x * g_armFwd, rel.y * g_armScale + hf.y * g_armFwd, rel.z * g_armScale};
        if (len(rel) > a.maxReach) { float k = a.maxReach / len(rel); rel = {rel.x*k, rel.y*k, rel.z*k}; }
        V target = add(anchor, rel);
        natives::invoke(N_SET_IK_TARGET, ped, a.ikIndex, 0, 0, target.x, target.y, target.z, 0, a.blendIn, a.blendOut);
        static int dbg = 0;
        if (dbg < 6) { ++dbg; vrlog::write("arm %s: controller rel (%.2f %.2f %.2f) target (%.1f %.1f %.1f) anchor z %.1f", a.side, rel.x, rel.y, rel.z, target.x, target.y, target.z, anchor.z); }
    }
}

// systems.weapon_aim. 0.4.8: the shot goes where the drawn pistol points, not along the OpenXR "aim" pose.
// 0.4.7 user report: hands/pistol look right but bullets fly up - on Pico Neo 3 via SteamVR the aim pose is tilted
// against the grip pose. Now: direction = grip -Y (the same axis hands.cpp uses for wrist -> knuckles = barrel),
// origin = the right hand bone. GTA5VR.ini aim_source=grip|aim (aim = 0.4.7 behaviour), aim_pitch = degrees (+ up).
// 0.4.9 crosshair: GTA's own reticle sits in the middle of the screen = where the HEAD looks, not where the pistol
// points, and the scripted VR camera hides it anyway. The mod casts a ray along the barrel, finds what it hits and draws
// a small cross there (GTA5VR.ini crosshair=1, crosshair_size in 1/1000 of the screen height, 0 = off).
int g_xhair = -1, g_xhairSize = 6; ULONGLONG g_xhDbg = 0;
void crosshair(int ped, V origin, V dir) {
    if (g_xhair < 0) {
        char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string d = m; d = d.substr(0, d.find_last_of("\\/") + 1) + "GTA5VR.ini";
        g_xhair = GetPrivateProfileIntA("vr", "crosshair", 1, d.c_str()) != 0 ? 1 : 0;
        g_xhairSize = std::max(2, std::min(40, (int)GetPrivateProfileIntA("vr", "crosshair_size", 6, d.c_str())));
        vrlog::write("aim: GTA5VR.ini crosshair=%d crosshair_size=%d", g_xhair, g_xhairSize);
    }
    if (!g_xhair) return;
    natives::invoke(N_HIDE_HUD_COMPONENT_THIS_FRAME, 14);   // GTA's centre reticle (would point where the head looks)
    V start = add(origin, {dir.x*0.3f, dir.y*0.3f, dir.z*0.3f});
    V end = add(origin, {dir.x*kAimRay, dir.y*kAimRay, dir.z*kAimRay});
    int h = natives::invoke<int>(N_START_EXPENSIVE_SYNCHRONOUS_SHAPE_TEST_LOS_PROBE, start.x, start.y, start.z, end.x, end.y, end.z, 511, ped, 7);
    int hit = 0, ent = 0; Vector3 hp{}, nrm{};
    natives::invoke<int>(N_GET_SHAPE_TEST_RESULT, h, &hit, &hp, &nrm, &ent);
    V p = end; if (hit) p = {hp.x, hp.y, hp.z};
    float sx = 0, sy = 0;
    if (!natives::invoke<int>(N_GET_SCREEN_COORD_FROM_WORLD_COORD, p.x, p.y, p.z, &sx, &sy)) return;
    float ar = natives::invoke<float>(N_GET_ASPECT_RATIO, 0); if (ar < 0.5f || ar > 5.f) ar = 16.f / 9.f;
    float hgt = g_xhairSize / 1000.f, th = hgt * 0.3f;   // arm length / thickness, screen-height units
    int r = ent ? 255 : 255, g = ent ? 60 : 255, b = ent ? 60 : 255;   // red when it points at a person / car / object
    natives::invoke(N_DRAW_RECT, sx, sy, (hgt * 2.f + th) / ar, th, 0, 0, 0, 160, 0);   // dark outline for bright scenes
    natives::invoke(N_DRAW_RECT, sx, sy, th / ar, hgt * 2.f + th, 0, 0, 0, 160, 0);
    natives::invoke(N_DRAW_RECT, sx, sy, (hgt * 2.f) / ar, th * 0.5f, r, g, b, 230, 0);
    natives::invoke(N_DRAW_RECT, sx, sy, (th * 0.5f) / ar, hgt * 2.f, r, g, b, 230, 0);
    ULONGLONG now = GetTickCount64();
    if (now - g_xhDbg > 10000) { g_xhDbg = now; vrlog::write("aim: crosshair at screen (%.2f %.2f), %s %.1f m", sx, sy, hit ? "hit" : "no hit,", len(sub(p, origin))); }
}
int g_aimSrc = -1; float g_aimPitch = 0.f; int g_handBonePed = 0, g_rHandIdx = -1; ULONGLONG g_aimDbg = 0;
float pitchOf(V d) { float l = len(d); return l > 1e-4f ? asinf(fmaxf(-1.f, fminf(1.f, d.z / l))) * 57.29578f : 0.f; }
void weaponAim(int ped, const VrState& s, bool inVehicle, float yaw, V anchor) {
    if (inVehicle) return;
    if (g_aimSrc < 0) {
        char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string d = m; d = d.substr(0, d.find_last_of("\\/") + 1) + "GTA5VR.ini";
        char buf[16] = {0};
        GetPrivateProfileStringA("vr", "aim_source", "", buf, sizeof buf, d.c_str());
        if (!buf[0]) strcpy(buf, "grip");
        g_aimSrc = (_stricmp(buf, "aim") == 0) ? 1 : 0;
        g_aimPitch = (int)GetPrivateProfileIntA("vr", "aim_pitch", 0, d.c_str()) * 0.0174533f;
        vrlog::write("aim: GTA5VR.ini aim_source=%s aim_pitch=%.0f", g_aimSrc ? "aim" : "grip", g_aimPitch * 57.3f);
    }
    const XrPoseF3& g = s.pose[IN_RIGHT_GRIP_POSE];
    const XrPoseF3& a = s.pose[IN_RIGHT_AIM_POSE];
    bool useAim = g_aimSrc == 1 || !g.valid;
    const XrPoseF3& p = useAim ? a : g;
    if (!p.valid) return;
    bool firing = s.value[IN_FIRE] >= kTrigger;
    // barrel axis in controller space, tilted by aim_pitch about controller X (+ = up for a pistol grip)
    float cp = cosf(g_aimPitch), sp = sinf(g_aimPitch);
    V local = useAim ? V{0.f, sp, -cp} : V{0.f, -cp, -sp};
    V dir = xrToGta(qrot(p, local), yaw);
    { float l = len(dir); if (l > 1e-4f) dir = {dir.x / l, dir.y / l, dir.z / l}; }
    // origin: the real right hand of the character (falls back to the controller position)
    if (ped != g_handBonePed) { g_handBonePed = ped; g_rHandIdx = natives::invoke<int>(N_GET_PED_BONE_INDEX, ped, 57005 /*SKEL_R_Hand*/); }
    V origin = add(anchor, xrToGta(sub(xrPos(p), xrPos(s.head)), yaw));
    if (g_rHandIdx >= 0) {
        Vector3 h = natives::invokeV3(N_GET_WORLD_POSITION_OF_ENTITY_BONE, ped, g_rHandIdx);
        V hv{h.x, h.y, h.z}; if (len(sub(hv, anchor)) < 2.f) origin = hv;
    }
    V t = add(origin, {dir.x*kAimRay, dir.y*kAimRay, dir.z*kAimRay});
    bool armed = natives::invoke<int>(N_IS_PED_ARMED, ped, 6) != 0;   // 6 = guns + throwables (not fists / melee)
    if (armed) crosshair(ped, origin, dir);
    if (!firing) return;
    natives::invoke(N_SET_PED_SHOOTS_AT_COORD, ped, t.x, t.y, t.z, 1);
    ULONGLONG now = GetTickCount64();
    if (now - g_aimDbg > 1000) {
        g_aimDbg = now;
        V dg = g.valid ? xrToGta(qrot(g, {0,-1,0}), yaw) : V{0,0,0};
        V da = a.valid ? xrToGta(qrot(a, {0,0,-1}), yaw) : V{0,0,0};
        V dh = xrToGta(qrot(s.head, {0,0,-1}), yaw);
        vrlog::write("aim: shot via %s, pitch shot %.0f | grip %.0f | aim pose %.0f | head %.0f deg", useAim ? "aim pose" : "grip", pitchOf(dir), pitchOf(dg), pitchOf(da), pitchOf(dh));
    }
}

// right stick turning. 0.4.9: smooth turn by default (GTA5VR.ini turn_mode=smooth|snap, turn_speed deg/s, snap_angle).
bool g_turnLatch = false; int g_turnMode = -1; float g_turnSpeed = 120.f, g_snapAngle = 30.f;
int g_bodyFollow = 1; float g_bodyDead = 35.f, g_bodySpeed = 240.f; bool g_bodyTurning = false; ULONGLONG g_lastTick = 0;
void readTurnIni() {
    if (g_turnMode >= 0) return;
    char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string d = m; d = d.substr(0, d.find_last_of("\\/") + 1) + "GTA5VR.ini";
    char buf[16] = {0};
    GetPrivateProfileStringA("vr", "turn_mode", "", buf, sizeof buf, d.c_str());
    if (!buf[0]) strcpy(buf, "smooth");
    g_turnMode = (_stricmp(buf, "snap") == 0) ? 1 : 0;
    g_turnSpeed = (float)std::max(20, std::min(720, (int)GetPrivateProfileIntA("vr", "turn_speed", 120, d.c_str())));
    g_snapAngle = (float)std::max(5, std::min(90, (int)GetPrivateProfileIntA("vr", "snap_angle", 30, d.c_str())));
    g_bodyFollow = GetPrivateProfileIntA("vr", "body_follow", 1, d.c_str()) != 0;
    g_bodyDead = (float)std::max(0, std::min(120, (int)GetPrivateProfileIntA("vr", "body_deadzone", 35, d.c_str())));
    g_bodySpeed = (float)std::max(30, std::min(1080, (int)GetPrivateProfileIntA("vr", "body_speed", 240, d.c_str())));
    vrlog::write("turn: GTA5VR.ini turn_mode=%s turn_speed=%.0f snap_angle=%.0f body_follow=%d body_deadzone=%.0f body_speed=%.0f",
                 g_turnMode ? "snap" : "smooth", g_turnSpeed, g_snapAngle, g_bodyFollow, g_bodyDead, g_bodySpeed);
}
float frameDt() {
    ULONGLONG now = GetTickCount64();
    float dt = g_lastTick ? (now - g_lastTick) / 1000.f : 0.f; g_lastTick = now;
    return fmaxf(0.f, fminf(0.1f, dt));   // pauses / hitches do not cause a big jump
}
void snapTurn(const VrState& s, float dt) {
    readTurnIni();
    float x = s.value[IN_TURN_X];
    if (g_turnMode == 0) {
        if (fabsf(x) < 0.2f) return;
        float k = (fabsf(x) - 0.2f) / 0.8f; k = k * k * (x > 0 ? 1.f : -1.f);   // soft start: small tilt = slow turn
        float d = k * g_turnSpeed * dt;
        g_baseYaw -= d; g_vehYawOff -= d;
        return;
    }
    if (!g_turnLatch && fabsf(x) > 0.7f) { float d = (x > 0 ? g_snapAngle : -g_snapAngle); g_baseYaw -= d; g_vehYawOff -= d; g_turnLatch = true; }
    if (fabsf(x) < 0.3f) g_turnLatch = false;
}
float wrap180(float a) { while (a > 180.f) a -= 360.f; while (a < -180.f) a += 360.f; return a; }
// 0.4.9: the character turns after the view on foot. Once the view is more than body_deadzone degrees away from where
// the body faces, the body turns (body_speed deg/s) until it faces the view again. While walking GTA turns the body
// itself (stick forward = where you look), so this only acts while standing / aiming.
void bodyFollow(int ped, const VrState& s, bool inVehicle, float dt) {
    if (!g_bodyFollow || inVehicle || !g_camYawSet) { g_bodyTurning = false; return; }
    if (natives::invoke<int>(N_IS_PED_RAGDOLL, ped) || natives::invoke<int>(N_IS_PED_GETTING_INTO_A_VEHICLE, ped)) { g_bodyTurning = false; return; }
    bool moving = fabsf(s.value[IN_MOVE_X]) > kDeadzone || fabsf(s.value[IN_MOVE_Y]) > kDeadzone;
    if (moving) { g_bodyTurning = false; return; }
    float pedH = natives::invoke<float>(N_GET_ENTITY_HEADING, ped);
    float diff = wrap180(g_lastCamYaw - pedH);
    bool aiming = s.value[IN_AIM] > kTrigger || s.value[IN_FIRE] > kTrigger;
    float dead = aiming ? 10.f : g_bodyDead;    // aiming: keep the body almost square to the view
    if (!g_bodyTurning && fabsf(diff) > dead) g_bodyTurning = true;
    if (g_bodyTurning) {
        float step = g_bodySpeed * dt;
        if (fabsf(diff) <= step || fabsf(diff) < 3.f) { natives::invoke(N_SET_ENTITY_HEADING, ped, g_lastCamYaw); g_bodyTurning = false; }
        else natives::invoke(N_SET_ENTITY_HEADING, ped, pedH + (diff > 0 ? step : -step));
    }
}

// 0.4.6: B = get in / out of a vehicle as a TASK. 0.4.5 fed INPUT_ENTER through SET_CONTROL_VALUE_NEXT_FRAME, which
// the game does not treat as a fresh press -> "can't get out of the car". Y = next weapon you own, right stick click =
// holster (fists). There was no weapon selection at all before (the weapon wheel needs the gamepad/mouse).
uint32_t joaat(const char* k) { uint32_t h = 0; for (; *k; ++k) { char c = *k; if (c >= 'A' && c <= 'Z') c += 32; h += (uint8_t)c; h += h << 10; h ^= h >> 6; } h += h << 3; h ^= h >> 11; h += h << 15; return h; }
const char* kWeapons[] = {"WEAPON_UNARMED","WEAPON_KNIFE","WEAPON_NIGHTSTICK","WEAPON_HAMMER","WEAPON_BAT","WEAPON_CROWBAR","WEAPON_GOLFCLUB",
  "WEAPON_PISTOL","WEAPON_COMBATPISTOL","WEAPON_APPISTOL","WEAPON_PISTOL50","WEAPON_SNSPISTOL","WEAPON_HEAVYPISTOL","WEAPON_VINTAGEPISTOL","WEAPON_STUNGUN",
  "WEAPON_MICROSMG","WEAPON_SMG","WEAPON_ASSAULTSMG","WEAPON_COMBATPDW","WEAPON_MG","WEAPON_COMBATMG","WEAPON_GUSENBERG",
  "WEAPON_ASSAULTRIFLE","WEAPON_CARBINERIFLE","WEAPON_ADVANCEDRIFLE","WEAPON_SPECIALCARBINE","WEAPON_BULLPUPRIFLE",
  "WEAPON_PUMPSHOTGUN","WEAPON_SAWNOFFSHOTGUN","WEAPON_ASSAULTSHOTGUN","WEAPON_BULLPUPSHOTGUN","WEAPON_HEAVYSHOTGUN",
  "WEAPON_SNIPERRIFLE","WEAPON_HEAVYSNIPER","WEAPON_MARKSMANRIFLE","WEAPON_GRENADELAUNCHER","WEAPON_RPG","WEAPON_MINIGUN",
  "WEAPON_GRENADE","WEAPON_STICKYBOMB","WEAPON_MOLOTOV","WEAPON_SMOKEGRENADE"};
const int kWeaponCount = sizeof(kWeapons) / sizeof(kWeapons[0]);
bool g_prevBtn[IN_COUNT] = {};
bool pressed(const VrState& s, int i) { bool now = s.value[i] > 0.5f; bool hit = now && !g_prevBtn[i]; return hit; }
void nextWeapon(int ped) {
    uint32_t cur = natives::invoke<uint32_t>(N_GET_SELECTED_PED_WEAPON, ped);
    int ci = 0; for (int i = 0; i < kWeaponCount; ++i) if (joaat(kWeapons[i]) == cur) ci = i;
    for (int k = 1; k <= kWeaponCount; ++k) {
        int i = (ci + k) % kWeaponCount; uint32_t h = joaat(kWeapons[i]);
        if (i == 0 || natives::invoke<int>(N_HAS_PED_GOT_WEAPON, ped, h, 0)) { natives::invoke(N_SET_CURRENT_PED_WEAPON, ped, h, 1); vrlog::write("weapon: %s", kWeapons[i]); return; }
    }
}
// 0.6.4: pause menu and phone from the controllers. Both are driven with simulated KEYBOARD presses (SendInput,
// scancodes), the same keys a PC player uses: the game reacts to them in every menu, while SET_CONTROL_VALUE_NEXT_FRAME
// is not taken as a fresh press (0.4.6 lesson). Keys go out only while the GTA window has focus.
//   Pause menu: Esc. Arrows = navigate, Enter = select, Backspace = back, Q / E = previous / next tab.
//   Phone: Up arrow takes it out; arrows / Enter / Backspace work inside; Backspace on the home screen puts it away.
bool gameHasFocus() { DWORD pid = 0; HWND w = GetForegroundWindow(); if (w) GetWindowThreadProcessId(w, &pid); return pid == GetCurrentProcessId(); }
int g_heldVk = 0; ULONGLONG g_releaseAt = 0;
void sendKey(int vk, bool down) {
    INPUT in{}; in.type = INPUT_KEYBOARD;
    in.ki.wScan = (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT) in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    SendInput(1, &in, sizeof(in));
}
void releaseKey() { if (g_heldVk) { sendKey(g_heldVk, false); g_heldVk = 0; } }
// press now, release ~70 ms later (a down+up in the same instant can fall between two game frames)
void tapKey(int vk, const char* why) {
    if (!gameHasFocus()) return;
    releaseKey();
    sendKey(vk, true); g_heldVk = vk; g_releaseAt = GetTickCount64() + 70;
    vrlog::write("menu: key %s (%s)", vk == VK_ESCAPE ? "Esc" : vk == VK_RETURN ? "Enter" : vk == VK_BACK ? "Backspace" : vk == VK_UP ? "Up" : vk == VK_DOWN ? "Down" : vk == VK_LEFT ? "Left" : vk == VK_RIGHT ? "Right" : vk == 'Q' ? "Q" : vk == 'E' ? "E" : "?", why);
}
void keyTick() { if (g_heldVk && GetTickCount64() >= g_releaseAt) releaseKey(); }
// stick -> arrow keys with auto-repeat (first repeat after 400 ms, then every 180 ms)
int g_stickDir = 0; ULONGLONG g_stickNext = 0;
void stickArrows(float x, float y, const char* why) {
    int dir = 0;
    if (fabsf(x) > 0.6f || fabsf(y) > 0.6f) dir = fabsf(x) > fabsf(y) ? (x > 0 ? VK_RIGHT : VK_LEFT) : (y > 0 ? VK_UP : VK_DOWN);
    ULONGLONG now = GetTickCount64();
    if (!dir) { g_stickDir = 0; return; }
    if (dir != g_stickDir) { g_stickDir = dir; g_stickNext = now + 400; tapKey(dir, why); }
    else if (now >= g_stickNext) { g_stickNext = now + 180; tapKey(dir, why); }
}
int g_tabDir = 0;
bool g_phoneWas = false;
// returns true while the phone is out: the left stick / A / B then drive the phone instead of the character
bool phoneButtons(int ped, const VrState& s) {
    bool out = natives::invoke<int>(N_IS_PED_RUNNING_MOBILE_PHONE_TASK, ped) != 0;
    if (out != g_phoneWas) { g_phoneWas = out; vrlog::write("phone: %s", out ? "out" : "put away"); }
    bool grip = s.value[IN_PHONE] > 0.7f, gripHit = grip && !g_prevBtn[IN_PHONE];
    if (pressed(s, IN_MENU)) tapKey(VK_ESCAPE, "menu button: pause menu");
    if (!out) {
        if (gripHit) tapKey(VK_UP, "left grip: take out the phone");
        return false;
    }
    stickArrows(s.value[IN_MOVE_X], s.value[IN_MOVE_Y], "phone: left stick");
    if (pressed(s, IN_JUMP)) tapKey(VK_RETURN, "phone: A = select");
    if (pressed(s, IN_ENTER_VEHICLE) || gripHit) tapKey(VK_BACK, "phone: B / left grip = back, put away");
    return true;
}
void specialButtons(int ped, const VrState& s, bool inVehicle) {
    if (pressed(s, IN_ENTER_VEHICLE)) {
        if (inVehicle) {
            int veh = natives::invoke<int>(N_GET_VEHICLE_PED_IS_IN, ped, 0);
            natives::invoke(N_TASK_LEAVE_VEHICLE, ped, veh, 0);
            vrlog::write("B: leave vehicle %d", veh);
        } else {
            Vector3 p = natives::invokeV3(N_GET_ENTITY_COORDS, ped, 1);
            int veh = natives::invoke<int>(N_GET_CLOSEST_VEHICLE, p.x, p.y, p.z, 7.f, 0, 70);
            if (!veh) veh = natives::invoke<int>(N_GET_CLOSEST_VEHICLE, p.x, p.y, p.z, 7.f, 0, 127);
            if (veh) { natives::invoke(N_TASK_ENTER_VEHICLE, ped, veh, 10000, -1, 2.f, 1, (const char*)nullptr); vrlog::write("B: enter vehicle %d", veh); }
            else vrlog::write("B: no vehicle within 7 m");
        }
    }
    if (pressed(s, IN_NEXT_WEAPON)) nextWeapon(ped);
    if (pressed(s, IN_HOLSTER)) { natives::invoke(N_SET_CURRENT_PED_WEAPON, ped, joaat("WEAPON_UNARMED"), 1); vrlog::write("weapon: holstered"); }
}
// 0.4.6 diagnostics: every button / trigger change is logged (first 300), so a log shows which controller buttons
// actually reach the mod on Pico Neo 3 through PICO Connect + SteamVR.
int g_inLog = 0;
void logInputs(const VrState& s) {
    for (int i = 0; i < IN_COUNT; ++i) {
        if (kInputs[i].type == XrType::Pose) continue;
        bool now = (i == IN_MOVE_X || i == IN_MOVE_Y || i == IN_TURN_X) ? fabsf(s.value[i]) > 0.5f : s.value[i] > 0.5f;
        if (now != g_prevBtn[i] && g_inLog < 300) { ++g_inLog; vrlog::write("input: %s %s (%.2f)", kInputs[i].action, now ? "DOWN" : "up", s.value[i]); }
        g_prevBtn[i] = now;
    }
}

// 0.6.6: virtual steering wheel. In a vehicle, holding both controllers in front of you like a wheel and turning them
// steers the car: the tilt of the left->right hand line (in the room, no matter where you look) is the wheel angle.
// Right hand lower = turn right. wheel_angle (deg, default 90) = full lock, 4 deg deadzone, the stick still overrides.
// Only active while the hands are 20-90 cm apart and in front of the head (resting hands on the lap -> no input).
// GTA5VR.ini [vr] wheel=0 turns it off.
int g_wheelIni = -1; float g_wheelMax = 90.f; ULONGLONG g_wheelDbg = 0; bool g_wheelOn = false;
float wheelSteer(const VrState& s) {
    if (g_wheelIni < 0) {
        char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string d = m; d = d.substr(0, d.find_last_of("\\/") + 1) + "GTA5VR.ini";
        g_wheelIni = GetPrivateProfileIntA("vr", "wheel", 1, d.c_str()) != 0 ? 1 : 0;
        g_wheelMax = (float)std::max(30, std::min(180, (int)GetPrivateProfileIntA("vr", "wheel_angle", 90, d.c_str())));
        vrlog::write("wheel: GTA5VR.ini wheel=%d wheel_angle=%.0f", g_wheelIni, g_wheelMax);
    }
    if (!g_wheelIni) return 0.f;
    const XrPoseF3& L = s.pose[IN_LEFT_GRIP_POSE]; const XrPoseF3& R = s.pose[IN_RIGHT_GRIP_POSE];
    if (!L.valid || !R.valid || !s.head.valid) return 0.f;
    V d = sub(xrPos(R), xrPos(L));                          // OpenXR: X right, Y up, -Z forward
    float horiz = sqrtf(d.x*d.x + d.z*d.z), dist = len(d);
    V mid = {(L.px + R.px) * 0.5f, (L.py + R.py) * 0.5f, (L.pz + R.pz) * 0.5f};
    V hf = qrot(s.head, {0, 0, -1}); hf.y = 0.f; { float l = len(hf); if (l > 1e-3f) { hf.x /= l; hf.z /= l; } }
    V toMid = sub(mid, xrPos(s.head));
    float ahead = toMid.x * hf.x + toMid.z * hf.z;       // how far in front of the head the hands are
    bool active = dist > 0.20f && dist < 0.90f && ahead > 0.12f && horiz > 0.05f;
    if (active != g_wheelOn) { g_wheelOn = active; vrlog::write("wheel: %s (hands %.2f m apart, %.2f m in front)", active ? "hands on the wheel" : "hands off the wheel", dist, ahead); }
    if (!active) return 0.f;
    float ang = atan2f(-d.y, horiz) * 57.29578f;          // right hand lower -> positive -> turn right
    float a = fabsf(ang) < 4.f ? 0.f : (ang > 0 ? ang - 4.f : ang + 4.f);
    float v = fmaxf(-1.f, fminf(1.f, a / (g_wheelMax - 4.f)));
    ULONGLONG now = GetTickCount64();
    if (fabsf(v) > 0.05f && now - g_wheelDbg > 2000) { g_wheelDbg = now; vrlog::write("wheel: angle %.0f deg -> steer %.2f", ang, v); }
    return v;
}

// systems.controller_input: every non-pose row of sheets/inputs.json feeds its GTA control
bool g_phoneOut = false;
void controllerInput(const VrState& s, bool inVehicle) {
    for (int i = 0; i < IN_COUNT; ++i) {
        if (g_phoneOut && (i == IN_MOVE_X || i == IN_MOVE_Y || i == IN_JUMP || i == IN_FIRE || i == IN_AIM || i == IN_RELOAD || i == IN_SPRINT)) continue;
        const InputRow& r = kInputs[i];
        if (r.type == XrType::Pose) continue;
        if (i == IN_FIRE && !inVehicle) continue;            // on foot, fire goes through weapon_aim
        int ctl = inVehicle ? r.controlVehicle : r.control;
        if (ctl < 0) continue;
        float v = s.value[i];
        // nothing is sent while a stick is centred / a trigger is released.
        // Axes take -1..1 (MOVE_UD: -1 = forward, MOVE_LR: +1 = right). 0.4.x sent 0..1 with 0.5 as centre,
        // which pushed the character backwards/right all the time ("crooked" controls).
        if (i == IN_MOVE_X && inVehicle && fabsf(v) < kDeadzone) {   // 0.6.6: stick idle -> virtual wheel steers
            float w = wheelSteer(s);
            if (w != 0.f) natives::invoke<int>(N_SET_CONTROL_VALUE_NEXT_FRAME, 0, ctl, w);
            continue;
        }
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

bool g_radarChecked = false; ULONGLONG g_mmTime = 0;
// 0.5.0 minimap: where GTA draws the radar, from the safe-zone setting (Settings > Display > Safezone size) and the
// aspect ratio - the formula the game's own HUD layout follows (radar = 1/(4*aspect) of the width, 0.188 of the
// height, inset 0.5*(1-safezone) from the left/bottom; the health/armour bars are inside that box).
void minimapSource() {
    ULONGLONG now = GetTickCount64();
    if (g_radarChecked && now - g_mmTime < 3000) return;
    g_mmTime = now;
    float sz = natives::invoke<float>(N_GET_SAFE_ZONE_SIZE); if (!(sz >= 0.85f && sz <= 1.01f)) sz = 1.f;
    float ar = natives::invoke<float>(N_GET_ASPECT_RATIO, 0); if (!(ar > 0.9f && ar < 5.f)) ar = 16.f / 9.f;
    float inset = 0.5f * (1.f - sz) * 1.f;                 // 0.05 * (1-sz) * 10
    float w = 1.f / (4.f * ar), h = 0.188f;
    float x0 = inset, yb = 1.f - inset;
    float pad = 0.006f;
    float sx0 = fmaxf(0.f, x0 - pad), sx1 = fminf(1.f, x0 + w + pad);
    float sy0 = fmaxf(0.f, yb - h - pad), sy1 = fminf(1.f, yb + pad);
    blit::setMinimapSource(sx0, sy0, sx1, sy1, ar);
    if (!g_radarChecked) {
        g_radarChecked = true;
        if (natives::invoke<int>(N_IS_RADAR_HIDDEN)) { natives::invoke(N_DISPLAY_RADAR, 1); vrlog::write("minimap: radar was hidden - switched on"); }
        vrlog::write("minimap: safezone %.2f, aspect %.2f -> radar at x %.3f-%.3f, y %.3f-%.3f of the screen", sz, ar, sx0, sx1, sy0, sy1);
    }
}
void releaseCamera() {
    g_radarChecked = false;
    showHead();
    g_neckPed = 0; g_camMode = 0;
    if (g_cam) { natives::invoke(N_DETACH_CAM, g_cam); natives::invoke(N_RENDER_SCRIPT_CAMS, 0, 0, 0, 1, 0, 0); natives::invoke(N_SET_CAM_ACTIVE, g_cam, 0); natives::invoke(N_DESTROY_CAM, g_cam, 0); g_cam = 0; }
}


// 0.6.0: short on-screen messages (bottom of the screen = also visible in the headset), Russian, UTF-8.
// A text component holds at most ~99 bytes, so long texts are split on UTF-8 character boundaries.
void showText(const char* t, int ms) {
    natives::invoke(N_BEGIN_TEXT_COMMAND_PRINT, "STRING");
    std::string all = t; size_t i = 0;
    while (i < all.size()) {
        size_t n = std::min<size_t>(90, all.size() - i);
        while (i + n < all.size() && ((unsigned char)all[i + n] & 0xC0) == 0x80) --n;
        std::string part = all.substr(i, n); i += n;
        natives::invoke(N_ADD_TEXT_COMPONENT_SUBSTRING_PLAYER_NAME, part.c_str());
    }
    natives::invoke(N_END_TEXT_COMMAND_PRINT, ms, 1);
}
// Russian text only when the game itself runs in Russian (its font then surely has Cyrillic), otherwise English
int g_lang = -1;
const char* tr(const char* ru, const char* en) { if (g_lang < 0) g_lang = natives::invoke<int>(N_GET_CURRENT_LANGUAGE); return g_lang == 7 ? ru : en; }
std::atomic<bool> g_autoOff{false};
bool g_wasRunning = false, g_hello = false; ULONGLONG g_onTime = 0, g_waitHint = 0;
}

namespace game {
void notify(const char*) { showText(tr("VR ещё запускается, подождите...", "VR is still starting, please wait..."), 3000); }
void toggle() {
    g_enabled = !g_enabled;
    if (g_enabled) { ++g_gen; g_yawRefSet = false; g_wasRunning = false; g_onTime = GetTickCount64(); g_waitHint = 0; showText(tr("VR включается... Наденьте шлем и смотрите прямо.", "VR is starting... Put the headset on and look straight ahead."), 5000); }
    else showText(tr("VR выключен. F8 - включить снова.", "VR off. F8 = turn it on again."), 3000);
    vrlog::write("F8: VR %s", g_enabled ? "on" : "off");
}
// called from the render thread: the message is shown by the script thread in tick()
void forceOff() { if (g_enabled) { g_enabled = false; g_autoOff = true; vrlog::write("VR switched off automatically"); } }
int generation() { return g_gen; }
void recenter() { g_yawRefSet = false; vrlog::write("F9: recenter"); if (g_enabled) showText(tr("Направление сброшено: \"прямо\" - туда, куда вы смотрите.", "Recentered: straight ahead = where you look now."), 2500); }
bool enabled() { return g_enabled; }

bool g_inSwitch = false;
void toggleArms() { g_armsOn = !g_armsOn; vrlog::write("F11: arm IK %s", g_armsOn ? "on" : "off"); }

void tick() {
    if (!natives::ready()) return;
    keyTick();   // 0.6.4: release a simulated key press after ~70 ms (also when VR is off)
    bool loading = natives::invoke<int>(N_GET_IS_LOADING_SCREEN_ACTIVE) != 0;
    if (g_autoOff.exchange(false)) showText(tr("VR выключился из-за ошибки. Проверьте, что SteamVR запущен и шлем подключён, и нажмите F8.", "VR stopped because of an error. Check that SteamVR runs and the headset is connected, then press F8."), 7000);
    if (!g_hello && !loading && !g_enabled) { g_hello = true; showText(tr("GTA V Hands VR: запустите SteamVR, наденьте шлем и нажмите F8.", "GTA V Hands VR: start SteamVR, put the headset on and press F8."), 7000); }
    if (!onlineGuard()) { if (g_enabled) { g_enabled = false; showText(tr("GTA Online: VR-мод отключён (только сюжетный режим).", "GTA Online: VR mod switched off (story mode only)."), 5000); } releaseCamera(); return; }
    if (!g_enabled) { releaseCamera(); return; }
    if (loading) { releaseCamera(); return; }
    if (natives::invoke<int>(N_IS_PAUSE_MENU_ACTIVE)) {
        // 0.6.4: pause menu from the controllers (the image in the headset is the flat game screen meanwhile)
        releaseCamera();
        VrState m = xr::snapshot();
        if (m.running) {
            stickArrows(m.value[IN_MOVE_X], m.value[IN_MOVE_Y], "pause menu: left stick");
            if (pressed(m, IN_JUMP) || (m.value[IN_FIRE] > kTrigger && !g_prevBtn[IN_FIRE])) tapKey(VK_RETURN, "pause menu: A / right trigger = select");
            if (pressed(m, IN_ENTER_VEHICLE)) tapKey(VK_BACK, "pause menu: B = back");
            if (pressed(m, IN_MENU)) tapKey(VK_ESCAPE, "pause menu: menu button = close");
            int tab = m.value[IN_TURN_X] > 0.7f ? 1 : (m.value[IN_TURN_X] < -0.7f ? -1 : 0);
            if (tab && tab != g_tabDir) tapKey(tab > 0 ? 'E' : 'Q', "pause menu: right stick = tab");
            g_tabDir = tab;
            logInputs(m);
        }
        return;
    }
    // 0.5.0: character switch (Michael / Franklin / Trevor): the game flies its own sky camera - let it, then take over
    // the new character (camera, head hiding, hands are re-bound to it in hands.cpp)
    if (natives::invoke<int>(N_IS_PLAYER_SWITCH_IN_PROGRESS)) {
        if (!g_inSwitch) { g_inSwitch = true; vrlog::write("switch: character switch started - VR camera paused"); }
        releaseCamera(); return;
    }
    if (g_inSwitch) { g_inSwitch = false; g_yawRefSet = false; vrlog::write("switch: finished - VR camera on the new character"); }
    VrState s = xr::snapshot();
    if (!s.running) {
        releaseCamera();
        ULONGLONG now = GetTickCount64();
        if (now - g_onTime > 8000 && now - g_waitHint > 10000) { g_waitHint = now; showText(tr("Жду шлем... Проверьте, что SteamVR запущен и шлем подключён.", "Waiting for the headset... Check that SteamVR runs and the headset is connected."), 5000); }
        return;
    }
    if (!g_wasRunning) { g_wasRunning = true; showText(tr("VR включён. F8 - выключить, F9 - смотреть прямо.", "VR on. F8 = off, F9 = recenter."), 5000); }
    if (g_lastTick && GetTickCount64() - g_lastTick > 500) g_lastTick = 0;
    int ped = natives::invoke<int>(N_PLAYER_PED_ID);
    bool inVehicle = natives::invoke<int>(N_IS_PED_IN_ANY_VEHICLE, ped, 0) != 0;
    float dt = frameDt();
    snapTurn(s, dt);
    float yaw = frameYaw(ped, s);
    V anchor = headCamera(ped, s, yaw, inVehicle);
    bodyFollow(ped, s, inVehicle, dt);
    minimapSource();
    armFollow(ped, s, inVehicle, yaw, anchor);
    // 0.6.2: the game pulls the free (left) hand onto the gun grip for a two-handed hold - in VR each hand belongs to its
    // own controller. Reset flags last one frame, so it is set every frame. CPED_RESET_FLAG_CancelLeftHandGripIk = 324.
    if (!inVehicle) natives::invoke(N_SET_PED_RESET_FLAG, ped, 324, 1);
    { const XrPoseF3* g[2] = {&s.pose[IN_LEFT_GRIP_POSE], &s.pose[IN_RIGHT_GRIP_POSE]}; hands::tick(ped, g, yaw); }
    hideHead(ped);
    weaponAim(ped, s, inVehicle, yaw, anchor);
    g_phoneOut = phoneButtons(ped, s);
    if (!g_phoneOut) specialButtons(ped, s, inVehicle);
    controllerInput(s, inVehicle);
    logInputs(s);   // must run last: updates the previous-button state used by pressed()
}
}
