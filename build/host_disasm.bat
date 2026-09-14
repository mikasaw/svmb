@echo off
if exist "%~dp0vmenv.bat" call "%~dp0vmenv.bat"
rem host-side forensics: disassemble svmb.sys for RIP resolution
"C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\dumpbin.exe" /DISASM %REPO%\x64\Debug\svmb.sys > %WINDBG_TEST%\logs\svmb_disasm.txt 2>&1
exit /b %ERRORLEVEL%
