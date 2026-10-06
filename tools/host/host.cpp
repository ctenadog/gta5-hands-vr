// GTA5VR_Host.exe - the OpenXR side of GTA V Hands VR (since 0.4.0).
// SteamVR displayed frames from a plain process (xrtest.exe) but never from inside GTA5.exe, so the OpenXR session
// lives here. GTA5VR.asi starts this program on F8 (as a child of explorer.exe, without Steam's game ids), hands every
// game frame over (shared GPU texture, or shared memory as a fallback) and reads head/controller poses back.
// Usage: GTA5VR_Host.exe <GTA5.exe pid>. Log: GTA5VR_Host.log next to it.
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "shared.h"
#include "log.h"

#define L vrlog::write
namespace {
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
XRFN(xrDestroySession) XRFN(xrGetInstanceProperties)
#undef XRFN

bridge::Shm* shm = nullptr;
HANDLE gameProc = nullptr;
XrInstance inst = XR_NULL_HANDLE; XrSystemId sys = 0; XrSession sess = XR_NULL_HANDLE;
XrSpace space = XR_NULL_HANDLE, viewSpace = XR_NULL_HANDLE;
XrSwapchain sc[2] = {}; std::vector<XrSwapchainImageD3D11KHR> scImg[2];
XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
XrActionSet aset = XR_NULL_HANDLE; XrAction act[IN_COUNT] = {}; XrSpace actSpace[IN_COUNT] = {};
int64_t fmt = 0;
ID3D11Device* dev = nullptr; ID3D11Device1* dev1 = nullptr; ID3D11DeviceContext* ctx = nullptr;
// last game frame per eye (mono: eye 0 only, shown to both eyes)
ID3D11Texture2D* eyeCopy[2] = {}; uint32_t copyW[2] = {}, copyH[2] = {}; float copyPose[2][7] = {}; bool have[2] = {}; bool mono = true;
// frame transport
LONG myTexGen = 0; int method = 0;
ID3D11Texture2D* shTex = nullptr; IDXGIKeyedMutex* km = nullptr; LONG lastFrameNo = 0;
HANDLE fmap = nullptr; uint8_t* frm = nullptr; LONG lastSeq = 0; std::vector<uint8_t> cpuBuf;
float tanHalf = 1.f;
CRITICAL_SECTION trLock; volatile bool trQuit = false;   // 0.4.2: transport answered from its own thread
unsigned stFrames = 0, stLayered = 0, stNew = 0, stEndErr = 0, stLockBusy = 0;

template<class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }
bool ok(XrResult r, const char* w) { if (XR_FAILED(r)) { L("%s FAILED (%d)", w, (int)r); return false; } return true; }
void msg(const char* m) { strncpy(shm->hostMsg, m, sizeof(shm->hostMsg) - 1); }
void fatal(const char* m) { L("FATAL: %s", m); msg(m); MemoryBarrier(); shm->hostState = bridge::H_FATAL; }
bool gameAlive() { return WaitForSingleObject(gameProc, 0) == WAIT_TIMEOUT; }
XrPath path(const char* s) { XrPath p = XR_NULL_PATH; xrStringToPath(inst, s, &p); return p; }
XrPoseF3 conv(const XrPosef& p, bool valid) { return {p.orientation.x,p.orientation.y,p.orientation.z,p.orientation.w,p.position.x,p.position.y,p.position.z,valid}; }

std::string regStr(HKEY root, const char* key, const char* val) {
    char buf[1024]; DWORD n = sizeof(buf);
    if (RegGetValueA(root, key, val, RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, buf, &n) == ERROR_SUCCESS) return buf;
    return "";
}
bool fileExists(const std::string& p) { return !p.empty() && GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
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
    xrSuggestInteractionProfileBindings(inst, &sb);
    return true;
}

void releaseTransport() {
    if (km) { rel(km); } rel(shTex);
    if (frm) { UnmapViewOfFile(frm); frm = nullptr; }
    if (fmap) { CloseHandle(fmap); fmap = nullptr; }
    method = 0; lastFrameNo = 0; lastSeq = 0;
}
const char* methodName(int m) { return m == bridge::M_NTNAME ? "GPU shared texture (NT handle)" : m == bridge::M_LEGACY ? "GPU shared texture (legacy handle)" : m == bridge::M_CPU ? "shared memory (CPU copy)" : "none"; }

// the game published a new way to receive frames: open it and answer
void checkTransport() {
    LONG g = shm->texGen;
    if (g == myTexGen) return;
    myTexGen = g;
    releaseTransport();
    int m = shm->texMethod;
    L("game offers frame transport %d: %s %ux%u", (int)g, methodName(m), shm->texW, shm->texH); HRESULT hr = E_FAIL; char why[200] = "";
    if (m == bridge::M_NTNAME) {
        if (dev1) hr = dev1->OpenSharedResource1((HANDLE)(uintptr_t)shm->texHandle, __uuidof(ID3D11Texture2D), (void**)&shTex);
        if (FAILED(hr) && dev1) { L("  open by duplicated handle failed 0x%08x, trying by name", (unsigned)hr);
            hr = dev1->OpenSharedResourceByName(shm->texName, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, __uuidof(ID3D11Texture2D), (void**)&shTex); }
        if (FAILED(hr)) snprintf(why, sizeof why, "OpenSharedResource1/ByName 0x%08x", (unsigned)hr);
    } else if (m == bridge::M_LEGACY) {
        hr = dev->OpenSharedResource((HANDLE)(uintptr_t)shm->texHandle, __uuidof(ID3D11Texture2D), (void**)&shTex);
        if (FAILED(hr)) snprintf(why, sizeof why, "OpenSharedResource 0x%08x", (unsigned)hr);
    } else if (m == bridge::M_CPU) {
        fmap = OpenFileMappingW(FILE_MAP_READ, FALSE, shm->texName);
        frm = fmap ? (uint8_t*)MapViewOfFile(fmap, FILE_MAP_READ, 0, 0, 0) : nullptr;
        hr = frm ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        if (FAILED(hr)) snprintf(why, sizeof why, "OpenFileMapping 0x%08x", (unsigned)hr);
        cpuBuf.resize((size_t)shm->texW * shm->texH * 4);
    }
    if (SUCCEEDED(hr) && shTex) {
        hr = shTex->QueryInterface(__uuidof(IDXGIKeyedMutex), (void**)&km);
        if (FAILED(hr)) snprintf(why, sizeof why, "no keyed mutex 0x%08x", (unsigned)hr);
        D3D11_TEXTURE2D_DESC d; shTex->GetDesc(&d);
        if (SUCCEEDED(hr) && (d.Width > vcv[0].recommendedImageRectWidth || d.Height > vcv[0].recommendedImageRectHeight)) { hr = E_INVALIDARG; snprintf(why, sizeof why, "texture %ux%u larger than the eye", d.Width, d.Height); }
    }
    bool good = SUCCEEDED(hr);
    if (good) { method = m; L("frame transport %d: %s %ux%u OK", (int)g, methodName(m), shm->texW, shm->texH); msg(methodName(m)); }
    else { L("frame transport %d: %s FAILED: %s", (int)g, methodName(m), why); msg(why); releaseTransport(); }
    shm->ackOk = good ? 1 : 0;
    MemoryBarrier();
    shm->ackGen = g;
}

DWORD WINAPI transportThread(void*) {
    while (!trQuit) { EnterCriticalSection(&trLock); checkTransport(); LeaveCriticalSection(&trLock); Sleep(5); }
    return 0;
}

// copy the newest game frame (if any) into eyeCopy
void pullFrame() {
    if (method == bridge::M_NTNAME || method == bridge::M_LEGACY) {
        if (km->AcquireSync(0, 0) != S_OK) { ++stLockBusy; return; }
        LONG fn = shm->frameNo;
        if (fn != lastFrameNo) {
            lastFrameNo = fn;
            int e = shm->frameEye; mono = e < 0; int t = mono ? 0 : (e & 1);
            ctx->CopyResource(eyeCopy[t], shTex);
            memcpy(copyPose[t], shm->framePose, sizeof(copyPose[t]));
            copyW[t] = shm->texW; copyH[t] = shm->texH; have[t] = true; ++stNew;
        }
        km->ReleaseSync(0);
    } else if (method == bridge::M_CPU) {
        auto* h = (const bridge::FrameHdr*)frm;
        LONG s1 = h->seq; MemoryBarrier();
        if ((s1 & 1) || s1 == lastSeq) return;
        uint32_t w = h->w, hh = h->h; if (w != shm->texW || hh != shm->texH || !w) return;
        float pose[7]; memcpy(pose, h->pose, sizeof pose); int e = h->eye;
        memcpy(cpuBuf.data(), frm + bridge::kFrameHdr, (size_t)w * hh * 4);
        MemoryBarrier();
        if (h->seq != s1) return;   // overwritten while copying: take the next one
        lastSeq = s1;
        mono = e < 0; int t = mono ? 0 : (e & 1);
        D3D11_BOX b{0, 0, 0, w, hh, 1};
        ctx->UpdateSubresource(eyeCopy[t], 0, &b, cpuBuf.data(), w * 4, 0);
        memcpy(copyPose[t], pose, sizeof pose); copyW[t] = w; copyH[t] = hh; have[t] = true; ++stNew;
    }
}

bool setup() {
    std::string rt = shm->runtime;
    L("runtime setting: %s", rt.c_str());
    for (const char* n : {"SteamAppId", "SteamGameId", "SteamOverlayGameId", "SteamEnv"}) SetEnvironmentVariableA(n, nullptr);
    if (rt != "system") {
        std::string j = findSteamVrJson();
        if (!j.empty()) { SetEnvironmentVariableA("XR_RUNTIME_JSON", j.c_str()); L("runtime json: %s", j.c_str()); }
        else L("SteamVR not found - using the Windows active runtime");
    }
    HMODULE dll = LoadLibraryA("openxr_loader.dll");
    if (!dll) { fatal("openxr_loader.dll not found next to GTA5VR_Host.exe"); return false; }
    gpa = (PFN_xrGetInstanceProcAddr)GetProcAddress(dll, "xrGetInstanceProcAddr");
    gpa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
    const char* ext[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "GTA5VR Host"); strcpy(ci.applicationInfo.engineName, "GTA5VR");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0); ci.enabledExtensionCount = 1; ci.enabledExtensionNames = ext;
    if (!ok(xrCreateInstance(&ci, &inst), "xrCreateInstance")) { fatal("xrCreateInstance failed - is SteamVR running and the headset connected?"); return false; }
#define G(n) gpa(inst, #n, (PFN_xrVoidFunction*)&n);
    G(xrGetSystem) G(xrCreateSession) G(xrCreateReferenceSpace) G(xrEnumerateViewConfigurationViews) G(xrCreateSwapchain)
    G(xrEnumerateSwapchainImages) G(xrEnumerateSwapchainFormats) G(xrPollEvent) G(xrBeginSession) G(xrEndSession) G(xrWaitFrame)
    G(xrBeginFrame) G(xrEndFrame) G(xrLocateViews) G(xrLocateSpace) G(xrAcquireSwapchainImage) G(xrWaitSwapchainImage)
    G(xrReleaseSwapchainImage) G(xrStringToPath) G(xrCreateActionSet) G(xrCreateAction) G(xrSuggestInteractionProfileBindings)
    G(xrAttachSessionActionSets) G(xrSyncActions) G(xrGetActionStateFloat) G(xrGetActionStateBoolean) G(xrCreateActionSpace)
    G(xrDestroyInstance) G(xrGetD3D11GraphicsRequirementsKHR) G(xrRequestExitSession) G(xrDestroySession) G(xrGetInstanceProperties)
#undef G
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES}; xrGetInstanceProperties(inst, &ip); L("runtime: %s", ip.runtimeName);
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO}; gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
    for (int i = 0; i < 120 && shm->want && gameAlive(); ++i) { r = xrGetSystem(inst, &gi, &sys); if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE) break; if (i % 10 == 0) L("waiting for the headset... (%d s)", i / 2); Sleep(500); }
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE && !shm->want) { L("cancelled: VR was switched off (F8) while connecting"); return false; }
    if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE && !gameAlive()) { L("cancelled: game closed while connecting"); return false; }
    if (r == XR_SUCCESS) L("headset found");
    if (!ok(r, "xrGetSystem")) { fatal("xrGetSystem failed - headset not connected to SteamVR?"); return false; }
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR}; xrGetD3D11GraphicsRequirementsKHR(inst, sys, &req);
    IDXGIFactory1* fac = nullptr; IDXGIAdapter1* ad = nullptr; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if (memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) { L("GPU: %ls", d.Description); break; }
        ad->Release(); ad = nullptr;
    }
    if (fac) fac->Release();
    D3D_FEATURE_LEVEL fls[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 2, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (hr == E_INVALIDARG) hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls + 1, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (ad) ad->Release();
    if (FAILED(hr)) { char b[96]; snprintf(b, sizeof b, "D3D11CreateDevice failed 0x%08x", (unsigned)hr); fatal(b); return false; }
    dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1);
    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; gb.device = dev;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO}; si.next = &gb; si.systemId = sys;
    if (!ok(xrCreateSession(inst, &si, &sess), "xrCreateSession")) { fatal("xrCreateSession failed"); return false; }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; xrCreateReferenceSpace(sess, &rs, &space);
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW; xrCreateReferenceSpace(sess, &rs, &viewSpace);
    uint32_t n = 2; xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv);
    uint32_t fc = 0; xrEnumerateSwapchainFormats(sess, 0, &fc, nullptr); std::vector<int64_t> fmts(fc); xrEnumerateSwapchainFormats(sess, fc, &fc, fmts.data());
    std::string offered; for (auto x : fmts) offered += std::to_string(x) + " ";
    // eye images must be copyable from the game frame (R8G8B8A8): only that format family
    for (int64_t want : {(int64_t)DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, (int64_t)DXGI_FORMAT_R8G8B8A8_UNORM}) { for (auto x : fmts) if (x == want) { fmt = x; break; } if (fmt) break; }
    L("eye %ux%u, runtime offers [%s], using %d", vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight, offered.c_str(), (int)fmt);
    if (!fmt) { fatal("the runtime offers no R8G8B8A8 eye format"); return false; }
    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo c{XR_TYPE_SWAPCHAIN_CREATE_INFO}; c.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT; c.format = fmt;
        c.sampleCount = 1; c.width = vcv[e].recommendedImageRectWidth; c.height = vcv[e].recommendedImageRectHeight; c.faceCount = c.arraySize = c.mipCount = 1;
        if (!ok(xrCreateSwapchain(sess, &c, &sc[e]), "xrCreateSwapchain")) { fatal("xrCreateSwapchain failed"); return false; }
        uint32_t k = 0; xrEnumerateSwapchainImages(sc[e], 0, &k, nullptr); scImg[e].assign(k, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(sc[e], k, &k, (XrSwapchainImageBaseHeader*)scImg[e].data());
        D3D11_TEXTURE2D_DESC t{}; t.Width = c.width; t.Height = c.height; t.MipLevels = 1; t.ArraySize = 1; t.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        t.SampleDesc = {1, 0}; t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(dev->CreateTexture2D(&t, nullptr, &eyeCopy[e]))) { fatal("eye copy texture failed"); return false; }
    }
    if (!createActions()) { fatal("OpenXR actions failed"); return false; }
    for (int i = 0; i < IN_COUNT; ++i) if (kInputs[i].type == XrType::Pose) {
        XrActionSpaceCreateInfo a{XR_TYPE_ACTION_SPACE_CREATE_INFO}; a.action = act[i]; a.poseInActionSpace.orientation.w = 1;
        xrCreateActionSpace(sess, &a, &actSpace[i]);
    }
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO}; at.countActionSets = 1; at.actionSets = &aset;
    ok(xrAttachSessionActionSets(sess, &at), "attach actions");
    shm->eyeW = vcv[0].recommendedImageRectWidth; shm->eyeH = vcv[0].recommendedImageRectHeight;
    MemoryBarrier();
    shm->hostState = bridge::H_READY;
    L("ready, waiting for frames from the game");
    return true;
}

void publishState(const XrFrameState& fs, const XrViewState& vs, const XrView* views, bool running) {
    VrState s{};
    XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION}; xrLocateSpace(viewSpace, space, fs.predictedDisplayTime, &hl);
    s.head = conv(hl.pose, (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0);
    for (int e = 0; e < 2; ++e) { s.eye[e] = conv(views[e].pose, true); s.eyeFovDeg[e] = (views[e].fov.angleUp - views[e].fov.angleDown) * 57.29578f; }
    if (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) {
        float t = 0.f;
        for (int e = 0; e < 2; ++e) for (float a : {views[e].fov.angleUp, -views[e].fov.angleDown, views[e].fov.angleRight, -views[e].fov.angleLeft}) t = std::max(t, tanf(a));
        if (t > 0.3f && t < 4.f) tanHalf = t;
    }
    s.fovDeg = 2.f * atanf(tanHalf) * 57.29578f;
    for (int i = 0; i < IN_COUNT; ++i) {
        XrActionStateGetInfo g{XR_TYPE_ACTION_STATE_GET_INFO}; g.action = act[i];
        if (kInputs[i].type == XrType::Pose) {
            XrSpaceLocation l{XR_TYPE_SPACE_LOCATION}; xrLocateSpace(actSpace[i], space, fs.predictedDisplayTime, &l);
            s.pose[i] = conv(l.pose, (l.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0);
        } else if (kInputs[i].type == XrType::Float) {
            XrActionStateFloat f{XR_TYPE_ACTION_STATE_FLOAT}; xrGetActionStateFloat(sess, &g, &f); s.value[i] = f.currentState;
        } else {
            XrActionStateBoolean b{XR_TYPE_ACTION_STATE_BOOLEAN}; xrGetActionStateBoolean(sess, &g, &b); s.value[i] = b.currentState ? 1.f : 0.f;
        }
    }
    s.running = running;
    InterlockedIncrement(&shm->vrSeq); MemoryBarrier();
    shm->vr = s;
    MemoryBarrier(); InterlockedIncrement(&shm->vrSeq);
}
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int) {
    char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string dir = m; dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    SetCurrentDirectoryA(dir.c_str());
    vrlog::fileName() = "GTA5VR_Host.log";
    DWORD pid = (DWORD)strtoul(cmd ? cmd : "", nullptr, 10);
    L("GTA5VR_Host 0.4.2 started for GTA5.exe pid %lu", (unsigned long)pid);
    wchar_t name[64]; bridge::shmName(pid, name, 64);
    HANDLE map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!map) { L("no shared memory %ls (start this from the game with F8)", name); return 2; }
    shm = (bridge::Shm*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(bridge::Shm));
    if (!shm || shm->magic != bridge::kMagic || shm->version != bridge::kVersion) { L("shared memory version mismatch - reinstall the mod"); return 3; }
    gameProc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!gameProc) { L("game process not found"); return 4; }
    myTexGen = shm->texGen;   // anything published before this helper started belongs to an old helper
    if (!setup()) { Sleep(200); return 5; }
    InitializeCriticalSection(&trLock);
    HANDLE trThread = CreateThread(nullptr, 0, transportThread, nullptr, 0, nullptr);

    bool running = false, exitReq = false, quit = false; XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    ULONGLONG lastStat = GetTickCount64(); bool wasTest = false;
    while (!quit) {
        InterlockedIncrement(&shm->heartbeat);
        bool alive = gameAlive();
        if ((!alive || !shm->want) && !exitReq) {
            L(alive ? "VR off (F8) - ending the session" : "game closed - ending the session");
            exitReq = true;
            if (running) xrRequestExitSession(sess); else quit = true;
        }
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state = ((XrEventDataSessionStateChanged*)&ev)->state; L("session state %d", (int)state);
                if (state == XR_SESSION_STATE_READY && !exitReq) { XrSessionBeginInfo b{XR_TYPE_SESSION_BEGIN_INFO}; b.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; running = ok(xrBeginSession(sess, &b), "xrBeginSession"); if (running) shm->hostState = bridge::H_RUNNING; }
                if (state == XR_SESSION_STATE_STOPPING) { xrEndSession(sess); running = false; if (exitReq) quit = true; }
                if (state == XR_SESSION_STATE_EXITING || state == XR_SESSION_STATE_LOSS_PENDING) quit = true;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (exitReq && !running && state != XR_SESSION_STATE_STOPPING) quit = quit || state == XR_SESSION_STATE_IDLE || state == XR_SESSION_STATE_UNKNOWN || state == XR_SESSION_STATE_READY;
        if (!running) { Sleep(10); continue; }

        XrFrameState fs{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        if (!ok(xrWaitFrame(sess, &wi, &fs), "xrWaitFrame")) { Sleep(5); continue; }
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO}; xrBeginFrame(sess, &bi);
        EnterCriticalSection(&trLock); if (method) pullFrame(); LeaveCriticalSection(&trLock);
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO}; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = fs.predictedDisplayTime; li.space = space;
        XrViewState vs{XR_TYPE_VIEW_STATE}; XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t vn = 2; xrLocateViews(sess, &li, &vs, 2, &vn, v);
        XrActiveActionSet as{aset, XR_NULL_PATH}; XrActionsSyncInfo sy{XR_TYPE_ACTIONS_SYNC_INFO}; sy.countActiveActionSets = 1; sy.activeActionSets = &as;
        xrSyncActions(sess, &sy);
        publishState(fs, vs, v, state == XR_SESSION_STATE_FOCUSED || state == XR_SESSION_STATE_VISIBLE);
        XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION}; xrLocateSpace(viewSpace, space, fs.predictedDisplayTime, &hl);

        bool test = shm->test != 0;
        if (test != wasTest) { L("F10 test colour %s", test ? "ON" : "off"); wasTest = test; }
        XrCompositionLayerProjectionView pv[2];
        float ha = atanf(tanHalf);
        bool canShow = test || (mono ? have[0] : (have[0] && have[1]));
        for (int e = 0; e < 2; ++e) {
            int src = mono ? 0 : e;
            uint32_t idx; XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; xrAcquireSwapchainImage(sc[e], &ai, &idx);
            XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; w.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(sc[e], &w);
            if (test) {
                D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = (DXGI_FORMAT)fmt; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
                ID3D11RenderTargetView* rtv = nullptr;
                if (SUCCEEDED(dev->CreateRenderTargetView(scImg[e][idx].texture, &rv, &rtv))) { const float c[2][4] = {{1, 0, 1, 1}, {0, 1, 0, 1}}; ctx->ClearRenderTargetView(rtv, c[e]); rtv->Release(); }
            } else if (have[src]) {
                D3D11_BOX b{0, 0, 0, copyW[src], copyH[src], 1};
                ctx->CopySubresourceRegion(scImg[e][idx].texture, 0, 0, 0, 0, eyeCopy[src], 0, &b);
            }
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(sc[e], &ri);
            pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv[e].subImage.swapchain = sc[e];
            if (test) {
                pv[e].pose = v[e].pose; pv[e].fov = v[e].fov;
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)vcv[e].recommendedImageRectWidth, (int32_t)vcv[e].recommendedImageRectHeight}};
            } else {
                // the pose the game camera rendered this image with; without one (menus, no VR camera yet) the image follows the head
                const float* p = copyPose[src];
                if (bridge::poseOk(p)) pv[e].pose = {{p[0], p[1], p[2], p[3]}, {p[4], p[5], p[6]}};
                else pv[e].pose = hl.pose;
                pv[e].fov = {-ha, ha, ha, -ha};
                pv[e].subImage.imageRect = {{0, 0}, {(int32_t)std::max(1u, copyW[src]), (int32_t)std::max(1u, copyH[src])}};
            }
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; layer.space = space; layer.viewCount = 2; layer.views = pv;
        const XrCompositionLayerBaseHeader* ls[] = {(XrCompositionLayerBaseHeader*)&layer};
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO}; ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        bool valid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && fs.shouldRender && canShow;
        ei.layerCount = valid ? 1 : 0; ei.layers = ls;
        XrResult er = xrEndFrame(sess, &ei);
        if (XR_FAILED(er)) { if (!stEndErr) L("xrEndFrame failed (%d)", (int)er); ++stEndErr; }
        ++stFrames; if (valid) ++stLayered;
        ULONGLONG now = GetTickCount64();
        if (now - lastStat > 5000) {
            float sec = (now - lastStat) / 1000.f;
            L("stats: headset %.0f fps, with image %u, new game frames %.0f/s, endFrame errors %u, transport busy %u, transport %s, state %d",
              stFrames / sec, stLayered, stNew / sec, stEndErr, stLockBusy, methodName(method), (int)state);
            shm->hostFps = (LONG)(stFrames / sec); shm->hostNewFrames = (LONG)(stNew / sec);
            stFrames = stLayered = stNew = stEndErr = stLockBusy = 0; lastStat = now;
        }
    }
    trQuit = true; if (trThread) { WaitForSingleObject(trThread, 2000); CloseHandle(trThread); }
    releaseTransport();
    if (sess) xrDestroySession(sess);
    if (inst) xrDestroyInstance(inst);
    VrState s{}; InterlockedIncrement(&shm->vrSeq); shm->vr = s; InterlockedIncrement(&shm->vrSeq);
    shm->hostState = bridge::H_EXITED;
    L("helper finished");
    return 0;
}
