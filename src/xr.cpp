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
#undef XRFN

XrInstance inst = XR_NULL_HANDLE; XrSystemId sys = 0; XrSession sess = XR_NULL_HANDLE;
XrSpace stage = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
XrSwapchain sc[2] = {}; std::vector<XrSwapchainImageD3D11KHR> scImg[2];
XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW},{XR_TYPE_VIEW_CONFIGURATION_VIEW}};
XrActionSet aset = XR_NULL_HANDLE; XrAction act[IN_COUNT] = {}; XrSpace actSpace[IN_COUNT] = {};
XrView lastView[2] = {{XR_TYPE_VIEW},{XR_TYPE_VIEW}};
bool scHasImage[2] = {};
bool sessionRunning = false;
bool wantRunning = false;          // F8: VR on. Session is begun only while this is true.
bool exitRequested = false;
bool g_fatal = false;            // something hung: F8 must be pressed again
XrSessionState lastState = XR_SESSION_STATE_UNKNOWN;
std::mutex mtx; VrState state{};
uint64_t frameNo = 0;

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
                vrlog::write("xr: session ended (VR off)");
            }
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
    if (!ok(xrCreateSession(inst, &si, &sess), "xrCreateSession (GPU must match the headset's adapter)")) return false;
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; xrCreateReferenceSpace(sess, &rs, &stage);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;  xrCreateReferenceSpace(sess, &rs, &viewSpace);
    // CopySubresourceRegion needs the same format family as GTA's backbuffer
    int64_t fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    { uint32_t fc = 0; xrEnumerateSwapchainFormats(sess, 0, &fc, nullptr); std::vector<int64_t> f(fc);
      xrEnumerateSwapchainFormats(sess, fc, &fc, f.data());
      for (auto x : f) if (x == bbFormat) { fmt = x; break; } }
    vrlog::write("xr: backbuffer format %d, swapchain format %d", (int)bbFormat, (int)fmt);
    uint32_t n = 2; xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv);
    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        sci.format = fmt; sci.sampleCount = 1;
        sci.width = vcv[e].recommendedImageRectWidth; sci.height = vcv[e].recommendedImageRectHeight;
        sci.faceCount = 1; sci.arraySize = 1; sci.mipCount = 1;
        if (!ok(xrCreateSwapchain(sess, &sci, &sc[e]), "xrCreateSwapchain")) return false;
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
bool failed() { return g_fatal; }
void poll() { pollEvents(); }

void onPresent(ID3D11DeviceContext* ctx, ID3D11Texture2D* bb) {
    pollEvents();
    if (!sessionRunning) { std::lock_guard<std::mutex> l(mtx); state.running = false; return; }
    XrFrameState fs{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (g_fatal) { std::lock_guard<std::mutex> l(mtx); state.running = false; return; }
    ULONGLONG t0 = GetTickCount64();
    XrResult wfr = xrWaitFrame(sess, &wi, &fs);
    ULONGLONG tw = GetTickCount64() - t0;
    if (frameNo < 5 || tw > 500) vrlog::write("xr: frame %llu xrWaitFrame %d took %llu ms", (unsigned long long)frameNo, (int)wfr, tw);
    if (XR_FAILED(wfr)) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO}; xrBeginFrame(sess, &bi);

    int eye;
    { std::lock_guard<std::mutex> l(mtx); eye = state.renderEye; }   // the eye this frame was rendered for
    // copy this frame into its eye
    uint32_t idx; XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_SUCCEEDED(xrAcquireSwapchainImage(sc[eye], &ai, &idx))) {
        XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; w.timeout = 100000000;   // 100 ms
        XrResult wr = XR_TIMEOUT_EXPIRED;
        for (int t = 0; t < 10 && wr == XR_TIMEOUT_EXPIRED; ++t) wr = xrWaitSwapchainImage(sc[eye], &w);   // must succeed before release
        if (wr != XR_SUCCESS) { vrlog::write("xr: swapchain image never became ready (%d) - VR switched off", (int)wr); g_fatal = true; }
        D3D11_TEXTURE2D_DESC sd; bb->GetDesc(&sd);
        D3D11_BOX box{0,0,0, (UINT)std::min<uint32_t>(sd.Width, vcv[eye].recommendedImageRectWidth), (UINT)std::min<uint32_t>(sd.Height, vcv[eye].recommendedImageRectHeight), 1};
        if (wr == XR_SUCCESS) {
            ctx->CopySubresourceRegion(scImg[eye][idx].texture, 0, 0, 0, 0, bb, 0, &box);   // TODO verify: backbuffer format/size vs swapchain
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(sc[eye], &ri); scHasImage[eye] = true;
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
    // the eye that was just rendered keeps the view it was rendered with
    lastView[eye] = views[eye];

    XrCompositionLayerProjectionView pv[2];
    for (int e = 0; e < 2; ++e) {
        pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        pv[e].pose = lastView[e].pose; pv[e].fov = lastView[e].fov;
        pv[e].subImage.swapchain = sc[e];
        pv[e].subImage.imageRect = {{0,0},{(int32_t)vcv[e].recommendedImageRectWidth,(int32_t)vcv[e].recommendedImageRectHeight}};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; layer.space = stage; layer.viewCount = 2; layer.views = pv;
    const XrCompositionLayerBaseHeader* layers[] = {(XrCompositionLayerBaseHeader*)&layer};
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO}; ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool both = scHasImage[0] && scHasImage[1];
    ei.layerCount = (both && fs.shouldRender) ? 1 : 0; ei.layers = layers;
    XrResult er = xrEndFrame(sess, &ei);
    if (XR_FAILED(er) && (frameNo < 5 || frameNo % 300 == 0)) vrlog::write("xr: xrEndFrame failed (%d)", (int)er);
    if (frameNo < 5) vrlog::write("xr: frame %llu submitted (eye %d, layers %u, shouldRender %d)", (unsigned long long)frameNo, eye, ei.layerCount, (int)fs.shouldRender);
    if (GetTickCount64() - t0 > 3000) { vrlog::write("xr: one frame took over 3 s - VR switched off to keep the game alive"); g_fatal = true; }

    s.renderEye = eye ^ 1; s.running = true; ++frameNo;
    std::lock_guard<std::mutex> l(mtx); state = s;
}

VrState snapshot() { std::lock_guard<std::mutex> l(mtx); return state; }
void shutdown() { if (inst) xrDestroyInstance(inst); inst = XR_NULL_HANDLE; }
}
