// GTA5VR.asi — a Script Hook V script. Loaded by Ultimate ASI Loader (or SHV's own dinput8 loader).
// Needs ScriptHookV.dll (installed by the player). SHV refuses GTA Online, so the mod is story-mode only.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "natives.h"
#include "xr.h"
#include "game.h"
#include "log.h"

namespace {
HMODULE g_mod = nullptr;
bool g_sessionTried = false;
bool g_xrOk = false;

// sheet hooks.present: SHV calls this on every IDXGISwapChain::Present
void onPresent(void* swapChain) {
    if (!g_xrOk) return;
    auto* sw = static_cast<IDXGISwapChain*>(swapChain);
    ID3D11Device* dev = nullptr;
    if (FAILED(sw->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) return;
    if (!g_sessionTried) { g_sessionTried = true; xr::startSession(dev); }
    ID3D11DeviceContext* ctx = nullptr; ID3D11Texture2D* bb = nullptr;
    dev->GetImmediateContext(&ctx);
    if (SUCCEEDED(sw->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) { xr::onPresent(ctx, bb); bb->Release(); }
    ctx->Release(); dev->Release();
}

// sheet hooks.script_tick: SHV runs this as a game script (natives are legal here)
void scriptMain() {
    vrlog::write("script thread started (SHV game version id %d); F8 toggles VR", shv::gameVersion());
    for (;;) {
        if (GetAsyncKeyState(kToggleVk) & 1) game::toggle();
        game::tick();
        shv::wait(0);
    }
}

DWORD WINAPI boot(LPVOID) {
    vrlog::write("GTA5VR 0.2.0 loading (Script Hook V build, story mode only)");
    // SHV may be loaded after us by the ASI loader: retry for ~30 s.
    bool ok = false;
    for (int i = 0; i < 60 && !(ok = natives::init()); ++i) Sleep(500);
    if (!ok) { vrlog::write("VR stays OFF: %s", natives::lastError()); return 0; }
    g_xrOk = xr::loadLoader();
    if (!g_xrOk) vrlog::write("no OpenXR runtime/headset: camera mod runs flat (start Pico Connect / SteamVR first)");
    shv::registerPresent(&onPresent);
    shv::registerScript(g_mod, &scriptMain);
    vrlog::write("registered with Script Hook V");
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE m, DWORD r, LPVOID) {
    if (r == DLL_PROCESS_ATTACH) { g_mod = m; DisableThreadLibraryCalls(m); CreateThread(nullptr, 0, boot, nullptr, 0, nullptr); }
    if (r == DLL_PROCESS_DETACH) { shv::unregisterPresent(&onPresent); shv::unregisterScript(m); xr::shutdown(); }
    return TRUE;
}
