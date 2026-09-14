@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "C:\Windows\Minidump\091126-10687-01.dmp" "%REPO%\tests\bsod_2319_r76.dmp"
"%VMRUN%" -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost "%VM_VMX%" "C:\Windows\Minidump\091126-12546-01.dmp" "%REPO%\tests\bsod_2247_pre.dmp"
exit /b %ERRORLEVEL%
