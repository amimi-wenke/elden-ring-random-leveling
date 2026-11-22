@echo off
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch.ps1" %*
set "RANDOM_LEVELING_EXIT=%ERRORLEVEL%"
if not "%RANDOM_LEVELING_EXIT%"=="0" pause
exit /b %RANDOM_LEVELING_EXIT%
