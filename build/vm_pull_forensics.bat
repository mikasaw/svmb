@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem round-14: pull forensics file to host
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r14_forensics.txt" "%HOST_HOME%\r14_forensics_out.txt"
exit /b %ERRORLEVEL%
