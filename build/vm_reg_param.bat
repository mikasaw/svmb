@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - set an arbitrary Parameters\<name> DWORD (r58 A/B knobs).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v %1 /t REG_DWORD /d %2 /f > %GUEST_DESKTOP%\reg_add.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
