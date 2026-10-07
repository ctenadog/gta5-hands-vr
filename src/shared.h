// Shared memory between GTA5VR.asi (inside GTA5.exe) and GTA5VR_Host.exe (the separate OpenXR process).
// 0.3.x proved that SteamVR shows frames from a plain process (xrtest) but never from inside GTA5.exe,
// so since 0.4.0 OpenXR runs only in the helper; the game side just hands over frames and reads poses.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cwchar>
#include "xr.h"

namespace bridge {
constexpr uint32_t kMagic = 0x47355652u;   // "G5VR"
constexpr uint32_t kVersion = 3;   // 0.6.4: two more controller inputs (menu, phone)
enum Method : int32_t { M_NONE = 0, M_LEGACY = 1, M_NTNAME = 2, M_CPU = 3, M_END = 4 };
enum HostState : int32_t { H_STARTING = 0, H_READY = 1, H_RUNNING = 2, H_EXITED = 8, H_FATAL = 9 };
constexpr uint32_t kFrameHdr = 256;        // CPU path: header size before the pixels in the frame mapping

struct FrameHdr {                           // CPU path (seqlock: odd seq = being written)
    volatile LONG seq; uint32_t w, h, pitch; float pose[7]; int32_t eye; int32_t fmt;
};

struct Shm {
    uint32_t magic, version, gamePid;
    // game -> host
    volatile LONG want;          // F8
    volatile LONG test;          // F10
    volatile LONG runGen;        // bumped on every F8 start; an older helper exits when it differs
    int32_t stereo;
    char runtime[16];            // "steamvr" | "system"
    volatile LONG texGen;        // bumped when a new frame transport is published
    int32_t texMethod; uint64_t texHandle; uint32_t texW, texH, texFmt; wchar_t texName[96];
    float framePose[7]; int32_t frameEye; volatile LONG frameNo;   // GPU paths: written under the keyed mutex
    // host -> game
    volatile LONG ackGen, ackOk;
    volatile LONG hostState, heartbeat, hostFps, hostNewFrames;
    uint32_t eyeW, eyeH;          // per-eye image size the headset wants
    char hostMsg[256];
    volatile LONG vrSeq; VrState vr;   // seqlock
};

inline void shmName(DWORD pid, wchar_t* buf, size_t n) { swprintf(buf, n, L"Local\\GTA5VR_shm_%lu", (unsigned long)pid); }
inline bool poseOk(const float* p) { float n = p[0]*p[0] + p[1]*p[1] + p[2]*p[2] + p[3]*p[3]; return n > 0.9f && n < 1.1f; }
}
