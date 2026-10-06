# GTA V Hands VR (DEV 0.2.7, early test build)

VR mod for **GTA V Legacy story mode**: head-tracked first-person camera, the character's arms follow your VR controllers,
the right controller aims, the trigger fires. Script Hook V + OpenXR (SteamVR by default; Pico via Pico Connect / Streaming Assistant).

> Early test build. Not playable yet: the headset currently shows the SteamVR "loading" screen instead of the game,
> and the wrists point down. Send `GTA5VR.log` with every report.

## Текущее состояние (RU)
| Что | Статус |
|---|---|
| Загрузка через Script Hook V, F8 вкл/выкл | работает |
| Подключение к SteamVR | работает |
| Отправка кадров в SteamVR (~60 к/с, без ошибок) | работает |
| Руки двигаются за контроллерами (положение) | работает |
| Картинка игры в шлеме | **не работает** — «загрузка» SteamVR, ищем причину (F10) |
| Поворот кистей рук | **не работает** — Script Hook V не умеет поворачивать кисть |
| Камера от головы, стрельба контроллером, поворот стиком, stereo=1 | не проверено |
| Руль двумя руками | нет (планируется) |

## Все возможности
**VR**
- OpenXR, по умолчанию SteamVR; `runtime=system` в `GTA5VR.ini` — среда OpenXR из Windows.
- VR включается только по **F8** в игре; SteamVR можно запустить в любой момент до F8.
- Режим моно (по умолчанию) или стерео (`stereo=1`, экспериментально).
- Камера: поворот и наклон головы + перемещение головы в комнате до 40 см.
- **F9** — «вперёд» туда, куда смотрите; правый стик — поворот на 30°.

**Руки и оружие**
- Руки персонажа тянутся к контроллерам (**F11** — вкл/выкл).
- Правый курок — выстрел туда, куда направлен правый контроллер; левый курок — прицеливание.

**Движение**
- Левый стик — ходьба (вперёд = куда смотрите), нажатие стика — бег.
- A — прыжок, B — сесть/выйти из машины, X — перезарядка.
- В машине: правый курок — газ, левый — тормоз, левый стик — руль.

**Безопасность и диагностика**
- Сам выключается в GTA Online, при зависании кадра (>3 с), в меню паузы и на загрузках.
- **F10** — залить шлем цветом (левый глаз розовый, правый зелёный): проверка, доходят ли кадры.
- `GTA5VR.log` — подробный журнал со статистикой кадров.

**Установка**
- `setup.bat` одним запуском: находит GTA V, удаляет старые файлы мода, скачивает мод и Script Hook V, копирует всё.

## Установка (RU)
1. Закройте GTA V.
2. Откройте **Releases → latest**: https://github.com/ctenadog/gta5-hands-vr/releases/tag/latest
3. В разделе **Assets** скачайте **`setup.bat`** и запустите двойным щелчком
   (если Windows предупредит — «Подробнее» → «Выполнить в любом случае»).
4. Script Hook V уже стоит — нажмите Enter, чтобы оставить. В конце должно быть «ГОТОВО».
5. Подключите шлем к SteamVR, запустите GTA V в **сюжетном** режиме, посмотрите прямо и нажмите **F8**.

Не скачивайте «Code → Download ZIP» / «Source code» — там исходники, а не готовый мод.
Ручная установка: распакуйте `GTA5VR-0.2.0.zip` в любую папку (не в папку игры) и запустите `install.bat`;
`ScriptHookV.dll` + `dinput8.dll` (папка bin архива Script Hook V) положите рядом с `GTA5.exe`.

## Клавиши и кнопки
| Клавиша / кнопка | Действие |
|---|---|
| F8 | VR вкл/выкл |
| F9 | сбросить направление «вперёд» |
| F10 | тестовый цвет в шлеме |
| F11 | руки вкл/выкл |
| Левый стик / нажатие | ходьба / бег (в машине — руль) |
| Правый стик влево/вправо | поворот на 30° |
| Правый курок | выстрел (в машине — газ) |
| Левый курок | прицеливание (в машине — тормоз) |
| A / B / X | прыжок / сесть-выйти из машины / перезарядка |

## Настройки `GTA5VR.ini` (папка игры, создаётся при первом F8)
| Параметр | Значения |
|---|---|
| `runtime` | `steamvr` (по умолчанию) или `system` |
| `stereo` | `0` моно (по умолчанию), `1` стерео (экспериментально) |
| `latency` | `1` по умолчанию; если картинка «плывёт» — `2` или `0` |

После изменения нажмите F8 дважды. Подробно: `README_RU.txt`.

Только сюжетный режим. Не используйте моды в GTA Online — бан.

## Install (EN)
Download **`setup.bat`** from Releases → latest (Assets) and run it: it finds GTA V, removes old mod files, downloads the mod
and Script Hook V and copies everything next to `GTA5.exe`. Connect the headset to SteamVR, start GTA V story mode, press **F8**.
Keys: F8 VR on/off, F9 recenter, F10 test colour, F11 arms on/off. Settings in `GTA5VR.ini`: `runtime`, `stereo`, `latency`.
Status: frames reach SteamVR but the headset still shows the SteamVR loading screen; wrists point down; camera/aim untested.

## Build
`sh ci/build.sh` on Linux with mingw-w64 (GitHub Actions does this on every push and publishes the `latest` release).
Game data lives in `sheets/*.json`; `tools/gen.py` generates `src/generated/sheets.h`; `tools/preflight.py` checks the sheets.
Change history: `MODLOG.md`.

## Credits / licenses
Mod code: MIT. OpenXR Loader: Khronos Group, Apache-2.0 (downloaded at build time).
Script Hook V by Alexander Blade is required and not included.
