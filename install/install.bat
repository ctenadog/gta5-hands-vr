@echo off
chcp 65001 >nul
rem GTA V Hands VR installer / ustanovshik: copies the mod into your GTA V Legacy folder.
set "GTA5VR_HERE=%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "[Console]::OutputEncoding=[Text.Encoding]::UTF8; & ([scriptblock]::Create((Get-Content -Raw -Encoding UTF8 -LiteralPath '%~dp0install.ps1')))"
pause
