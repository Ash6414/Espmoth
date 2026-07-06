@echo off
setlocal

if "%~1"=="" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RunBridgeStatusTest.ps1" -Port COM9 -MonitorSeconds 180
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RunBridgeStatusTest.ps1" %*
)

exit /b %ERRORLEVEL%
