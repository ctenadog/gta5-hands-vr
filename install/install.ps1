# GTA V Hands VR installer. Finds GTA V Legacy (GTA5.exe), removes old mod files, copies GTA5VR.asi + openxr_loader.dll next to it,
# checks Script Hook V and the ASI loader. Never touches GTA Online files.
$ErrorActionPreference = 'Stop'
$here = if ($env:GTA5VR_HERE) { $env:GTA5VR_HERE.TrimEnd('\') } else { Split-Path -Parent $MyInvocation.MyCommand.Path }
function Ok($p) { $p -and (Test-Path (Join-Path $p 'GTA5.exe')) }

foreach ($f in 'GTA5VR.asi','openxr_loader.dll') {
  if (-not (Test-Path (Join-Path $here $f))) {
    Write-Host "ОШИБКА: рядом с install.bat нет файла $f ($here)."
    Write-Host 'Скорее всего, вы скачали ИСХОДНЫЙ КОД (Code -> Download ZIP) - в нём нет готового мода.'
    Write-Host 'Скачайте GTA5VR-0.2.0.zip: https://github.com/ctenadog/gta5-hands-vr/releases/tag/latest (раздел Assets),'
    Write-Host 'распакуйте его в любую папку (например, Загрузки) и запустите install.bat ОТТУДА.'
    exit 1
  }
}

$cands = @()
foreach ($k in 'HKLM:\SOFTWARE\WOW6432Node\Rockstar Games\Grand Theft Auto V','HKLM:\SOFTWARE\WOW6432Node\Rockstar Games\GTAV') {
  try { $v = Get-ItemProperty $k -ErrorAction Stop
        foreach ($n in 'InstallFolder','InstallFolderSteam','InstallFolderEpic') { if ($v.$n) { $cands += $v.$n; $cands += (Split-Path $v.$n -Parent) } } } catch {}
}
try { $s = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath
      $cands += "$s\steamapps\common\Grand Theft Auto V"
      $lf = "$s\steamapps\libraryfolders.vdf"
      if (Test-Path $lf) { Select-String -Path $lf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $cands += ($_.Matches[0].Groups[1].Value -replace '\\\\','\') + '\steamapps\common\Grand Theft Auto V' } } } catch {}
$cands += 'C:\Program Files\Rockstar Games\Grand Theft Auto V','C:\Program Files\Epic Games\GTAV','D:\SteamLibrary\steamapps\common\Grand Theft Auto V','E:\SteamLibrary\steamapps\common\Grand Theft Auto V'

$game = $cands | Where-Object { Ok $_ } | Select-Object -First 1
if (-not $game) {
  Write-Host 'GTA V (GTA5.exe) не найдена автоматически.'
  $game = Read-Host 'Вставьте путь к папке GTA V (где лежит GTA5.exe)'
  $game = $game.Trim('"')
  if (-not (Ok $game)) { Write-Host "В папке $game нет GTA5.exe. GTA V Enhanced (GTA5_Enhanced.exe) не поддерживается."; exit 1 }
}
Write-Host "Папка GTA V: $game"

while (Get-Process -Name 'GTA5','GTAVLauncher','PlayGTAV' -ErrorAction SilentlyContinue) { Read-Host 'GTA V запущена - закройте игру и нажмите Enter' | Out-Null }
# старые файлы мода
foreach ($d in $game, (Join-Path $game 'scripts'), (Join-Path $game 'plugins'), (Join-Path $game 'asi')) {
  $o = Join-Path $d 'GTA5VR.asi'; if ((Test-Path $o) -and ($d -ne $here)) { Remove-Item $o -Force; Write-Host "Удалён старый $o" }
}
if (Test-Path (Join-Path $game 'GTA5VR')) { Remove-Item (Join-Path $game 'GTA5VR') -Recurse -Force; Write-Host 'Удалена старая папка GTA5VR' }
if (Test-Path (Join-Path $game 'GTA5VR.log')) { Move-Item (Join-Path $game 'GTA5VR.log') (Join-Path $game 'GTA5VR.old.log') -Force }
if ((Resolve-Path $here).Path -ne (Resolve-Path $game).Path) {
Copy-Item (Join-Path $here 'GTA5VR.asi') $game -Force
Copy-Item (Join-Path $here 'openxr_loader.dll') $game -Force
}
$doc = Join-Path $game 'GTA5VR'; New-Item -ItemType Directory -Force $doc | Out-Null
foreach ($r in 'GTA5VR-README.txt','README_RU.txt') { if (Test-Path (Join-Path $here $r)) { Copy-Item (Join-Path $here $r) $doc -Force } }
if (Test-Path (Join-Path $here 'licenses')) { Copy-Item (Join-Path $here 'licenses') $doc -Recurse -Force }
Write-Host 'Скопированы GTA5VR.asi и openxr_loader.dll.'

$miss = $false
if (-not (Test-Path (Join-Path $game 'ScriptHookV.dll'))) {
  Write-Host ''; Write-Host 'НЕ ХВАТАЕТ: ScriptHookV.dll. Скачайте Script Hook V (откроется сайт), из папки bin скопируйте ScriptHookV.dll и dinput8.dll в папку GTA V.'
  Start-Process 'https://www.dev-c.com/gtav/scripthookv/'; $miss = $true
}
$asiLoader = @('dinput8.dll','version.dll','winmm.dll','dsound.dll','xinput1_3.dll') | Where-Object { Test-Path (Join-Path $game $_) }
if (-not $asiLoader) { Write-Host 'НЕ ХВАТАЕТ: dinput8.dll (загрузчик ASI). Он лежит в архиве Script Hook V, в папке bin.'; $miss = $true }

Write-Host ''
if ($miss) { Write-Host 'Скопируйте недостающие файлы, затем запустите Pico Connect, потом GTA V (сюжетный режим) и нажмите F8.' }
else { Write-Host 'Готово. Запустите Pico Connect (среда OpenXR), потом GTA V (сюжетный режим) и нажмите F8. Лог: GTA5VR.log' }
Write-Host 'Только сюжетный режим. Не используйте моды в GTA Online.'
Write-Host 'Инструкция: README_RU.txt'
