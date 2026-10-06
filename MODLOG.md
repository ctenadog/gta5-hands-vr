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

## 0.2.3 (2026-10-06)
- User log: SteamVR picked correctly (runtime SteamVR/OpenXR, same GPU RTX 2060 SUPER). xrCreateSwapchain failed -26
  (XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED): backbuffer is 87 (B8G8R8A8_UNORM), mod asked for 28 which SteamVR does not offer.
  Also: after the session ended (state 8 EXITING) the next F8 never created a new session.
- Fix: swapchain format chosen only from the runtime's list (prefers RGBA/BGRA sRGB); the log lists offered formats.
  Frame goes to the eye via a small shader blit (src/blit.cpp): any format, BGRA->RGBA, centre crop to eye aspect,
  sRGB handled; GTA's D3D state saved/restored with SwapDeviceContextState. Copy no longer needs matching formats.
- Session lifecycle: on EXITING/LOSS_PENDING the session, swapchains and spaces are destroyed; every F8 press can create a new one.
  VR state "running" cleared when the session stops, so the camera does not stick with stale poses.

## 0.2.4 (2026-10-06)
- User log 0.2.3: SteamVR offers [29 91 2 10 24 40 55 45 20], mod uses 29; blit ready; session reaches FOCUSED (5),
  first frames submitted with layers 1. User sees endless SteamVR loading. Only first 5 frames were logged -> unknown why.
- Added periodic stats to the log (every 2 s first 30 s, then 10 s): game fps, frames to headset, frames with image,
  xrEndFrame/acquire/blit failures with last error code, invalid poses, shouldRender=0, slowest xrWaitFrame.
- ctx->Flush() after each eye blit (SteamVR compositor reads the image from another process).
- Only valid tracked view poses are stored per eye; layer skipped while a pose quaternion is invalid (was zero before both eyes had a pose).

## 0.2.5 (2026-10-06)
- User report on 0.2.4: no image in the headset, camera and hands shake, controller movement crooked.
- Movement: SET_CONTROL_VALUE_NEXT_FRAME axes take -1..1; mod sent 0..1 with 0.5 as centre -> constant push. Now stick value * sign.
- Shake: camera and IK targets were anchored to the head bone (bobs with walk anim) and rotated by ped heading (swung when
  the character turned). Now world-locked: anchor = ped root + eye_height (+ room offset clamped 0.4 m), frame yaw changes
  only on F8/F9 recenter and right-stick snap turn (30 deg). Gameplay cam relative heading follows the head so stick-forward = look direction.
- Layer: submitted with the poses the game camera actually used (previous frame), not the newly located ones;
  symmetric square fov = camera fov (was the eye's asymmetric fov -> stretched/swimming image).
- Default mode now mono (camera at head centre, same image to both eyes); alternate-eye stereo behind GTA5VR.ini stereo=1.
- F10 test pattern: colour gradient drawn into the eye images instead of the game, to tell "no frames reach SteamVR" from camera problems.
- Sheets: inputs.turn_x, natives.SET_GAMEPLAY_CAM_RELATIVE_HEADING, settings.eye_height.

## 0.2.6 (2026-10-06)
- User log 0.2.5: SteamVR receives every frame with an image (~60 fps, 0 errors). User: camera broken, arms point down, image not right.
- Camera: game camera now gets yaw+pitch only (roll 0; GTA roll sign unverified, a wrong sign tilted the world). The exact
  roll-free pose the camera was set to is passed to the layer (history buffer, GTA5VR.ini latency=1 frames by default),
  so the compositor reprojects head tilt + latency correctly instead of using a pose the image was not rendered with.
- Mono: backbuffer copied once per frame (was twice -> fps drop to ~40).
- Arms: SET_PED_CAN_ARM_IK(ped,1) every frame, blend in 100 ms, reach 0.7 m; first 6 IK targets logged; F11 toggles arm IK.

## 0.2.7 (2026-10-06)
- User: headset shows the SteamVR home + "loading" while the game runs; 0.2.5 log shows ~60 fps submitted with layers, 0 errors.
  Arms move but the hands (wrists) point down.
- F10 now clears the eye images to solid colours with ClearRenderTargetView (magenta left / green right): no shader, no state swap,
  to tell "SteamVR ignores our layer" from "the blit writes nothing".
- Wrists: SET_IK_TARGET only places the hand; no known native sets hand rotation -> not fixable without engine hooks (told the user).

## Docs 2026-10-06 (0.2.7)
- README.md, README_RU.txt, GTA5VR-README.txt rewritten: honest status table (works / broken / untested), full feature list,
  keys F8-F11, controller map, GTA5VR.ini settings (runtime, stereo, latency), F10 check, uninstall. setup.bat end text: F10/F11 hint.
