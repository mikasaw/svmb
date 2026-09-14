@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r89: Parameters\ReadVmEnable knob (1 = capability on [default],
rem 0 = refuse CR3_READVM with STATUS_NOT_SUPPORTED). Takes effect at the
rem next DriverEntry - restart the service to apply. Args: %1 = 0|1
rem v2: reg add output captured via steps log (silent-fail lesson)
if "%1"=="" goto :usage
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v ReadVmEnable /t REG_DWORD /d %1 /f >> %GUEST_DESKTOP%\svmb_steps.log 2>&1 < NUL"
exit /b %ERRORLEVEL%
:usage
echo usage: vm_reg_readvm.bat ^<0^|1^>
exit /b 2
