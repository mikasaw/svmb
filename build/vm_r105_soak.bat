@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r105 soak: BehaveResponse=1 stays armed - every iteration's probe gets
rem alerted and suspended, then resumed (no suspended-process accumulation;
rem the 16-slot table fills and the resume belt / S1 decline paths get
rem real exercise). Args: %1 = iterations (default 12, ~7min).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
if "%~1"=="" (set N=12) else (set N=%~1)
set V="%GUEST_TEST_DIR%\svmbctl.exe"
set OUT=%GUEST_DESKTOP%\r105_soak.txt
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c del /q %OUT% 2>nul < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest_rpm_r104.ps1" "%GUEST_DESKTOP%\rpm_r104.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest_rpm_r105_hatch.ps1" "%GUEST_DESKTOP%\r105_hatch.ps1"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% mod attach sysaudit >> %OUT% 2>&1 < NUL"
for /l %%i in (1,1,%N%) do (
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c echo === iter %%i === >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c powershell -NoProfile -ExecutionPolicy Bypass -File %GUEST_DESKTOP%\r105_hatch.ps1 >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c ping -n 5 127.0.0.1 >nul < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% cr3 resume all >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c taskkill /im notepad.exe /f >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c taskkill /im powershell.exe /f >> %OUT% 2>&1 < NUL"
  "%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% behavior >> %OUT% 2>&1 < NUL"
)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% cr3 suspended >> %OUT% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %V% info >> %OUT% 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%OUT%" "%WINDBG_TEST%\logs\r105_soak.txt"
exit /b %ERRORLEVEL%
