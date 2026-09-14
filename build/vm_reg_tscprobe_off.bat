@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r92: disable the TSC offset probe after the measurement (a permanent
rem non-zero offset would skew guest time sources).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v TscProbe /t REG_DWORD /d 0 /f > %GUEST_DESKTOP%\reg_tscprobe.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
