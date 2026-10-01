@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r80 A/B leg: set WiggleMode=%1, start svc cold, 5x probee2e, pull
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v WiggleMode /t REG_DWORD /d %1 /f < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc start svmb < NUL & ping -n 3 127.0.0.1 >nul & del %GUEST_DESKTOP%\abprobe.log < NUL & %GUEST_TEST_DIR%\abprobe.bat) > %GUEST_DESKTOP%\r80_ab.txt 2>&1 < NUL"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "%GUEST_DESKTOP%\r80_ab.txt" "%REPO%\tests\r80_ab%1_out.txt"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (sc stop svmb < NUL) > NUL 2>&1 < NUL"
exit /b %ERRORLEVEL%
