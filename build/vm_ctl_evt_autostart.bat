@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem svmb test flow - FRESH service-lifecycle event probe (who started svmb).
rem Uses wevtutil text dump + findstr (no powershell quoting traps in vmrun).
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (wevtutil qe System /c:600 /rd:true /f:text > %GUEST_DESKTOP%\evt_full.txt 2>&1 & findstr /i /c:svmb %GUEST_DESKTOP%\evt_full.txt > %GUEST_DESKTOP%\fresh.txt 2>&1 & findstr /c:TimeCreated %GUEST_DESKTOP%\evt_full.txt >> %GUEST_DESKTOP%\fresh.txt 2>&1 & wmic os get lastbootuptime >> %GUEST_DESKTOP%\fresh.txt 2>&1) < NUL"
exit /b %ERRORLEVEL%
