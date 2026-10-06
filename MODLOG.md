# GTA V Hands VR — MODLOG
Goal: head-tracked stereo first-person + arms following OpenXR controllers + controller aiming. Story mode only.

## 0.1.0 (superseded)
Standalone .asi without Script Hook V: needed per-build native hashes + byte patterns from GTA5.exe 1.0.3889.0. Never ran.

## 0.2.0 2026-10-06: Script Hook V
- Natives go through ScriptHookV.dll exports (nativeInit/nativePush64/nativeCall), resolved with GetProcAddress;
  SHV maps nativedb hashes to the running build, so no per-build data is needed.
- Script thread = scriptRegister + scriptWait(0) loop; frames = presentCallbackRegister.
- The .asi LoadLibrary's ScriptHookV.dll itself and retries for 30 s.
- Export names written from memory of the SHV SDK: if one is wrong, GTA5VR.log says "export missing: <name>".
- Preflight: 0 unfilled cells. Still untested in game/headset (stereo AER, IK, camera roll sign, input paths).
- Melty draft: GTA V Hands VR, modId f602c4c6-5677-4e51-92d8-5acfc524323e (no release yet). SHV = manual step for players.
