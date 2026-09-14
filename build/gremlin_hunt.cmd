@echo off
setlocal
set OUT=%USERPROFILE%\Desktop\gremlin_out.txt
(echo ===TASKS_FULL=== 
schtasks /query /fo LIST /v 2>&1
echo ===WMI_CONSUMERS===
wmic /namespace:\root\subscription path __EventConsumer get /format:list 2>&1
wmic /namespace:\root\subscription path __EventFilter get /format:list 2>&1
wmic /namespace:\root\subscription path __FilterToConsumerBinding get /format:list 2>&1
echo ===REG_SVMB_SEARCH===
reg query HKLM\SYSTEM\CurrentControlSet\Services /s /f svmb 2>&1
reg query HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run /s 2>&1
reg query HKCU\SOFTWARE\Microsoft\Windows\CurrentVersion\Run /s 2>&1
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v Userinit 2>&1
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" /v Shell 2>&1
echo ===STARTUP_DIRS===
dir /s /b "%USERPROFILE%\AppData\Roaming\Microsoft\Windows\Start Menu\Programs\Startup" 2>&1
dir /s /b "C:\ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp" 2>&1
echo ===EVT_1074===
findstr /i /c:maintenance %USERPROFILE%\Desktop\evt_full.txt 2>&1
) > %OUT% 2>&1
endlocal
