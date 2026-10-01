# svmb 测试虚拟机环境记录(VMware Workstation)

> 本文是 `tests/VM_SETUP.md` 的补充:**环境现状与操作规程**。首次搭建看 VM_SETUP,
> 日常测试/恢复/取证按本文。最后更新:2026-09-05(快照 3 固化后)。

## 1. 环境事实

| 项 | 值 |
|---|---|
| VMX | `<vm-dir>\Windows 10 x64.vmx` |
| guest 账户 | `<guest-user>` / `<guest-pass>`(本地管理员;已与 Winlogon 自动登录同步) |
| vmrun | `C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe` |
| 封装脚本 | `<host>/.agents/skills/vmware-file-transfer/scripts/vm-guest.sh`(to-guest/to-host/run/list/start) |
| guest 部署目录 | `<guest-user>\Desktop\driverTest\svmb-test\`(svmb.sys + svmbctl.exe + run_regression.cmd) |
| 快照 | `快照 3`(2026-09-05 基线:含下述全部 guest 设置;旧"快照 1"已删) |
| guest 系统 | Windows 10 Pro 18363 x64,build 18363.592,测试模式水印 |

## 2. 宿主机侧(.vmx)关键项

| 项 | 值 | 说明 |
|---|---|---|
| `vhv.enable` | TRUE | 嵌套 SVM,必须有 |
| `numvcpus` / `cpuid.coresPerSocket` | 12 / 3 | **VMware UI 会用内存配置回写 .vmx,手工改动可能被覆盖**;改完务必 `listProcessesInGuest` 确认 guest 实际核数(或 `svmbctl info` 的 cores total) |
| `serial0.fileType/fileName` | file / `...\guest_com1.log` | 串口黑匣子(COM1 落盘到宿主机文件);**当前该通道无输出,待查 SerialPut**(见 §7) |
| `msg.autoAnswer` | TRUE | triple fault 对话框自动应答,避免 vmx 派发线程被模态框卡死 |
| 内存 | 8192 MB | |

`.vmx` 修改必须在 VM **关机**状态做;开机后 UI/vmx 进程可能回写覆盖。

## 3. guest 侧基线(已固化进快照 3)

| 项 | 值 | 命令 |
|---|---|---|
| 自动登录 | AutoAdminLogon=1,`<guest-user>`/`<guest-pass>`,域空 | `reg add HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon /v ...`(4 项) |
| 崩溃恢复 | `bootstatuspolicy IgnoreAllFailures` | 脏断电后不再弹"恢复"界面 |
| 测试签名 | testsigning Yes | |
| Hyper-V | hypervisorlaunchtype Off;HVCI(VBS 内存完整性)关闭 | SVM 必须未被占用 |
| 内核调试 | `debug Yes`,`debugtype NET`,busparams 3.0.0,hostip <lan-ip>,port 50000,key <kdnet-key> | WinDbg Preview 的 Default Connection 与之匹配 |
| svmb 服务 | `start= demand`,`binPath= <guest-user>\Desktop\driverTest\svmb-test\svmb.sys`,默认停止 | `sc start svmb` 由探针/回归脚本自己执行 |
| SerialLog 注册表 | **未设置** | 需要时:`reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v SerialLog /t REG_DWORD /d 1 /f`(装服务后、`sc start` 前) |

## 4. 操作通道(vmrun)

- 全部 guest 操作需 `-gu <guest-user> -gp <guest-pass>`,且**参数顺序**:`vmrun -gu <guest-user> -gp <guest-pass> <op> <vmx> [op参数]`。
- guest 路径用反斜杠单引号;宿主机路径用正斜杠。
- 文件传输后必须 `verify-hash.sh` 做 SHA256 双向校验。

### 已知坑(全部实测踩过)

1. **`runProgramInGuest` 直启 `cmd.exe` 会永久挂起**(进程在但永不退出,无 AutoRun/IFEO 原因);`reg.exe` 部分参数异常。**一律用 PowerShell 包装**:
   `runProgramInGuest <vmx> 'C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe' -NoProfile -ExecutionPolicy Bypass -File <script.ps1>`;
   批处理/长命令写进 .ps1 或由 .ps1 内 `cmd.exe /c "..."` 执行。
2. `shutdown.exe /r|/s` 经 vmrun 返回 1 且**不生效**;重启/关机用 `vmrun reset <vmx> soft`(优雅)/ `stop <vmx> hard`(断电)。
3. **MKS 控制台的合成输入是死路**(点击/键盘/Ctrl+G 抓取都进不去),不要尝试 GUI 自动化操作 guest;VMware UI 菜单(虚拟机→电源)走 host 侧,可用。
4. HGFS 共享文件夹被 `sharedFolder.maxNum = "0"` 禁用,`\\vmware-host\...` 在 guest 内不可用。
5. `vmrun captureScreen` 需要 `-gu/-gp`,且 VM 处于僵死态时会超时。
6. Tools 的**会话实例**(vmtoolsd in user session)缺失时,程序启动报"VMware Tools 未在客户机中运行",但进程/文件查询仍可用——此时重启 guest 即可恢复。
7. 崩溃(三重故障)后 vmx 进入 "VM not running" 状态:所有 VIX 命令被 postpone;恢复流程 = `stop hard` → `start`。若 stop 也卡,先确认 msg.autoAnswer 生效/关掉弹窗,再 stop hard;终极手段杀 vmware-vmx(可能需管理员)。
8. **硬断电会丢最近 ~2-3 分钟内新建的文件**(NTFS 元数据未刷盘)。取证文件要么周期性落盘,要么走"崩溃前轮询转储"模式(probe7 模式:每 5s 独立文件)。

## 5. 崩溃取证规程(vCPU shutdown / triple fault)

1. `vmware.log`(VM 目录)grep `Triple fault`——记录时间戳与 vcpu 编号,是唯一不受 guest 死活影响的证据。
2. triple fault **不产生 guest minidump**(无 bugcheck 分发),事件日志只有 Kernel-Power 41/6008。
3. 探针模式(probe7):`start` 后每 5s `svmbctl info` + `svmbctl log > klog_NN.txt`(独立文件),崩溃只丢最后一片。
4. 串口黑匣子(SerialLog+serial0→file)设计上可存活,**实测无输出待修**;另外 VMware 对 file 型串口的 flush 时机未验证,勿依赖。
5. 死后恢复:`stop hard` → `start` → 取 klog 分片 + probe*_out.txt。

## 6. WinDbg(KDNET)

- WinDbg Preview 已配置 Default Connection:`net:port=50000,key=<kdnet-key>`。
- guest 重启后 KDNET 自动重连;首断(int 3)按 `g` 放行。
- 常用:`bu svmb!Hypervisor::Start`、`bu svmb!svmb::_svmb_vmm_loop`(extern C);符号路径加 `<repo>\x64\Release`。
- 调试器中断(broken)状态下 vmrun guest 操作会卡死——操作前确认提示符 `*BUSY*`。
- KD 抓不到 triple fault(vCPU 直接进 shutdown);调试器价值在**生前下断单步**(VM_SETUP §6)。

## 7. 已知环境/工具问题(待办)

| 问题 | 状态 |
|---|---|
| 串口黑匣子无输出(SerialWrite 走 `__outbyte(0x3F8)`,机制正确但零字节;从未实测通过) | 待查:driver/src/platform/logger.cpp SerialPut / VMware file 串口 flush 行为 |
| VMware UI 回写 .vmx 覆盖 numvcpus=1(单核实验未真正跑过;probe7 实际跑在多核下) | 待做:关机→改 .vmx→不开 UI 直接 `vmrun start`→`svmbctl info` 核对核数 |
| 12 vCPU 下崩溃时间随机(170ms~70s,跨驱动加载期与运行期) | 待做:单核复现 → 竞态 vs 确定性路径 |
| probe7 的 klog 分片因硬断电丢失(本地仅存 probe7_out.txt 部分步骤日志) | 教训:取证脚本应在崩溃前把分片同步到宿主机 |
