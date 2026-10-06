# 0.4.0 - OpenXR moved out of the game process

Evidence: xrtest.exe (plain process, same OpenXR calls) shows colours in the Pico; the mod inside GTA5.exe
submitted ~60 fps with images, 0 errors, state FOCUSED, and SteamVR still showed "Waiting..." (vrcompositor: "Timed out").
Renaming xrtest to GTA5.exe still showed colours, so the exe name is not the cause - the game process is.

Change:
- New GTA5VR_Host.exe (tools/host/host.cpp): owns the whole OpenXR session (instance, swapchains, actions, frame loop),
  its own D3D11 device, log GTA5VR_Host.log. Started by the mod on F8 with Steam ids removed from its environment and,
  by default, with explorer.exe as parent (GTA5VR.ini host_under_explorer=1). Exits on F8 off or when the game closes.
- GTA5VR.asi no longer loads openxr_loader.dll. It hands frames to the helper and reads poses through shared memory
  (src/shared.h). Frame transport, negotiated and confirmed by the helper: 1 shared texture with NT handle,
  2 legacy shared handle, 3 CPU copy via shared memory (max 1440 px high, works on any driver).
- F10 test colour is drawn by the helper, so it works even before any game frame arrives.
- own_device / hide_steam_id settings removed (obsolete).
Untested on Windows.
