@echo off
rem Local per-machine VM environment. Copy this file to vmenv.bat (which is
rem git-ignored) and fill in YOUR values. NEVER commit the real vmenv.bat.
rem ---- host side ----
set "VMRUN=C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
set "REPO=%~dp0.."
set "HOST_HOME=C:\Users\your-host-user"
set "WINDBG_TEST=C:\Users\your-host-user\AiCode\windbg-test"
rem ---- guest side ----
set "VM_VMX=C:\VMs\Windows 10 x64\Windows 10 x64.vmx"
set "VM_USER=your-guest-user"
set "VM_PASS=your-guest-password"
set "GUEST_PROFILE=C:\Users\your-guest-user"
set "GUEST_TEST_DIR=C:\Users\your-guest-user\Desktop\driverTest\svmb-test"
set "GUEST_DESKTOP=C:\Users\your-guest-user\Desktop"
