$out = "$env:USERPROFILE\Desktop\serial_dev.txt"
'SERIALCOMM map:' | Out-File $out -Encoding ascii
Get-ItemProperty 'HKLM:\HARDWARE\DEVICEMAP\SERIALCOMM' | Format-List | Out-String |
    Add-Content $out

'' | Add-Content $out
'Serial devices (class Ports):' | Add-Content $out
Get-PnpDevice -Class Ports | ForEach-Object {
    $_.InstanceId + '  [' + $_.Status + ']  ' + $_.Name | Add-Content $out
}

'' | Add-Content $out
'Allocated IO ports for PNP0501:' | Add-Content $out
Get-WmiObject Win32_PnPAllocatedResource | ForEach-Object {
    if ($_.Dependent -match 'PNP0501')
    {
        $port = [wmi]$_.Antecedent
        '  ' + $port.StartingAddress + '-' + $port.EndingAddress | Add-Content $out
        $_.Antecedent | Add-Content $out
        $_.Dependent | Add-Content $out
    }
}
