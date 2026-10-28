@echo off
title HITMAN Co-op uninstaller
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer.ps1" -Uninstall -NoFirewall %*
echo.
pause
