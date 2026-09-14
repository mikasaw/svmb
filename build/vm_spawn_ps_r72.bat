@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (start "" /min powershell -NoProfile -Command while(1){} & echo SPAWN_OK) > %GUEST_DESKTOP%\r72_spawn.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
