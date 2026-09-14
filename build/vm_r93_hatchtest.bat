@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r93 hatch variant test: A=r72 powershell, B=cmd /c victim, C=direct cmd
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (start "" /min powershell -NoProfile -Command echo A_OK & echo A_DONE) > %GUEST_DESKTOP%\hatch_A.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (start "" /min cmd /c %GUEST_DESKTOP%\susp_victim.cmd & echo B_DONE) > %GUEST_DESKTOP%\hatch_B.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (start "" /min %GUEST_DESKTOP%\susp_victim.cmd & echo C_DONE) > %GUEST_DESKTOP%\hatch_C.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo ===A=== & type %GUEST_DESKTOP%\hatch_A.txt & echo ===B=== & type %GUEST_DESKTOP%\hatch_B.txt & echo ===C=== & type %GUEST_DESKTOP%\hatch_C.txt) > %GUEST_DESKTOP%\r93_hatch_diag.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r93_hatch_diag.txt" "%WINDBG_TEST%\logs\r93_hatch_diag.txt"
exit /b %ERRORLEVEL%
