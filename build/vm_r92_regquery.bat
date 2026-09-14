@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r87: local VM env (credentials/paths) - never commit
rem r92: read back the TscProbe knob value (captured, per the r66 truth law).
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg query HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v TscProbe >> %GUEST_DESKTOP%\regquery_r92.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\regquery_r92.txt" "%WINDBG_TEST%\logs\regquery_r92.txt"
exit /b %ERRORLEVEL%
