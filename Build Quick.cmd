@echo off
setlocal
rem Quick build: compile only, no regression gate, no packaging.
rem Output: debug\Aperture.Release.exe (run it directly; the installed
rem debug\Aperture.exe / release\Aperture.exe apps are not touched).
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
if errorlevel 1 pause
