@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RunFastUploadBenchmark.ps1" -Port COM7
set "code=%errorlevel%"
echo.
pause
exit /b %code%
