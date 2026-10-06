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
M3 g_cal[2]; bool g_calSet[2] = {false, false};
float g_written[2][3][3]; bool g_haveWritten = false; int g_persistLog = 0;
// Background writer: the game recomputes the skeleton after scripts run (0.2.9 log: "overwritten by the game"), so a
// write from the script thread is lost before rendering. A separate thread re-writes the target rotation continuously,
// hitting the window between the animation update and the copy to the renderer. Translation is never touched.
// Safety: it only writes while the script thread refreshed the target within the last 50 ms (stops in menus, on death,
// on VR off, on any failed check), and only into the array the script thread validated this frame.
std::atomic<uintptr_t> g_wArr{0}; std::atomic<ULONGLONG> g_wBeat{0}; std::mutex g_wMtx;
float g_wTarget[2][3][3]; bool g_wHave[2] = {false, false}; std::atomic<unsigned> g_wCount{0};
HANDLE g_wThread = nullptr;
DWORD WINAPI writer(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // 0.4.3: was ABOVE_NORMAL + hot spin, starved the frame copy
    for (;;) {
        uintptr_t arr = g_wArr.load();
        if (!arr || GetTickCount64() - g_wBeat.load() > 50) { Sleep(5); continue; }
        float t[2][3][3]; bool have[2];
        { std::lock_guard<std::mutex> l(g_wMtx); memcpy(t, g_wTarget, sizeof(t)); have[0] = g_wHave[0]; have[1] = g_wHave[1]; }
        for (int h = 0; h < 2; ++h) if (have[h]) {
            float* f = (float*)(arr + (uintptr_t)g_handIdx[h] * 64);
            for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) f[c * 4 + r] = t[h][c][r];
        }
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
        char path[96]; snprintf(path, sizeof(path), "ped+0x%X", g_offs[0]);
        for (int d = 1; d < g_depth; ++d) { size_t l = strlen(path); snprintf(path + l, sizeof(path) - l, " ->+0x%X", g_offs[d]); }
        vrlog::write("hands: FOUND bone matrices (%s space) at %s, %d candidates, %llu ms. Press F12 again to rotate hands.",
            g_worldSpace ? "world" : "object", path, tested, GetTickCount64() - t0);
    }
    else vrlog::write("hands: bone matrices NOT found (%d candidates, %llu ms) - send this log", tested, GetTickCount64() - t0);
}
}

namespace hands {
void toggle() {
    if (!g_found) { g_searched = false; g_wantSearch = true; vrlog::write("F12: searching the skeleton (VR must be on, stand still)..."); return; }
    g_on = !g_on; g_calSet[0] = g_calSet[1] = false; g_haveWritten = false; g_persistLog = 0;
    g_wArr = 0; { std::lock_guard<std::mutex> l(g_wMtx); g_wHave[0] = g_wHave[1] = false; }
    if (g_on && !g_wThread) g_wThread = CreateThread(nullptr, 0, writer, nullptr, 0, nullptr);
    vrlog::write("F12: hand rotation %s (hold the controllers the way the hands are held now)", g_on ? "ON" : "off");
}

void tick(int ped, const XrPoseF3* gripLeftRight[2], float yawDeg) {
    uintptr_t pedAddr = (uintptr_t)shv::entityAddress(ped);
    if (!g_searched) {
        if (!g_wantSearch) return;
        g_wantSearch = false;
        if (!pedAddr) { vrlog::write("hands: Script Hook V has no getScriptHandleBaseAddress - not possible"); g_searched = true; return; }
        search(ped, pedAddr);
        return;
    }
    if (!g_on || !g_found || !pedAddr) { g_wArr = 0; return; }
    uintptr_t arr = resolve(pedAddr);
    M3 E; float epos[3];
    if (!arr || !entityMatrix(pedAddr, E, epos)) { g_wArr = 0; vrlog::write("hands: path no longer valid - rotation off"); g_on = false; return; }
    static ULONGLONG lastRate = 0;
    if (GetTickCount64() - lastRate > 5000) { lastRate = GetTickCount64(); vrlog::write("hands: background writes in the last 5 s: %u", g_wCount.exchange(0)); }
    for (int h = 0; h < 2; ++h) {
        uintptr_t m = arr + (uintptr_t)g_handIdx[h] * 64;
        if (!readable(m, 64) || !writable(m, 48)) { g_wArr = 0; vrlog::write("hands: matrix not writable - rotation off"); g_on = false; return; }
        // sanity: the bone translation must still be where the game puts the hand
        Vector3 w = natives::invokeV3(N_GET_WORLD_POSITION_OF_ENTITY_BONE, ped, g_handIdx[h]);
        float* f = (float*)m;
        float want[3] = {w.x, w.y, w.z};
        if (!g_worldSpace) { float d[3] = {w.x - epos[0], w.y - epos[1], w.z - epos[2]}; for (int i = 0; i < 3; ++i) want[i] = E.m[0][i]*d[0] + E.m[1][i]*d[1] + E.m[2][i]*d[2]; }
        for (int i = 0; i < 3; ++i) if (!(fabsf(f[12 + i] - want[i]) < 0.1f)) { g_wArr = 0; vrlog::write("hands: bone check failed (%.2f vs %.2f) - rotation off", f[12 + i], want[i]); g_on = false; return; }
        // did last frame's write survive until now? (tells whether the game overwrites it before rendering)
        if (g_haveWritten && g_persistLog < 4) {
            float diff = 0; for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) diff += fabsf(f[c * 4 + r] - g_written[h][c][r]);
            vrlog::write("hands: %s hand matrix since last write: %s (diff %.3f)", h ? "right" : "left", diff < 0.01f ? "kept" : "overwritten by the game", diff);
            if (h == 1) ++g_persistLog;
        }
        const XrPoseF3* p = gripLeftRight[h];
        if (!p || !p->valid) continue;
        M3 bone{};   // current bone basis (columns) in world space
        for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) bone.m[r][c] = f[c * 4 + r];
        if (!g_worldSpace) bone = mul(E, bone);
        M3 ctrl = mul(xrToGtaRot(yawDeg), fromQuat(p->qx, p->qy, p->qz, p->qw));
        if (!g_calSet[h]) { g_cal[h] = mul(tr(ctrl), bone); g_calSet[h] = true; vrlog::write("hands: %s hand calibrated", h ? "right" : "left"); }
        M3 target = mul(ctrl, g_cal[h]);
        if (!g_worldSpace) target = mul(tr(E), target);
        for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) { f[c * 4 + r] = target.m[r][c]; g_written[h][c][r] = target.m[r][c]; }
        { std::lock_guard<std::mutex> l(g_wMtx); memcpy(g_wTarget[h], g_written[h], sizeof(g_written[h])); g_wHave[h] = true; }
    }
    g_haveWritten = true;
    g_wArr = arr; g_wBeat = GetTickCount64();
}

}
