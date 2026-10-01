@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - shutdown/restart event hunt (1074 with comments).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (wevtutil qe System /c:400 /rd:true /f:text > %GUEST_DESKTOP%\evt_full.txt 2>&1 & findstr /i /c:1074 %GUEST_DESKTOP%\evt_full.txt > %GUEST_DESKTOP%\fresh.txt 2>&1 & findstr /i /c:maintenance %GUEST_DESKTOP%\evt_full.txt >> %GUEST_DESKTOP%\fresh.txt 2>&1 & findstr /c:Date: %GUEST_DESKTOP%\evt_full.txt >> %GUEST_DESKTOP%\fresh.txt 2>&1) < NUL"
exit /b %ERRORLEVEL%
