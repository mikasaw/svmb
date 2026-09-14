@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r66 Leg B prep: kill r65-era leftover watchdog cmd + spinner powershell,
rem rotate the overnight melt_watch.log to preserve the 10h dataset.
rem No inner quotes allowed (vmrun quoting layer trap).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (taskkill /F /PID 3956 & taskkill /F /PID 6356 & ping -n 2 127.0.0.1 >nul & move /y %GUEST_DESKTOP%\melt_watch.log %GUEST_DESKTOP%\melt_watch_overnight_r65.log & dir /b %GUEST_DESKTOP%\melt_watch*.log) > %GUEST_DESKTOP%\r66_legB_prep.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
