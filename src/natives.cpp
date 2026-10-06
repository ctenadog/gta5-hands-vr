#include <windows.h>
#include "natives.h"
#include "log.h"

namespace {
// MSVC-mangled exports of ScriptHookV.dll (same names the SHV SDK's .lib imports).
using RegFn      = void(*)(HMODULE, void(*)());
using UnregFn    = void(*)(HMODULE);
using WaitFn     = void(*)(DWORD);
using InitFn     = void(*)(uint64_t);
using Push64Fn   = void(*)(uint64_t);
using CallFn     = uint64_t*(*)();
using PresRegFn  = void(*)(shv::PresentCb);
using VerFn      = int(*)();
RegFn pReg; UnregFn pUnreg; WaitFn pWait; InitFn pInit; Push64Fn pPush; CallFn pCall;
PresRegFn pPresReg, pPresUnreg; VerFn pVer;
bool g_ready = false;
const char* g_err = "not initialised";
template<class T> bool get(HMODULE m, T& out, const char* name) {
    out = reinterpret_cast<T>(GetProcAddress(m, name));
    if (!out) vrlog::write("ScriptHookV.dll: export missing: %s", name);
    return out != nullptr;
}
}

namespace shv {
bool load() {
    HMODULE m = GetModuleHandleA("ScriptHookV.dll");
    if (!m) m = LoadLibraryA("ScriptHookV.dll");
    if (!m) { g_err = "ScriptHookV.dll not found next to GTA5.exe (install Script Hook V: dev-c.com/gtav/scripthookv)"; return false; }
    bool ok = get(m, pReg,  "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z")
            & get(m, pUnreg,"?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z")
            & get(m, pWait, "?scriptWait@@YAXK@Z")
            & get(m, pInit, "?nativeInit@@YAX_K@Z")
            & get(m, pPush, "?nativePush64@@YAX_K@Z")
            & get(m, pCall, "?nativeCall@@YAPEA_KXZ")
            & get(m, pPresReg,   "?presentCallbackRegister@@YAXP6AXPEAX@Z@Z")
            & get(m, pPresUnreg, "?presentCallbackUnregister@@YAXP6AXPEAX@Z@Z");
    get(m, pVer, "?getGameVersion@@YA?AW4eGameVersion@@XZ");   // optional, log only
    if (!ok) { g_err = "ScriptHookV.dll is missing exports (update Script Hook V)"; return false; }
    g_ready = true; g_err = "";
    return true;
}
bool registerScript(void* mod, ScriptFn fn) { if (!pReg) return false; pReg((HMODULE)mod, fn); return true; }
void unregisterScript(void* mod) { if (pUnreg) pUnreg((HMODULE)mod); }
bool registerPresent(PresentCb cb) { if (!pPresReg) return false; pPresReg(cb); return true; }
void unregisterPresent(PresentCb cb) { if (pPresUnreg) pPresUnreg(cb); }
void wait(unsigned ms) { if (pWait) pWait(ms); }
int  gameVersion() { return pVer ? pVer() : -1; }
}

namespace natives {
bool init() { return shv::load(); }
bool ready() { return g_ready; }
const char* lastError() { return g_err; }
void begin(NativeId id) { pInit(kNatives[id].hash); }
void push(uint64_t raw) { pPush(raw); }
uint64_t* call() { return pCall(); }
}
