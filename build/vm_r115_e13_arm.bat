@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r115 E13 leg: attach sysaudit FIRST (storm bursts hold the IOCTL gate
rem ~8s/cycle and would stall later execs), then push + launch the storm
rem loop (-noWait, watchdog pattern). Assumes the service is RUNNING on
rem the E12 build (vm_r111_dbg_restore already applied this boot).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe mod attach sysaudit >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe sysaudit >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromHostToGuest "%VM_VMX%" "%REPO%\build\guest\storm_loop_r115.bat" "%GUEST_TEST_DIR%\storm_loop_r115.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" -noWait "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\storm_loop_r115.bat < NUL"
ping -n 15 127.0.0.1 >nul
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\storm_loop.log" "%REPO%\tests\r115_storm_loop_out.txt"
exit /b %ERRORLEVEL%
