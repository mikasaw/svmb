$c = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -match "svmb-test" } | Select-Object -First 1
if ($c) {
    Export-Certificate -Cert $c -FilePath "$PSScriptRoot\svmb-test.cer" | Out-Null
    "EXPORTED " + $c.Subject
} else {
    "CERT_NOT_FOUND"
}
