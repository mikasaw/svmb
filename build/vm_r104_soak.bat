@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r104 light soak (r104 probe incl. the open+read+write loop - exercises the CID resolver path per iteration): armed sysaudit + probe churn; every iteration kills
rem its notepad AFTER the probe, so many (caller, handle) pairs go stale -
rem the resolver's fail/backoff/dead paths and the retry caps get real
rem exercise. Args: %1 = iterations (default 18, ~8min).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
if "%~1"=="" (set N=18) else (set N=%~1)
set V="%GUEST_TEST_DIR%\svmbctl.exe"
set OUT=%GUEST_DESKTOP%\r104_soak.txt
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c del /q %OUT% 2>nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest_rpm_r104.ps1" "%GUEST_DESKTOP%\rpm_r104.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% mod attach sysaudit >> %OUT% 2>&1 < NUL"
for /l %%i in (1,1,%N%) do (
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c echo === iter %%i === >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_DESKTOP%\rpm_r104.ps1 >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c taskkill /im notepad.exe /f >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% sysaudit >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% behavior >> %OUT% 2>&1 < NUL"
)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% info >> %OUT% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%OUT%" "%WINDBG_TEST%\logs\r104_soak.txt"
exit /b %ERRORLEVEL%
