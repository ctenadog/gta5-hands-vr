# GTA V Hands VR installer. Finds GTA V Legacy (GTA5.exe), copies GTA5VR.asi + openxr_loader.dll next to it,
# checks Script Hook V and the ASI loader. Never touches GTA Online files.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Ok($p) { $p -and (Test-Path (Join-Path $p 'GTA5.exe')) }

foreach ($f in 'GTA5VR.asi','openxr_loader.dll') {
  if (-not (Test-Path (Join-Path $here $f))) {
    Write-Host "ERROR: $f is not next to install.bat ($here)."
    Write-Host 'You probably downloaded the SOURCE code (Code -> Download ZIP). That zip has no built mod.'
    Write-Host 'Download GTA5VR-0.2.0.zip from Releases -> latest (or Actions -> build -> Artifacts -> GTA5VR),'
    Write-Host 'unpack it to any folder (e.g. Downloads), and run install.bat from THERE.'
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
  Write-Host 'GTA V (GTA5.exe) not found automatically.'
  $game = Read-Host 'Paste the GTA V folder path (the one with GTA5.exe)'
  $game = $game.Trim('"')
  if (-not (Ok $game)) { Write-Host "No GTA5.exe in $game. Note: GTA V Enhanced (GTA5_Enhanced.exe) is not supported."; exit 1 }
}
Write-Host "GTA V folder: $game"

if ((Resolve-Path $here).Path -ne (Resolve-Path $game).Path) {
Copy-Item (Join-Path $here 'GTA5VR.asi') $game -Force
Copy-Item (Join-Path $here 'openxr_loader.dll') $game -Force
}
$doc = Join-Path $game 'GTA5VR'; New-Item -ItemType Directory -Force $doc | Out-Null
if (Test-Path (Join-Path $here 'GTA5VR-README.txt')) { Copy-Item (Join-Path $here 'GTA5VR-README.txt') $doc -Force }
if (Test-Path (Join-Path $here 'licenses')) { Copy-Item (Join-Path $here 'licenses') $doc -Recurse -Force }
Write-Host 'Copied GTA5VR.asi and openxr_loader.dll.'

$miss = $false
if (-not (Test-Path (Join-Path $game 'ScriptHookV.dll'))) {
  Write-Host ''; Write-Host 'MISSING: ScriptHookV.dll. Download Script Hook V, copy ScriptHookV.dll and dinput8.dll into the GTA V folder.'
  Start-Process 'https://www.dev-c.com/gtav/scripthookv/'; $miss = $true
}
$asiLoader = @('dinput8.dll','version.dll','winmm.dll','dsound.dll','xinput1_3.dll') | Where-Object { Test-Path (Join-Path $game $_) }
if (-not $asiLoader) { Write-Host 'MISSING: an ASI loader (dinput8.dll). It comes in the Script Hook V zip.'; $miss = $true }

Write-Host ''
if ($miss) { Write-Host 'After copying the missing files, start Pico Connect, then GTA V story mode, press F8.' }
else { Write-Host 'Done. Start Pico Connect (set as OpenXR runtime), then GTA V story mode, press F8. Log: GTA5VR.log' }
Write-Host 'Story mode only. Do not use mods in GTA Online.'
