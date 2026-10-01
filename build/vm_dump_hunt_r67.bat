@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r67: hunt for crash evidence after the 05:47:57 spontaneous reboot.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (dir C:\Windows\Minidump & echo ===EVT=== & wevtutil qe System /c:8 /rd:true /f:text) > %GUEST_DESKTOP%\r67_dump.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
