GTA V Hands VR 0.2.7 (DEV, early test build) - story mode only
==============================================================
Goal: head-tracked first-person VR for GTA V Legacy, arms follow the controllers, right controller aims, trigger fires.

STATUS (from real-PC logs: Pico + SteamVR)
  Works:   loads via Script Hook V, F8 on/off, SteamVR connection, frames submitted to SteamVR (~60 fps, no errors),
           arms move to controller positions, auto-off on hangs.
  Broken:  headset shows the SteamVR home with "loading" instead of the game (under investigation, use F10);
           wrists point down (Script Hook V can set hand position, not hand rotation).
  Untested: head camera, controller aiming/shooting, snap turn, stereo=1. No two-hand steering yet.

FEATURES
  OpenXR, SteamVR by default (runtime=system = Windows active runtime); VR starts only on F8.
  Mono (default) or alternate-eye stereo (stereo=1). Head rotation + up to 40 cm room-scale offset.
  Arms follow controllers (F11 toggle). Right trigger shoots where the right controller points, left trigger aims.
  Left stick walk (forward = where you look), stick click sprint, right stick 30-degree snap turn.
  A jump, B enter/exit vehicle, X reload. In cars: right trigger gas, left trigger brake, left stick steer.
  Switches off in GTA Online, on hangs (>3 s frame), camera released in pause menu/loading screens. GTA5VR.log with frame stats.
  setup.bat: finds GTA V, removes old mod files, downloads the mod and Script Hook V, installs everything.

KEYS: F8 VR on/off (look straight ahead), F9 recenter, F10 test colour (magenta left / green right eye), F11 arms on/off.
GTA5VR.ini: runtime=steamvr|system, stereo=0|1, latency=1 (try 2 or 0 if the image swims).

INSTALL: run setup.bat from https://github.com/ctenadog/gta5-hands-vr/releases/tag/latest (Assets).
REQUIRED: Script Hook V by Alexander Blade (setup.bat downloads it) - https://www.dev-c.com/gtav/scripthookv/
  ScriptHookV.dll + dinput8.dll next to GTA5.exe; must match your game version.
RUN: connect the headset to SteamVR, start GTA V story mode, press F8.

Story mode only. Using mods in GTA Online can get your account banned - do not.
Russian guide: README_RU.txt. Log: GTA5VR.log in the game folder.
Credits: OpenXR Loader (Khronos, Apache-2.0). Script Hook V (Alexander Blade) is not included.
