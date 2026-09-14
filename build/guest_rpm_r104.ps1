# r101 E2E probe: exercise ALL five sysaudit sense pages.
#   NTCF  - notepad spawn does constant file opens
#   NTRD  - every ReadProcessMemory (incl. the 64B small-copy loop that
#           never reaches MmCopyVirtualMemory - the r100 blind spot)
#   NTWR  - WriteProcessMemory to a scratch page (VirtualAllocEx'd, so
#           notepad's RX image is never written)
#   COPY  - the 1MB read/write take MmCopyVirtualMemory's large path
#   NTOP  - the OpenProcess call itself
# Handle mask 0x1F0FFF (r99 used 0x1A = read-only, so WPM failed err 5).
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
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern IntPtr VirtualAllocEx(IntPtr h, IntPtr addr, uint size, uint type, uint protect);
}
'@
$h = [RPM]::OpenProcess(0x1F0FFF, $false, $p.Id)
Write-Output ("probe: mypid=" + $PID + " open handle=" + $h + " targetpid=" + $p.Id)
$buf = New-Object byte[] 1048576
$rd = 0
$base = [IntPtr]::new(0x10000)
try { $base = $p.MainModule.BaseAddress } catch {}
$ok1 = [RPM]::ReadProcessMemory($h, $base, $buf, 1048576, [ref]$rd)
Write-Output ("probe: read1m ok=" + $ok1 + " bytes=" + $rd)
# 64B loop, 50ms spacing (context switches flush the NPT TLB so the X-deny
# faults - Round 100 sampling law). r104: each iteration RE-OPENS the
# process - a single OpenProcess call is one sensing lottery ticket
# (r104 E2E: it slid past), twenty spaced opens make the NTOP target
# attribution actually observable.
$hits = 0
foreach ($i in 1..200) {
  $ok3 = [RPM]::ReadProcessMemory($h, $base, $buf, 64, [ref]$rd)
  if ($ok3) { $hits++ }
}
Write-Output ("probe: read-loop 64B ok=" + $hits + "/200")
# CloseHandle P/Invoke for the r104 open-loop
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class CH {
  [DllImport("kernel32.dll", SetLastError=true)]
  public static extern bool CloseHandle(IntPtr h);
}
'@
# r101: write leg - scratch page + 1MB write (the MmCopyVirtualMemory
# write-side entry); the scratch is also the r104 open-loop's write target
$scratch = [RPM]::VirtualAllocEx($h, [IntPtr]::Zero, 65536, 0x3000, 0x04)
Write-Output ("probe: scratch=" + $scratch)
$wr = 0
$ok2 = [RPM]::WriteProcessMemory($h, $scratch, $buf, 1048576, [ref]$wr)
Write-Output ("probe: write1m ok=" + $ok2 + " err=" + [Runtime.InteropServices.Marshal]::GetLastWin32Error())

# r104 open+read+write loop, 50ms spacing: twenty OpenProcess calls make
# the NTOP CLIENT_ID attribution observable despite the sampling law
$rd = 0; $wr = 0
foreach ($i in 1..20) {
  $h2 = [RPM]::OpenProcess(0x1F0FFF, $false, $p.Id)
  $ok3 = [RPM]::ReadProcessMemory($h2, $base, $buf, 64, [ref]$rd)
  [void][RPM]::WriteProcessMemory($h2, $scratch, $buf, 64, [ref]$wr)
  [void][CH]::CloseHandle($h2)
  Start-Sleep -Milliseconds 50
}
Write-Output ("probe: open+read+write loop 64B x20 done")
