@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (dir C:\Windows\Minidump & wevtutil qe System /q:"*[System[(EventID=1001 or EventID=6008)]]" /c:3 /rd:true /f:text) > %GUEST_DESKTOP%\r76_dump.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r76_dump.txt" "%REPO%\tests\r76_dump_out.txt"
exit /b %ERRORLEVEL%
