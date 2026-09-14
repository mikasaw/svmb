@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
set V="%VM_VMX%"
set R=C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe
for %%i in (0 1 2 3 4 5) do %R% -T ws -gu %VM_USER% -gp %VM_PASS% CopyFileFromGuestToHost %V% %GUEST_DESKTOP%\ep%%i.txt %WINDBG_TEST%\logs\ep%%i.txt
exit /b %ERRORLEVEL%
