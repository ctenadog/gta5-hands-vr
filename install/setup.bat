@echo off
chcp 65001 >nul
title GTA V Hands VR setup
set "GTA5VR_HERE=%~dp0"
set "GTA5VR_SELF=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=[IO.File]::ReadAllText($env:GTA5VR_SELF,[Text.Encoding]::UTF8); $i=$s.IndexOf('#PS'+'START'); Invoke-Expression $s.Substring($i)"
echo.
pause
exit /b
#PSSTART
# GTA V Hands VR - установка одним скриптом.
# 1) находит GTA V, 2) скачивает мод (GTA5VR-*.zip) и Script Hook V, 3) копирует всё в папку игры.
# Если сайт не даёт скачать автоматически - открывает страницу в браузере и ждёт файл в "Загрузках".
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0 Safari/537.36'
$RepoApi = 'https://api.github.com/repos/ctenadog/gta5-hands-vr/releases/tags/latest'
$RelPage = 'https://github.com/ctenadog/gta5-hands-vr/releases/tag/latest'
$ShvPage = 'https://www.dev-c.com/gtav/scripthookv/'

function Say($t) { Write-Host $t }
function Step($t) { Write-Host ''; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Ok($p) { $p -and (Test-Path (Join-Path $p 'GTA5.exe')) }

# ---------- 1. Папка игры ----------
Step '1/4 Ищу GTA V'
$cands = @()
if ($env:GTA5VR_GAME) { $cands += $env:GTA5VR_GAME }
foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\Rockstar Games\Grand Theft Auto V','HKLM:\SOFTWARE\WOW6432Node\Rockstar Games\GTAV') {
  try { $v = Get-ItemProperty $k -ErrorAction Stop
        foreach ($n in 'InstallFolder','InstallFolderSteam','InstallFolderEpic') { if ($v.$n) { $cands += $v.$n; $cands += (Split-Path $v.$n -Parent) } } } catch {}
}
try { $s = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
      $cands += "$s\steamapps\common\Grand Theft Auto V"
      $lf = "$s\steamapps\libraryfolders.vdf"
      if (Test-Path $lf) { Select-String -Path $lf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $cands += ($_.Matches[0].Groups[1].Value -replace '\\\\','\') + '\steamapps\common\Grand Theft Auto V' } } } catch {}
foreach ($d in 'C','D','E','F','G') { $cands += "${d}:\SteamLibrary\steamapps\common\Grand Theft Auto V"; $cands += "${d}:\Program Files\Rockstar Games\Grand Theft Auto V"; $cands += "${d}:\Program Files\Epic Games\GTAV" }
# скрипт запущен из папки внутри игры (например ...\Grand Theft Auto V\gta5-hands-vr-main\install)
$here = $PSScriptRoot; if ($env:GTA5VR_HERE) { $here = $env:GTA5VR_HERE.TrimEnd('\') }
$p = $here
for ($i = 0; $i -lt 4 -and $p; $i++) { $cands += $p; $p = Split-Path $p -Parent }

$game = $cands | Where-Object { Ok $_ } | Select-Object -First 1
while (-not $game) {
  Say 'GTA V (GTA5.exe) не найдена автоматически.'
  $g = (Read-Host 'Вставьте путь к папке GTA V (где лежит GTA5.exe)').Trim('"')
  if (Ok $g) { $game = $g } else { Say "В папке '$g' нет GTA5.exe. (GTA V Enhanced не поддерживается.)" }
}
$game = (Resolve-Path $game).Path
$gameVer = (Get-Item (Join-Path $game 'GTA5.exe')).VersionInfo.FileVersion
Say "Папка GTA V: $game"
Say "Версия GTA5.exe: $gameVer"

$work = Join-Path $env:TEMP 'GTA5VR-setup'
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
try { $dl = (New-Object -ComObject Shell.Application).NameSpace('shell:Downloads').Self.Path } catch { $dl = Join-Path $env:USERPROFILE 'Downloads' }

function Wait-Download($pattern, $what, $url) {
  $since = Get-Date
  # уже скачанный ранее файл тоже подходит
  $old = Get-ChildItem $dl -Filter $pattern -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($old) {
    $a = Read-Host "Найден $($old.Name) в Загрузках. Использовать его? (Enter = да, n = скачать заново)"
    if ($a -ne 'n') { return $old.FullName }
  }
  Say "Не получилось скачать $what автоматически. Открываю страницу в браузере:"
  Say "  $url"
  Say "Скачайте файл $pattern в папку '$dl' - скрипт сам его подхватит (ждёт до 15 минут)."
  Start-Process $url
  $deadline = (Get-Date).AddMinutes(15)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $f = Get-ChildItem $dl -Filter $pattern -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $since } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($f) {
      Start-Sleep -Seconds 2   # дать браузеру дописать файл
      try { $s = [IO.File]::Open($f.FullName,'Open','Read','None'); $s.Close(); Say "Найден: $($f.Name)"; return $f.FullName } catch {}
    }
  }
  throw "Файл $pattern не появился в '$dl'."
}

# ---------- 2. Мод ----------
Step '2/4 Скачиваю мод GTA V Hands VR'
$modZip = $null
try {
  $rel = Invoke-RestMethod -Uri $RepoApi -UserAgent $UA -Headers @{ Accept = 'application/vnd.github+json' }
  $asset = $rel.assets | Where-Object { $_.name -like 'GTA5VR-*.zip' } | Select-Object -First 1
  if ($asset) {
    $modZip = Join-Path $work $asset.name
    Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $modZip -UserAgent $UA -UseBasicParsing
    Say "Скачан $($asset.name)"
  }
} catch { Say '(репозиторий приватный или GitHub недоступен - нужен вход в браузере)' }
if (-not $modZip) { $modZip = Wait-Download 'GTA5VR-*.zip' 'мод' $RelPage }
$modDir = Join-Path $work 'mod'
Expand-Archive $modZip $modDir -Force
$asi = Get-ChildItem $modDir -Recurse -Filter 'GTA5VR.asi' | Select-Object -First 1
if (-not $asi) { throw "В архиве $modZip нет GTA5VR.asi. Нужен GTA5VR-x.x.x.zip из Assets, а не Source code." }
$modDir = $asi.DirectoryName

# ---------- 3. Script Hook V ----------
Step '3/4 Script Hook V'
$haveShv = (Test-Path (Join-Path $game 'ScriptHookV.dll')) -and (Test-Path (Join-Path $game 'dinput8.dll'))
$a = ''
if ($haveShv) { $a = Read-Host 'Script Hook V уже установлен. Обновить? (y = да, Enter = оставить)' }
if (-not $haveShv -or $a -eq 'y') {
  $shvZip = $null
  try {
    $html = (Invoke-WebRequest -Uri $ShvPage -UserAgent $UA -UseBasicParsing).Content
    $m = [regex]::Match($html, '(?i)(?:href=")?((?:https?://www\.dev-c\.com)?/?files/ScriptHookV_[0-9a-z\.]+\.zip)')
    if ($m.Success) {
      $u = $m.Groups[1].Value; if ($u -notmatch '^https?://') { $u = 'https://www.dev-c.com/' + $u.TrimStart('/') }
      $shvZip = Join-Path $work (Split-Path $u -Leaf)
      Invoke-WebRequest -Uri $u -OutFile $shvZip -UserAgent $UA -Headers @{ Referer = $ShvPage } -UseBasicParsing
      if ((Get-Item $shvZip).Length -lt 50000) { $shvZip = $null } else { Say "Скачан $(Split-Path $u -Leaf)" }
    }
  } catch { $shvZip = $null }
  if (-not $shvZip) { $shvZip = Wait-Download 'ScriptHookV*.zip' 'Script Hook V (кнопка Download внизу страницы)' $ShvPage }
  $shvDir = Join-Path $work 'shv'
  Expand-Archive $shvZip $shvDir -Force
  $shvDll = Get-ChildItem $shvDir -Recurse -Filter 'ScriptHookV.dll' | Select-Object -First 1
  $din = Get-ChildItem $shvDir -Recurse -Filter 'dinput8.dll' | Select-Object -First 1
  if (-not $shvDll -or -not $din) { throw "В архиве $shvZip нет ScriptHookV.dll / dinput8.dll." }
  $shvVer = [regex]::Match((Split-Path $shvZip -Leaf), '\d+\.\d+\.\d+\.\d+').Value
  if ($shvVer -and $gameVer -and ($gameVer -replace '\s','') -notlike "*$shvVer*") {
    Say "ВНИМАНИЕ: Script Hook V $shvVer, а игра $gameVer. Если версии не совпадают, мод не загрузится - дождитесь обновления Script Hook V."
  }
  Copy-Item $shvDll.FullName $game -Force
  Copy-Item $din.FullName $game -Force
  Say 'Скопированы ScriptHookV.dll и dinput8.dll.'
} else { Say 'Оставляю установленный Script Hook V.' }

# ---------- 4. Установка мода ----------
Step '4/4 Копирую мод в папку игры'
Copy-Item (Join-Path $modDir 'GTA5VR.asi') $game -Force
Copy-Item (Join-Path $modDir 'openxr_loader.dll') $game -Force
$doc = Join-Path $game 'GTA5VR'; New-Item -ItemType Directory -Force $doc | Out-Null
foreach ($r in 'GTA5VR-README.txt','README_RU.txt') { if (Test-Path (Join-Path $modDir $r)) { Copy-Item (Join-Path $modDir $r) $doc -Force } }
if (Test-Path (Join-Path $modDir 'licenses')) { Copy-Item (Join-Path $modDir 'licenses') $doc -Recurse -Force }
Say 'Скопированы GTA5VR.asi и openxr_loader.dll.'

# лишняя папка с исходниками внутри игры
$src = Get-ChildItem $game -Directory -Filter 'gta5-hands-vr*' -ErrorAction SilentlyContinue
if ($src -and -not ($here -and $here.StartsWith($src[0].FullName))) {
  $a = Read-Host "В папке игры лежит $($src[0].Name) (исходный код, игре не нужен). Удалить? (y/Enter)"
  if ($a -eq 'y') { Remove-Item $src[0].FullName -Recurse -Force; Say 'Удалено.' }
} elseif ($src) { Say "Папку $($src[0].Name) в папке игры можно удалить после установки - игре она не нужна." }

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
Write-Host ''
Write-Host 'ГОТОВО.' -ForegroundColor Green
Say '1. Запустите Pico Connect (или SteamVR) и подключите шлем.'
Say '2. Запустите GTA V, выберите СЮЖЕТНЫЙ режим.'
Say '3. Нажмите F8 - включить/выключить VR.'
Say "Лог: $game\GTA5VR.log - пришлите его, если что-то не работает."
Say 'Только сюжетный режим. Не используйте моды в GTA Online.'
