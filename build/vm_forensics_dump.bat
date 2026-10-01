@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem round-14 passive forensics: minidump dir + bugcheck/WHEA event log entries
rem (evidence for the guest-requested hard reset ~2.5min after ctl start)
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% runProgramInGuest "%VM_VMX%" "C:\Windows\System32\cmd.exe" "/c (dir /a C:\Windows\Minidump & dir /a C:\Windows\MEMORY.DMP & wevtutil qe System /q:\"*[System[(EventID=1001 or EventID=41)]]\" /c:5 /rd:true /f:text & wevtutil qe System /q:\"*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger']]]\" /c:5 /rd:true /f:text) > %GUEST_DESKTOP%\r14_forensics.txt 2>&1 < NUL"
exit /b %ERRORLEVEL%
