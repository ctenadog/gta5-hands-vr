GTA V Hands VR 0.2.2 (DEV, untested) - story mode only
=====================================================
Head-tracked first-person VR for GTA V Legacy: your arms follow the controllers, right controller aims, trigger fires.

EASIEST: run setup.bat - it finds GTA V, downloads Script Hook V and installs everything.

REQUIRED (setup.bat installs it, or do it manually):
  Script Hook V by Alexander Blade - https://www.dev-c.com/gtav/scripthookv/
  Copy ScriptHookV.dll and dinput8.dll (from its bin folder) into the GTA V folder (next to GTA5.exe).
  Script Hook V must match your game version; after a game update wait for an SHV update.

Headset: Pico 4 / Pico Neo via Pico Connect (or Streaming Assistant) or SteamVR.
The mod uses SteamVR by default (even if another OpenXR runtime is active). GTA5VR.ini: runtime=system to use the Windows active runtime.
Start SteamVR with the headset connected before pressing F8.

Controls: F8 toggle VR (look straight ahead), F9 recenter. Left stick move, right trigger fire, left trigger aim, A jump, B enter/exit vehicle,
X reload, left stick click sprint. In cars: head look + buttons (two-hand steering planned).

Story mode only. Script Hook V refuses GTA Online; the mod also switches off in any online session.
Using mods in GTA Online can get your account banned - do not.

Russian guide: README_RU.txt
Log: GTA5VR.log in the game folder.
Credits: OpenXR Loader (Khronos, Apache-2.0). Script Hook V (Alexander Blade) is not included.
