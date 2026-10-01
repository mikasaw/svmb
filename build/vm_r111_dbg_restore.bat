@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r111: restore the Debug r110 build for the default-threshold armed
rem soak baseline (knobs are already at defaults from the r109 epilogue).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c sc stop svmb > %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
ping -n 5 127.0.0.1 >nul
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Debug\svmb.sys" "%GUEST_TEST_DIR%\svmb.sys"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Debug\svmbctl.exe" "%GUEST_TEST_DIR%\svmbctl.exe"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc start svmb & echo === SOAK LEG %DATE% %TIME% ===) >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
