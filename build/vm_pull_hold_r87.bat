@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87 readvm E2E: pull the holdpage target's pid/base file
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\hold_r87.txt" "%WINDBG_TEST%\logs\hold_r87.txt"
exit /b %ERRORLEVEL%
