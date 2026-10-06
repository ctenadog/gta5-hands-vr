// Game side of the VR bridge (0.4.0). OpenXR runs in GTA5VR_Host.exe, never inside GTA5.exe:
// 0.2.x-0.3.x submitted perfect frames from inside the game and SteamVR still showed "Waiting...", while
// xrtest.exe (same calls, separate process) displayed fine. This file starts the helper on F8, hands every game frame
// over and reads head/controller poses back.
// Frame transport, tried in this order, each confirmed by the helper:
//   1. shared texture with an NT handle (GTA's device draws, the helper copies), 2. legacy shared handle,
//   3. CPU copy through shared memory (slower, but works on any driver).
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#include <cmath>
#include "xr.h"
#include "shared.h"
#include "log.h"
#include "blit.h"

namespace {
template<class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
HANDLE g_map = nullptr; bridge::Shm* g_shm = nullptr;
HANDLE g_host = nullptr;                 // helper process
bool g_want = false, g_fatal = false, g_test = false;
bool g_stereo = false; int g_latency = 1; bool g_parentShell = true; char g_runtime[16] = "steamvr";
int g_renderEye = 0;
// poses the script thread set the game camera to (newest last); a frame carries the one g_latency frames back
XrPoseF3 camHist[8]; int camCount = 0; std::mutex camMtx;
// transport
int g_method = 0;                        // method currently published or in use
bool g_waitAck = false; ULONGLONG g_pubTime = 0; bool g_transportOk = false; int g_tryIdx = 0;
const int kOrder[] = {bridge::M_NTNAME, bridge::M_LEGACY, bridge::M_CPU}; const int kOrderN = 3;
ID3D11Texture2D* g_tex = nullptr; IDXGIKeyedMutex* g_km = nullptr; HANDLE g_nt = nullptr;
ID3D11Texture2D* g_stage[2] = {}; int g_stageIdx = 0; bool g_stageFull[2] = {}; float g_stagePose[2][7]; int g_stageEye[2];
HANDLE g_fmap = nullptr; uint8_t* g_frm = nullptr;
ID3D11Texture2D* g_cpuRt = nullptr;     // CPU path: blit target before the staging copy
uint32_t g_w = 0, g_h = 0;
LONG g_lastBeat = 0; ULONGLONG g_beatTime = 0;
ULONGLONG g_lastStat = 0; unsigned g_frames = 0, g_handed = 0, g_busy = 0;

std::string gameDir() { char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string s = m; return s.substr(0, s.find_last_of("\\/") + 1); }

void readIni() {
    std::string ini = gameDir() + "GTA5VR.ini";
    auto defInt = [&](const char* k, const char* v) { if (GetPrivateProfileIntA("vr", k, -12345, ini.c_str()) == -12345) WritePrivateProfileStringA("vr", k, v, ini.c_str()); };
    char rt[16]; GetPrivateProfileStringA("vr", "runtime", "", rt, sizeof rt, ini.c_str());
    if (!rt[0]) { WritePrivateProfileStringA("vr", "runtime", "steamvr", ini.c_str()); strcpy(rt, "steamvr"); }
    strcpy(g_runtime, _stricmp(rt, "system") == 0 ? "system" : "steamvr");
    defInt("stereo", "0"); defInt("latency", "1"); defInt("host_under_explorer", "1");
    g_parentShell = GetPrivateProfileIntA("vr", "host_under_explorer", 1, ini.c_str()) != 0;
    g_stereo = GetPrivateProfileIntA("vr", "stereo", 0, ini.c_str()) != 0;
    g_latency = std::max(0, std::min(6, (int)GetPrivateProfileIntA("vr", "latency", 1, ini.c_str())));
    vrlog::write("xr: GTA5VR.ini runtime=%s stereo=%d latency=%d host_under_explorer=%d", g_runtime, g_stereo ? 1 : 0, g_latency, g_parentShell ? 1 : 0);
}

bool ensureShm() {
    if (g_shm) return true;
    wchar_t name[64]; bridge::shmName(GetCurrentProcessId(), name, 64);
    g_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(bridge::Shm), name);
    if (!g_map) { vrlog::write("xr: shared memory failed (%lu)", GetLastError()); return false; }
    g_shm = (bridge::Shm*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::Shm));
    if (!g_shm) return false;
    memset(g_shm, 0, sizeof(bridge::Shm));
    g_shm->magic = bridge::kMagic; g_shm->version = bridge::kVersion; g_shm->gamePid = GetCurrentProcessId();
    g_shm->hostState = bridge::H_EXITED;
    return true;
}

void releaseTransport() {
    if (g_km) { rel(g_km); }
    rel(g_tex); rel(g_cpuRt); rel(g_stage[0]); rel(g_stage[1]); g_stageFull[0] = g_stageFull[1] = false;
    if (g_nt) { CloseHandle(g_nt); g_nt = nullptr; }
    if (g_frm) { UnmapViewOfFile(g_frm); g_frm = nullptr; }
    if (g_fmap) { CloseHandle(g_fmap); g_fmap = nullptr; }
    g_method = 0; g_waitAck = false; g_transportOk = false;
}

// environment without Steam's game ids: SteamVR must see the helper as a plain OpenXR app
std::vector<wchar_t> cleanEnv() {
    std::vector<wchar_t> out; wchar_t* env = GetEnvironmentStringsW();
    for (wchar_t* p = env; *p; p += wcslen(p) + 1) {
        if (!_wcsnicmp(p, L"SteamAppId=", 11) || !_wcsnicmp(p, L"SteamGameId=", 12) || !_wcsnicmp(p, L"SteamOverlayGameId=", 19) ||
            !_wcsnicmp(p, L"SteamEnv=", 9) || !_wcsnicmp(p, L"XR_RUNTIME_JSON=", 16) || !_wcsnicmp(p, L"ENABLE_VK_LAYER_VALVE", 21)) continue;
        out.insert(out.end(), p, p + wcslen(p) + 1);
    }
    FreeEnvironmentStringsW(env); out.push_back(0); if (out.size() == 1) out.push_back(0);
    return out;
}

bool launchHost() {
    std::string dir = gameDir(), exe = dir + "GTA5VR_Host.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { vrlog::write("xr: GTA5VR_Host.exe not found next to GTA5.exe - run setup.bat again"); return false; }
    std::wstring wexe(exe.begin(), exe.end()), wdir(dir.begin(), dir.end());
    std::wstring cmd = L"\"" + wexe + L"\" " + std::to_wstring(GetCurrentProcessId());
    auto env = cleanEnv();
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si); PROCESS_INFORMATION pi{};
    // parent = explorer.exe (not GTA5.exe, which Steam launched as app 271590), so SteamVR does not tie it to the game
    HANDLE shell = nullptr; DWORD shellPid = 0; HWND sw = GetShellWindow();
    if (sw && g_parentShell) GetWindowThreadProcessId(sw, &shellPid);
    if (shellPid) shell = OpenProcess(PROCESS_CREATE_PROCESS, FALSE, shellPid);
    SIZE_T sz = 0; std::vector<char> attr;
    bool useParent = false;
    if (shell) {
        InitializeProcThreadAttributeList(nullptr, 1, 0, &sz); attr.resize(sz);
        si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attr.data();
        useParent = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &sz) &&
                    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &shell, sizeof(shell), nullptr, nullptr);
    }
    BOOL ok = CreateProcessW(wexe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | (useParent ? EXTENDED_STARTUPINFO_PRESENT : 0),
                             env.data(), wdir.c_str(), useParent ? &si.StartupInfo : (STARTUPINFOW*)&si, &pi);
    if (!ok && useParent) {
        vrlog::write("xr: start under explorer.exe failed (%lu), starting normally", GetLastError());
        STARTUPINFOW s2{}; s2.cb = sizeof(s2);
        ok = CreateProcessW(wexe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, env.data(), wdir.c_str(), &s2, &pi);
        useParent = false;
    }
    if (useParent) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (shell) CloseHandle(shell);
    if (!ok) { vrlog::write("xr: could not start GTA5VR_Host.exe (%lu)", GetLastError()); return false; }
    CloseHandle(pi.hThread);
    g_host = pi.hProcess;
    vrlog::write("xr: GTA5VR_Host.exe started (pid %lu%s) - it runs OpenXR; its log is GTA5VR_Host.log", pi.dwProcessId, useParent ? ", parent explorer.exe" : "");
    return true;
}

bool hostAlive() { return g_host && WaitForSingleObject(g_host, 0) == WAIT_TIMEOUT; }

void stopHost(bool wait) {
    if (!g_host) return;
    if (wait && WaitForSingleObject(g_host, 3000) == WAIT_TIMEOUT) { vrlog::write("xr: helper did not exit - terminating it"); TerminateProcess(g_host, 1); }
    CloseHandle(g_host); g_host = nullptr;
}

// publish one transport method; the helper answers via ackGen/ackOk
bool publish(ID3D11Device* dev, int m) {
    releaseTransport();
    uint32_t ew = g_shm->eyeW, eh = g_shm->eyeH;
    if (!ew || !eh) return false;
    g_w = ew; g_h = eh;
    if (m == bridge::M_CPU && g_h > 1440) { g_w = (uint32_t)(g_w * 1440.0 / g_h) & ~1u; g_h = 1440; }   // keep the CPU copy affordable
    D3D11_TEXTURE2D_DESC t{}; t.Width = g_w; t.Height = g_h; t.MipLevels = 1; t.ArraySize = 1; t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    t.SampleDesc = {1, 0}; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = E_FAIL; const char* step = "";
    wchar_t name[96]; swprintf(name, 96, L"Local\\GTA5VR_frame_%lu_%ld", (unsigned long)GetCurrentProcessId(), g_shm->texGen + 1);
    uint64_t handle = 0;
    if (m == bridge::M_NTNAME) {
        t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = dev->CreateTexture2D(&t, nullptr, &g_tex); step = "CreateTexture2D";
        IDXGIResource1* r1 = nullptr;
        if (SUCCEEDED(hr)) { hr = g_tex->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1); step = "IDXGIResource1"; }
        if (SUCCEEDED(hr)) { hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, name, &g_nt); step = "CreateSharedHandle"; }
        rel(r1);
        HANDLE dup = nullptr;
        if (SUCCEEDED(hr) && !DuplicateHandle(GetCurrentProcess(), g_nt, g_host, &dup, 0, FALSE, DUPLICATE_SAME_ACCESS)) vrlog::write("xr: DuplicateHandle failed (%lu) - helper will open by name", GetLastError());
        handle = (uint64_t)(uintptr_t)dup;
    } else if (m == bridge::M_LEGACY) {
        t.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = dev->CreateTexture2D(&t, nullptr, &g_tex); step = "CreateTexture2D";
        IDXGIResource* r = nullptr; HANDLE h = nullptr;
        if (SUCCEEDED(hr)) { hr = g_tex->QueryInterface(__uuidof(IDXGIResource), (void**)&r); step = "IDXGIResource"; }
        if (SUCCEEDED(hr)) { hr = r->GetSharedHandle(&h); step = "GetSharedHandle"; }
        rel(r); handle = (uint64_t)(uintptr_t)h;
    } else if (m == bridge::M_CPU) {
        hr = dev->CreateTexture2D(&t, nullptr, &g_cpuRt); step = "render texture";
        D3D11_TEXTURE2D_DESC s = t; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
        for (int i = 0; i < 2 && SUCCEEDED(hr); ++i) { hr = dev->CreateTexture2D(&s, nullptr, &g_stage[i]); step = "staging texture"; }
        if (SUCCEEDED(hr)) {
            DWORD size = bridge::kFrameHdr + g_w * g_h * 4;
            g_fmap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, name);
            g_frm = g_fmap ? (uint8_t*)MapViewOfFile(g_fmap, FILE_MAP_ALL_ACCESS, 0, 0, 0) : nullptr;
            if (!g_frm) { hr = HRESULT_FROM_WIN32(GetLastError()); step = "file mapping"; }
            else { auto* fh = (bridge::FrameHdr*)g_frm; fh->seq = 0; fh->w = g_w; fh->h = g_h; fh->pitch = g_w * 4; }
        }
    }
    if (SUCCEEDED(hr) && g_tex) { hr = g_tex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&g_km); step = "keyed mutex"; }
    if (FAILED(hr)) { vrlog::write("xr: transport %d: %s failed (0x%08x)", m, step, (unsigned)hr); releaseTransport(); return false; }
    g_shm->texMethod = m; g_shm->texHandle = handle; g_shm->texW = g_w; g_shm->texH = g_h; g_shm->texFmt = t.Format;
    wcscpy(g_shm->texName, name);
    MemoryBarrier();
    InterlockedIncrement(&g_shm->texGen);
    g_method = m; g_waitAck = true; g_pubTime = GetTickCount64();
    vrlog::write("xr: offered frame transport %d (%ux%u) to the helper", m, g_w, g_h);
    return true;
}

void currentPose(float* p) {
    std::lock_guard<std::mutex> l(camMtx);
    if (camCount == 0) { memset(p, 0, 7 * sizeof(float)); return; }
    const XrPoseF3& c = camHist[std::max(0, camCount - 1 - g_latency)];
    p[0] = c.qx; p[1] = c.qy; p[2] = c.qz; p[3] = c.qw; p[4] = c.px; p[5] = c.py; p[6] = c.pz;
}

void fail(const char* why) {
    if (g_fatal) return;
    vrlog::write("VR switched off: %s", why);
    g_fatal = true;
}
}

namespace xr {
void setWanted(bool on) {
    if (on == g_want) return;
    g_want = on;
    if (!ensureShm()) { if (on) fail("shared memory"); return; }
    if (on) {
        g_fatal = false;
        stopHost(true);
        readIni();
        releaseTransport(); g_tryIdx = 0;
        strcpy(g_shm->runtime, g_runtime); g_shm->stereo = g_stereo ? 1 : 0; g_shm->test = g_test ? 1 : 0;
        g_shm->ackGen = 0; g_shm->ackOk = 0; g_shm->eyeW = g_shm->eyeH = 0; g_shm->hostMsg[0] = 0;
        g_shm->hostState = bridge::H_STARTING; g_shm->vr.running = false;
        InterlockedIncrement(&g_shm->runGen);
        MemoryBarrier();
        g_shm->want = 1;
        g_lastBeat = g_shm->heartbeat; g_beatTime = GetTickCount64(); g_lastStat = 0;
        if (!launchHost()) fail("GTA5VR_Host.exe could not start");
    } else {
        g_shm->want = 0;
        vrlog::write("xr: VR off - asking the helper to end the session");
        // the helper ends its session itself; the handle is closed on the next start or at exit
        releaseTransport();
    }
}

bool failed() { return g_fatal && g_want; }   // 0.4.2: a failure of the previous attempt must not cancel the next F8

void onPresent(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* bb) {
    if (!g_want || g_fatal || !g_shm) return;
    ULONGLONG now = GetTickCount64();
    LONG hs = g_shm->hostState;
    if (hs == bridge::H_FATAL) { char b[300]; snprintf(b, sizeof b, "helper error: %s (see GTA5VR_Host.log)", g_shm->hostMsg); fail(b); return; }
    if (!hostAlive()) { fail("GTA5VR_Host.exe exited (see GTA5VR_Host.log)"); return; }
    LONG beat = g_shm->heartbeat;
    if (beat != g_lastBeat) { g_lastBeat = beat; g_beatTime = now; }
    else if (hs >= bridge::H_READY && now - g_beatTime > 8000) { fail("GTA5VR_Host.exe stopped responding"); return; }
    if (hs < bridge::H_READY) {   // helper still connecting to SteamVR
        static ULONGLONG lastWait = 0;
        if (now - lastWait > 5000) { lastWait = now; vrlog::write("xr: waiting for the helper to connect to SteamVR (keep the headset on, do not press F8)"); }
        return;
    }

    if (!blit::init(dev)) { fail("blit init failed"); return; }
    // transport negotiation
    if (!g_transportOk && !g_waitAck) {
        while (g_tryIdx < kOrderN && !publish(dev, kOrder[g_tryIdx])) ++g_tryIdx;
        if (g_tryIdx >= kOrderN) { fail("no way to hand frames to the helper (see GTA5VR_Host.log)"); return; }
        return;   // 0.4.2: wait for the answer from the next frame on (0.4.1 compared with a time taken BEFORE publishing -> unsigned underflow -> instant "did not answer")
    }
    if (g_waitAck) {
        if (g_shm->ackGen == g_shm->texGen) {
            g_waitAck = false;
            if (g_shm->ackOk) { g_transportOk = true; vrlog::write("xr: helper accepted transport %d - frames are flowing", g_method); }
            else { vrlog::write("xr: helper rejected transport %d: %s", g_method, g_shm->hostMsg); releaseTransport(); ++g_tryIdx; }
        } else if (now > g_pubTime && now - g_pubTime > 20000) {
            vrlog::write("xr: helper did not answer for transport %d in 20 s (helper state %ld, heartbeat %ld, msg '%s')", g_method, (long)g_shm->hostState, (long)g_shm->heartbeat, g_shm->hostMsg);
            releaseTransport(); ++g_tryIdx;
        }
        return;
    }
    if (!g_transportOk) return;

    float pose[7]; currentPose(pose);
    int eye = g_stereo ? g_renderEye : -1;
    float aspect = (float)g_w / (float)g_h;
    ++g_frames;
    if (g_method == bridge::M_NTNAME || g_method == bridge::M_LEGACY) {
        if (g_km->AcquireSync(0, 4) != S_OK) { ++g_busy; }
        else {
            bool d = blit::draw(ctx, bb, g_tex, DXGI_FORMAT_R8G8B8A8_UNORM, false, aspect, false, true);
            memcpy(g_shm->framePose, pose, sizeof pose); g_shm->frameEye = eye;
            if (d) InterlockedIncrement(&g_shm->frameNo);
            g_km->ReleaseSync(0);
            ctx->Flush();
            if (d) { ++g_handed; g_renderEye ^= 1; }
        }
    } else if (g_method == bridge::M_CPU) {
        // read back the frame copied last time (one frame of latency, no GPU stall), then queue this one
        int prev = g_stageIdx ^ 1;
        if (g_stageFull[prev]) {
            D3D11_MAPPED_SUBRESOURCE ms;
            if (SUCCEEDED(ctx->Map(g_stage[prev], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ms))) {
                auto* fh = (bridge::FrameHdr*)g_frm;
                InterlockedIncrement(&fh->seq); MemoryBarrier();
                uint8_t* dst = g_frm + bridge::kFrameHdr;
                for (uint32_t y = 0; y < g_h; ++y) memcpy(dst + (size_t)y * g_w * 4, (uint8_t*)ms.pData + (size_t)y * ms.RowPitch, (size_t)g_w * 4);
                memcpy(fh->pose, g_stagePose[prev], sizeof fh->pose); fh->eye = g_stageEye[prev];
                MemoryBarrier(); InterlockedIncrement(&fh->seq);
                ctx->Unmap(g_stage[prev], 0);
                g_stageFull[prev] = false; ++g_handed;
            } else ++g_busy;
        }
        if (!g_stageFull[g_stageIdx] && blit::draw(ctx, bb, g_cpuRt, DXGI_FORMAT_R8G8B8A8_UNORM, false, aspect, false, true)) {
            ctx->CopyResource(g_stage[g_stageIdx], g_cpuRt);
            memcpy(g_stagePose[g_stageIdx], pose, sizeof pose); g_stageEye[g_stageIdx] = eye;
            g_stageFull[g_stageIdx] = true; g_stageIdx ^= 1; g_renderEye ^= 1;
        }
    }
    if (!g_lastStat) g_lastStat = now;
    if (now - g_lastStat > 5000) {
        float sec = (now - g_lastStat) / 1000.f;
        vrlog::write("xr: stats: game %.0f fps, handed to helper %.0f fps, busy %u, transport %d; helper: headset %ld fps, new frames %ld/s",
                     g_frames / sec, g_handed / sec, g_busy, g_method, g_shm->hostFps, g_shm->hostNewFrames);
        g_frames = g_handed = g_busy = 0; g_lastStat = now;
    }
}

VrState snapshot() {
    VrState s{};
    if (!g_shm) return s;
    for (int i = 0; i < 50; ++i) {
        LONG a = g_shm->vrSeq; MemoryBarrier();
        if (a & 1) { YieldProcessor(); continue; }
        s = g_shm->vr; MemoryBarrier();
        if (g_shm->vrSeq == a) break;
    }
    s.running = s.running && g_want && !g_fatal && g_transportOk && g_shm->hostState == bridge::H_RUNNING;
    s.stereo = g_stereo; s.renderEye = g_renderEye;
    if (s.fovDeg < 30.f) s.fovDeg = 90.f;
    return s;
}

void shutdown() {
    // called from DllMain: never wait here; the helper notices the game is gone and exits by itself
    if (g_shm) g_shm->want = 0;
    stopHost(false);
}

XrPoseF3 removeRoll(const XrPoseF3& p) {
    // forward = q * (0,0,-1); rebuild q = yaw(Y) * pitch(X) with the same forward and no roll
    float x = p.qx, y = p.qy, z = p.qz, w = p.qw;
    float fx = -(2*(x*z + w*y)), fy = -(2*(y*z - w*x)), fz = -(1 - 2*(x*x + y*y));
    float yaw = atan2f(-fx, -fz), pitch = asinf(std::max(-1.f, std::min(1.f, fy)));
    float cy = cosf(yaw/2), sy = sinf(yaw/2), cp = cosf(pitch/2), sp = sinf(pitch/2);
    XrPoseF3 r = p;   // (0,sy,0,cy) * (sp,0,0,cp)
    r.qx = cy*sp; r.qy = sy*cp; r.qz = -sy*sp; r.qw = cy*cp;
    return r;
}
void reportCameraPose(const XrPoseF3& u) {
    XrPoseF3 p = u;
    float n = u.qx*u.qx + u.qy*u.qy + u.qz*u.qz + u.qw*u.qw;
    if (n < 0.9f || n > 1.1f) return;
    std::lock_guard<std::mutex> l(camMtx);
    if (camCount == 8) { for (int i = 1; i < 8; ++i) camHist[i-1] = camHist[i]; camCount = 7; }
    camHist[camCount++] = p;
}
void toggleTestPattern() {
    g_test = !g_test;
    if (g_shm) g_shm->test = g_test ? 1 : 0;
    vrlog::write("F10: test colour %s", g_test ? "ON (headset should show magenta left eye / green right eye)" : "off");
}
}
