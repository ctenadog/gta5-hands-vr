// xrtest.exe - minimal OpenXR + D3D11 test, no GTA. Clears the left eye magenta, the right eye green for 60 s.
// Put it next to openxr_loader.dll (the GTA V folder after setup.bat) and run it while SteamVR is running.
// Writes xrtest.log next to itself.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

static FILE* g_log = nullptr;
static void L(const char* f, ...) {
    char b[1024]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a);
    SYSTEMTIME t; GetLocalTime(&t);
    printf("[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, b);
    if (g_log) { fprintf(g_log, "[%02d:%02d:%02d] %s\n", t.wHour, t.wMinute, t.wSecond, b); fflush(g_log); }
}
static PFN_xrGetInstanceProcAddr gpa;
#define F(n) static PFN_##n n;
F(xrCreateInstance) F(xrGetSystem) F(xrGetD3D11GraphicsRequirementsKHR) F(xrCreateSession) F(xrCreateReferenceSpace)
F(xrEnumerateViewConfigurationViews) F(xrEnumerateSwapchainFormats) F(xrCreateSwapchain) F(xrEnumerateSwapchainImages)
F(xrPollEvent) F(xrBeginSession) F(xrEndSession) F(xrWaitFrame) F(xrBeginFrame) F(xrEndFrame) F(xrLocateViews)
F(xrAcquireSwapchainImage) F(xrWaitSwapchainImage) F(xrReleaseSwapchainImage) F(xrGetInstanceProperties)
F(xrDestroySession) F(xrDestroyInstance) F(xrRequestExitSession)
#undef F
static bool ok(XrResult r, const char* w) { if (XR_FAILED(r)) { L("%s FAILED (%d)", w, (int)r); return false; } return true; }

int main(int argc, char** argv) {
    char m[MAX_PATH]; GetModuleFileNameA(nullptr, m, MAX_PATH); std::string dir = m; dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    g_log = fopen((dir + "xrtest.log").c_str(), "w");
    bool steamvr = !(argc > 1 && _stricmp(argv[1], "system") == 0);
    L("xrtest 1.0 - left eye MAGENTA, right eye GREEN for 60 s (arg 'system' = Windows active runtime)");
    if (steamvr) {
        const char* cands[] = {"F:\\stes\\steamapps\\common\\SteamVR\\steamxr_win64.json", "C:\\Program Files (x86)\\Steam\\steamapps\\common\\SteamVR\\steamxr_win64.json"};
        for (auto c : cands) if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES) { SetEnvironmentVariableA("XR_RUNTIME_JSON", c); L("runtime json: %s", c); break; }
    }
    HMODULE dll = LoadLibraryA((dir + "openxr_loader.dll").c_str());
    if (!dll) dll = LoadLibraryA("openxr_loader.dll");
    if (!dll) { L("openxr_loader.dll not found - put xrtest.exe into the GTA V folder"); getchar(); return 1; }
    gpa = (PFN_xrGetInstanceProcAddr)GetProcAddress(dll, "xrGetInstanceProcAddr");
    gpa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
    const char* ext[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "GTA5VR xrtest"); strcpy(ci.applicationInfo.engineName, "xrtest");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0); ci.enabledExtensionCount = 1; ci.enabledExtensionNames = ext;
    XrInstance inst{};
    if (!ok(xrCreateInstance(&ci, &inst), "xrCreateInstance (SteamVR running?)")) { getchar(); return 1; }
#define G(n) gpa(inst, #n, (PFN_xrVoidFunction*)&n);
    G(xrGetSystem) G(xrGetD3D11GraphicsRequirementsKHR) G(xrCreateSession) G(xrCreateReferenceSpace) G(xrEnumerateViewConfigurationViews)
    G(xrEnumerateSwapchainFormats) G(xrCreateSwapchain) G(xrEnumerateSwapchainImages) G(xrPollEvent) G(xrBeginSession) G(xrEndSession)
    G(xrWaitFrame) G(xrBeginFrame) G(xrEndFrame) G(xrLocateViews) G(xrAcquireSwapchainImage) G(xrWaitSwapchainImage)
    G(xrReleaseSwapchainImage) G(xrGetInstanceProperties) G(xrDestroySession) G(xrDestroyInstance) G(xrRequestExitSession)
#undef G
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES}; xrGetInstanceProperties(inst, &ip); L("runtime: %s", ip.runtimeName);
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO}; gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY; XrSystemId sys{};
    if (!ok(xrGetSystem(inst, &gi, &sys), "xrGetSystem (headset connected?)")) { getchar(); return 1; }
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR}; xrGetD3D11GraphicsRequirementsKHR(inst, sys, &req);
    IDXGIFactory1* fac = nullptr; IDXGIAdapter1* ad = nullptr; CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&fac);
    for (UINT i = 0; fac && fac->EnumAdapters1(i, &ad) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d; ad->GetDesc1(&d);
        if (memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0) { L("GPU: %ls", d.Description); break; }
        ad->Release(); ad = nullptr;
    }
    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(ad, ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    if (FAILED(hr)) { L("D3D11CreateDevice failed 0x%08x", (unsigned)hr); getchar(); return 1; }
    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR}; gb.device = dev;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO}; si.next = &gb; si.systemId = sys; XrSession s{};
    if (!ok(xrCreateSession(inst, &si, &s), "xrCreateSession")) { getchar(); return 1; }
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; rs.poseInReferenceSpace.orientation.w = 1;
    XrSpace space{}; xrCreateReferenceSpace(s, &rs, &space);
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}; uint32_t n = 2;
    xrEnumerateViewConfigurationViews(inst, sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, vcv);
    uint32_t fc = 0; xrEnumerateSwapchainFormats(s, 0, &fc, nullptr); std::vector<int64_t> fmts(fc); xrEnumerateSwapchainFormats(s, fc, &fc, fmts.data());
    int64_t fmt = fmts.empty() ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : fmts[0];
    L("eye %ux%u, format %d", vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight, (int)fmt);
    XrSwapchain sc[2]; std::vector<XrSwapchainImageD3D11KHR> img[2];
    for (int e = 0; e < 2; ++e) {
        XrSwapchainCreateInfo c{XR_TYPE_SWAPCHAIN_CREATE_INFO}; c.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT; c.format = fmt;
        c.sampleCount = 1; c.width = vcv[e].recommendedImageRectWidth; c.height = vcv[e].recommendedImageRectHeight; c.faceCount = c.arraySize = c.mipCount = 1;
        if (!ok(xrCreateSwapchain(s, &c, &sc[e]), "xrCreateSwapchain")) { getchar(); return 1; }
        uint32_t k = 0; xrEnumerateSwapchainImages(sc[e], 0, &k, nullptr); img[e].assign(k, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        xrEnumerateSwapchainImages(sc[e], k, &k, (XrSwapchainImageBaseHeader*)img[e].data());
    }
    bool running = false, quit = false; ULONGLONG start = GetTickCount64(), lastStat = start; unsigned frames = 0, layered = 0, errs = 0;
    while (!quit) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(inst, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto st = ((XrEventDataSessionStateChanged*)&ev)->state; L("session state %d", (int)st);
                if (st == XR_SESSION_STATE_READY) { XrSessionBeginInfo b{XR_TYPE_SESSION_BEGIN_INFO}; b.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; running = ok(xrBeginSession(s, &b), "xrBeginSession"); }
                if (st == XR_SESSION_STATE_STOPPING) { xrEndSession(s); running = false; }
                if (st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING) quit = true;
            }
            ev = {XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (GetTickCount64() - start > 60000 && running) { xrRequestExitSession(s); start = ~0ull >> 1; }
        if (!running) { Sleep(10); continue; }
        XrFrameState fs{XR_TYPE_FRAME_STATE}; XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
        if (!ok(xrWaitFrame(s, &wi, &fs), "xrWaitFrame")) continue;
        XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO}; xrBeginFrame(s, &bi);
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO}; li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO; li.displayTime = fs.predictedDisplayTime; li.space = space;
        XrViewState vs{XR_TYPE_VIEW_STATE}; XrView v[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}; uint32_t vn = 2; xrLocateViews(s, &li, &vs, 2, &vn, v);
        XrCompositionLayerProjectionView pv[2];
        for (int e = 0; e < 2; ++e) {
            uint32_t idx; XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO}; xrAcquireSwapchainImage(sc[e], &ai, &idx);
            XrSwapchainImageWaitInfo w{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; w.timeout = XR_INFINITE_DURATION; xrWaitSwapchainImage(sc[e], &w);
            D3D11_RENDER_TARGET_VIEW_DESC rv{}; rv.Format = (DXGI_FORMAT)fmt; rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            ID3D11RenderTargetView* rtv = nullptr;
            if (SUCCEEDED(dev->CreateRenderTargetView(img[e][idx].texture, &rv, &rtv))) { const float c[2][4] = {{1, 0, 1, 1}, {0, 1, 0, 1}}; ctx->ClearRenderTargetView(rtv, c[e]); rtv->Release(); }
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xrReleaseSwapchainImage(sc[e], &ri);
            pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}; pv[e].pose = v[e].pose; pv[e].fov = v[e].fov;
            pv[e].subImage.swapchain = sc[e]; pv[e].subImage.imageRect = {{0, 0}, {(int32_t)vcv[e].recommendedImageRectWidth, (int32_t)vcv[e].recommendedImageRectHeight}};
        }
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; layer.space = space; layer.viewCount = 2; layer.views = pv;
        const XrCompositionLayerBaseHeader* ls[] = {(XrCompositionLayerBaseHeader*)&layer};
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO}; ei.displayTime = fs.predictedDisplayTime; ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        bool valid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && fs.shouldRender;
        ei.layerCount = valid ? 1 : 0; ei.layers = ls;
        if (XR_FAILED(xrEndFrame(s, &ei))) ++errs;
        ++frames; if (valid) ++layered;
        if (GetTickCount64() - lastStat > 5000) { L("stats: %u frames, %u with image, %u endFrame errors", frames, layered, errs); frames = layered = errs = 0; lastStat = GetTickCount64(); }
    }
    xrDestroySession(s); xrDestroyInstance(inst);
    L("done. Did the headset show MAGENTA (left) / GREEN (right)? Press Enter.");
    getchar();
    return 0;
}
