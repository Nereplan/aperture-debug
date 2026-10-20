@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%
"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe" @"C:\Users\Alperen\AppData\Local\Codex\tools\scratch\aperture-flat-sift-20261020\oracle.rsp"
exit /b %errorlevel%
