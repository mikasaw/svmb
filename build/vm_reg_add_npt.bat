@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - step: enable NPT via Parameters\NptEnable=1. reg add
rem auto-creates the Parameters key. Must run BEFORE sc start so the
rem next DriverEntry loads with NptEnable=1.
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v NptEnable /t REG_DWORD /d 1 /f > %GUEST_DESKTOP%\reg_add.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
