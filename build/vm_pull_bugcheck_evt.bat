@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - look for crash dumps in the guest.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (dir C:\Windows\Minidump & dir C:\Windows\MEMORY.DMP & wevtutil qe System /c:120 /rd:true /f:text | findstr /i /c:bugcheck /c:0x000000 /c:svmb) > %GUEST_DESKTOP%\fresh4.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
