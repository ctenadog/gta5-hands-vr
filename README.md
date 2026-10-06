# GTA V Hands VR (DEV 0.2.0, untested)

Head-tracked first-person VR for **GTA V Legacy story mode**: the character's arms follow your VR controllers,
the right controller aims, the trigger fires. Script Hook V + OpenXR (Pico via Pico Connect, or SteamVR).

> Work in progress: this build has never been run in the game or a headset yet. Expect bugs; send `GTA5VR.log`.

## Установка (RU)
1. Скачайте Script Hook V: https://www.dev-c.com/gtav/scripthookv/ — скопируйте `ScriptHookV.dll` и `dinput8.dll` в папку с `GTA5.exe`.
2. Скачайте `GTA5VR-0.2.0.zip` из **Releases → latest** (справа на странице репозитория), распакуйте.
3. Запустите `install.bat` — он сам найдёт GTA V и скопирует `GTA5VR.asi` и `openxr_loader.dll`.
4. Запустите Pico Connect (сделайте его OpenXR-рантаймом), затем GTA V в сюжетном режиме, нажмите **F8**.

Только сюжетный режим. Не используйте моды в GTA Online — бан.

## Install (EN)
Script Hook V (`ScriptHookV.dll` + `dinput8.dll`) next to `GTA5.exe`, then unpack the release zip and run `install.bat`.
Start Pico Connect / SteamVR first, then GTA V story mode, press F8.

## Build
`sh ci/build.sh` on Linux with mingw-w64 (GitHub Actions does this on every push and publishes the `latest` release).
Game data lives in `sheets/*.json`; `tools/gen.py` generates `src/generated/sheets.h`; `tools/preflight.py` checks the sheets.

## Credits / licenses
Mod code: MIT. OpenXR Loader: Khronos Group, Apache-2.0 (downloaded at build time).
Script Hook V by Alexander Blade is required and not included.
