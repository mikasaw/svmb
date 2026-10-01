@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r92: pull the fresh4.txt ring dump (vm_ctl_log_fresh's target).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\fresh4.txt" "%WINDBG_TEST%\logs\fresh4_r92.txt"
exit /b %ERRORLEVEL%
