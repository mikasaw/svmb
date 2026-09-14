@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
start "" /min powershell -NoProfile -Command "Start-Sleep 7200"
exit /b 0
