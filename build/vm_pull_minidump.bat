@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (dir /od C:\Windows\Minidump\ & powershell -NoProfile -Command \"Copy-Item (Get-ChildItem C:\Windows\Minidump\*.dmp | Sort-Object LastWriteTime | Select-Object -Last 1).FullName %GUEST_DESKTOP%\latest.dmp\") > %GUEST_DESKTOP%\fresh2.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
