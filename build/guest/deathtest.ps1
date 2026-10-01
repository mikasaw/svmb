# r76 death-sweep negative test: protect a page in THIS process, then exit
# WITHOUT unprotecting - the driver's process-death sweep must release it.
Add-Type -Namespace Win32 -Name Mem -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern IntPtr VirtualAlloc(IntPtr lpAddress, uint dwSize, uint flAllocationType, uint flProtect);
"@
$p = [Win32.Mem]::VirtualAlloc([IntPtr]::Zero, 0x1000, 0x3000, 0x04)
if ($p -eq [IntPtr]::Zero) { Write-Error "VirtualAlloc failed"; exit 1 }
# fault the page in - a demand-zero page has no physical backing and the
# driver rejects regions on not-present pages (r76)
[System.Runtime.InteropServices.Marshal]::WriteByte($p, 0, 0x41)
$me = [System.Diagnostics.Process]::GetCurrentProcess().Id
$baseHex = "{0:x}" -f $p.ToInt64()
Write-Host "alloc pid=$me base=$baseHex"
& "$env:USERPROFILE\Desktop\driverTest\svmb-test\svmbctl.exe" cr3 protect $me $baseHex 1000
if ($LASTEXITCODE -ne 0) { Write-Error "protect failed"; exit 1 }
Write-Host "protected - now dying WITHOUT unprotect"
exit 0
