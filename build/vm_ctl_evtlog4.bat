@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c wevtutil qe System /q:\"*[System[(EventID=7031) or (EventID=7034) or (EventID=7036) or (EventID=1001) or (EventID=41)]]\" /c:10 /rd:true /f:text > %GUEST_DESKTOP%\evt.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
