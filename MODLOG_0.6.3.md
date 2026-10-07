# 0.6.3 - auto-update

- 20 s after the game starts the mod downloads releases/latest/version.txt (new CI asset). If it is newer than the
  installed version, GTA5VR-<ver>.zip is downloaded to %TEMP%\GTA5VR-update in the background and a hidden
  PowerShell script (parent explorer.exe) waits for GTA5.exe to exit, then replaces GTA5VR.asi, GTA5VR_Host.exe and
  openxr_loader.dll. Result: GTA5VR_update.log in the game folder. In-game message when an update is ready.
- GTA5VR.ini [vr] auto_update=0 disables it.
- Version now lives only in src/version.h; ci/build.sh takes the zip name from it.
- uninstall.bat also removes GTA5VR_update.log and %TEMP%\GTA5VR-update.
- setup.bat updates itself: on start it reads version.txt; if a newer release exists it downloads the new setup.bat,
  replaces itself and continues with it. It shows the installed mod version (read from GTA5VR.asi) and the latest one;
  if they match it says "already the latest version" and asks whether to reinstall. CI writes the version into setup.bat.
