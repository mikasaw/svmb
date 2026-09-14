@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (echo ---1074--- & wevtutil qe System /c:20 /rd:true /f:text /q:*[System[EventID=1074]] & echo ---6008--- & wevtutil qe System /c:10 /rd:true /f:text /q:*[System[EventID=6008]] & echo ---41--- & wevtutil qe System /c:10 /rd:true /f:text /q:*[System[EventID=41]]) > %GUEST_DESKTOP%\fresh4.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
