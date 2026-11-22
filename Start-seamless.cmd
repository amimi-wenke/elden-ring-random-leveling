@echo off
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" -Seamless %*
set "ONE_LIFE_EXIT=%ERRORLEVEL%"
if not "%ONE_LIFE_EXIT%"=="0" pause
exit /b %ONE_LIFE_EXIT%
