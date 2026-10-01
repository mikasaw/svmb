@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r91 plan C- E2E: pull the victim's output AFTER resume
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\susp_r91.txt" "%WINDBG_TEST%\logs\susp_r91_done.txt"
exit /b %ERRORLEVEL%
