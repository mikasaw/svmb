@echo off
rem svmb - test-sign driver + ctl tool with a self-signed code-signing cert.
rem Usage: build\sign.cmd [Debug^|Release]   (default Release)
setlocal
set CERT=svmb-test
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release

rem locate newest WDK signtool
set SDKBIN=
for /f "delims=" %%C in ('dir /b "C:\Program Files (x86)\Windows Kits\10\bin\10.*" 2^>nul') do set SDKBIN=C:\Program Files (x86)\Windows Kits\10\bin\%%C
if "%SDKBIN%"=="" (
    echo [!] WDK not found
    exit /b 1
)
set SIGNTOOL=%SDKBIN%\x64\signtool.exe
if not exist "%SIGNTOOL%" set SIGNTOOL=%SDKBIN%\x86\signtool.exe

rem create the code-signing cert once (CurrentUser\My)
powershell -NoProfile -Command "$c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=%CERT%' }; if (-not $c) { New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=%CERT%' -KeyUsage DigitalSignature -CertStoreLocation Cert:\CurrentUser\My | Out-Null; 'cert created' } else { 'cert exists' }"

echo [+] signing x64\%CONFIG%\svmb.sys
"%SIGNTOOL%" sign /v /fd sha256 /n %CERT% /s My "%~dp0..\x64\%CONFIG%\svmb.sys"
echo [+] signing x64\%CONFIG%\svmbctl.exe
"%SIGNTOOL%" sign /v /fd sha256 /n %CERT% /s My "%~dp0..\x64\%CONFIG%\svmbctl.exe"

echo.
echo Done. Copy svmb.sys + svmbctl.exe into the test VM, then inside the VM:
echo   bcdedit /set testsigning on   (one-time, reboot)
echo   sc create svmb type= kernel binPath= C:\svmb\svmb.sys
echo   sc start svmb
endlocal
