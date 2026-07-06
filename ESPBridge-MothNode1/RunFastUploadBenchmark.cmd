@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RunFastUploadBenchmark.ps1" -Port COM9
set "code=%errorlevel%"
echo.
pause
exit /b %code%
