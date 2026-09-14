# r99 E2E: spawn notepad, cross-process RPM+WPM it - the calls must land
# in the sysaudit ring with THIS powershell's pid as the caller.
Start-Process notepad -WindowStyle Hidden
Start-Sleep -Milliseconds 1500
$p = Get-Process notepad -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $p) { Write-Output "NO_NOTEPAD"; exit 1 }
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RPM {
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern IntPtr OpenProcess(uint a, bool inh, int pid);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool ReadProcessMemory(IntPtr h, IntPtr b, byte[] buf, int size, out int read);
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool WriteProcessMemory(IntPtr h, IntPtr b, byte[] buf, int size, out int written);
}
'@
$h = [RPM]::OpenProcess(0x1A, $false, $p.Id)
Write-Output ("probe: mypid=" + $PID + " open handle=" + $h + " targetpid=" + $p.Id)
$buf = New-Object byte[] 1048576
$rd = 0
$base = [IntPtr]::new(0x10000)
try { $base = $p.MainModule.BaseAddress } catch {}
$ok1 = [RPM]::ReadProcessMemory($h, $base, $buf, 1048576, [ref]$rd)
Write-Output ("probe: read ok=" + $ok1 + " bytes=" + $rd)
# r100: 20 reads spaced 50ms apart - the spacing forces context switches,
# which flush the NPT TLB so the X-deny actually faults (a tight loop rides
# one cached RWX translation and never trips - Round 100 sampling law)
$rd = 0
foreach ($i in 1..20) {
  $ok3 = [RPM]::ReadProcessMemory($h, $base, $buf, 64, [ref]$rd)
  Start-Sleep -Milliseconds 50
}
Write-Output ("probe: read-loop 64B x20 done")
$hits = 0
foreach ($i in 1..200) {
  $ok3 = [RPM]::ReadProcessMemory($h, $base, $buf, 64, [ref]$rd)
  if ($ok3) { $hits++ }
}
Write-Output ("probe: read-loop 64B ok=" + $hits + "/200")
$wr = 0
$ok2 = [RPM]::WriteProcessMemory($h, $base, $buf, 1048576, [ref]$wr)
Write-Output ("probe: write ok=" + $ok2 + " err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())
