@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r111: swap in the Release/SVMB_PRODUCTION build for the production
rem regression leg. sc stop + 3s before overwriting the locked binaries.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c sc stop svmb > %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
ping -n 5 127.0.0.1 >nul
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Release\svmb.sys" "%GUEST_TEST_DIR%\svmb.sys"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\x64\Release\svmbctl.exe" "%GUEST_TEST_DIR%\svmbctl.exe"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc start svmb & echo === RELEASE LEG %DATE% %TIME% ===) >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
