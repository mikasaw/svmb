@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - spawn notepad without waiting (-noWait), then kill it.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest -noWait "%VM_VMX%" "C:\Windows\notepad.exe"
exit /b %ERRORLEVEL%
