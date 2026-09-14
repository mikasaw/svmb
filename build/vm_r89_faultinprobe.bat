@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r89 bisect: single faultin read against the PS holder's PRESENT marker
rem page (exercises FaultInPages + EFLAGS.AC with zero AVs). Args:
rem %1=pid %2=marker-va(hex)
if "%1"=="" goto :usage
if "%2"=="" goto :usage
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c %GUEST_TEST_DIR%\svmbctl.exe cr3 readvm %1 %2 32 faultin >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
:usage
echo usage: vm_r89_faultinprobe.bat ^<pid^> ^<marker-va-hex^>
exit /b 2
