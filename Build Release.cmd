@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build_and_release.ps1"
if errorlevel 1 pause
