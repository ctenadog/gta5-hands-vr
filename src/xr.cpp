// OpenXR on GTA's own D3D11 device. Alternate-eye rendering: GTA renders one eye per frame
// (the head camera puts the scripted camera at that eye), and this copies the frame into that eye's swapchain.
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <mutex>
#include <vector>
#include <cstring>
#include <cmath>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xr.h"
#include "log.h"
#include "blit.h"
#include <tlhelp32.h>
#include <string>

namespace {
HMODULE g_dll = nullptr;
PFN_xrGetInstanceProcAddr gpa = nullptr;
#define XRFN(n) PFN_##n n = nullptr;
XRFN(xrCreateInstance) XRFN(xrGetSystem) XRFN(xrCreateSession) XRFN(xrCreateReferenceSpace)
XRFN(xrEnumerateViewConfigurationViews) XRFN(xrCreateSwapchain) XRFN(xrEnumerateSwapchainImages)
XRFN(xrEnumerateSwapchainFormats) XRFN(xrPollEvent) XRFN(xrBeginSession) XRFN(xrEndSession)
XRFN(xrWaitFrame) XRFN(xrBeginFrame) XRFN(xrEndFrame) XRFN(xrLocateViews) XRFN(xrLocateSpace)
XRFN(xrAcquireSwapchainImage) XRFN(xrWaitSwapchainImage) XRFN(xrReleaseSwapchainImage)
XRFN(xrStringToPath) XRFN(xrCreateActionSet) XRFN(xrCreateAction) XRFN(xrSuggestInteractionProfileBindings)
XRFN(xrAttachSessionActionSets) XRFN(xrSyncActions) XRFN(xrGetActionStateFloat) XRFN(xrGetActionStateBoolean)
XRFN(xrCreateActionSpace) XRFN(xrDestroyInstance) XRFN(xrGetD3D11GraphicsRequirementsKHR) XRFN(xrRequestExitSession)
XRFN(xrDestroySession) XRFN(xrDestroySwapchain) XRFN(xrDestroySpace)
#undef XRFN

XrInstance inst = XR_NULL_HANDLE; XrSystemId sys = 0; XrSession sess = XR_NULL_HANDLE;
XrSpace stage = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
XrSwapchain sc[2] = {}; std::vector<XrSwapchainImageD3D11KHR> scImg[2];
XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
XrActionSet aset = XR_NULL_HANDLE; XrAction act[IN_COUNT] = {}; XrSpace actSpace[IN_COUNT] = {};
XrView lastView[2] = {{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
XrView pendView[2] = {{XR_TYPE_VIEW},{XR_TYPE_VIEW}};   // views located last frame = the poses the game camera used for THIS frame
XrPosef pendHead{{0,0,0,0},{0,0,0}}, lastHead{{0,0,0,0},{0,0,0}};
bool g_stereo = false;            // GTA5VR.ini [vr] stereo=1: alternate-eye stereo (experimental); default mono
bool g_test = false;              // F10 test pattern
float g_tanHalf = 1.f;            // symmetric tan(half vertical fov) used for the game camera and the layer
int g_latency = 1;                // GTA5VR.ini latency=N: game frames between setting the camera and the frame reaching Present
// poses the script thread set the game camera to (newest last); the layer uses the one g_latency frames back
XrPosef camHist[8]; int camCount = 0; std::mutex camMtx;
bool scHasImage[2] = {};
bool sessionRunning = false;
bool wantRunning = false;          // F8: VR on. Session is begun only while this is true.
bool exitRequested = false;
bool g_fatal = false;            // something hung: F8 must be pressed again
XrSessionState lastState = XR_SESSION_STATE_UNKNOWN;
std::mutex mtx; VrState state{};
uint64_t frameNo = 0;
int64_t scFormat = 0;
// periodic stats (written to the log every 2 s for the first 30 s, then every 10 s)
struct Stats { unsigned presents = 0, submitted = 0, layered = 0, endFail = 0, acqFail = 0, blitFail = 0, badPose = 0, noRender = 0, slow = 0; int lastErr = 0; ULONGLONG maxMs = 0; } st;
ULONGLONG stStart = 0, stLast = 0;
void statsTick() {
    ULONGLONG now = GetTickCount64();
    if (!stStart) { stStart = stLast = now; return; }
    ULONGLONG every = (now - stStart < 30000) ? 2000 : 10000;
    if (now - stLast < every) return;
    float sec = (now - stLast) / 1000.f;
    vrlog::write("xr: stats %.0fs: game %.0f fps, to headset %.0f fps (with image %u), endFrame errors %u (last %d), acquire fail %u, blit fail %u, bad pose %u, shouldRender=0 %u, slowest frame %llu ms",
        sec, st.presents / sec, st.submitted / sec, st.layered, st.endFail, st.lastErr, st.acqFail, st.blitFail, st.badPose, st.noRender, st.maxMs);
    const auto& q = lastHead.orientation; const auto& t = lastHead.position;
    vrlog::write("xr: layer pose q(%.2f %.2f %.2f %.2f) p(%.2f %.2f %.2f) half-fov %.1f deg, cam history %d", q.x, q.y, q.z, q.w, t.x, t.y, t.z, atanf(g_tanHalf) * 57.29578f, camCount);
    st = Stats{}; stLast = now;
}
bool poseOk(const XrPosef& p) { const auto& q = p.orientation; float n = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w; return n > 0.9f && n < 1.1f; }

// destroys the session and everything made from it, so the next F8 starts clean
void destroySession() {
    for (int i = 0; i < IN_COUNT; ++i) if (actSpace[i]) { xrDestroySpace(actSpace[i]); actSpace[i] = XR_NULL_HANDLE; }
    for (int e = 0; e < 2; ++e) { if (sc[e]) xrDestroySwapchain(sc[e]); sc[e] = XR_NULL_HANDLE; scImg[e].clear(); scHasImage[e] = false; }
    if (stage) xrDestroySpace(stage);
    if (viewSpace) xrDestroySpace(viewSpace);
    stage = viewSpace = XR_NULL_HANDLE;
    if (sess) xrDestroySession(sess);
    sess = XR_NULL_HANDLE;
    sessionRunning = false; exitRequested = false; lastState = XR_SESSION_STATE_UNKNOWN; frameNo = 0;
    for (auto& v : lastView) v = {XR_TYPE_VIEW};
    for (auto& v : pendView) v = {XR_TYPE_VIEW};
    pendHead = lastHead = XrPosef{{0,0,0,0},{0,0,0}};
    { std::lock_guard<std::mutex> l(camMtx); camCount = 0; }
    stStart = 0; st = Stats{};
    blit::reset();
    std::lock_guard<std::mutex> l(mtx); state.running = false;
    vrlog::write("xr: session destroyed");
}

bool ok(XrResult r, const char* what) { if (XR_FAILED(r)) { vrlog::write("xr: %s failed (%d)", what, (int)r); return false; } return true; }
XrPath path(const char* s) { XrPath p = XR_NULL_PATH; xrStringToPath(inst, s, &p); return p; }
XrPoseF3 conv(const XrPosef& p, bool valid) { return {p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w,p.position.x,p.position.y,p.position.z,valid}; }

bool createActions() {
    XrActionSetCreateInfo ai{XR_TYPE_ACTION_SET_CREATE_INFO}; strcpy(ai.actionSetName, "gta5vr"); strcpy(ai.localizedActionSetName, "GTA V VR");
    if (!ok(xrCreateActionSet(inst, &ai, &aset), "xrCreateActionSet")) return false;
    std::vector<XrActionSuggestedBinding> pico, touch;
    for (int i = 0; i < IN_COUNT; ++i) {
        XrActionCreateInfo c{XR_TYPE_ACTION_CREATE_INFO};
        strcpy(c.actionName, kInputs[i].action); strcpy(c.localizedActionName, kInputs[i].action);
        c.actionType = kInputs[i].type == XrType::Pose ? XR_ACTION_TYPE_POSE_INPUT : kInputs[i].type == XrType::Float ? XR_ACTION_TYPE_FLOAT_INPUT : XR_ACTION_TYPE_BOOLEAN_INPUT;
        if (!ok(xrCreateAction(aset, &c, &act[i]), kInputs[i].action)) return false;
        pico.push_back({act[i], path(kInputs[i].pathPico4)});
        touch.push_back({act[i], path(kInputs[i].pathTouch)});
    }
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path("/interaction_profiles/oculus/touch_controller");
    sb.suggestedBindings = touch.data(); sb.countSuggestedBindings = (uint32_t)touch.size();
    ok(xrSuggestInteractionProfileBindings(inst, &sb), "touch bindings");
    sb.interactionProfile = path("/interaction_profiles/bytedance/pico4_controller");
    sb.suggestedBindings = pico.data(); sb.countSuggestedBindings = (uint32_t)pico.size();
    xrSuggestInteractionProfileBindings(inst, &sb);   // only valid when the runtime has XR_BD_controller_interaction
    return true;
}

void beginIfReady() {
    if (!wantRunning || sessionRunning || lastState != XR_SESSION_STATE_READY) return;
    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO}; bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    sessionRunning = ok(xrBeginSession(sess, &bi), "xrBeginSession");
    if (sessionRunning) { exitRequested = false; vrlog::write("xr: session begun (VR on)"); }
}

void pollEvents() {
    if (!inst) return;
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* s = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            lastState = s->state;
            vrlog::write("xr: session state %d", (int)s->state);
            if (s->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(sess); sessionRunning = false; scHasImage[0] = scHasImage[1] = false;
                { std::lock_guard<std::mutex> l(mtx); state.running = false; }   // camera must not keep using stale poses
                vrlog::write("xr: session ended (VR off)");
            }
            if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) { destroySession(); return; }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    beginIfReady();
}
}

namespace {
std::string regStr(HKEY root, const char* key, const char* val) {
    char buf[1024]; DWORD n = sizeof(buf);
    if (RegGetValueA(root, key, val, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &n) == ERROR_SUCCESS) return buf;
    return "";
}
bool processRunning(const char* exe) {
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (h == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32 e{sizeof(e)}; bool f = false;
    for (BOOL ok = Process32First(h, &e); ok; ok = Process32Next(h, &e)) if (_stricmp(e.szExeFile, exe) == 0) { f = true; break; }
    CloseHandle(h); return f;
}
bool fileExists(const std::string& p) { return !p.empty() && GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
// SteamVR's OpenXR manifest: from the Khronos AvailableRuntimes list, else from Steam's library folders.
std::string findSteamVrJson() {
    HKEY k;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0, KEY_READ | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        char name[1024]; for (DWORD i = 0;; ++i) { DWORD n = sizeof(name); if (RegEnumValueA(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::string v = name; for (auto& c : v) c = (char)tolower(c);
            if (v.find("steamxr_win64.json") != std::string::npos && fileExists(name)) { RegCloseKey(k); return name; } }
        RegCloseKey(k);
    }
    std::string steam = regStr(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath");
    std::vector<std::string> libs; if (!steam.empty()) libs.push_back(steam);
    if (FILE* f = fopen((steam + "/steamapps/libraryfolders.vdf").c_str(), "r")) {
        char line[2048];
        while (fgets(line, sizeof(line), f)) { const char* q = strstr(line, "\"path\""); if (!q) continue;
            q = strchr(q + 6, '"'); if (!q) continue; const char* e = strchr(q + 1, '"'); if (!e) continue;
            std::string p(q + 1, e); std::string o; for (size_t i = 0; i < p.size(); ++i) { if (p[i] == '\\' && i + 1 < p.size() && p[i+1] == '\\') ++i; o += p[i]; }
            libs.push_back(o); }
        fclose(f);
    }
    for (auto& l : libs) { std::string j = l + "\\steamapps\\common\\SteamVR\\steamxr_win64.json"; if (fileExists(j)) return j; }
    return "";
}
std::string iniPath() { char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string s = m; return s.substr(0, s.find_last_of("\\/") + 1) + "GTA5VR.ini"; }
}

namespace xr {
// GTA5VR.ini [vr] runtime = steamvr (default) | system.  steamvr: use SteamVR even when Pico Connect / Oculus
// is the Windows "active OpenXR runtime"; system: whatever runtime Windows has set active.
void pickRuntime() {
    std::string ini = iniPath();
    if (!fileExists(ini)) WritePrivateProfileStringA("vr", "runtime", "steamvr", ini.c_str());
    char rt[64]; GetPrivateProfileStringA("vr", "runtime", "steamvr", rt, sizeof(rt), ini.c_str());
    std::string active = regStr(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime");
    vrlog::write("xr: Windows active OpenXR runtime: %s", active.empty() ? "(none)" : active.c_str());
    vrlog::write("xr: GTA5VR.ini runtime=%s; SteamVR running: %s", rt, processRunning("vrserver.exe") ? "yes" : "no");
    if (GetPrivateProfileIntA("vr", "stereo", -1, ini.c_str()) < 0) WritePrivateProfileStringA("vr", "stereo", "0", ini.c_str());
    g_stereo = GetPrivateProfileIntA("vr", "stereo", 0, ini.c_str()) != 0;
    if (GetPrivateProfileIntA("vr", "latency", -1, ini.c_str()) < 0) WritePrivateProfileStringA("vr", "latency", "1", ini.c_str());
    g_latency = std::max(0, std::min(6, (int)GetPrivateProfileIntA("vr", "latency", 1, ini.c_str())));
    vrlog::write("xr: latency=%d frames (GTA5VR.ini latency, 0..6)", g_latency);
    vrlog::write("xr: mode %s (GTA5VR.ini stereo=%d)", g_stereo ? "stereo (alternate eye, experimental)" : "mono (same image both eyes, stable)", g_stereo ? 1 : 0);
    if (_stricmp(rt, "system") == 0) return;
    std::string j = findSteamVrJson();
    if (j.empty()) { vrlog::write("xr: SteamVR not found - using the Windows active runtime"); return; }
    SetEnvironmentVariableA("XR_RUNTIME_JSON", j.c_str());   // read by openxr_loader at xrCreateInstance
    vrlog::write("xr: using SteamVR: %s", j.c_str());
}

bool loadLoader() {
    if (inst) return true;
    pickRuntime();
    if (!g_dll) g_dll = LoadLibraryA("openxr_loader.dll");
    if (!g_dll) { vrlog::write("xr: openxr_loader.dll not found next to GTA5.exe"); return false; }
    gpa = (PFN_xrGetInstanceProcAddr)GetProcAddress(g_dll, "xrGetInstanceProcAddr");
    if (!gpa) return false;
    gpa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
    const char* ext[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "Grand Theft Auto V"); strcpy(ci.applicationInfo.engineName, "GTA5VR"); ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1,0,0);
    ci.enabledExtensionCount = 1; ci.enabledExtensionNames = ext;
    if (!ok(xrCreateInstance(&ci, &inst), "xrCreateInstance (is SteamVR running and the headset connected?)")) { inst = XR_NULL_HANDLE; return false; }
    { XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES}; PFN_xrGetInstanceProperties gip = nullptr;
      gpa(inst, "xrGetInstanceProperties", (PFN_xrVoidFunction*)&gip); if (gip && XR_SUCCEEDED(gip(inst, &ip))) vrlog::write("xr: runtime in use: %s", ip.runtimeName); }
#define L(n) gpa(inst, #n, (PFN_xrVoidFunction*)&n);
    L(xrGetSystem) L(xrCreateSession) L(xrCreateReferenceSpace) L(xrEnumerateViewConfigurationViews) L(xrCreateSwapchain)
    L(xrEnumerateSwapchainImages) L(xrEnumerateSwapchainFormats) L(xrPollEvent) L(xrBeginSession) L(xrEndSession) L(xrWaitFrame)
    L(xrBeginFrame) L(xrEndFrame) L(xrLocateViews) L(xrLocateSpace) L(xrAcquireSwapchainImage) L(xrWaitSwapchainImage)
    L(xrReleaseSwapchainImage) L(xrStringToPath) L(xrCreateActionSet) L(xrCreateAction) L(xrSuggestInteractionProfileBindings)
    L(xrAttachSessionActionSets) L(xrSyncActions) L(xrGetActionStateFloat) L(xrGetActionStateBoolean) L(xrCreateActionSpace)
    L(xrDestroyInstance) L(xrGetD3D11GraphicsRequirementsKHR) L(xrRequestExitSession)
    L(xrDestroySession) L(xrDestroySwapchain) L(xrDestroySpace)
#undef L
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO}; gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!ok(xrGetSystem(inst, &gi, &sys), "xrGetSystem (headset connected?)")) { xrDestroyInstance(inst); inst = XR_NULL_HANDLE; return false; }
    vrlog::write("xr: OpenXR instance ready");
    return createActions();
}

bool startSession(ID3D11Device* dev, DXGI_FORMAT bbFormat) {
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    xrGetD3D11GraphicsRequirementsKHR(inst, sys, &req);   // required call before xrCreateSession
    { IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr; DXGI_ADAPTER_DESC d{};
      if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && SUCCEEDED(dd->GetAdapter(&ad)) && SUCCEEDED(ad->GetDesc(&d)))
          vrlog::write("xr: GTA GPU %ls, headset GPU %s", d.Description, memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0 ? "same" : "DIFFERENT (set GTA to the headset GPU)");
      if (ad) ad->Release();
      if (dd) dd->Release(); }
    // the runtime touches GTA's device from its own thread: without this the game can hang
    { ID3D10Multithread* mt = nullptr; if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), (void**)&mt))) { mt->SetMultithreadProtected(TRUE); mt->Release(); } }
    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; gb.device = dev;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO}; si.next = &gb; si.systemId = sys;
    if (!blit::init(dev)) return false;
    if (!ok(xrCreateSession(inst, &si, &sess), "xrCreateSession (GPU must match the headset's adapter)")) { sess = XR_NULL_HANDLE; return false; }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; xrCreateReferenceSpace(sess, &rs, &stage);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;  xrCreateReferenceSpace(sess, &rs, &viewSpace);
    // pick only a format the runtime actually offers (SteamVR rejected GTA's BGRA format 87 with -26);
    // the shader blit converts BGRA/RGBA and gamma, so any 8-bit colour format works
    int64_t fmt = 0; std::string offered;
    { uint32_t fc = 0; xrEnumerateSwapchainFormats(sess, 0, &fc, nullptr); std::vector<int64_t> f(fc);
      xrEnumerateSwapchainFormats(sess, fc, &fc, f.data());
      for (auto x : f) offered += std::to_string(x) + " ";
      const int64_t pref[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
      for (auto want : pref) { for (auto x : f) if (x == want) { fmt = x; break; } if (fmt) break; }
      if (!fmt && !f.empty()) fmt = f[0]; }
    scFormat = fmt;
    vrlog::write("xr: backbuffer format %d, runtime offers [%s], using %d", (int)bbFormat, offered.c_str(), (int)fmt);
    uint32_t n = 2; xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv);
    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sci.format = fmt; sci.sampleCount = 1;
        sci.width = vcv[e].recommendedImageRectWidth; sci.height = vcv[e].recommendedImageRectHeight;
        sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
        if (!ok(xrCreateSwapchain(sess, &sci, &sc[e]), "xrCreateSwapchain")) { destroySession(); return false; }
        uint32_t c = 0; xrEnumerateSwapchainImages(sc[e], 0, &c, nullptr);
        scImg[e].assign(c, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(sc[e], c, &c, (XrSwapchainImageBaseHeader*)scImg[e].data());
    }
    for (int i = 0; i < IN_COUNT; ++i) if (kInputs[i].type == XrType::Pose) {
        XrActionSpaceCreateInfo a{XR_TYPE_ACTION_SPACE_CREATE_INFO}; a.action = act[i]; a.poseInActionSpace.orientation.w = 1;
        xrCreateActionSpace(sess, &a, &actSpace[i]);
    }
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; at.countActionSets = 1; at.actionSets = &aset;
    ok(xrAttachSessionActionSets(sess, &at), "attach actions");
    vrlog::write("xr: session created %ux%u per eye", vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);
    return true;
}

void setWanted(bool on) {
    if (on && !wantRunning) g_fatal = false;
    wantRunning = on;
    if (!sess) return;
    if (!on && sessionRunning && !exitRequested) { exitRequested = true; ok(xrRequestExitSession(sess), "xrRequestExitSession"); }
}
bool hasSession() { return sess != XR_NULL_HANDLE; }
bool needsSession() { return inst && !sess; }
bool failed() { return g_fatal; }
void poll() { pollEvents(); }

void onPresent(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb) {
    pollEvents();
    if (sessionRunning) { ++st.presents; statsTick(); }
    if (!sessionRunning) { std::lock_guard<std::mutex> l(mtx); state.running = false; return; }
    XrFrameState fs{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (g_fatal) { std::lock_guard<std::mutex> l(mtx); state.running = false; return; }
    ULONGLONG t0 = GetTickCount64();
    XrResult wfr = xrWaitFrame(sess, &wi, &fs);
    ULONGLONG tw = GetTickCount64() - t0;
    if (tw > st.maxMs) st.maxMs = tw;
    if (frameNo < 5 || tw > 500) vrlog::write("xr: frame %llu xrWaitFrame %d took %llu ms", (unsigned long long)frameNo, (int)wfr, tw);
    if (XR_FAILED(wfr)) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO}; xrBeginFrame(sess, &bi);

    int eye;
    { std::lock_guard<std::mutex> l(mtx); eye = state.renderEye; }   // the eye this frame was rendered for
    // stereo: this frame goes into its eye; mono: the same frame goes into both eyes
    for (int target = 0; target < 2; ++target) {
    if (g_stereo && target != eye) continue;
    uint32_t idx; XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult ar = xrAcquireSwapchainImage(sc[target], &ai, &idx);
    if (XR_FAILED(ar)) { ++st.acqFail; st.lastErr = ar; }
    if (XR_SUCCEEDED(ar)) {
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; w.timeout = 100000000;   // 100 ms
        XrResult wr = XR_TIMEOUT_EXPIRED;
        for (int t = 0; t < 10 && wr == XR_TIMEOUT_EXPIRED; ++t) wr = xrWaitSwapchainImage(sc[target], &w);   // must succeed before release
        if (wr != XR_SUCCESS) { vrlog::write("xr: swapchain image never became ready (%d) - VR switched off", (int)wr); g_fatal = true; }
        if (wr == XR_SUCCESS) {
            float aspect = (float)vcv[target].recommendedImageRectWidth / (float)vcv[target].recommendedImageRectHeight;
            bool srgb = scFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || scFormat == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;   // GTA's image is already gamma-encoded
            bool drawn;
            if (g_test) {
                // F10: the simplest possible write - clear the eye image to a solid colour, no shader, no state swap.
                // Left eye magenta, right eye green. If even this does not show, SteamVR is not displaying our layer at all.
                ID3D11Device* d = nullptr; scImg[target][idx].texture->GetDevice(&d);
                D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = (DXGI_FORMAT)scFormat; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ID3D11RenderTargetView* rtv = nullptr;
                drawn = d && SUCCEEDED(d->CreateRenderTargetView(scImg[target][idx].texture, &rv, &rtv));
                if (drawn) { const float c[2][4] = {{1, 0, 1, 1}, {0, 1, 0, 1}}; ctx->ClearRenderTargetView(rtv, c[target]); rtv->Release(); }
                else if (frameNo % 120 == 0) vrlog::write("xr: F10 clear: CreateRenderTargetView failed (format %d)", (int)scFormat);
                if (d) d->Release();
            } else drawn = blit::draw(ctx, bb, scImg[target][idx].texture, (DXGI_FORMAT)scFormat, srgb, aspect, false, target == 0 || g_stereo);
            if (!drawn) { ++st.blitFail; if (frameNo < 5) vrlog::write("xr: blit failed for eye %d", target); }
            ctx->Flush();   // hand the eye image to the GPU now: SteamVR's compositor reads it from another process
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(sc[target], &ri);
            if (drawn) scHasImage[target] = true;
        }
    }
    }

    // locate head, eyes and controllers for the next frame
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO}; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = fs.predictedDisplayTime; li.space = stage;
    XrViewState vs{XR_TYPE_VIEW_STATE}; XrView views[2] = {{XR_TYPE_VIEW},{XR_TYPE_VIEW}}; uint32_t vn = 2;
    xrLocateViews(sess, &li, &vs, 2, &vn, views);
    XrActiveActionSet as{aset, XR_NULL_PATH}; XrActionsSyncInfo sy{XR_TYPE_ACTIONS_SYNC_INFO}; sy.countActiveActionSets = 1; sy.activeActionSets = &as;
    xrSyncActions(sess, &sy);
    VrState s{};
    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION}; xrLocateSpace(viewSpace, stage, fs.predictedDisplayTime, &hl);
    s.head = conv(hl.pose, (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0);
    for (int e = 0; e < 2; ++e) {
        s.eye[e] = conv(views[e].pose, true);
        s.eyeFovDeg[e] = (views[e].fov.angleUp - views[e].fov.angleDown) * 57.29578f;
    }
    // one symmetric square fov covering both eyes' (asymmetric) fov: the game renders a symmetric camera,
    // so the layer must be submitted with exactly that fov or the image is stretched / swims
    if (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) {
        float t = 0.f;
        for (int e = 0; e < 2; ++e) for (float a : {views[e].fov.angleUp, -views[e].fov.angleDown, views[e].fov.angleRight, -views[e].fov.angleLeft}) t = std::max(t, tanf(a));
        if (t > 0.3f && t < 4.f) g_tanHalf = t;
    }
    s.fovDeg = 2.f * atanf(g_tanHalf) * 57.29578f;
    s.stereo = g_stereo;
    for (int i = 0; i < IN_COUNT; ++i) {
        XrActionStateGetInfo g{XR_TYPE_ACTION_STATE_GET_INFO}; g.action = act[i];
        if (kInputs[i].type == XrType::Pose) {
            XrSpaceLocation l{XR_TYPE_SPACE_LOCATION}; xrLocateSpace(actSpace[i], stage, fs.predictedDisplayTime, &l);
            s.pose[i] = conv(l.pose, (l.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
        } else if (kInputs[i].type == XrType::Float) {
            XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT}; xrGetActionStateFloat(sess, &g, &f); s.value[i] = f.currentState;
        } else {
            XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN}; xrGetActionStateBoolean(sess, &g, &b); s.value[i] = b.currentState ? 1.f : 0.f;
        }
    }
    // The layer must carry exactly the orientation the game camera rendered this image with: the roll-free pose the
    // script thread set g_latency game frames ago. The compositor then corrects the rest (head tilt, latency) itself.
    { std::lock_guard<std::mutex> l(camMtx);
      if (camCount > 0) { int i = std::max(0, camCount - 1 - g_latency); lastHead = camHist[i]; } }
    if (g_stereo) { lastView[eye].pose = lastHead; lastView[eye].fov = views[eye].fov; }

    float ha = atanf(g_tanHalf);
    XrCompositionLayerProjectionView pv[2];
    for (int e = 0; e < 2; ++e) {
        pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        pv[e].pose = (g_stereo && poseOk(lastView[e].pose)) ? lastView[e].pose : lastHead;
        pv[e].fov = {-ha, ha, ha, -ha};
        pv[e].subImage.swapchain = sc[e];
        pv[e].subImage.imageRect = {{0,0},{(int32_t)vcv[e].recommendedImageRectWidth,(int32_t)vcv[e].recommendedImageRectHeight}};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; layer.space = stage; layer.viewCount = 2; layer.views = pv;
    const XrCompositionLayerBaseHeader* layers[] = {(XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO}; ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool both = scHasImage[0] && scHasImage[1];
    bool poses = poseOk(lastHead);   // a zero quaternion makes xrEndFrame fail
    if (!poses) ++st.badPose;
    if (!fs.shouldRender) ++st.noRender;
    ei.layerCount = (both && poses && fs.shouldRender) ? 1 : 0; ei.layers = layers;
    XrResult er = xrEndFrame(sess, &ei);
    ++st.submitted; if (ei.layerCount) ++st.layered;
    if (XR_FAILED(er)) { ++st.endFail; st.lastErr = er; if (st.endFail == 1) vrlog::write("xr: xrEndFrame failed (%d)", (int)er); }
    if (frameNo < 5) vrlog::write("xr: frame %llu submitted (eye %d, layers %u, shouldRender %d)", (unsigned long long)frameNo, eye, ei.layerCount, (int)fs.shouldRender);
    if (GetTickCount64() - t0 > 3000) { vrlog::write("xr: one frame took over 3 s - VR switched off to keep the game alive"); g_fatal = true; }

    s.renderEye = eye ^ 1; s.running = true; ++frameNo;
    std::lock_guard<std::mutex> l(mtx); state = s;
}

VrState snapshot() { std::lock_guard<std::mutex> l(mtx); return state; }
void shutdown() { if (inst) xrDestroyInstance(inst); inst = XR_NULL_HANDLE; }
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
    XrPosef p{{u.qx, u.qy, u.qz, u.qw}, {u.px, u.py, u.pz}};
    if (!poseOk(p)) return;
    std::lock_guard<std::mutex> l(camMtx);
    if (camCount == 8) { for (int i = 1; i < 8; ++i) camHist[i-1] = camHist[i]; camCount = 7; }
    camHist[camCount++] = p;
}
void toggleTestPattern() { g_test = !g_test; vrlog::write("F10: test colour %s", g_test ? "ON (headset should show magenta left eye / green right eye)" : "off"); }
}
