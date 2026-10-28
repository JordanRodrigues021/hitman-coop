@echo off
title HITMAN Co-op installer
if not exist "%~dp0installer.ps1" (
    echo The installer files are missing. Right-click the zip, choose "Extract All...",
    echo then double-click Install.bat in the extracted folder.
    echo.
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer.ps1" %*
echo.
pause
