// Calling GTA V natives through Script Hook V (ScriptHookV.dll, installed by the player).
// SHV maps the original nativedb hashes to the running game build itself, so no per-build table is needed.
// SHV's exports are resolved at runtime with GetProcAddress: the SHV SDK is neither linked nor shipped.
#pragma once
#include <cstdint>
#include <type_traits>
#include <cstring>
#include "generated/sheets.h"

struct Vector3 { float x; uint32_t _px; float y; uint32_t _py; float z; uint32_t _pz; };

namespace shv {
using ScriptFn  = void(*)();
using PresentCb = void(*)(void* swapChain);
bool load();                                  // false = ScriptHookV.dll missing or too old
bool registerScript(void* module, ScriptFn fn);
void unregisterScript(void* module);
bool registerPresent(PresentCb cb);
void unregisterPresent(PresentCb cb);
void wait(unsigned ms);
int  gameVersion();
uint8_t* entityAddress(int handle);           // SHV getScriptHandleBaseAddress, nullptr if unavailable
}

namespace natives {
bool init();          // = shv::load()
bool ready();
const char* lastError();
void begin(NativeId id);
void push(uint64_t raw);
uint64_t* call();     // script thread only

template<class T> inline void pushArg(T v) {
    static_assert(sizeof(T) <= 8, "arg too big");
    uint64_t raw = 0; std::memcpy(&raw, &v, sizeof(T)); push(raw);
}
template<class R = void, class... A> R invoke(NativeId id, A... a) {
    if (!ready()) { if constexpr (!std::is_void_v<R>) return R{}; else return; }
    begin(id); (pushArg(a), ...);
    uint64_t* r = call();
    if constexpr (!std::is_void_v<R>) { R out{}; if (r) std::memcpy(&out, r, sizeof(R)); return out; }
}
template<class... A> Vector3 invokeV3(NativeId id, A... a) {
    Vector3 v{}; if (!ready()) return v;
    begin(id); (pushArg(a), ...);
    uint64_t* r = call(); if (r) std::memcpy(&v, r, sizeof(v)); return v;
}
}
