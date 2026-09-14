Get-WmiObject Win32_PortResource | Where-Object { $_.Start -lt 4096 } |
    Sort-Object Start |
    ForEach-Object { '{0}-{1}  {2}' -f $_.Start, $_.End, $_.Name } |
    Out-File -FilePath $env:USERPROFILE\Desktop\com_ports.txt -Encoding ascii
