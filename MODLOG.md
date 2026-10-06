# GTA V Legacy VR (Pico) — MODLOG
Goal v0.1: head-tracked stereo first-person + arms following OpenXR controllers + controller aiming. Story mode only.
Route: native .asi on Ultimate ASI Loader (Melty auto-installs v9.7.4). No Script Hook V (manual install, can't ship).
Bundled: OpenXR loader 1.1.63 (Apache-2.0, Khronos), MinHook (BSD-2, TsudaKageyu).
Target build: 1.0.3889.0 (most Melty players; Minecraft x GTA V pins it).
## Preflight 2026-10-06: NOT CLEAN
- natives.hash_3889: 15 cells empty. Without Script Hook V the mod must call natives through GTA5.exe's own
  native table, whose hashes are remapped per build. Public 3889 crossmaps come from online mod-menu projects: not used.
  Must be read from the player's GTA5.exe 1.0.3889.0 (on the PC with the game).
- hooks.native_lookup.pattern: byte pattern of the native registration/lookup in GTA5.exe 3889 — needs the exe.
- All 56 rows unverified in the running game (no game / headset in the cloud sandbox).
Next step: on the PC with the game, dump the 3889 native table + pattern (universal-modder reverse-engineering skill), fill sheets, rerun tools/preflight.py.
## Build 2026-10-06 (cloud sandbox, mingw-w64 cross-compile)
- DEV build compiles: build/GTA5VR-0.1.0.zip (GTA5VR.asi + openxr_loader.dll + licenses + readme).
- Melty: inspect_package OK after mapping openxr_loader.dll; validate_recipe valid, 5/5 files placed; one_click_check: YES (pinned to 1.0.3889.0).
- The DEV build does nothing in game yet: natives::init() refuses (hash_3889 + pattern empty) and logs "VR stays OFF".
- Not tested: no game, no GPU, no headset here. Nothing uploaded to Melty, no draft created.
Melty draft: GTA V Hands VR, modId f602c4c6-5677-4e51-92d8-5acfc524323e, MIT, remix allowed, no release/media yet.
## 0.2.0 2026-10-06: switched to Script Hook V (user request)
- natives now go through ScriptHookV.dll exports (nativeInit/nativePush64/nativeCall), resolved with GetProcAddress;
  SHV maps nativedb hashes to the running build -> sheet natives.hash_3889 and hooks.native_lookup removed.
- script thread = scriptRegister + scriptWait(0) loop; frames = presentCallbackRegister. MinHook removed.
- The .asi LoadLibrary's ScriptHookV.dll itself (Ultimate ASI Loader loads only .asi), retries 30 s.
- SHV is manual install -> Melty one-click check will report a manual step (expected). Players must copy ScriptHookV.dll.
- Preflight: unfilled cells 0. Still untested in game/headset (stereo AER, IK, camera roll sign, input paths).

## 0.2.1 (2026-10-06)
- User report: main menu ignores input. Log: OpenXR session created at the menu, before F8.
- Cause: g_enabled defaulted to true, so VR started by itself at the menu.
- Fix: VR starts OFF, no OpenXR session until F8 in story mode; F8 off ends the session (xrRequestExitSession).
- Swapchain format now matches the GTA backbuffer; swapchain wait capped at 100 ms.
- Installers remove old mod files first (GTA5VR.asi in root/scripts/plugins/asi, openxr_loader.dll, GTA5VR folder, stray archive files, source folders in the game dir); old log kept as GTA5VR.old.log; waits until GTA V is closed.

## 0.2.2 (2026-10-06)
- User report: SteamVR did not pick up GTA; after F8 the view went first person, spun and froze.
- SteamVR: before creating the OpenXR instance the mod points the loader at SteamVR's steamxr_win64.json (XR_RUNTIME_JSON),
  even if Pico Connect / Oculus is the Windows active runtime. GTA5VR.ini [vr] runtime=steamvr|system. App name "Grand Theft Auto V".
  OpenXR now starts on F8 (not at game start), so SteamVR can be started any time before F8. Log shows active runtime, runtime used, GPU match.
- Spinning: controller input sent a constant "centre" value every frame even with sticks released -> now nothing is sent inside a deadzone,
  triggers only above threshold. Head yaw at F8 becomes "straight ahead" (was absolute headset yaw). F9 recenters.
- Freeze: ID3D10Multithread protection on GTA's device; swapchain image is released only after a successful wait;
  a frame over 3 s switches VR off automatically instead of hanging; camera destroyed on exit; camera released on pause menu/loading screens.
