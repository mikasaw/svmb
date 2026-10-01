@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat" >nul
signtool sign /fd sha256 /n svmb-test /s My "%REPO%\x64\Release\svmb.sys"
rem verify output stays for diagnostics but is NOT fatal: the host has no
rem svmb-test root in its trust stores, so /pa always reports the
rem untrusted-root error here - the guest (Root+TrustedPeople install +
rem testsigning) is where the chain actually validates
signtool verify /pa "%REPO%\x64\Release\svmb.sys"
exit /b 0
