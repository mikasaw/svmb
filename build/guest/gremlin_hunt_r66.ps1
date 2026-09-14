$ErrorActionPreference = "Continue"
$out = "$env:USERPROFILE\Desktop\r66_gremlin.txt"
"=== r66 gremlin hunt $(Get-Date -Format o) ===" | Out-File $out -Encoding utf8

"=== PROCS (powershell/cmd/conhost/wscript) ===" | Out-File $out -Append -Encoding utf8
Get-CimInstance Win32_Process | Where-Object { $_.Name -match "powershell|cmd|conhost|wscript|cscript" } |
  Select-Object ProcessId, ParentProcessId, Name, CreationDate, CommandLine |
  Format-List | Out-File $out -Append -Encoding utf8

"=== WMI PERMANENT SUBSCRIPTIONS ===" | Out-File $out -Append -Encoding utf8
"--- consumers ---" | Out-File $out -Append -Encoding utf8
Get-CimInstance -Namespace root\subscription -ClassName __EventConsumer 2>$null |
  Format-List Name, __CLASS, CommandLineTemplate, ScriptFileName, ScriptText |
  Out-File $out -Append -Encoding utf8
"--- filters ---" | Out-File $out -Append -Encoding utf8
Get-CimInstance -Namespace root\subscription -ClassName __EventFilter 2>$null |
  Format-List Name, Query | Out-File $out -Append -Encoding utf8
"--- bindings ---" | Out-File $out -Append -Encoding utf8
Get-CimInstance -Namespace root\subscription -ClassName __FilterToConsumerBinding 2>$null |
  Format-List Filter, Consumer | Out-File $out -Append -Encoding utf8

"=== SVMB SERVICE FAILURE ACTIONS ===" | Out-File $out -Append -Encoding utf8
sc.exe qfailure svmb 2>&1 | Out-File $out -Append -Encoding utf8
sc.exe query svmb 2>&1 | Out-File $out -Append -Encoding utf8
sc.exe qc svmb 2>&1 | Out-File $out -Append -Encoding utf8

"=== SCHEDULED TASKS (non-Microsoft) ===" | Out-File $out -Append -Encoding utf8
Get-ScheduledTask 2>$null | Where-Object { $_.TaskPath -notlike "\Microsoft*" } |
  Select-Object TaskPath, TaskName, State | Format-Table -AutoSize |
  Out-File $out -Append -Encoding utf8

"=== AUTOLOGON / WINLOGON ===" | Out-File $out -Append -Encoding utf8
Get-ItemProperty "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" |
  Select-Object AutoAdminLogin, AutoAdminLogon, DefaultUserName |
  Format-List | Out-File $out -Append -Encoding utf8

"=== DONE ===" | Out-File $out -Append -Encoding utf8
