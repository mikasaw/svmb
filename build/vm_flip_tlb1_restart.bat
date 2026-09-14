@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v TlbMode /t REG_DWORD /d 1 /f & sc stop svmb & ping -n 26 127.0.0.1 >nul & sc start svmb) > %GUEST_DESKTOP%\fresh.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
