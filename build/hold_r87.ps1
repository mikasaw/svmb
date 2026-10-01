# r87/r88 readvm E2E: resident marker-holder process (non-svmbctl so the
# exclusively-opened device handle stays free for the reader instance).
# Publishes pid + marker VA (written bytes) + hole VA (allocated, NEVER
# touched - strategy B fault-in target). Always writes the result file.
$out = "$env:USERPROFILE\Desktop\hold_r87.txt"
try {
    $s = 'SVMB-R87MARK12'
    $p = [Runtime.InteropServices.Marshal]::AllocHGlobal(4096)
    [Runtime.InteropServices.Marshal]::Copy(
        [Text.Encoding]::ASCII.GetBytes($s), 0, $p, $s.Length)
    # large alloc -> VirtualAlloc-backed (demand-zero): pages the heap never
    # touches stay NOT PRESENT, which strategy B needs; skip the header page
    $h = [Runtime.InteropServices.Marshal]::AllocHGlobal(4 * 1024 * 1024)
    $hp = [int64]$h + 0x1000
    $hole = [int64]([math]::Floor($hp / 4096) * 4096)
    [IO.File]::WriteAllText($out,
        ('pid=' + $pid + ' va=' + $p.ToString('x') + ' hole=' + $hole.ToString('x')))
} catch {
    [IO.File]::WriteAllText($out, ('ERR=' + $_.Exception.Message))
}
Start-Sleep 90
