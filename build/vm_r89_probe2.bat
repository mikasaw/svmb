@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r89 kill E2E probe with CAPTURED output. Args: %1=pid %2=base(hex)
if "%1"=="" goto :usage
if "%2"=="" goto :usage
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (%GUEST_TEST_DIR%\svmbctl.exe cr3 probe %1 %2 & %GUEST_TEST_DIR%\svmbctl.exe cr3 stats) >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
:usage
echo usage: vm_r89_probe2.bat ^<pid^> ^<base-hex^>
exit /b 2
