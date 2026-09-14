# r72 long-lived watched session: settle -> (storm churn exercises the
# fixed disarm path) -> 2h sleep keeps the armed slots alive for soaking
$log = "$env:USERPROFILE\Desktop\storm_status.txt"
"storm start $(Get-Date -Format o) pid=$PID" | Out-File $log -Encoding utf8

Start-Sleep -Seconds 20

$src = @"
using System;
using System.Threading;
public static class Storm {
    public static int Run(int n) {
        int made = 0;
        for (int i = 0; i < n; i++) {
            Thread t = new Thread(delegate() { Thread.Sleep(1); });
            t.Start();
            t.Join();
            made++;
        }
        return made;
    }
}
"@
Add-Type -TypeDefinition $src

for ($r = 1; $r -le 4; $r++) {
    $made = [Storm]::Run(500)
    "round $r made=$made $(Get-Date -Format o)" | Out-File $log -Append -Encoding utf8
    Start-Sleep -Seconds 5
}

"storm done, sleeping 7200s $(Get-Date -Format o)" | Out-File $log -Append -Encoding utf8
Start-Sleep -Seconds 7200
"storm end $(Get-Date -Format o)" | Out-File $log -Append -Encoding utf8
