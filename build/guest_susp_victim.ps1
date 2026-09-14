# r93 soak suspend victim: hatchable probee2e suspend run (r87 hatcher
# shape - cmd `start` is dead under VIX sessions, Start-Process is the
# sanctioned detour). The driver freezes this process mid-IOCTL on the
# bypass trip; the soak loop resumes it and this run then completes with
# the PASS line as evidence.
& "$env:USERPROFILE\Desktop\driverTest\svmb-test\svmbctl.exe" cr3 probee2e suspend > "$env:USERPROFILE\Desktop\susp_r93.txt" 2>&1
