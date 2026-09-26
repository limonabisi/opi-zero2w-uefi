@echo off
rem Drag the Windows ARM64 ISO onto this file.
if "%~1"=="" (echo Drag the Windows ARM64 ISO file onto this .bat file. & pause & exit /b)
net session >nul 2>&1 || (powershell -NoProfile -Command "Start-Process -Verb RunAs -FilePath '%~f0' -ArgumentList '\"%~1\"'" & exit /b)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-windows-image.ps1" -Iso "%~1"
pause
