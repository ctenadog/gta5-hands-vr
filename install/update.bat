@echo off
chcp 65001 >nul
title GTA V Hands VR - обновление
set "GTA5VR_HERE=%~dp0"
set "GTA5VR_SELF=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=[IO.File]::ReadAllText($env:GTA5VR_SELF,[Text.Encoding]::UTF8); $i=$s.IndexOf('#PS'+'START'); Invoke-Expression $s.Substring($i)"
echo.
pause
exit /b
#PSSTART
# GTA V Hands VR - обновление. Проверяет GitHub, и если вышла новая версия - скачивает и ставит её.
# Script Hook V и настройки не трогает.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0 Safari/537.36'
$Base = 'https://github.com/ctenadog/gta5-hands-vr/releases'
$ModFiles = 'GTA5VR.asi','GTA5VR_Host.exe','openxr_loader.dll'
function Say($t) { Write-Host "  $t" }
function Ok($p) { $p -and (Test-Path (Join-Path $p 'GTA5.exe')) }

Write-Host ''
Write-Host '  GTA V Hands VR - обновление' -ForegroundColor Green
try {
  # ---------- игра ----------
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
  foreach ($d in 'C','D','E','F','G','H') { $cands += "${d}:\SteamLibrary\steamapps\common\Grand Theft Auto V"; $cands += "${d}:\Program Files\Rockstar Games\Grand Theft Auto V"; $cands += "${d}:\Program Files\Epic Games\GTAV" }
  $here = $env:GTA5VR_HERE; if ($here) { $here = $here.TrimEnd('\'); $cands += $here; $cands += (Split-Path $here -Parent) }
  $game = $cands | Where-Object { Ok $_ } | Select-Object -First 1
  while (-not $game) {
    $g = (Read-Host '  Не нашёл игру. Вставьте путь к папке GTA V (где GTA5.exe) и нажмите Enter').Trim('"')
    if (Ok $g) { $game = $g }
  }
  $game = (Resolve-Path $game).Path
  Say "Игра: $game"

  # ---------- версии ----------
  $asiPath = Join-Path $game 'GTA5VR.asi'
  if (-not (Test-Path $asiPath)) { throw 'Мод не установлен. Сначала запустите setup.bat.' }
  $inst = [regex]::Match([Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($asiPath)), 'GTA5VR (\d+\.\d+\.\d+) loading').Groups[1].Value
  if (-not $inst) { $inst = '0.0.0' }
  $url = $null; $latest = $null
  try {
    $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/ctenadog/gta5-hands-vr/releases/tags/latest' -UserAgent $UA -TimeoutSec 20
    $a = $rel.assets | Where-Object { $_.name -match '^GTA5VR-(\d+\.\d+\.\d+)\.zip$' } | Select-Object -First 1
    if ($a) { $url = $a.browser_download_url; $latest = [regex]::Match($a.name, '\d+\.\d+\.\d+').Value }
  } catch {}
  if (-not $url) {   # запасной путь, если API GitHub временно не отвечает
    $html = (Invoke-WebRequest -Uri "$Base/expanded_assets/latest" -UserAgent $UA -UseBasicParsing -TimeoutSec 20).Content
    $m = [regex]::Match($html, 'GTA5VR-(\d+\.\d+\.\d+)\.zip')
    if ($m.Success) { $latest = $m.Groups[1].Value; $url = "$Base/download/latest/GTA5VR-$latest.zip" }
  }
  if (-not $url) { throw 'Не получилось узнать последнюю версию на GitHub. Проверьте интернет и попробуйте позже.' }
  Say "Установлена версия: $inst"
  Say "Последняя на GitHub: $latest"
  if ([version]$inst -ge [version]$latest) {
    Write-Host ''
    Write-Host '  У вас уже последняя версия - обновлять нечего.' -ForegroundColor Green
    return
  }

  # ---------- игра должна быть закрыта ----------
  $procs = 'GTA5','GTAVLauncher','PlayGTAV','GTA5VR_Host'
  while (Get-Process -Name $procs -ErrorAction SilentlyContinue) {
    $k = Read-Host '  GTA V запущена. Закройте игру и нажмите Enter (или введите k и Enter - закрою сам)'
    if ($k -eq 'k') { Get-Process -Name $procs -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2 }
  }

  # ---------- скачать и поставить ----------
  Say "Скачиваю $latest..."
  $work = Join-Path $env:TEMP 'GTA5VR-update'
  Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
  New-Item -ItemType Directory -Force $work | Out-Null
  $zip = Join-Path $work "GTA5VR-$latest.zip"
  Invoke-WebRequest -Uri $url -OutFile $zip -UserAgent $UA -UseBasicParsing -TimeoutSec 300
  Expand-Archive $zip (Join-Path $work 'x') -Force
  $src = (Get-ChildItem (Join-Path $work 'x') -Recurse -Filter 'GTA5VR.asi' | Select-Object -First 1).DirectoryName
  if (-not $src) { throw 'Скачанный архив повреждён - запустите update.bat ещё раз.' }
  foreach ($f in $ModFiles) { if (-not (Test-Path (Join-Path $src $f))) { throw "В архиве нет $f - запустите update.bat ещё раз." } }
  # старые копии на случай отката
  $bak = Join-Path $game 'GTA5VR_backup'
  New-Item -ItemType Directory -Force $bak | Out-Null
  foreach ($f in $ModFiles) { $p = Join-Path $game $f; if (Test-Path $p) { Copy-Item $p $bak -Force } }
  foreach ($f in $ModFiles) { Copy-Item (Join-Path $src $f) $game -Force }
  # новый установщик/удалятор/обновлятор - рядом с этим файлом, если он лежит не в папке игры
  if ($here -and $here -ne $game) { foreach ($b in 'setup.bat','uninstall.bat') { $p = Join-Path $src $b; if (Test-Path $p) { Copy-Item $p $here -Force -ErrorAction SilentlyContinue } } }
  $newUpd = Join-Path $src 'update.bat'
  if ((Test-Path $newUpd) -and $env:GTA5VR_SELF) { Copy-Item $newUpd (Join-Path $work 'update-new.bat') -Force }
  foreach ($l in 'GTA5VR.log','GTA5VR_Host.log') { $p = Join-Path $game $l; if (Test-Path $p) { Move-Item $p (Join-Path $game ($l -replace '\.log$','.old.log')) -Force } }
  Remove-Item $zip -Force -ErrorAction SilentlyContinue
  Write-Host ''
  Write-Host "  ОБНОВЛЕНО: $inst -> $latest" -ForegroundColor Green
  Say 'Запускайте игру как обычно. Старые файлы сохранены в папке GTA5VR_backup в папке игры.'
  Say "Что нового: $Base/tag/latest"
  # заменить сам update.bat на новую версию после выхода (нельзя переписать файл, пока он выполняется)
  $nu = Join-Path $work 'update-new.bat'
  if (Test-Path $nu) { Start-Process -WindowStyle Hidden cmd.exe -ArgumentList '/c', "ping -n 3 127.0.0.1 >nul & copy /y `"$nu`" `"$env:GTA5VR_SELF`" >nul" }
} catch {
  Write-Host ''
  Write-Host "  ОШИБКА: $($_.Exception.Message)" -ForegroundColor Red
  Say 'Мод не обновлён. Можно просто запустить setup.bat - он поставит последнюю версию.'
}
