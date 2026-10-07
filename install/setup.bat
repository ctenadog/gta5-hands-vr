@echo off
chcp 65001 >nul
title GTA V Hands VR - установка
set "GTA5VR_HERE=%~dp0"
set "GTA5VR_SELF=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=[IO.File]::ReadAllText($env:GTA5VR_SELF,[Text.Encoding]::UTF8); $i=$s.IndexOf('#PS'+'START'); Invoke-Expression $s.Substring($i)"
echo.
pause
exit /b
#PSSTART
# GTA V Hands VR - установка. Просто запустите этот файл: он сам найдёт игру, скачает и поставит всё нужное.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$UA = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/129.0 Safari/537.36'
$RepoApi = 'https://api.github.com/repos/ctenadog/gta5-hands-vr/releases/tags/latest'
$RelPage = 'https://github.com/ctenadog/gta5-hands-vr/releases/tag/latest'
$ShvPage = 'https://www.dev-c.com/gtav/scripthookv/'
$ModFiles = 'GTA5VR.asi','GTA5VR_Host.exe','openxr_loader.dll'

function Say($t) { Write-Host "  $t" }
function Step($n, $t) { Write-Host ''; Write-Host "[$n/4] $t" -ForegroundColor Cyan }
function Ok($p) { $p -and (Test-Path (Join-Path $p 'GTA5.exe')) }

Write-Host ''
Write-Host '  GTA V Hands VR - установка' -ForegroundColor Green
Write-Host '  Ничего нажимать не нужно, пока скрипт сам не попросит.'

try {
# ---------- 1. Игра ----------
Step 1 'Ищу GTA V'
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
$here = $PSScriptRoot; if ($env:GTA5VR_HERE) { $here = $env:GTA5VR_HERE.TrimEnd('\') }
$p = $here
for ($i = 0; $i -lt 4 -and $p; $i++) { $cands += $p; $p = Split-Path $p -Parent }
$game = $cands | Where-Object { Ok $_ } | Select-Object -First 1
while (-not $game) {
  Say 'Не нашёл игру автоматически.'
  $g = (Read-Host '  Вставьте путь к папке GTA V (там, где лежит GTA5.exe) и нажмите Enter').Trim('"')
  if (Ok $g) { $game = $g } else { Say "В папке '$g' нет GTA5.exe. (GTA V Enhanced не поддерживается - нужна обычная GTA V / Legacy.)" }
}
$game = (Resolve-Path $game).Path
Say "Нашёл: $game"
$procs = 'GTA5','GTAVLauncher','PlayGTAV','GTA5VR_Host','xrtest'
if (Get-Process -Name $procs -ErrorAction SilentlyContinue) {
  Say 'Игра сейчас запущена - её нужно закрыть.'
  while (Get-Process -Name $procs -ErrorAction SilentlyContinue) {
    $a = Read-Host '  Закройте GTA V и нажмите Enter (или введите k и Enter - закрою сам)'
    if ($a -eq 'k') { Get-Process -Name $procs -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2 }
  }
}

$work = Join-Path $env:TEMP 'GTA5VR-setup'
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $work | Out-Null
try { $dl = (New-Object -ComObject Shell.Application).NameSpace('shell:Downloads').Self.Path } catch { $dl = Join-Path $env:USERPROFILE 'Downloads' }

function Wait-Download($pattern, $what, $url) {
  Say "Не получилось скачать $what автоматически - открываю страницу в браузере."
  Say "Скачайте там файл ($pattern). Скрипт сам найдёт его в папке Загрузки (ждёт 15 минут)."
  $since = Get-Date
  Start-Process $url
  $deadline = (Get-Date).AddMinutes(15)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
    $f = Get-ChildItem $dl -Filter $pattern -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -gt $since } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($f) {
      Start-Sleep -Seconds 2
      try { $s = [IO.File]::Open($f.FullName,'Open','Read','None'); $s.Close(); Say "Получен: $($f.Name)"; return $f.FullName } catch {}
    }
  }
  throw "Файл $pattern так и не появился в папке Загрузки."
}

# ---------- 2. Мод ----------
Step 2 'Скачиваю мод'
$modDir = $null
# setup.bat запущен из распакованного архива мода - берём файлы рядом с ним
if ($here -and @($ModFiles | Where-Object { Test-Path (Join-Path $here $_) }).Count -eq $ModFiles.Count) { $modDir = $here; Say 'Беру файлы мода из этой папки.' }
if (-not $modDir) {
  $modZip = $null
  try {
    $rel = Invoke-RestMethod -Uri $RepoApi -UserAgent $UA -Headers @{ Accept = 'application/vnd.github+json' }
    $asset = $rel.assets | Where-Object { $_.name -like 'GTA5VR*.zip' } | Select-Object -First 1
    if ($asset) {
      $modZip = Join-Path $work $asset.name
      Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $modZip -UserAgent $UA -UseBasicParsing
      Say 'Готово.'
    }
  } catch { $modZip = $null }
  if (-not $modZip) { $modZip = Wait-Download 'GTA5VR*.zip' 'мод' $RelPage }
  $x = Join-Path $work 'mod'
  Expand-Archive $modZip $x -Force
  $asi = Get-ChildItem $x -Recurse -Filter 'GTA5VR.asi' | Select-Object -First 1
  if (-not $asi) { throw 'В скачанном архиве нет мода. Нужен файл GTA5VR*.zip со страницы релиза, а не "Source code".' }
  $modDir = $asi.DirectoryName
}

# ---------- 3. Script Hook V ----------
Step 3 'Script Hook V (нужен для мода)'
if ((Test-Path (Join-Path $game 'ScriptHookV.dll')) -and (Test-Path (Join-Path $game 'dinput8.dll'))) {
  Say 'Уже установлен - оставляю как есть.'
} else {
  $shvZip = $null
  try {
    $html = (Invoke-WebRequest -Uri $ShvPage -UserAgent $UA -UseBasicParsing).Content
    $m = [regex]::Match($html, '(?i)(?:href=")?((?:https?://www\.dev-c\.com)?/?files/ScriptHookV_[0-9a-z\.]+\.zip)')
    if ($m.Success) {
      $u = $m.Groups[1].Value; if ($u -notmatch '^https?://') { $u = 'https://www.dev-c.com/' + $u.TrimStart('/') }
      $shvZip = Join-Path $work (Split-Path $u -Leaf)
      Invoke-WebRequest -Uri $u -OutFile $shvZip -UserAgent $UA -Headers @{ Referer = $ShvPage } -UseBasicParsing
      if ((Get-Item $shvZip).Length -lt 50000) { $shvZip = $null }
    }
  } catch { $shvZip = $null }
  if (-not $shvZip) {
    $old = Get-ChildItem $dl -Filter 'ScriptHookV*.zip' -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($old) { $shvZip = $old.FullName; Say "Беру уже скачанный $($old.Name) из Загрузок." }
    else { $shvZip = Wait-Download 'ScriptHookV*.zip' 'Script Hook V (кнопка Download внизу страницы)' $ShvPage }
  }
  $shvDir = Join-Path $work 'shv'
  Expand-Archive $shvZip $shvDir -Force
  $shvDll = Get-ChildItem $shvDir -Recurse -Filter 'ScriptHookV.dll' | Select-Object -First 1
  $din = Get-ChildItem $shvDir -Recurse -Filter 'dinput8.dll' | Select-Object -First 1
  if (-not $shvDll -or -not $din) { throw "В архиве $shvZip нет ScriptHookV.dll / dinput8.dll." }
  Copy-Item $shvDll.FullName $game -Force
  Copy-Item $din.FullName $game -Force
  Say 'Установлен.'
}

# ---------- 4. Установка ----------
Step 4 'Ставлю мод в папку игры'
function Remove-Old($p) { if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue } }
# старые версии и мусор от прошлых установок
foreach ($d in (Join-Path $game 'scripts'), (Join-Path $game 'plugins'), (Join-Path $game 'asi')) { Remove-Old (Join-Path $d 'GTA5VR.asi') }
foreach ($f in 'xrtest.exe','xrtest.log','GTA5VR.old.log','GTA5VR_Host.old.log','GTA5VR-README.txt','README_RU.txt','install.bat','install.ps1','steamxr_win64.json','GTA5VR') { Remove-Old (Join-Path $game $f) }
$lic = Join-Path $game 'licenses'
if ((Test-Path (Join-Path $lic 'OpenXR-Loader-LICENSE.txt')) -and ((Get-ChildItem $lic -Force | Measure-Object).Count -eq 1)) { Remove-Old $lic }
if ($here -ne $game) { Remove-Old (Join-Path $game 'setup.bat') }
foreach ($src in (Get-ChildItem $game -Directory -Filter 'gta5-hands-vr*' -ErrorAction SilentlyContinue)) { if (-not ($here -and $here.StartsWith($src.FullName))) { Remove-Old $src.FullName } }
# старые настройки: с 0.6.0 мод сам знает правильные значения, файл больше не нужен
$ini = Join-Path $game 'GTA5VR.ini'
if (Test-Path $ini) { Move-Item $ini (Join-Path $game 'GTA5VR.ini.old') -Force }
# новые логи - с чистого листа
foreach ($l in 'GTA5VR.log','GTA5VR_Host.log') { Remove-Old (Join-Path $game $l) }
foreach ($f in $ModFiles) { Copy-Item (Join-Path $modDir $f) $game -Force }
Get-ChildItem $dl -Filter 'GTA5VR*.zip' -ErrorAction SilentlyContinue | ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
Say 'Готово.'

Write-Host ''
Write-Host '  УСТАНОВЛЕНО. Как играть:' -ForegroundColor Green
Write-Host '   1. Подключите шлем и запустите SteamVR.'
Write-Host '   2. Запустите GTA V и загрузите СЮЖЕТНЫЙ режим.'
Write-Host '   3. Нажмите F8 - VR включится (через 5-15 секунд). F8 ещё раз - выключить.'
Write-Host '      F9 - "смотреть прямо" (если вид повернулся не туда).'
Write-Host ''
Write-Host '  Управление: левый стик - ходить (нажать - бег), правый - поворот,'
Write-Host '  правый курок - стрелять / газ, левый курок - целиться / тормоз,'
Write-Host '  A - прыжок, B - сесть в машину / выйти, X - перезарядка, Y - сменить оружие.'
Write-Host ''
Write-Host '  Обновить мод потом: запустите update.bat (сам скачает новую версию, если она вышла).'
Write-Host '  Только сюжетный режим, не GTA Online. Удалить мод: uninstall.bat'
Write-Host "  Если что-то не работает - пришлите файлы GTA5VR.log и GTA5VR_Host.log из папки игры."
} catch {
  Write-Host ''
  Write-Host "  ОШИБКА: $($_.Exception.Message)" -ForegroundColor Red
  Write-Host '  Мод не установлен. Пришлите текст ошибки - помогу.'
}
