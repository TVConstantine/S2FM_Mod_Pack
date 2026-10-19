@echo off
rem Installs S2FM Mod Pack into Half-Life: Alyx. Close the game first.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\s2fm_mods.ps1" -Action install %*
echo.
pause
