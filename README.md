# GTA V Hands VR (DEV 0.2.0, untested)

Head-tracked first-person VR for **GTA V Legacy story mode**: the character's arms follow your VR controllers,
the right controller aims, the trigger fires. Script Hook V + OpenXR (Pico via Pico Connect, or SteamVR).

> Work in progress: this build has never been run in the game or a headset yet. Expect bugs; send `GTA5VR.log`.

## Установка (RU) — одним скриптом
1. Откройте **Releases → latest**: https://github.com/ctenadog/gta5-hands-vr/releases/tag/latest
2. В разделе **Assets** скачайте **`setup.bat`** и запустите его двойным щелчком
   (если Windows предупредит — «Подробнее» → «Выполнить в любом случае»).
3. Скрипт сам найдёт GTA V, скачает мод и Script Hook V и скопирует в папку игры
   `GTA5VR.asi`, `openxr_loader.dll`, `ScriptHookV.dll`, `dinput8.dll`.
   Если сайт Script Hook V не даст скачать автоматически — откроется браузер, нажмите **Download**,
   скрипт сам заберёт файл из «Загрузок».
4. Запустите Pico Connect (сделайте его OpenXR-рантаймом), затем GTA V в сюжетном режиме, нажмите **F8**.

Не скачивайте «Code → Download ZIP» / «Source code» — там исходники, а не готовый мод.
Ручная установка: распакуйте `GTA5VR-0.2.0.zip` в любую папку и запустите `install.bat`;
Script Hook V (`ScriptHookV.dll` + `dinput8.dll` из папки bin) положите рядом с `GTA5.exe` сами.
Подробно: `README_RU.txt`.

Только сюжетный режим. Не используйте моды в GTA Online — бан.

## Install (EN)
Easiest: download **`setup.bat`** from Releases → latest (Assets) and run it. It finds GTA V, downloads the mod
and Script Hook V, and copies everything next to `GTA5.exe` (if dev-c.com blocks the download, a browser opens —
click Download and the script picks the file up from Downloads).
Manual: put Script Hook V (`ScriptHookV.dll` + `dinput8.dll`) next to `GTA5.exe`, unpack the release zip, run `install.bat`.
Start Pico Connect / SteamVR first, then GTA V story mode, press F8.

## Build
`sh ci/build.sh` on Linux with mingw-w64 (GitHub Actions does this on every push and publishes the `latest` release).
Game data lives in `sheets/*.json`; `tools/gen.py` generates `src/generated/sheets.h`; `tools/preflight.py` checks the sheets.

## Credits / licenses
Mod code: MIT. OpenXR Loader: Khronos Group, Apache-2.0 (downloaded at build time).
Script Hook V by Alexander Blade is required and not included.
