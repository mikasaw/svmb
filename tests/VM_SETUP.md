# svmb 测试虚拟机环境搭建指南（VMware + Windows x64 客户机）

svmb 基于 AMD-V (SVM)。测试需要在 **VMware 虚拟机**里嵌套运行 SVM，本文是从零到
「svmbctl demo 全绿」的完整路径，以及常见故障对照表。

> 宿主机必须是 AMD 处理器。Intel 平台无法提供嵌套 SVM，框架会在能力检测阶段
> 明确报 `NotAmd/NoSvmBit` 而不是蓝屏。

## 1. VMware 宿主侧配置

1. 确认宿主机 CPU 为 AMD（任务管理器 → 性能 → CPU 查看）。
2. 关闭目标虚拟机，编辑其 `.vmx` 文件，加入：

   ```
   vhv.enable = "TRUE"
   ```

3. 虚拟机硬件版本建议 17 及以上；内存 ≥ 4GB，CPU ≥ 2 核（多核才能验证逐核进出）。
4. （可选）配置 WinDbg 内核调试串口：
   - VMware：添加串口设备 → **Use output named pipe** → `\\.\pipe\com_1`，
     **This end is the server**（VMware 做服务端），**The other end is an
     application**（WinDbg 是应用端）
   - WinDbg：File → Kernel Debug → COM → 端口 `\\.\pipe\com_1`，波特率 115200
   - 客户机管理员命令行（`/debug on` 经常被漏掉，漏了则调试通道不启用）：

     ```bat
     bcdedit /debug on
     bcdedit /dbgsettings serial debugport:1 baudrate:115200
     ```
   - 验证传输：WinDbg 里按 `Ctrl+Break` 能中断进客户机即通
5. （可选）COM1 直写镜像用于崩溃取证：`LogSetSerial(true)` 开关默认关闭。
6. **打一个快照**。首次运行 hypervisor 是蓝屏风险最高的时刻。

## 2. 客户机（Windows 10/11 x64）准备

以管理员身份运行：

```bat
rem 关闭 Hyper-V（最常见失败原因 —— Hyper-V 占用 SVM 后 SVMDIS=1）
dism /online /disable-feature /featurename:Microsoft-Hyper-V-All
bcdedit /set hypervisorlaunchtype off

rem 关闭内核隔离/内存完整性（VBS/HVCI 会把系统架到 Hyper-V 上）
rem GUI 路径: Windows 安全中心 → 设备安全性 → 内核隔离 → 内存完整性 = 关

rem 允许加载测试签名驱动
bcdedit /set testsigning on

shutdown /r /t 0
```

重启后桌面右下角出现「测试模式」水印即说明 testsigning 生效。

验证嵌套 SVM 可用（可选，装好驱动后 svmbctl info 会直接给结论）：
Sysinternals `coreinfo64.exe -v` 应显示 HYPERVISOR = 0 且 SVM 相关位存在
（未进虚拟化时 HYPERVISOR 位应为 0；进了虚拟化后 svmb 不会伪造该位，
因为我们用 CPUID 透传 + 只隐藏 SVM 位）。

## 3. 部署

1. 开发机上先构建并签名：

   ```bat
   cd <repo>
   build\sign.cmd Release
   ```

2. 把 `x64\Release\svmb.sys` 和 `x64\Release\svmbctl.exe` 拷进客户机
   （共享文件夹 / 拖拽 / 网络），例如放到 `C:\svmb\`。

3. 客户机管理员命令行：

   ```bat
   sc create svmb type= kernel binPath= C:\svmb\svmb.sys start= demand
   sc start svmb
   ```

   卸载：

   ```bat
   sc stop svmb
   sc delete svmb
   ```

## 4. 测试流程（对应 M1+M2 验收）

### 4.1 一键回归（推荐）

把 `tests\run_regression.cmd` 与 `svmb.sys`、`svmbctl.exe` 放同一目录，
管理员 cmd 执行：

```bat
run_regression.cmd            rem 默认 storm=100000 cycle=100
run_regression.cmd 5000 20    rem 自定义迭代数
```

脚本自动完成：驱动安装（sc create/start，容忍已安装状态）→ info 基线
（非 OFF 先自动归一化）→ start → info 断言 `svm cpuid bit: hidden`（P0-1
防回归）→ storm → demo → 挂载 demo_cpuid → cycle → mod list 断言模块在
cycle 后仍存活（P2-5）→ stop → info 断言 OFF → 内核日志转储。全绿时自动
`sc stop`/`sc delete` 验证干净卸载（DevUnload 路径）；任一步失败则保留
现场并打印失败清单与排查提示，退出码 1。

### 4.2 手动分步（等效流程，调试用）

```bat
cd C:\svmb

svmbctl info
rem 期望: hv state OFF, magic ok, cores = 实际核数

svmbctl start
rem 期望: 无输出错误; 系统不崩; 光标/输入正常（全核已进入 VMRUN）

svmbctl info
rem 期望: hv state RUNNING, cores virtualized = 全部核
rem       svm cpuid bit: hidden (ok, hidden while running)  <-- P0-1 断言

svmbctl storm 100000
rem 期望: 打印 hypercall exit 总数 = 核数 x 100000, 系统稳定

svmbctl demo
rem 期望: cpuid 0x40000100 attach 前后签名变化, 显示 "[+] demo module exit handler is live"

svmbctl mod attach demo_cpuid
svmbctl cycle 100
svmbctl mod list
rem 期望: cycle 后列表仍包含 demo_cpuid（进出虚拟化压力不丢模块, P2-5）

svmbctl stop
rem 期望: 回到 OFF

svmbctl log
rem 期望: 能看到 enter/exit/devirtualize 等内核日志
```

## 5. 故障对照表

| 现象 / 日志 | 原因 | 处理 |
|---|---|---|
| `SVM check failed: 3` (SvmdisSet) | Hyper-V/BIOS 占用了 SVM | 检查第 2 节是否全部执行；VMware `vhv.enable` 是否生效 |
| `SVM check failed: 1` (NotAmd) | 宿主机是 Intel | 换 AMD 宿主机，svmb 不支持 Intel VT-x |
| `SVM check failed: 4` (NoNpt) | 嵌套未开启 NPT | 确认 `vhv.enable = "TRUE"`，VMware 版本 ≥ 16 |
| `sc start` 报 577 / 无法验证签名 | 测试签名未开 | `bcdedit /set testsigning on` 后重启 |
| `sc start` 报 c000036/驱动加载失败 | 缺签名 | 运行 `build\sign.cmd` |
| start 后立即蓝屏 | VMRUN/VMCB 首次进入失败 | 用快照回滚；开 WinDbg（串口命名管道 `\\.\pipe\com_1`，115200）连上后重现，把调用栈和 `svmbctl log` 内容发给开发定位 |
| `SVM check failed: 6` (NoNrips) | CPU 或嵌套实现不支持 NRIP 自动保存 | 框架的 RIP 前进机制依赖 VMCB.NRip，无法在此环境运行；更换宿主 CPU 或更高版本 VMware |
| start 正常但 `demo` 显示 NOT working | 模块未注册/出口分发问题 | `svmbctl log` 查 attach 失败原因；确认 `mod list` 有 demo_cpuid |
| 系统假死（无蓝屏无响应） | GIF/中断语义问题 | 关机回滚快照；该现象务必带日志上报（属于框架级 bug） |

## 6. WinDbg 连接与日志（start 失败排查必读）

1. **先连调试器再跑 start**：客户机 `bcdedit /debug on` +
   `bcdedit /dbgsettings serial debugport:1 baudrate:115200`（串口 1 即
   VMware 命名管道串口 `\\.\pipe\com_1`），WinDbg → File → Kernel Debug → COM
   连接并 Ctrl+Break 验证。**vCPU 死掉后 WinDbg 连接即失效，必须生前连。**
2. **日志过滤器已自动打开**：驱动 DriverEntry 调用
   `DbgSetDebugFilterState(IHVDRIVER, DPFLTR_MASK, TRUE)`，组件 77 全级别
   可见，不再需要手工 `ed nt!Kd_IHVDRIVER_Mask` 或注册表掩码。日志格式
   `[svmb][cN] ...`。
3. **在驱动代码上下断点（bu 未解析断点，驱动加载前设置）**：

   ```
   bu svmb!Hypervisor::Start          ; start IOCTL 入口
   bu svmb!svmb::_svmb_vmm_loop       ; VMRUN 序列入口（extern C）
   g
   ```
   - `Start` 断不下来 = IOCTL 层就失败，看 svmbctl 输出的错误码
     （6 = NoNrips，宿主/VMware 不支持 NRIP，无法运行本框架）；
   - `_svmb_vmm_loop` 断下来后单步 `vmrun`，vCPU 关闭发生在哪条指令即定位。
4. **串口黑匣子（不依赖 WinDbg 存活）**：`DbgPrintEx` 的输出在 vCPU 死亡时
   随 VM 一起消失；串口镜像逐字节直写 COM1，VMware 串口配成
   **Use output file** 后每行日志实时落盘，死后可直接读文件：

   ```bat
   rem 永久开关（驱动加载时读取，sc stop/start 生效）：
   reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v SerialLog /t REG_DWORD /d 1
   rem 或运行期开关（hypervisor 运行中）：
   svmbctl serial on
   ```
   注意：串口已被内核调试占用时不要开 SerialLog（会污染 KD 协议）；
   二选一：调试器走管道，或串口落盘作黑匣子。

## 7. start 触发 vCPU 关闭（VMware shutdown）排查流程

1. **记下死前最后一条日志**（WinDbg 或串口文件），按位置分档：
   - 没有任何 `[svmb]` 行 → 死在驱动加载/IOCTL 之前，与 hypervisor 无关；
   - 停在 `config: serial=... hideCpuid=... protectHsave=...` 之后、
     `core N entering VMRUN` 之前 → 死在进入序列（CheckCpu/MSRPM/VMCB 构建），
     属框架与该 VMware/CPU 组合的兼容性问题，带上日志回报；
   - 已出现 `core N entering VMRUN` 甚至 `virtualization active` → 死在
     guest 侧第一条被拦截指令的处理路径，做第 2 步二分。
2. **二分新行为开关**（改动注册表后 `sc stop svmb && sc start svmb` 重读）：

   ```bat
   rem 疑犯 A：CPUID SVM/NPT 位隐藏（本次修复 P0-1 起真正生效）
   reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v HideCpuidBits /t REG_DWORD /d 0
   rem 疑犯 B：VM_HSAVE_PA 拦截（本次修复 P2-6 起新增）
   reg add HKLM\SYSTEM\CurrentControlSet\Services\svmb\Parameters /v ProtectHsave /t REG_DWORD /d 0

   svmbctl start
   rem 哪个开关关掉后不再死，根因即在该行为，把结果回报给开发
   ```
3. **二分仍死** → 大概率是进入序列本身与该环境的嵌套 SVM 兼容性问题
   （NRIP/嵌套 VMCB 一致性检查），非本次修复引入。用第 6 节的 bu 断点+
   单步拿到 VMRUN 前后的 VMCB 状态（`dt svmb!svmb::VMCB poi(svmb!svmb::Hypervisor+xx)`）
   与 `svmbctl log`、串口文件一起回报。
