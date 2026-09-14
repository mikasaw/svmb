@echo off
rem r70 guest-side launcher: start must live in a FILE (vmrun /c layer eats
rem the empty-title quotes - spawn_and_watch precedent).
start "" /min powershell -NoProfile -ExecutionPolicy Bypass -File %USERPROFILE%\Desktop\thread_storm.ps1
exit /b 0
