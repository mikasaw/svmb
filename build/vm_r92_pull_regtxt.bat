@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r92: pull the reg add capture from the knob write.
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\reg_tscprobe.txt" "%WINDBG_TEST%\logs\reg_tscprobe.txt"
exit /b %ERRORLEVEL%
