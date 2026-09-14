@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87/r88 readvm E2E part 3: stats -> cross-process readvm -> stats, three
rem sequential guest calls (the parenthesized-&-group shape silently exits 1
rem under vmrun with substituted args - do not "optimize" back into one line)
rem Args: %1=pid %2=va(hex) [%3=extra flag word, e.g. faultin]
if "%1"=="" goto :usage
if "%2"=="" goto :usage
set EXTRA=%3
if not "%EXTRA%"=="" set EXTRA= %EXTRA%
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 stats >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
if errorlevel 1 goto :guestfail
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 readvm %1 %2 32%EXTRA% >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
if errorlevel 1 goto :guestfail
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 stats >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
if errorlevel 1 goto :guestfail
exit /b 0
:usage
echo usage: vm_r87_read.bat ^<pid^> ^<va-hex^> [faultin]
exit /b 2
:guestfail
echo guest leg failed (rc nonzero) - check svmb_steps.log
exit /b 1
