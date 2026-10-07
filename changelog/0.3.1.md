# 0.3.1 — separate D3D11 device for SteamVR

Evidence (user's vrcompositor.txt, session 20:22–20:26, PID 18684):
- `Startup 24 presents`, then `Timed out 15707 presents` — SteamVR received ~60 fps but treated GTA as not responding for the whole session.
- Many `AcquireSync FAILED with WAIT_TIMEOUT` / `ReleaseSync FAILED 0x887a0001` — the compositor could not take the shared eye textures.

Change:
- New GTA5VR.ini `[vr] own_device=1` (default). The mod creates its own D3D11 device on the headset adapter (LUID from xrGetD3D11GraphicsRequirementsKHR) and gives only that device to OpenXR/SteamVR.
- Per eye a shared keyed-mutex texture (eye size, swapchain format). GTA's device draws the frame into it with the existing shader blit (key 0 -> 1), our device copies it into the swapchain image (key 1 -> 0), flushes, submits.
- GTA's device is no longer set multithread-protected and is never touched by the runtime in this mode.
- `own_device=0` restores the 0.3.0 behaviour (GTA's device).
- Stats line now shows `shared-texture sync fail N, device own|game`.

Untested in game/headset.
