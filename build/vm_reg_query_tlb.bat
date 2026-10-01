@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg query HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v TlbMode > %GUEST_DESKTOP%\reg_q.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
