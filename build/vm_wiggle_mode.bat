@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r80 A/B: set registry WiggleMode (0=off 1=back-to-back 2=real wiggle)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add \"HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters\" /v WiggleMode /t REG_DWORD /d %1 /f > %GUEST_DESKTOP%\wigmode.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
