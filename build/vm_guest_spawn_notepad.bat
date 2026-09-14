@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - spawn notepad.exe detached (r41: vmrun -noWait runs the
rem exe directly - `start` inside a redirected cmd rejects its stdin).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\notepad.exe"
exit /b %ERRORLEVEL%
