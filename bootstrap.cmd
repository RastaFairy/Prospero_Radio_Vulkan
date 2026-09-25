@echo off
setlocal
rem Windows bootstrap: run this from PowerShell or cmd, NOT through `wsl.exe`.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
exit /b %ERRORLEVEL%
