@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem r113: fetch the two newest guest minidumps for offline cdb analysis
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "C:\Windows\Minidump\092826-11937-01.dmp" "%REPO%\tests\r113_freeze\d1.dmp"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "C:\Windows\Minidump\092826-10625-01.dmp" "%REPO%\tests\r113_freeze\d2.dmp"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "C:\Windows\Minidump\092826-10609-01.dmp" "%REPO%\tests\r113_freeze\d3.dmp"
exit /b %ERRORLEVEL%
