@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r102 E2E (default-threshold leg): attach sysaudit -> probe -> drain ->
rem behavior snapshot. NOTE the r102 sampling-law finding: the 200x tight
rem 64B loop is mostly NOT sensed (cached TLB translations slide - the
rem r100 law), so with the default 128/window threshold alerts=0 here is
rem the CORRECT outcome. The alert edge is demonstrated by
rem vm_r102_alert.bat (BehaveRdThs=2 knob + service restart).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
set V="%GUEST_TEST_DIR%\svmbctl.exe"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c del /q %GUEST_DESKTOP%\r102_e2e.txt 2>nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest_rpm_r101.ps1" "%GUEST_DESKTOP%\rpm_r101.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% mod attach sysaudit >> %GUEST_DESKTOP%\r102_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% sysaudit >> %GUEST_DESKTOP%\r102_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_DESKTOP%\rpm_r101.ps1 >> %GUEST_DESKTOP%\r102_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% sysaudit >> %GUEST_DESKTOP%\r102_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% behavior >> %GUEST_DESKTOP%\r102_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% log >> %GUEST_DESKTOP%\r102_e2e.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r102_e2e.txt" "%WINDBG_TEST%\logs\r102_e2e.txt"
exit /b %ERRORLEVEL%
