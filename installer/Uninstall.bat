@echo off
rem Removes S2FM Mod Pack from Half-Life: Alyx and puts back what was there. Close the game first.
rem Add -Purge to also remove presets, settings and crash reports.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0files\s2fm_mods.ps1" -Action uninstall %*
echo.
pause
