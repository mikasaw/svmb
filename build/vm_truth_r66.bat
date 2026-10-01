@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r66 ground truth: in-group sc query + qc + driverquery, proper NUL guard,
rem independent output filename. Tests whether bare sc query 1060 is a condrv
rem artifact.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc query svmb & sc qc svmb & driverquery | findstr /i svmb & echo ===LASTBOOT=== & wmic os get lastbootuptime) > %GUEST_DESKTOP%\r66_truth.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
