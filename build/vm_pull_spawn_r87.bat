@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87 readvm E2E: pull the spawn group's captured output (do NOT run any
rem guest command first - it would overwrite the captured error output)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\hold_err.txt" "%WINDBG_TEST%\logs\hold_err.txt"
exit /b %ERRORLEVEL%
