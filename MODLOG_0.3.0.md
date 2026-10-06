## 0.3.0 (2026-10-06)
- 0.2.9 log: skeleton FOUND at ped+0xFF8 ->+0x18 (object space, 64-byte matrices; L hand idx 53, R hand idx 87, head 119,
  pelvis 1; build 3889, SHV game version id 102). Writes from the script thread are overwritten by the game each frame.
- Background writer thread re-writes the hand rotation continuously (between animation update and render copy);
  writes only while the script thread validated the array in the last 50 ms. Logs writes per 5 s. Proper fix later: hook after anim update.
- SteamVR still shows a "Grand Theft Auto V" tile with "Waiting..." although SteamAppId was removed in-process (log confirms):
  Steam itself tells SteamVR that app 271590 launched -> Desktop Game Theatre waits. User workaround: launch GTA5.exe directly
  (not through Steam / PlayGTAV), or untick Desktop Game Theatre; start SteamVR after the game is in story mode, then F8.
