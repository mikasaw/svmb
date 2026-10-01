@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo ===HATCH=== & type %GUEST_DESKTOP%\hatch_r93.txt & echo. & echo ===VICTIM_OUT=== & type %GUEST_DESKTOP%\susp_r93.txt & echo. & echo ===DIR=== & dir /b %GUEST_DESKTOP%\susp_victim.cmd) > %GUEST_DESKTOP%\r93_diag.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r93_diag.txt" "%WINDBG_TEST%\logs\r93_diag.txt"
exit /b %ERRORLEVEL%
