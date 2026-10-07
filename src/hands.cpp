// Hand (wrist) rotation from the controllers' orientation - EXPERIMENTAL, F12.
// Script Hook V has no native that rotates a ped bone, so this writes the bone matrices in the ped's skeleton directly.
// The skeleton layout of GTA5.exe 1.0.3889.0 is not hard-coded: on the first F12 the mod SEARCHES for it:
//   - asks the game (natives) where 4 bones of the player are in the world,
//   - walks pointers reachable from the player's ped object (3 levels, only readable memory - VirtualQuery checked),
//   - looks for an array of 4x4 float matrices (64-byte stride) whose rows 3 hold exactly those bone positions,
//     either in object space (relative to the ped) or in world space.
// The pointer path found is logged. F12 again turns rotation writing on/off. Every write is preceded by re-reading the
// path and re-checking that the hand bone is still where the game says it is; on any mismatch writing stops.
// Calibration: the moment writing is switched on, the current hand pose = current controller pose, so bone axes
// never have to be known; afterwards the hand turns exactly as the controller turns.
#include <windows.h>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string>
#include <cstdio>
#include <atomic>
#include <mutex>
#include "natives.h"
#include "log.h"
#include "hands.h"

namespace {
struct M3 { float m[3][3]; };   // m[row][col]; columns = basis vectors
M3 mul(const M3& a, const M3& b) { M3 r{}; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j]; return r; }
M3 tr(const M3& a) { M3 r{}; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i]; return r; }
M3 fromQuat(float x, float y, float z, float w) {
    M3 r{};
    r.m[0][0] = 1 - 2*(y*y + z*z); r.m[0][1] = 2*(x*y - z*w);     r.m[0][2] = 2*(x*z + y*w);
    r.m[1][0] = 2*(x*y + z*w);     r.m[1][1] = 1 - 2*(x*x + z*z); r.m[1][2] = 2*(y*z - x*w);
    r.m[2][0] = 2*(x*z - y*w);     r.m[2][1] = 2*(y*z + x*w);     r.m[2][2] = 1 - 2*(x*x + y*y);
    return r;
}
// OpenXR (X right, Y up, -Z forward) -> GTA (X right, Y forward, Z up), then yaw about Z (same as game.cpp xrToGta)
M3 xrToGtaRot(float yawDeg) {
    M3 c{}; c.m[0][0] = 1; c.m[1][2] = -1; c.m[2][1] = 1;
    float h = yawDeg * 0.0174533f, cs = cosf(h), sn = sinf(h);
    M3 z{}; z.m[0][0] = cs; z.m[0][1] = -sn; z.m[1][0] = sn; z.m[1][1] = cs; z.m[2][2] = 1;
    return mul(z, c);
}

// ---------- safe memory reads ----------
std::unordered_map<uintptr_t, bool> g_pageOk;
bool readable(uintptr_t a, size_t n) {
    if (a < 0x10000 || a > 0x7FFFFFFFFFFFull) return false;
    for (uintptr_t p = a & ~0xFFFull; p < a + n; p += 0x1000) {
        auto it = g_pageOk.find(p);
        bool ok;
        if (it != g_pageOk.end()) ok = it->second;
        else {
            MEMORY_BASIC_INFORMATION mi{};
            ok = VirtualQuery((void*)p, &mi, sizeof(mi)) == sizeof(mi) && mi.State == MEM_COMMIT &&
                 !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                 (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
            g_pageOk[p] = ok;
        }
        if (!ok) return false;
    }
    return true;
}
bool writable(uintptr_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi{};
    return VirtualQuery((void*)a, &mi, sizeof(mi)) == sizeof(mi) && mi.State == MEM_COMMIT && (mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
           a + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize;
}
bool rdPtr(uintptr_t a, uintptr_t& out) { if (!readable(a, 8)) return false; out = *(uintptr_t*)a; return out > 0x10000 && out < 0x7FFFFFFFFFFFull && (out & 7) == 0; }

// ---------- state ----------
struct Bone { int id; int index; float world[3]; float obj[3]; };
int g_offs[3] = {-1, -1, -1};   // pointer path from the ped object: ped+o0 -> +o1 -> +o2 (unused levels = -1)
int g_depth = 0;                 // how many dereferences lead to the matrix array
bool g_worldSpace = false;       // matrices hold world positions (else object space)
bool g_found = false, g_searched = false, g_on = false, g_wantSearch = false;
int g_handIdx[2] = {-1, -1};     // left, right bone indices
int g_headIdx = -1;               // SKEL_Head index
int g_neckIdx = -1;               // SKEL_Neck_1 index: collapse point for the hidden head (0.4.5)
// 0.4.4: head hiding. Bones of the head/face/hair = bones whose position (at search time) is within 0.25 m of the
// head bone and not below the neck. While hiding, their rotation/scale part is written as 0.001 (vertices collapse
// into the bone = invisible); translation is not touched. The game rebuilds the matrices every frame, so when the
// mod stops writing (VR off) the head is back the very next frame.
std::vector<int> g_headBones; std::atomic<bool> g_hide{false};
// 0.4.6 wrist rotation: absolute, no calibration. The hand's current frame is built from the skeleton itself
// (wrist -> middle-finger knuckle = forward, pinky knuckle -> index knuckle = up), the wanted frame from the controller
// grip pose (-Z = forward, +Y = up), and the difference is applied to the hand AND all its finger bones around the wrist.
// 0.3.0-0.4.5 rotated only the hand matrix with a calibration taken at F12 -> fingers stayed behind, sleeves/arms stretched.
int g_fing[2][20]; int g_fingN[2] = {0, 0}; int g_kIdx[2] = {-1, -1}, g_kMid[2] = {-1, -1}, g_kPinky[2] = {-1, -1};
float g_last[2][9]; bool g_lastSet[2] = {false, false};
std::atomic<unsigned> g_wApplied{0}; std::atomic<int> g_lastAngle[2] = {{0}, {0}};
bool g_userOff = false; int g_rotIni = -1; int g_autoSearch = 0; float g_pitchOff = 0.f, g_yawOff = 0.f, g_rollOff = 0.f; int g_flip = 1;
// Background writer: the game recomputes the skeleton after scripts run (0.2.9 log: "overwritten by the game"), so a
// write from the script thread is lost before rendering. A separate thread re-writes the target rotation continuously,
// hitting the window between the animation update and the copy to the renderer. Translation is never touched.
// Safety: it only writes while the script thread refreshed the target within the last 50 ms (stops in menus, on death,
// on VR off, on any failed check), and only into the array the script thread validated this frame.
std::atomic<uintptr_t> g_wArr{0}; std::atomic<ULONGLONG> g_wBeat{0}; std::mutex g_wMtx;
float g_wTarget[2][3][3]; bool g_wHave[2] = {false, false}; std::atomic<unsigned> g_wCount{0};   // wanted hand frame (fwd, up, right), array space
HANDLE g_wThread = nullptr;
typedef float F3[3];
void nrm(float* v) { float l = sqrtf(v[0]*v[0] + v[1]*v[1] + v[2]*v[2]); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
// rotate hand + fingers so the hand frame matches tgt (columns fwd, up, right). Writer thread only.
void applyHand(uintptr_t arr, int h, const float tgt[3][3]) {
    float* H = (float*)(arr + (uintptr_t)g_handIdx[h] * 64);
    if (g_lastSet[h]) { float d = 0; for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) d += fabsf(H[c * 4 + r] - g_last[h][c * 3 + r]); if (d < 1e-4f) return; }   // still ours
    if (g_kIdx[h] < 0 || g_kMid[h] < 0 || g_kPinky[h] < 0) return;
    const float* w = H + 12; const float* km = (const float*)(arr + (uintptr_t)g_kMid[h] * 64 + 48);
    const float* ki = (const float*)(arr + (uintptr_t)g_kIdx[h] * 64 + 48); const float* kp = (const float*)(arr + (uintptr_t)g_kPinky[h] * 64 + 48);
    float f[3] = {km[0] - w[0], km[1] - w[1], km[2] - w[2]}; nrm(f);
    float u[3] = {ki[0] - kp[0], ki[1] - kp[1], ki[2] - kp[2]};
    float dp = u[0]*f[0] + u[1]*f[1] + u[2]*f[2]; for (int i = 0; i < 3; ++i) u[i] -= dp * f[i]; nrm(u);
    float r[3] = {f[1]*u[2] - f[2]*u[1], f[2]*u[0] - f[0]*u[2], f[0]*u[1] - f[1]*u[0]};
    if (!(fabsf(r[0]) + fabsf(r[1]) + fabsf(r[2]) > 0.5f)) return;
    M3 Fb{}; for (int i = 0; i < 3; ++i) { Fb.m[i][0] = f[i]; Fb.m[i][1] = u[i]; Fb.m[i][2] = r[i]; }
    M3 Fc{}; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) Fc.m[i][j] = tgt[j][i];
    M3 D = mul(Fc, tr(Fb));
    float tr3 = D.m[0][0] + D.m[1][1] + D.m[2][2]; g_lastAngle[h] = (int)(acosf(fmaxf(-1.f, fminf(1.f, (tr3 - 1.f) * 0.5f))) * 57.3f);
    float wp[3] = {w[0], w[1], w[2]};
    auto rotBone = [&](int idx, bool movePos) {
        float* B = (float*)(arr + (uintptr_t)idx * 64);
        for (int c = 0; c < 3; ++c) { float v[3] = {B[c*4], B[c*4+1], B[c*4+2]}; for (int i = 0; i < 3; ++i) B[c*4+i] = D.m[i][0]*v[0] + D.m[i][1]*v[1] + D.m[i][2]*v[2]; }
        if (movePos) { float p[3] = {B[12] - wp[0], B[13] - wp[1], B[14] - wp[2]}; for (int i = 0; i < 3; ++i) B[12+i] = wp[i] + D.m[i][0]*p[0] + D.m[i][1]*p[1] + D.m[i][2]*p[2]; }
    };
    for (int k = 0; k < g_fingN[h]; ++k) rotBone(g_fing[h][k], true);
    rotBone(g_handIdx[h], false);
    for (int c = 0; c < 3; ++c) for (int rr = 0; rr < 3; ++rr) g_last[h][c * 3 + rr] = H[c * 4 + rr];
    g_lastSet[h] = true; g_wApplied.fetch_add(1);
}
DWORD WINAPI writer(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // 0.4.3: was ABOVE_NORMAL + hot spin, starved the frame copy
    for (;;) {
        uintptr_t arr = g_wArr.load();
        if (!arr || GetTickCount64() - g_wBeat.load() > 50) { Sleep(5); continue; }
        float t[2][3][3]; bool have[2];
        { std::lock_guard<std::mutex> l(g_wMtx); memcpy(t, g_wTarget, sizeof(t)); have[0] = g_wHave[0]; have[1] = g_wHave[1]; }
        if (g_hide.load()) {
            std::vector<int> hb; { std::lock_guard<std::mutex> l(g_wMtx); hb = g_headBones; }
            // 0.4.5: every hidden bone is also MOVED to the neck (except SKEL_Head, whose position the safety check reads).
            // 0.4.4 collapsed each bone at its own place: skin weighted between head and neck stretched into spikes
            // ("artifacts"), hair/face bits stayed around. Now it all shrinks into the neck, under the camera.
            int nk = g_neckIdx, hd = g_headIdx;
            float np[3] = {0, 0, 0}; bool haveNeck = nk >= 0;
            if (haveNeck) { const float* n = (const float*)(arr + (uintptr_t)nk * 64 + 48); np[0] = n[0]; np[1] = n[1]; np[2] = n[2]; }
            for (int i : hb) {
                float* f = (float*)(arr + (uintptr_t)i * 64);
                for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) f[c * 4 + r] = (c == r) ? 0.0001f : 0.f;
                if (haveNeck && i != hd) { f[12] = np[0]; f[13] = np[1]; f[14] = np[2]; }
            }
        }
        for (int h = 0; h < 2; ++h) if (have[h]) applyHand(arr, h, t[h]);
        g_wCount.fetch_add(1);
        for (int i = 0; i < 200; ++i) YieldProcessor();
        if ((g_wCount.load() & 63) == 0) Sleep(0);   // 0.4.3: give the core back regularly
    }
}

const int kBoneIds[4] = {18905 /*SKEL_L_Hand*/, 57005 /*SKEL_R_Hand*/, 31086 /*SKEL_Head*/, 0x2e28 /*SKEL_Pelvis*/};

bool entityMatrix(uintptr_t ped, M3& E, float pos[3]) {
    // CEntity transform at +0x60: four float4 rows = right, forward, up, position (layout stable since 2015)
    if (!readable(ped + 0x60, 64)) return false;
    const float* f = (const float*)(ped + 0x60);
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) E.m[r][c] = f[c * 4 + r];
    for (int i = 0; i < 3; ++i) pos[i] = f[12 + i];
    return true;
}

bool matchArray(uintptr_t arr, const std::vector<Bone>& b, bool world) {
    for (const Bone& x : b) {
        uintptr_t a = arr + (uintptr_t)x.index * 64 + 48;
        if (!readable(a, 12)) return false;
        const float* t = (const float*)a; const float* want = world ? x.world : x.obj;
        for (int i = 0; i < 3; ++i) if (!(fabsf(t[i] - want[i]) < 0.03f)) return false;
    }
    return true;
}

uintptr_t resolve(uintptr_t ped) {
    uintptr_t p = ped;
    for (int d = 0; d < g_depth; ++d) { uintptr_t n; if (!rdPtr(p + g_offs[d], n)) return 0; p = n; }
    return p;
}

void search(int ped, uintptr_t pedAddr) {
    g_searched = true; g_pageOk.clear();
    M3 E; float epos[3];
    if (!entityMatrix(pedAddr, E, epos)) { vrlog::write("hands: ped object not readable"); return; }
    Vector3 c = natives::invokeV3(N_GET_ENTITY_COORDS, ped, 1);
    vrlog::write("hands: ped object %p, entity matrix pos (%.2f %.2f %.2f) vs game coords (%.2f %.2f %.2f)", (void*)pedAddr, epos[0], epos[1], epos[2], c.x, c.y, c.z);
    if (fabsf(epos[0] - c.x) > 0.5f || fabsf(epos[1] - c.y) > 0.5f) { vrlog::write("hands: entity matrix offset 0x60 does not match - stop"); return; }
    std::vector<Bone> bones;
    for (int id : kBoneIds) {
        Bone b{}; b.id = id; b.index = natives::invoke<int>(N_GET_PED_BONE_INDEX, ped, id);
        if (b.index < 0 || b.index > 300) { vrlog::write("hands: bone %d index %d - stop", id, b.index); return; }
        Vector3 w = natives::invokeV3(N_GET_WORLD_POSITION_OF_ENTITY_BONE, ped, b.index);
        b.world[0] = w.x; b.world[1] = w.y; b.world[2] = w.z;
        float d[3] = {w.x - epos[0], w.y - epos[1], w.z - epos[2]};
        for (int i = 0; i < 3; ++i) b.obj[i] = E.m[0][i] * d[0] + E.m[1][i] * d[1] + E.m[2][i] * d[2];   // E^T * d
        vrlog::write("hands: bone %d index %d world (%.2f %.2f %.2f) object (%.3f %.3f %.3f)", id, b.index, w.x, w.y, w.z, b.obj[0], b.obj[1], b.obj[2]);
        bones.push_back(b);
    }
    g_handIdx[0] = bones[0].index; g_handIdx[1] = bones[1].index;
    ULONGLONG t0 = GetTickCount64(); int tested = 0;
    for (int world = 0; world < 2 && !g_found; ++world)
    for (int o0 = 0; o0 < 0x1400 && !g_found; o0 += 8) {
        uintptr_t p1; if (!rdPtr(pedAddr + o0, p1)) continue;
        ++tested; if (matchArray(p1, bones, world)) { g_depth = 1; g_offs[0] = o0; g_found = true; g_worldSpace = world; break; }
        for (int o1 = 0; o1 < 0x100 && !g_found; o1 += 8) {
            uintptr_t p2; if (!rdPtr(p1 + o1, p2)) continue;
            ++tested; if (matchArray(p2, bones, world)) { g_depth = 2; g_offs[0] = o0; g_offs[1] = o1; g_found = true; g_worldSpace = world; break; }
            for (int o2 = 0; o2 < 0x60; o2 += 8) {
                uintptr_t p3; if (!rdPtr(p2 + o2, p3)) continue;
                ++tested; if (matchArray(p3, bones, world)) { g_depth = 3; g_offs[0] = o0; g_offs[1] = o1; g_offs[2] = o2; g_found = true; g_worldSpace = world; break; }
            }
        }
    }
    if (g_found) {
        uintptr_t arr = resolve(pedAddr);
        int count = natives::invoke<int>(N_GET_ENTITY_BONE_COUNT, ped);
        if (count <= 0 || count > 400) count = 200;
        g_headIdx = bones[2].index;
        g_neckIdx = natives::invoke<int>(N_GET_PED_BONE_INDEX, ped, 39317 /*SKEL_Neck_1*/);
        if (g_neckIdx < 0 || g_neckIdx >= count) g_neckIdx = -1;
        const float* hp = (const float*)(arr + (uintptr_t)g_headIdx * 64 + 48);
        float head[3] = {hp[0], hp[1], hp[2]};
        std::vector<int> hb;
        for (int i = 0; i < count; ++i) {
            if (i == g_handIdx[0] || i == g_handIdx[1] || i == g_neckIdx) continue;
            uintptr_t a = arr + (uintptr_t)i * 64;
            if (!readable(a, 64)) break;
            const float* t = (const float*)(a + 48);
            float dx = t[0] - head[0], dy = t[1] - head[1], dz = t[2] - head[2];
            if (g_worldSpace) { float w[3] = {dx, dy, dz}; dx = E.m[0][0]*w[0]+E.m[1][0]*w[1]+E.m[2][0]*w[2]; dy = E.m[0][1]*w[0]+E.m[1][1]*w[1]+E.m[2][1]*w[2]; dz = E.m[0][2]*w[0]+E.m[1][2]*w[1]+E.m[2][2]*w[2]; }
            // fingers of a hand raised near the face at search time must not be hidden: skip bones close to a hand
            bool nearHand = false;
            for (int h = 0; h < 2; ++h) {
                const float* hp2 = (const float*)(arr + (uintptr_t)g_handIdx[h] * 64 + 48);
                float ex = t[0] - hp2[0], ey = t[1] - hp2[1], ez = t[2] - hp2[2];
                if (ex*ex + ey*ey + ez*ez < 0.18f*0.18f) nearHand = true;
            }
            if (!nearHand && dx*dx + dy*dy + dz*dz < 0.30f*0.30f && dz > -0.07f) hb.push_back(i);   // 0.4.5: was 0.25 m / -0.04 (hair, neck top stayed)
        }
        { std::lock_guard<std::mutex> l(g_wMtx); g_headBones = hb; }
        // fingers (+ weapon grip point PH_*_Hand) of each hand; knuckles 10 = index, 20 = middle, 40 = pinky
        static const int fl[2][16] = {
            {26610,4089,4090, 26611,4169,4170, 26612,4185,4186, 26613,4137,4138, 26614,4153,4154, 60309},
            {58866,64016,64017, 58867,64096,64097, 58868,64112,64113, 58869,64064,64065, 58870,64080,64081, 28422}};
        for (int h = 0; h < 2; ++h) {
            g_fingN[h] = 0;
            for (int k = 0; k < 16; ++k) {
                int bi = natives::invoke<int>(N_GET_PED_BONE_INDEX, ped, fl[h][k]);
                if (bi < 0 || bi >= count || bi == g_handIdx[h]) continue;
                g_fing[h][g_fingN[h]++] = bi;
                if (k == 3) g_kIdx[h] = bi; if (k == 6) g_kMid[h] = bi; if (k == 12) g_kPinky[h] = bi;
            }
            vrlog::write("hands: %s hand %d finger bones, knuckles index %d middle %d pinky %d", h ? "right" : "left", g_fingN[h], g_kIdx[h], g_kMid[h], g_kPinky[h]);
        }
        vrlog::write("hands: %d head/face bones of %d will be hidden in VR (collapsed into neck bone index %d)", (int)hb.size(), count, g_neckIdx);
        char path[96]; snprintf(path, sizeof(path), "ped+0x%X", g_offs[0]);
        for (int d = 1; d < g_depth; ++d) { size_t l = strlen(path); snprintf(path + l, sizeof(path) - l, " ->+0x%X", g_offs[d]); }
        vrlog::write("hands: FOUND bone matrices (%s space) at %s, %d candidates, %llu ms. (F12 = hand rotation)",
            g_worldSpace ? "world" : "object", path, tested, GetTickCount64() - t0);
    }
    else vrlog::write("hands: bone matrices NOT found (%d candidates, %llu ms) - send this log", tested, GetTickCount64() - t0);
}
}

namespace hands {
void setHideHead(bool on) {
    if (g_hide.load() == on) return;
    g_hide = on;
    if (on) {
        if (!g_searched) g_wantSearch = true;
        if (!g_wThread) g_wThread = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr);
        vrlog::write("head: hiding %s", g_found ? "on" : "on (searching the skeleton first, the game may pause for ~1 s)");
    } else vrlog::write("head: shown again");
}
void toggle() {
    if (!g_found) { g_searched = false; g_wantSearch = true; vrlog::write("F12: searching the skeleton (VR must be on, stand still)..."); return; }
    g_on = !g_on; g_userOff = !g_on; g_lastSet[0] = g_lastSet[1] = false;
    g_wArr = 0; { std::lock_guard<std::mutex> l(g_wMtx); g_wHave[0] = g_wHave[1] = false; }
    if (!g_on && !g_hide.load()) g_wArr = 0;
    if (g_on && !g_wThread) g_wThread = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr);
    vrlog::write("F12: hand rotation %s", g_on ? "ON" : "off");
}

void tick(int ped, const XrPoseF3* gripLeftRight[2], float yawDeg) {
    uintptr_t pedAddr = (uintptr_t)shv::entityAddress(ped);
    if (g_rotIni < 0) {
        char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string d = m; d = d.substr(0, d.find_last_of("\\/") + 1) + "GTA5VR.ini";
        if (GetPrivateProfileIntA("vr", "hand_rotation", -12345, d.c_str()) == -12345) WritePrivateProfileStringA("vr", "hand_rotation", "1", d.c_str());
        if (GetPrivateProfileIntA("vr", "hand_pitch", -12345, d.c_str()) == -12345) WritePrivateProfileStringA("vr", "hand_pitch", "0", d.c_str());
        g_rotIni = GetPrivateProfileIntA("vr", "hand_rotation", 1, d.c_str()) ? 1 : 0;
        g_pitchOff = (int)GetPrivateProfileIntA("vr", "hand_pitch", 0, d.c_str()) * 0.0174533f;
        // 0.4.7: hand_flip (1 = fixed direction, 0 = 0.4.6 behaviour), hand_yaw / hand_roll extra offsets in degrees
        if (GetPrivateProfileIntA("vr", "hand_flip", -12345, d.c_str()) == -12345) WritePrivateProfileStringA("vr", "hand_flip", "1", d.c_str());
        if (GetPrivateProfileIntA("vr", "hand_yaw", -12345, d.c_str()) == -12345) WritePrivateProfileStringA("vr", "hand_yaw", "0", d.c_str());
        if (GetPrivateProfileIntA("vr", "hand_roll", -12345, d.c_str()) == -12345) WritePrivateProfileStringA("vr", "hand_roll", "0", d.c_str());
        g_flip = GetPrivateProfileIntA("vr", "hand_flip", 1, d.c_str()) ? 1 : 0;
        g_yawOff = (int)GetPrivateProfileIntA("vr", "hand_yaw", 0, d.c_str()) * 0.0174533f;
        g_rollOff = (int)GetPrivateProfileIntA("vr", "hand_roll", 0, d.c_str()) * 0.0174533f;
        vrlog::write("hands: GTA5VR.ini hand_rotation=%d hand_flip=%d hand_pitch=%.0f hand_yaw=%.0f hand_roll=%.0f", g_rotIni, g_flip, g_pitchOff * 57.3f, g_yawOff * 57.3f, g_rollOff * 57.3f);
    }
    if (g_rotIni == 1 && !g_userOff && !g_searched && !g_wantSearch && g_autoSearch < 3) { ++g_autoSearch; g_wantSearch = true; vrlog::write("hands: searching the skeleton for hand rotation (game may pause ~1 s)"); }
    if (g_rotIni == 1 && !g_userOff && g_found && !g_on) { g_on = true; g_lastSet[0] = g_lastSet[1] = false; vrlog::write("hands: hand rotation ON (F12 = off)"); }
    if (g_on && !g_wThread) g_wThread = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr);
    if (!g_searched) {
        if (!g_wantSearch) return;
        g_wantSearch = false;
        if (!pedAddr) { vrlog::write("hands: Script Hook V has no getScriptHandleBaseAddress - not possible"); g_searched = true; return; }
        search(ped, pedAddr);
        return;
    }
    if ((!g_on && !g_hide.load()) || !g_found || !pedAddr) { g_wArr = 0; return; }
    uintptr_t arr = resolve(pedAddr);
    M3 E; float epos[3];
    if (!arr || !entityMatrix(pedAddr, E, epos)) { g_wArr = 0; vrlog::write("hands: path no longer valid - rotation and head hiding off"); g_on = false; g_found = false; return; }
    {   // the head bone must be where the game says (also guards the head hiding)
        uintptr_t m = arr + (uintptr_t)g_headIdx * 64;
        if (g_headIdx < 0 || !readable(m, 64) || !writable(m, 48)) { g_wArr = 0; vrlog::write("hands: skeleton not writable - off"); g_on = false; g_found = false; return; }
        Vector3 w = natives::invokeV3(N_GET_WORLD_POSITION_OF_ENTITY_BONE, ped, g_headIdx);
        float want[3] = {w.x, w.y, w.z};
        if (!g_worldSpace) { float d[3] = {w.x - epos[0], w.y - epos[1], w.z - epos[2]}; for (int i = 0; i < 3; ++i) want[i] = E.m[0][i]*d[0] + E.m[1][i]*d[1] + E.m[2][i]*d[2]; }
        const float* f = (const float*)m;
        for (int i = 0; i < 3; ++i) if (!(fabsf(f[12 + i] - want[i]) < 0.15f)) {
            g_wArr = 0; vrlog::write("hands: head bone check failed (%.2f vs %.2f) - will search again", f[12 + i], want[i]);
            static int re = 0; g_found = false; g_searched = false; g_on = false; if (g_hide.load() && ++re <= 3) g_wantSearch = true; return;
        }
    }
    if (!g_on) { g_wArr = arr; g_wBeat = GetTickCount64(); return; }   // head hiding only
    static ULONGLONG lastRate = 0;
    if (GetTickCount64() - lastRate > 5000) { lastRate = GetTickCount64(); vrlog::write("hands: rotation applied %u times in 5 s (writer passes %u), angle to controller L %d R %d deg", g_wApplied.exchange(0), g_wCount.exchange(0), g_lastAngle[0].load(), g_lastAngle[1].load()); }
    int maxIdx = 0; for (int h = 0; h < 2; ++h) { maxIdx = maxIdx > g_handIdx[h] ? maxIdx : g_handIdx[h]; for (int k = 0; k < g_fingN[h]; ++k) maxIdx = maxIdx > g_fing[h][k] ? maxIdx : g_fing[h][k]; }
    if (!readable(arr, (size_t)(maxIdx + 1) * 64) || !writable(arr + (uintptr_t)g_handIdx[0] * 64, 48) || !writable(arr + (uintptr_t)maxIdx * 64, 64)) { g_wArr = 0; vrlog::write("hands: matrices not writable - rotation off"); g_on = false; g_userOff = true; return; }
    M3 Et = tr(E);
    for (int h = 0; h < 2; ++h) {
        const XrPoseF3* p = gripLeftRight[h];
        if (!p || !p->valid) { std::lock_guard<std::mutex> l(g_wMtx); g_wHave[h] = false; continue; }
        // controller axes (OpenXR grip: -Z forward, +Y up), optional pitch offset about the controller X axis
        float cp = cosf(g_pitchOff), sp = sinf(g_pitchOff), cy = cosf(g_yawOff), sy = sinf(g_yawOff), cr = cosf(g_rollOff), sr = sinf(g_rollOff);
        M3 R = mul(mul(mul(fromQuat(p->qx, p->qy, p->qz, p->qw), M3{{{1,0,0},{0,cp,-sp},{0,sp,cp}}}), M3{{{cy,0,sy},{0,1,0},{-sy,0,cy}}}), M3{{{cr,-sr,0},{sr,cr,0},{0,0,1}}});
        // A fist closed around the controller handle: the knuckle row (pinky -> index) runs along the handle = grip -Z,
        // wrist -> middle knuckle is across it = grip +Y. (0.3.x-0.4.5 effectively matched hand forward to -Z: wrists bent.)
        // 0.4.7: OpenXR grip +X = right (palm side reversed per hand), -Z = fist axis, +Y = Z x X. Holding a pistol grip
        // (-Z up, +X right) gives +Y = BACKWARD, so wrist -> knuckles (forward) is grip -Y. 0.4.6 used +Y: every hand was
        // turned 180 deg about the knuckle row (log: "angle to controller" 150-175 deg, pistol pointing at the player).
        float sg = g_flip ? -1.f : 1.f;
        float fx[3] = {sg * R.m[0][1], sg * R.m[1][1], sg * R.m[2][1]}, ux[3] = {-R.m[0][2], -R.m[1][2], -R.m[2][2]};
        M3 Mw = xrToGtaRot(yawDeg);
        float fw[3], uw[3];
        for (int i = 0; i < 3; ++i) { fw[i] = Mw.m[i][0]*fx[0] + Mw.m[i][1]*fx[1] + Mw.m[i][2]*fx[2]; uw[i] = Mw.m[i][0]*ux[0] + Mw.m[i][1]*ux[1] + Mw.m[i][2]*ux[2]; }
        float fo[3], uo[3];
        if (g_worldSpace) { for (int i = 0; i < 3; ++i) { fo[i] = fw[i]; uo[i] = uw[i]; } }
        else for (int i = 0; i < 3; ++i) { fo[i] = Et.m[i][0]*fw[0] + Et.m[i][1]*fw[1] + Et.m[i][2]*fw[2]; uo[i] = Et.m[i][0]*uw[0] + Et.m[i][1]*uw[1] + Et.m[i][2]*uw[2]; }
        nrm(fo); float d = uo[0]*fo[0] + uo[1]*fo[1] + uo[2]*fo[2]; for (int i = 0; i < 3; ++i) uo[i] -= d * fo[i]; nrm(uo);
        float ro[3] = {fo[1]*uo[2] - fo[2]*uo[1], fo[2]*uo[0] - fo[0]*uo[2], fo[0]*uo[1] - fo[1]*uo[0]};
        std::lock_guard<std::mutex> l(g_wMtx);
        for (int i = 0; i < 3; ++i) { g_wTarget[h][0][i] = fo[i]; g_wTarget[h][1][i] = uo[i]; g_wTarget[h][2][i] = ro[i]; }
        g_wHave[h] = true;
    }
    g_wArr = arr; g_wBeat = GetTickCount64();
}

}
