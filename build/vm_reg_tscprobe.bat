@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r92: enable the TSC offset probe. Must run BEFORE sc start so the next
rem DriverEntry reads TscProbe=1 and stamps every guest VMCB with the probe
rem offset (TSC_PROBE_OFFSET = 0x4000000000).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v TscProbe /t REG_DWORD /d 1 /f > %GUEST_DESKTOP%\reg_tscprobe.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
