// GTA5VR.asi — a Script Hook V script. Loaded by Ultimate ASI Loader (or SHV's own dinput8 loader).
// Needs ScriptHookV.dll (installed by the player). SHV refuses GTA Online, so the mod is story-mode only.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "natives.h"
#include "xr.h"
#include "game.h"
#include "log.h"
#include "hands.h"

namespace {
HMODULE g_mod = nullptr;
bool g_xrOk = false;

// sheet hooks.present: SHV calls this on every IDXGISwapChain::Present
// Nothing VR happens until the player presses F8 in story mode: no OpenXR session in menus or loading screens,
// so the game is never throttled by the headset before that.
void onPresent(void* swapChain) {
    bool want = game::enabled();
    if (want && xr::failed()) { game::forceOff(); want = false; }
    if (g_xrOk) xr::setWanted(want);
    if (!want) { if (g_xrOk && xr::hasSession()) xr::poll(); return; }
    // OpenXR starts only now (F8), so SteamVR can be started any time before pressing F8
    if (!g_xrOk) {
        static int triedGen = -1;
        if (triedGen == game::generation()) return;
        triedGen = game::generation();
        vrlog::write("starting OpenXR...");
        g_xrOk = xr::loadLoader();
        if (!g_xrOk) { vrlog::write("VR could not start: start SteamVR, connect the headset, press F8 again"); game::forceOff(); return; }
        xr::setWanted(true);
    }
    auto* sw = static_cast<IDXGISwapChain*>(swapChain);
    ID3D11Device* dev = nullptr;
    if (FAILED(sw->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sw->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) { dev->Release(); return; }
    static int sessionGen = -1;   // one session attempt per F8 press; a new one after the previous session ended
    if (xr::needsSession() && sessionGen != game::generation()) {
        sessionGen = game::generation();
        D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
        if (!xr::startSession(dev, d.Format)) { vrlog::write("VR session could not start - see errors above"); game::forceOff(); bb->Release(); dev->Release(); return; }
    }
    if (!xr::hasSession()) { bb->Release(); dev->Release(); return; }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    xr::onPresent(ctx, bb);
    bb->Release(); ctx->Release(); dev->Release();
}

// sheet hooks.script_tick: SHV runs this as a game script (natives are legal here)
void scriptMain() {
    vrlog::write("script thread started (SHV game version id %d); F8 toggles VR, F9 recenters, F10 test pattern, F11 arms on/off, F12 hand rotation (experimental)", shv::gameVersion());
    for (;;) {
        if (GetAsyncKeyState(kToggleVk) & 1) game::toggle();
        if (GetAsyncKeyState(VK_F9) & 1) game::recenter();
        if (GetAsyncKeyState(VK_F10) & 1) xr::toggleTestPattern();
        if (GetAsyncKeyState(VK_F11) & 1) game::toggleArms();
        if (GetAsyncKeyState(VK_F12) & 1) hands::toggle();
        game::tick();
        shv::wait(0);
    }
}

DWORD WINAPI boot(LPVOID) {
    vrlog::write("GTA5VR 0.3.1 loading (Script Hook V build, story mode only)");
    // SHV may be loaded after us by the ASI loader: retry for ~30 s.
    bool ok = false;
    for (int i = 0; i < 60 && !(ok = natives::init()); ++i) Sleep(500);
    if (!ok) { vrlog::write("VR stays OFF: %s", natives::lastError()); return 0; }
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
