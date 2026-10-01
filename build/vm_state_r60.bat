@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (reg query HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters & systeminfo | findstr /C:"System Boot Time" & sc query svmb | findstr STATE & tasklist | findstr /I "svmbctl powershell") > %GUEST_DESKTOP%\r60state.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
