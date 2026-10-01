@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\bcd_hv.txt" "%HOST_HOME%\bcd_hv_out.txt"
exit /b %ERRORLEVEL%
