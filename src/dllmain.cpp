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

// sheet hooks.present: SHV calls this on every IDXGISwapChain::Present
// Nothing VR happens until the player presses F8 in story mode: no OpenXR session in menus or loading screens,
// so the game is never throttled by the headset before that.
void onPresent(void* swapChain) {
    bool want = game::enabled();
    if (want && xr::failed()) { game::forceOff(); want = false; }
    xr::setWanted(want);   // F8: starts / stops GTA5VR_Host.exe, which runs OpenXR outside the game process
    if (!want) return;
    auto* sw = static_cast<IDXGISwapChain*>(swapChain);
    ID3D11Device* dev = nullptr;
    if (FAILED(sw->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) return;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sw->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) { dev->Release(); return; }
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    xr::onPresent(dev, ctx, bb);
    bb->Release(); ctx->Release(); dev->Release();
}

// sheet hooks.script_tick: SHV runs this as a game script (natives are legal here)
// Keys: edge-detected on the "is down" bit with a 600 ms debounce. The old "pressed since last call" bit
// could fire twice for one press (key auto-repeat, focus switches to SteamVR), which turned VR off
// again a few seconds after F8 (0.4.0 log: "F8: VR on" ... "F8: VR off" with one press).
bool keyHit(int vk) {
    static bool down[256] = {};
    static ULONGLONG last[256] = {};
    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool hit = d && !down[vk];
    down[vk] = d;
    if (!hit) return false;
    ULONGLONG now = GetTickCount64();
    if (now - last[vk] < 600) return false;
    last[vk] = now;
    return true;
}

void scriptMain() {
    vrlog::write("script thread started (SHV game version id %d); F8 toggles VR, F9 recenters, F10 test pattern, F11 arms on/off, F12 hand rotation on/off", shv::gameVersion());
    ULONGLONG onAt = 0;
    for (;;) {
        if (keyHit(kToggleVk)) {
            ULONGLONG now = GetTickCount64();
            // while the helper is still connecting to SteamVR (can take 10-30 s) a second F8 within 3 s is ignored
            if (game::enabled() && now - onAt < 3000) vrlog::write("F8 ignored: VR is still starting (wait, the helper connects to SteamVR)");
            else { game::toggle(); if (game::enabled()) onAt = now; }
        }
        if (keyHit(VK_F9)) game::recenter();
        if (keyHit(VK_F10)) xr::toggleTestPattern();
        if (keyHit(VK_F11)) game::toggleArms();
        if (keyHit(VK_F12)) hands::toggle();
        game::tick();
        shv::wait(0);
    }
}

DWORD WINAPI boot(LPVOID) {
    vrlog::write("GTA5VR 0.4.8 loading (Script Hook V build, story mode only)");
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
