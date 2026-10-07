# 0.6.0 - cleanup, simpler to use

Mod
- Only two keys left: F8 = VR on/off, F9 = look straight ahead. Debug keys F10 (test colour), F11 (arm IK), F12 (hand rotation) removed - arms and hand rotation are always on.
- On-screen hints (visible on the monitor and in the headset), Russian when the game runs in Russian, English otherwise:
  first hint after loading, "VR is starting", "waiting for the headset" (repeats every 10 s), "VR on", "VR off",
  "recentered", error switch-off, GTA Online switch-off.
- GTA5VR.ini is no longer created: every setting has a built-in default. Keys still work if someone adds them by hand.
- Comfort vignette code removed completely.

Installer
- setup.bat: 4 clear steps, no questions unless needed (Script Hook V is kept if installed, game is closed with "k").
  Works both standalone (downloads the mod) and from an unpacked zip (uses the files next to it). Errors are shown in red with a plain message.
  Old GTA5VR.ini is renamed to GTA5VR.ini.old, old logs, xrtest, readme copies and leftovers are deleted.
- Zip now contains only: GTA5VR.asi, GTA5VR_Host.exe, openxr_loader.dll, setup.bat, uninstall.bat, README_RU.txt, licenses/.
  Removed: xrtest.exe, install.bat, install.ps1, GTA5VR-README.txt. Zip renamed GTA5VR-0.6.0.zip.
- Release assets: zip, setup.bat, uninstall.bat (xrtest.exe removed).
- README.md and README_RU.txt rewritten short.
