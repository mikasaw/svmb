@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r102 soak: sysaudit stays ARMED (5 sense pages) for the whole run;
rem each iteration hatches a probe (new powershell pid each time - the
rem 16-slot behavior table fills and TableFull must count, never wedge),
rem drains the ring, snapshots behavior. Thresholds stay at the alert-leg
rem knob values (BehaveRdThs=2) so the alert path is exercised too.
rem Args: %1 = iterations (default 60, ~25min).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
if "%~1"=="" (set N=60) else (set N=%~1)
set V="%GUEST_TEST_DIR%\svmbctl.exe"
set OUT=%GUEST_DESKTOP%\r102_soak.txt
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c del /q %OUT% 2>nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest_rpm_r101.ps1" "%GUEST_DESKTOP%\rpm_r101.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% mod attach sysaudit >> %OUT% 2>&1 < NUL"
for /l %%i in (1,1,%N%) do (
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c echo === iter %%i === >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_DESKTOP%\rpm_r101.ps1 >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c taskkill /im notepad.exe /f >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% sysaudit >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% behavior >> %OUT% 2>&1 < NUL"
)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% info >> %OUT% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%OUT%" "%WINDBG_TEST%\logs\r102_soak.txt"
exit /b %ERRORLEVEL%
