@echo off
chcp 65001 >nul
title GTA V Hands VR - удаление
set "GTA5VR_HERE=%~dp0"
set "GTA5VR_SELF=%~f0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$s=[IO.File]::ReadAllText($env:GTA5VR_SELF,[Text.Encoding]::UTF8); $i=$s.IndexOf('#PS'+'START'); Invoke-Expression $s.Substring($i)"
echo.
pause
exit /b
#PSSTART
# GTA V Hands VR - полное удаление мода из папки игры.
# Удаляет файлы мода, настройки и логи. Script Hook V и dinput8.dll - только если вы ответите "y".
$ErrorActionPreference = 'Continue'
function Say($t) { Write-Host $t }
function Step($t) { Write-Host ''; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Ok($p) { $p -and (Test-Path (Join-Path $p 'GTA5.exe')) }

Step '1/3 Ищу GTA V'
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
$here = $env:GTA5VR_HERE; if ($here) { $here = $here.TrimEnd('\') }
$p = $here
for ($i = 0; $i -lt 4 -and $p; $i++) { $cands += $p; $p = Split-Path $p -Parent }
$game = $cands | Where-Object { Ok $_ } | Select-Object -First 1
while (-not $game) {
  Say 'GTA V (GTA5.exe) не найдена автоматически.'
  $g = (Read-Host 'Вставьте путь к папке GTA V (где лежит GTA5.exe)').Trim('"')
  if (Ok $g) { $game = $g } else { Say "В папке '$g' нет GTA5.exe." }
}
$game = (Resolve-Path $game).Path
Say "Папка GTA V: $game"

# игра и помощник должны быть закрыты, иначе файлы заняты
$procs = 'GTA5','GTAVLauncher','PlayGTAV','GTA5VR_Host','xrtest'
while (Get-Process -Name $procs -ErrorAction SilentlyContinue) {
  $a = Read-Host 'GTA V или GTA5VR_Host ещё запущены. Закройте игру и нажмите Enter (или введите k, чтобы завершить их принудительно)'
  if ($a -eq 'k') { Get-Process -Name $procs -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2 }
}

Step '2/3 Удаляю мод'
$script:n = 0
function Remove-Old($p) {
  if (Test-Path -LiteralPath $p) {
    Remove-Item -LiteralPath $p -Recurse -Force -ErrorAction SilentlyContinue
    if (-not (Test-Path -LiteralPath $p)) { Say "  удалено: $p"; $script:n++ } else { Say "  НЕ удалось удалить (файл занят?): $p" }
  }
}
foreach ($d in $game, (Join-Path $game 'scripts'), (Join-Path $game 'plugins'), (Join-Path $game 'asi')) {
  foreach ($f in 'GTA5VR.asi','GTA5VR.asi.off','GTA5VR.asi.bak') { Remove-Old (Join-Path $d $f) }
}
foreach ($f in 'openxr_loader.dll','GTA5VR_Host.exe','xrtest.exe','xrtest.log','GTA5VR.ini',
               'GTA5VR.log','GTA5VR.old.log','GTA5VR_Host.log','GTA5VR_Host.old.log',
               'GTA5VR-README.txt','README_RU.txt','install.bat','install.ps1','steamxr_win64.json') { Remove-Old (Join-Path $game $f) }
Remove-Old (Join-Path $game 'GTA5VR')
$lic = Join-Path $game 'licenses'
if ((Test-Path (Join-Path $lic 'OpenXR-Loader-LICENSE.txt')) -and ((Get-ChildItem $lic -Force | Measure-Object).Count -eq 1)) { Remove-Old $lic }
if ($here -ne $game) { Remove-Old (Join-Path $game 'setup.bat') }
foreach ($src in (Get-ChildItem $game -Directory -Filter 'gta5-hands-vr*' -ErrorAction SilentlyContinue)) {
  if ($here -and $here.StartsWith($src.FullName)) { Say "  $($src.Name): скрипт запущен из неё - удалите её вручную" } else { Remove-Old $src.FullName }
}
Remove-Item (Join-Path $env:TEMP 'GTA5VR-setup') -Recurse -Force -ErrorAction SilentlyContinue
if ($script:n -eq 0) { Say '  файлов мода не найдено' }

Step '3/3 Script Hook V и ASI-загрузчик'
$shv = Join-Path $game 'ScriptHookV.dll'; $din = Join-Path $game 'dinput8.dll'
$other = @(Get-ChildItem $game -Filter '*.asi' -ErrorAction SilentlyContinue) + @(Get-ChildItem (Join-Path $game 'scripts') -Filter '*.asi' -ErrorAction SilentlyContinue)
if ((Test-Path $shv) -or (Test-Path $din)) {
  if ($other.Count -gt 0) { Say "  Внимание: найдены другие моды (.asi): $(($other | ForEach-Object Name) -join ', '). Им нужны Script Hook V и dinput8.dll." }
  else { Say '  Других .asi-модов не найдено.' }
  $a = Read-Host '  Удалить также Script Hook V (ScriptHookV.dll, dinput8.dll)? y = да, Enter = оставить'
  if ($a -eq 'y') { Remove-Old $shv; Remove-Old $din; Remove-Old (Join-Path $game 'ScriptHookV.log'); Remove-Old (Join-Path $game 'asiloader.log') }
  else { Say '  Script Hook V оставлен.' }
} else { Say '  Script Hook V не установлен.' }

Write-Host ''
Write-Host 'ГОТОВО: мод GTA V Hands VR удалён.' -ForegroundColor Green
Say 'Если игра всё равно не запускается: Steam -> GTA V -> Свойства -> Установленные файлы -> Проверить целостность файлов.'
