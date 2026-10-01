@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - pull the fresh dbgdr output back to the host.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\dbgdr_out.txt" "%WINDBG_TEST%\logs\dbgdr_out.txt"
exit /b %ERRORLEVEL%
