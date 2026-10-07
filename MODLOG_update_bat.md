# update.bat

- New install/update.bat (also in the zip and as a release asset): finds the game, reads the installed version from
  GTA5VR.asi, the latest version from the GitHub release, and if a newer one exists downloads GTA5VR-<ver>.zip,
  backs up the old files to GTA5VR_backup in the game folder and copies GTA5VR.asi, GTA5VR_Host.exe,
  openxr_loader.dll. Script Hook V and settings are not touched. If the game runs it asks to close it (k = force).
  After an update it replaces itself with the update.bat from the new release.
- setup.bat end text and README mention update.bat; uninstall.bat also removes GTA5VR_backup.
