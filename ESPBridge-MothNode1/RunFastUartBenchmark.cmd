@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0RunFastUartBenchmark.ps1" -Port COM7
