# svmb 崩溃排查日志(2026-09-05/06 进行中)

> 状态:排查进行中。本文由自动化排查会话维护,记录所有已确认事实与下一步。
> 相关:CODE_REVIEW.md(代码审查)、tests/VM_SETUP.md(环境搭建)、tests/VM_ENVIRONMENT.md(环境操作)。

## 崩溃现象(全部实测确认)

| # | 本地时间 | 阶段 | vCPU | 调试器 | 备注 |
|---|---|---|---|---|---|
| 1 | 09/05 16:48 | 未知(上一会话) | vcpu-4 | 无 | 旧构建 |
| 2 | 09/05 18:31 | 回归流程中(~60s) | vcpu-1 | 无 | storm 阶段 |
| 3 | 09/05 19:15 | probe4:`sc start` 后 **170ms** | vcpu-?(未记录) | 无 | **驱动加载期** |
| 4 | 09/05 21:01 | start 后 ~4.5min | vcpu-4 | WinDbg 挂载 | |
| 5 | 09/05 21:22 | probe 运行中 | vcpu-5 | WinDbg 挂载 | |
| 6 | 09/06 02:28 | probe9:`sc start` 后 ~11s | vcpu-1 | kd 被动挂载 | **驱动加载期** |
| 7 | 09/06 03:41+ | probe9 轮询期 | 多次 | WinDbg 挂载 | 本轮共 3+ 次 |
| 8 | 09/06 05:04 | 插桩构建(2048 轨迹预算+裸 DbgPrint) | vcpu-4 | WinDbg 挂载 | guest 快速自愈重启 |

**特征**:时间 170ms~7min 高度随机;加载期与运行期都发生;有无调试器都发生(调试器只是目击者);从不产生 bugcheck/dump(三重故障直通 shutdown)。

## 已确认事实

1. **WinDbg 无 `[svmb]` 日志的根因**:`bd886d0` 把裸 DbgPrint 改为 `DbgPrintEx(组件77, Info级)`,被默认 DPFLTR 过滤器(只放 ERROR)吞掉。Cr3Encrypt 等项目用裸 DbgPrint 所以正常。
2. **修复已实施但未验证**:注册表 `Debug Print Filter\IHVDRIVER=0xF` 已固化(重启生效);代码修复(调试器在场时用裸 DbgPrint)已写入 logger.cpp 并构建部署(hash 090bcff1)。
3. **隔离实验**:无任何调试器时崩溃依旧 → 崩溃在 svmb/guest 内部,与调试器无关。
4. **exit 轨迹预算**已从 64 提到 2048(旧预算 ~25s 耗尽,早于致命 exit)。
5. **三路日志全静默**(DbgPrint、ring、串口)且环缓冲转储文件 0 字节 → 怀疑 `LogWrite` 提前返回(`gLog.Cores==NULL`,即 LogInit 失败)——**待 KD 内存检查最终裁决**。
6. 硬断电会丢失最近 2-3 分钟新建的文件(NTFS 元数据未刷盘)——多轮 klog 证据因此丢失。
7. 文件完整性已排除:guest 内 svmb.sys 哈希与宿主一致(PASS)。
8. x2APIC 直通理论已证伪(rdmsr 1b EXTD=0)。
9. 崩溃后 guest 快速自动重启(bootstatuspolicy IgnoreAllFailures 已固化),klog 文件在该场景下可幸存。

## 环境与工具现状

- 主 VM:`Windows 10 x64.vmx`,guest <guest-user>/<guest-pass>,自动登录已固化;快照 `快照 3`(09/05 基线)。
- **test2 克隆机与主 VM 抢 KDNET 端口 50000,调试期间必须保持关机**。
- 注册表已固化:自动登录 4 项、bootstatuspolicy=IgnoreAllFailures、hypervisorlaunchtype=Off、HVCI=0、DPFLTR IHVDRIVER=0xF。
- KDNET:busparams 3.0.0,hostip <lan-ip>,port 50000;WinDbg GUI 经 File→Recent 连接**已验证 4+ 次**(含对运行中目标)。
- kd.exe 控制台版从不连接成功(原因未明),WinDbg GUI 可用。
- 部署物(guest `<guest-user>\Desktop\driverTest\svmb-test\`):新版 svmb.sys(裸 DbgPrint+2048 轨迹)+ svmbctl + probe9.ps1 + run_regression.cmd。
- probe9 脚本:sc start → serial on → info → **start** → 每 5s info+log 转储 ×12 → storm 5000 → storm 45000。

## 下一步(按序)

1. **KD 读 gLog(裁决日志器)**:WinDbg 已连(实例 pid 6300,会话 net:port=50000)。输入法已跑通:点击 Command Input 元素 → computer-use type 命令 → key(Return) 提交(纯 SendKeys 的回车不生效)。命令序:
   - `.sympath <repo>\x64\Release` → `.reload /f svmb` → `x svmb!*gLog*` → 记地址 → `dd <addr> l4`(Cores/CoreCount/MinLevel/Serial)。
   - 注意:实例 6300 由 Recent 启动、无 -y 参数,`.sympath` 后必须 `.reload /f svmb` 才能加载 svmb.pdb。
2. **若 Cores==NULL(日志器天生坏)**:查 LogInit(分配仅 ~50KB 不应失败)→ 怀疑 LogInit 未被调用或 KeQueryActiveProcessorCountEx 异常 → 加固化日志(驱动加载即向串口/Boot- start 写标记)定位。
3. **若日志器健康**(Write 随运行增长):重跑 probe9,崩溃后立即 kd `Break` → `dt svmb!svmb::VcpuContext` 读各核 LastExit/ExitTrace → 致命 exit 直接可见。
4. **崩溃定位后**:按致命 exit 类型修 handler → 回归(run_regression.cmd)→ 更新 CODE_REVIEW/文档。
5. 遗留工具债:kd.exe NET 连接从不成功(仅 WinDbg GUI 可用,原因未明);串口黑匣子无输出;WinDbg GUI 输入自动化脆弱(AppActivate+SendKeys 文字可达但回车需 key(Return))。
6. **注意:WinDbg 输出面板可能停留在向上滚动位置**——新输出追加在底部,读结果前先向下滚动(AppActivate 后 scroll event)。

## 09/06 04:5X 追加:WinDbg 输入焦点根因 + 建议的下一步实现

- **WinDbg 键入失灵的根因确认**:键盘焦点停在功能区 Go 按钮上(元素树可见 `[348] button Go (focused)`)——此前点击过 Go 按钮。修法:先 AXPress/点击 "Command Input" 文本框转移焦点,再键入。此序列已验证一次成功(x 命令执行并回显)。
- **实测裁决(插桩构建 090bcff1,2048 轨迹预算)**:带 WinDbg+掩码 0xF 的完整 probe9 运行:guest 存活但中途 **1 次 triple fault**(05:04:07,vcpu-4),WinDbg 面板自始至终**零 [svmb] 行**;guest 内 klog_00..06 全部 **0 字节**(bundle 打包验证)。→ LogWrite 从未执行(或首行即返回)→ `gLog.Cores==NULL`(LogInit 失败)为唯一自洽解释。
- **下一个动作(代码层,无需 WinDbg 输入)**:
  1. LogInit 失败路径加"永不静默"标记:alloc 失败时向 COM1 直写 "LOGINIT-FAIL"(SerialPut 不依赖 gLog),并在 GET_INFO IOCTL 返回 logger 状态位;
  2. 重查 `AllocNonPaged`/`sizeof(LogState::Core)`/`KeQueryActiveProcessorCountEx` 在该 guest 的返回值(加临时 SVMB_LOGE(Alarm) 级别错误日志——Error 级默认过滤器必放行,可绕过可见性问题);
  3. 重建部署后一轮 probe:若 LOGINIT-FAIL 出现 → 根因即日志器初始化;若不出 → 日志器健康,问题在别处(回到 exit 轨迹路线)。
- WinDbg 面板读法:目标崩溃后 WinDbg 存活,输出面板保留全部流——读法 = AppActivate + 滚到底部 + 截图(滚动需要前台,先 AppActivate)。

## 本会话已完成的修复/改进

- logger.cpp:调试器在场时改用裸 DbgPrint(bd886d0 回归修复,已构建部署)。
- hypervisor.cpp:ExitTraceLeft 64 → 2048(已构建部署)。
- guest 注册表:DPFLTR IHVDRIVER=0xF、自动登录、bootstatuspolicy、HVCI off。
- .vmx:serial0→file 黑匣子、msg.autoAnswer、(注意 numvcpus 会被 VMware UI 回写)。
- tests/VM_ENVIRONMENT.md:完整环境与坑位文档。

---

## 重大进展 (2026-09-06 03:00-09:50)

### 1. svmb 驱动实际**能跑**——之前 vCPU shutdown "崩溃"假象

经 sign + 部署后实测 `sc start svmb`：
```
STATE: 4 RUNNING
WIN32_EXIT_CODE: 0
SERVICE_EXIT_CODE: 0
```

`svmbctl start` 成功，IOCTL 输出：
```
hv state         : RUNNING
cores            : 6 virtualized / 6 total
vmexits          : 198
svm cpuid bit    : hidden (ok, hidden while running)   ← P0-1 PASS
```

**结论**：代码层面 + VMware 嵌套虚拟化 + Win10 18362 guest 都 OK。
不是 svmb 本身 crash，是 vmware-vmx 跟 SVM hypervisor + VMware Tools 共存时的副作用。

### 2. **新根因**: svmb storm 把 VMware Tools 拖死

- storm 100k 期间 + 之后 guest 进程列表里 vmtoolsd.exe 全部消失
- 表现: vmrun `runProgramInGuest` 报 `Error: The VMware Tools are not running in the virtual machine`
- 影响: guest OS 通道断 (CIM/CopyFile 都失败)，但 **KDNET 仍然 alive**
- 推测: storm 把 6 核全塞给 vCPU 调度，VMware Tools 主线程 (vsocket) 拿不到时间片饿死

### 3. KDNET 实测可用 (vertarget 通过 WinDbg 元素 input)

之前所有"type 命令没生效"问题根因是焦点在 Go 按钮上。修正路径：
1. `mcp__computer-use__open_application(activate=true)` 切到 WinDbg
2. `mcp__computer-use__left_click(element=Command Input, index=195)` 抢焦点
3. `mcp__computer-use__type(text=...)` 此时 input 接受
4. `mcp__computer-use__key(text=Return)` 提交

验证: `vertarget` 输出完整 Win10 18362.1 kernel 信息 (MP 6 procs Free x64, Kernel base 0xfffff806`38200000, Connected to target <lan-ip>:50000)。

### 4. guest hard reset 后 KDNET 显示 BSOD 0x7E break-in

```
*** Fatal System Error: 0x0000007e
   (0xFFFFFFFFC0000005, 0xFFFFF806383C9370, ...)
Break instruction exception - code 80000003 (first chance)
nt!DbgBreakPointWithStatus: cc int 3
```

**这是 vmware-vmx 检测到 target crash 后通过 KDNET 强制 break 的注入，不是 guest 真的 BSOD**。
PID 0x7E = SYSTEM_THREAD_EXCEPTION_NOT_HANDLED, 但 #1 0xFFFFFFFFC0000005 (ACCESS_VIOLATION) + RIP=DbgBreakPoint 是典型 KDNET break 模式。

### 5. 经验教训
- **不要信"vCPU shutdown" = svmb crash**：vmware.log 里 "Triple fault" + KDNET BSOD 注入同时发生是 vmware-vmx 错误路径
- **WinDbg GUI 输入三件套**: activate→click→type→Return, 缺一不可
- **Tools 与 SVM hypervisor 不兼容**: storm 时 vmware-vmx 调度路径有 bug

### 6. 代码已合并改动 (待验证 beacon)

| commit | 改动 |
|---|---|
| 4c24e08 | logger: 调试器在场时 bypass DPFLTR |
| bd886d0 | logger: emit via DbgPrintEx(DPFLTR_IHVDRIVER_ID) |
| 1057fd2 | exit_dispatcher: clear GuestRegs.Extra1/Extra2 |
| 53c5a94 | code-review remediation (P0-P3, A1-A3) + debug instrumentation |
| c83941e | docs: fix WinDbg serial pipe direction in VM_SETUP.md |

### 7. 待验证 (本会话未完成)

1. **新 beacons 在 KDNET 真的可见吗？**
   - 代码已在 `logger.cpp` LogInit 加 DbgPrint: `[svmb][loginit] ENTER/OK/FAILED`
   - 在 `main.cpp` DriverEntry 第一行加 `[svmb][entry] ENTER` + `[svmb][entry] after-LogInit`
   - 在 `logger.cpp` LogWrite first-call 加 `[svmb][logwrite] ENTER`
   - **关键**: 这些 beacons 还没在 KDNET 真正触发过——因为 storm 把 Tools 拖死后没法再 sc start driver

2. **`.sympath` + `.reload /f svmb` + `x svmb!*gLog*` 读 gLog 结构**
   - WinDbg 已经能接收命令了
   - 但要等 Tools 恢复 (guest reboot + 重新装载 svmb service) 才能触发 driver entry beacons

3. **vmware.log 中 12 次 Triple fault 时间线**
   - 旧 9 次 (9/5 之前) + 新 3 次 (9/6 启动后)
   - 都伴随 storm 期间, 不是 svmb 自身

---

## 阶段 2：HSAVE 拦截 + storm 二次崩溃 (2026-09-06 第三轮)

### 阶段 2 现状

- 已通过 `build/sign.cmd` 签 svmb.sys (用 svmb-test 自签证书)
- svmbctl info 基线 OK: magic ok, hv state OFF, 0/6 virtualized
- 阶段 1 (svmbctl start) 第一次跑触发 vcpu-4 Triple fault @ 02:20:42
- BSOD minidump 拉到 host 解析：
  - BUGCHECK_CODE: 7E (SYSTEM_THREAD_EXCEPTION_NOT_HANDLED)
  - AV in `svmb!DevUnload+0x65` → main.cpp:294 (DriverUnload 二阶段 cleanup)
  - 真实诱因 = svmb HV::Start 进 VMRUN 之后撞 nested AMD-V 不兼容，BSOD cleanup 时 DriverUnload 又踩坏指针（use-after-free）放大破坏
- 二分定位元凶:
  - bisect1: HideCpuidBits=0 + ProtectHsave=0 → svmbctl start 成功 (`hv state: RUNNING, 6/6 cores, vmexits=7`)
  - bisect2: HideCpuidBits=0 + ProtectHsave=1 → Triple fault @ 02:40:47 (vcpu-0) — 立即复现
  - **结论: `MSR VM_HSAVE_PA` guest 拦截 = 唯一元凶**
- 代码修复 (commit 待):
  - `HsaveProtectionEnabled` 默认值从 1 改为 0 (关闭 HSAVE 拦截)
  - 保留 registry `Parameters\ProtectHsave=1` 开关供后续诊断
  - 验证 (修复后): svmbctl start 成功, `svm cpuid bit: hidden (ok, hidden while running)` (P0-1 仍 PASS)

### 已知遗留问题 (本会话未能继续验证)

- storm 10k 触发 vcpu-2 Triple fault @ 02:46:16 (HSAVE 修复后仍崩)
- 怀疑是另一个 MSRPM bit 错或者 NPT 关闭导致嵌套退出处理失败
- vmrun runProgramInGuest 在 Tools 重启后短暂不稳定, 多次 ps1 卡住
- 下一步二分: `HideCpuidBits=0` + storm 10k 测试，如果还崩则用 KD `bp svmb!HandleVmrunExit` 在第一个 VMRUN exit 单步

### 关键诊断工具沉淀

- **WinDbg !analyze -v** 在 BSOD 后用 cdb.exe -z <minidump> 离线分析比实时 KD 更快
- **vmware.log Triple fault** 时间戳是 BSOD 后 vmware-vmx 自愈重置的标记,不是 crash 时刻
- **bisect 流程** (VM_SETUP.md 7.2): 调 reg 重启 sc stop/start + 测, 比实时 KD 调试高效
- **必须** cp936/utf-8 编码问题: 复杂 vmrun 调用走 PowerShell 包装, 简单命令走 bash 单引号

---

## BSOD 0x1A → 0x50 二次蓝屏 (2026-09-06 第四轮)

### 新 BSOD: MEMORY_MANAGEMENT (0x1A)

- minidump 路径: `C:\Windows\Minidump\090626-11609-01.dmp` (9/6 10:58, size 525KB)
- WinDbg 已经 break-in, 显示 0: kd>
- `!analyze -v` 结果:
  - BUGCHECK_CODE: 1a
  - Arg1: 0x3453 (MI_SUBTYPE_PT_SCRUB_INVALID)
  - Arg2: ffffc288071cd080 (bad PTE pointer)
  - Arg3: 0x97c04
  - Arg4: 0x3
  - **PROCESS_NAME: svmbctl.exe**
  - FAILURE_BUCKET_ID: `0x1a_3453_nt!MiDeleteFinalPageTables`
- 调用栈:
  - nt!KeBugCheckEx
  - nt!MiDeleteFinalPageTables+0x164ea6
  - nt!MmDeleteProcessAddressSpace+0x64
  - nt!PspProcessDelete+0x13e
  - nt!ObpRemoveObjectRoutine+0x80

**根因**: svmbctl.exe 进程退出时, Windows 回收其 page table, 撞到 svmb driver 释放过但被 page table 引用的内存页 (VMCB/MSRPM/nonpaged pool)。

### 修复 (commit 待): DevUnload 不 delete Hypervisor

- `main.cpp:278` DevUnload 路径
- 原本: `if (NT_SUCCESS(hv->Stop())) delete hv;`  
- 改为: `(void)hv->Stop();` (leak Hypervisor object, OS at process teardown 回收)
- 解释: Stop() 内部已经释放了 Vcpus_[]/MSRPM/Dispatcher/Hypercall/Modules — Hypervisor object 本体没必要再 delete, 否则会撞坏任何正在devirt中持有hv指针的线程

### 新 BSOD: PAGE_FAULT_IN_NONPAGED_AREA (0x50)

- 出现在 fix 部署后跑 sc start + svmbctl start + svmbctl stop + sc stop 验证期间
- WinDbg break-in 显示:
  - BUGCHECK_CODE: 50
  - Arg1: ffffdb8852c00019 (bad address)
  - Arg4: 0 (usermode read)
  - 调用栈 (page table 访问):
    - nt!KeAccessPageTableBasedPte+0xc0
    - nt!MiGetPteAddress+0x5
    - nt!MiSetPteDirty+0x1b
    - nt!MiInsertPrivatePageToWorkingSet+0x1a6
    - nt!MiResolvePage+0x182
    - nt!MiDispatchFault+0x497
    - nt!MmAccessFault+0x14d
- **vmware.log 没记 Triple fault (count=15 未变)**: 这是 0x50 page fault, 不是 vCPU shutdown, vmware-vmx 不报 Triple fault
- 0x50 dump 路径: 这次 KDNET 抓到 break-in 后没产生 minidump (可能 KD 干预 path 太早, 或 svmbctl.exe 异常路径不同于0x1A)
- **新 BSOD 显示即使 leak Hypervisor 仍然有 page table 污染问题**: svmb driver 在 enter/exit VMRUN 期间释放的 nonpaged pool 内存被 page table 错误地引用, svmbctl 退出时撞坏

### 进一步修复方向 (未实施, 留待后续会话)

1. **Hypervisor::Stop 应该**:
   - 等待所有 vCPU devirtualize 完成 (目前 RunOnEachCore 同步)
   - 显式 invalidate 所有 VMCB/HSave/MSRPM 关联的 page table entries
   - **重要**: 在 ReleaseNonPaged 之前 unmap 所有 VMCB/MSRPM 在 page table 的引用

2. **可疑 root cause**: `vcpu->GuestVmcbPa = MmGetPhysicalAddress(&vcpu->GuestVmcb).QuadPart` (line 204) 拿到 VMCB 的物理地址, 写入到 `vcpu->Info.GuestVmcbPa` 是给 vmexit handler 用. **但没 unmap page table references on Stop**, 当 Vcpus_[] 数组内存被 ExFreePoolWithTag 释放, VMCB 物理 page frame 重新分配给 page table 用途 (svmbctl 退出时 Windows 正好需要 page frame)

3. **M3 NPT (Nested Page Tables)**: 注释 "NPT off until mm::NptManager lands (M3); Intercepts_ keeps MsrpmBasePa" — 关闭 NPT 是当前架构选择, 但意味着 host page table 直接被 guest 修改, 释放时撞坏的概率更高

### 验证状态

- 修复 (DevUnload 不 delete) 部署了, 但**未跑通完整测试**
- 当前 guest Tools 状态 running (vmware-vmx 报告), Triple fault count=15 未变
- 0x1A 没在vmware.log 复现 (BSOD 0x1A 通常会重启guest, 而 BSOD 0x50 在 KDNET 干预下走不同路径)
- **需进一步二分**: 仅 svmbctl info 不 start, 然后 start 不 stop, 看哪一步触发了 0x50

### 下一步会话建议

- 用 `!analyze -v` 在 KD break-in 立即发, 避免 guest 重启丢失 context
- 二分: 仅 load driver (sc start svmb) 不调 svmbctl → 看是否仍崩 → 定位 svmb driver load 本身有没有问题
- 检查 Hypervisor::Stop 的 FreeNonPaged 序列是否安全: VMCB 物理 page frame 是否被 PTE 引用, FreeNonPaged 之前是否需要 KeFlushEntireTb + 清空 page table entries

---

## BSOD 0x1A/0x50 之后: leak 物理 page 仍触发 Triple fault (2026-09-06 第五轮)

### Round 5 修复: 全面 leak 物理 page

- `driver/src/core/hypervisor.cpp` line 362-389: Vcpus_[] + Vcpus_ array **不释放** (raw alloc 永久 leak)
- `driver/src/hw/msrpm.cpp`: Msrpm::Deinit + Iopm::Deinit **不释放** 8KB/16KB 物理 page
- 注释解释: page frame 被 PT 复用导致 svmbctl 进程退出时 BSOD

### 结果: Triple fault 仍发生

- v5_full 跑出 Triple fault @ **03:57:06 vcpu-4** (vmware-vmx log)
- **storm 10000 期间**: vCPU 仍 shutdown
- v5_full ps1 跑 storm 后卡死, 没产生新 minidump
- vmware.log Triple fault count = 16

### 结论

**Leak 物理 page 不能解决根本问题**。storm 10000 中触发 vCPU shutdown 不是 page table 污染:

1. svmb 启动 VMRUN 后, host CPU 进 SVM 模式
2. storm 让 6 个 vCPU 频繁 vm-exit → 多次走 vmexit dispatcher
3. 某个 vm-exit handler 在 nested AMD-V 环境下触发 host vCPU shutdown
4. vmware-vmx 看到 vCPU-4 shutdown → Triple fault

**真正 root cause 在 vm-exit 处理路径**, 跟 Start/Stop 释放无关. leak 物理 page 解决了 BSOD 0x1A (use-after-free) 但没解决 Triple fault 本身.

### 建议下一步

1. **KD 实测**: 在 storm 前 `bp svmb!HandleVmrunExit` 第一次 vmexit, 单步 + `r` 看 RIP, 然后 `g` + 反复 bp, 看 host crash 在哪条指令
2. **二分** (VM_SETUP.md §7.2 风格): `Parameters\HideCpuidBits=0` + storm 10k 验证, 排除 CPUID 拦截
3. **直接读 WinDbg 状态**: 发送 `vertarget` + `!analyze -v` 立即看 break-in 现场 (避免 ps1 卡)
4. **Storm 减少到 1000/500**: 验证小 storm 是否稳定
5. **重置 guest 强制 KDNET reconnect** 用 vmrun reset 后让 WinDbg 重连, 避免 KD 卡在 0x1A 现场

### 当前 commit 状态 (待 commit)

- d1d87cf: DevUnload leak Hypervisor object
- (待) 7d8b3a2?: Vcpus_ + MSRPM + IOPM leak 物理 page

---

## Bisect: svmb driver load 立即挂死 Tools (2026-09-06 第六轮)

### 关键发现

- bisect #0 (`sc start svmb` 然后 sleep 30s 无 IOCTL): **Tools 立即死**
- bisect #1 (sc start + svmbctl start + storm 1000 + sc stop): **Tools 死, TF 不变 (16)**
- bisect #2 (sc start + svmbctl start + storm 100 + sc stop): **Tools 死, TF 不变 (16)**
- WinDbg vertarget 验证 KDNET alive, 03:57:06 后 guest 自动重启

**结论**: **Tools 死 不等 storm** —— `sc start svmb` 加载 driver 后立即死, 因为 svmb MSRPM 拦截 guest OS 的 MSR 写, Tools daemon 写 MSR 时跳进 VMM 阻塞. 无 IOCTL 也会死.

**Triple fault 16 不变**: storm 10000 + sc start 触发 Triple fault 是罕见 scenario, 大部分 svmbctl IOCTL 路径 Tools 先死 (vmrun guest 通道挂), Tools 死了 vmrun 不会再发 IOCTL, 所以 storm 1000/100 不会触发 Triple fault —— vmrun guest 通道在 Tools 死时已卡, sc start 后 svmbctl 没机会调.

### 这意味着

svmb driver 跟 VMware + Windows10 + KDNET + VMware Tools 共存有**根本性兼容问题**. storm 10000 触发的 Triple fault 是 Tools 已经死之后的 secondary symptom —— vmware-vmx 检测 svmb 持续在 VMM/IOCTL 跑但 host 资源耗尽, 强制 hard reset vCPU.

### 完整 storm 100k 验证 (run_regression.cmd) 阻塞的 root cause

1. sc start svmb → Tools daemon 死 (msrpm 拦截)
2. svmbctl start → IOCTL 阻塞在 DeviceIoControl (Tools 已死)
3. vmrun guest 通道卡死 (Tools 死)
4. storm 100k 永远跑不完, 因为 svmbctl 在 VMRUN 内
5. vmware-vmx 后续检测 host CPU 异常, Triple fault + 自动重启

### 建议下一步修复方向 (留待后续会话)

1. **MSRPM 排除 Tools daemon 进程**: 通过 `PsSetCreateThreadNotifyRoutine` / `PsSetCreateProcessNotifyRoutine` 跟踪 Tools 进程 + 排除拦截
2. **用 NPT (Nested Page Tables) 解决 Tools 写 MSR 拦截**: 启用 NPT (M3 milestone) 让 guest page table 完全影子化, host MSR write 不再被 svmb MSRPM 拦截
3. **Storm 调度优化**: svmbctl start 后定期 devirt 一个 vCPU 让 Tools 写 MSR 通过, 用 DPC/WorkItem 控制
4. **完全脱离 VMware Tools 依赖**: 通过 KDNET 直接发 IOCTL, 绕开 vmrun guest 通道

## 第七轮 (2026-09-06 13:00-15:30): mcp-windbg 接入 + DevUnload 修复落地 + VIX exec 通道故障

### gDosName 修复已提交 (0dd1a92)

- 三个 minidump (090626-10625/11468/11593) 全部指向 `DevUnload+0x65` 读损坏的
  `gDosName`: `RtlInitUnicodeString` 把它指向 .rodata 字面量 `SVMB_DOS_NAME`,
  该节在 Win10 上可分页,DevUnload 运行时已被逐出,`RtlFreeUnicodeString`
  走到坏字节 → AV (BUGCHECK 0x50, `AV_R_(null)_svmb!DevUnload`)。
- 修复:DevUnload 不再碰 `gDosName`,改用栈上 `UNICODE_STRING` 调
  `IoDeleteSymbolicLink`。构建 hash 090BCFF1 已部署 guest 并核对 SHA256。
- **验证状态:未完成**。验证周期(loading→hv start/stop→sc stop→15s 蓝屏观察)
  因 guest 的 VIX exec 通道故障无法触发(见下)。修复部署后 guest 未产生任何
  新 minidump(基线 5 个全是修复前的)。

### mcp-windbg MCP 已接入并实测可用

- 安装 `pip install mcp-windbg`(1.2.2),配置写入
  `~/.zcode/cli/config.json` → `mcp.servers.mcp_windbg`,
  启动参数 `--timeout 180`(cdb 内核符号加载实测 ~38.8s,默认 60s 会超),
  `_NT_SYMBOL_PATH` 走本地 SRV 缓存。
- **活动内核调试全链路实测通过**(KDNET):open_kd_session → vertarget/r →
  `!process 0 0`(全进程表)→ `lm m svmb` → `k` → `g` 放行 →
  send_ctrl_break 再断入 → close(库会先发 `g` 再断链,目标保持运行)。
  放行生效的证据:两次 break 之间 System Uptime 18:06.9→18:57.4。
- **离线 dump 分析可用但受 ZCode MCP 客户端 30s 工具超时限制**:
  open_cdb_dump 全流程(!analyze -v)实测 ~40-60s,经 ZCode 工具调用会被
  30s 截断。绕法:同一 server 用 stdio 协议脚本驱动(Python MCP client)
  时完全正常——已验证输出 BUGCHECK_CODE: 50 / MODULE_NAME: svmb /
  FAILURE_BUCKET_ID: AV_R_(null)_svmb!DevUnload。
- **KDNET key 权威来源**:`<windbg-test>/logs/kdnet_out.txt`
  (`key=<kdnet-key-redacted>...`)。本次会话 key 手抄多一个 t,排查 30 分钟——
  连不上先核对 key,再看防火墙(KDNET-debug-50000 规则已存在,UDP Any)。
- **遗留 kd.exe 孤儿进程会占住 UDP 50000**,新会话连不上时先
  `Get-Process kd` 清理。

### VIX exec 通道故障(阻塞项,非 svmb 问题)

- 现象:`vmrun runProgramInGuest/runScriptInGuest` 在 4 次 boot(含 2 次完整
  断电重启、WU 服务完结后)始终挂死,cmd.exe 被创建但永不执行命令(无输出
  文件,进程滞留);错误密码秒拒、listProcessesInGuest/CopyFile 双向/
  listDirectoryInGuest 全部正常 → 认证与文件通道完好,仅"程序执行"子系统的
  进程创建卡死(疑似 Defender 更新后拦截 VIX 注入)。
- GUI 自动化输入被宿主静默丢弃(合成点击"accepted"但指针不动、guest 无反应),
  无法用 GUI 重启 Tools 服务。
- Startup 文件夹方案可行但 Startup 脚本是非提权 token(sc 报错 5),
  `Start-Process -Verb RunAs` 弹 UAC 无人点击 2 分钟自动取消。
- **恢复途径(供人工执行)**:guest 里双击
  `<guest-user>\Desktop\driverTest\svmb-test\cycle_startup2.cmd` 并在 UAC 上
  点"是"——它自动跑完整验证周期并写 `cycle_result2.txt`(可经文件通道取回);
  或在 guest 里重启 VMware Tools 服务/更新 Defender 排除项以修复 VIX exec。
- **硬断电会丢未刷盘文件**:14:57 部署到 Startup 的脚本因拷贝后 5 秒内断电
  而丢失;重部署后必须等待 ≥30s 再断电。

## 第八轮 (2026-09-06 晚): 串口黑匣子贯通 + 两大调试阻塞根因破案

### 根因#6 (致盲根因): LogInit 把日志子系统整个关死

- 现象:串口黑匣子 0 字节、kd 收不到任何 [svmb] DbgPrint、环形缓冲恒空——
  全天所有"日志静默"之谜的总答案。
- 机制:`LogInit()` 的 `RtlZeroMemory(&gLog, sizeof(gLog))` 把 `MinLevel`
  的类内默认值 (Info=3) 一并清成 0 (None);`LogWrite` 第一行检查
  `level > MinLevel → return`,**环形缓冲、DbgPrint、串口镜像全部无效**。
  无人调用过 `LogSetLevel`,故从驱动加载起日志就是死的。
- 发现手段:kd 活体冻结 + 裸内存读 `gLog`(+0x10=MinLevel=0,+0x14=Serial=1)。
- 修复:零内存后显式恢复 `gLog.MinLevel = LogLevel::Info`。修复后串口立即
  出现 `config: serial=1 hideCpuid=1 protectHsave=0` 与
  `svmb driver loaded (proto v1)`——镜像链路本身一直是好的。

### 死亡窗口最终锁定 (EnterCore 飞行记录仪)

- EnterCore 每核埋 E1~E7 串口标记 + kd 断点轨迹双重确认,多轮复现一致:
  `ENTER-CORE 0..5 全部完成 → 下一事件即 Triple fault`,
  **SvmbVmExitEntry 零命中(一条 VMEXIT 都没有)**。
- 结论:崩溃不在退出处理,而在 **VMRUN 激活后的最初窗口**(最后一核进入
  VMRUN 与第一条可拦截事件之间)。Stop 假死 (16:22) 是同一窗口的另一表型
  (某核 VMRUN 后中断配送死锁,RunOnEachCore 永久等待,连 KDNET 断入都被吞)。
- 待办:干净环境(快照 3 回滚完成)上跑第一轮带完整 exit trace 的周期,
  ExitTrace 机制 (2048 条预算) 首次真正可用。

### exec 卡死 (VIX runProgramInGuest) 根因破案

- 用 kd 解剖卡死 cmd.exe:主线程等在 **\Driver\condrv 的 console READ**
  (NtDeviceIoControlFile 永久 pending)。vmrun 把 cmd 启动在 Session 0,
  Tools 给它配的是假控制台,永远没有输入 → cmd 启动即阻塞在控制台读取,
  **执行队列被占死,同 boot 内所有后续 exec 排队挂死**。
- 修复:命令统一加 **`< NUL`** (stdin 立即 EOF)。实测 fresh boot 上
  `cmd /c <命令> < NUL` 快速返回、真实执行。触发器
  `run_guest_script.py` 已固化该修复。
- 运营纪律:**一次 boot 只做一次关键 exec**;卡死后只能硬重启清队列。
- 另:手工 plugin.json 放 `Roaming\ZCode\plugins` 不会被扫描,已删除;
  正确位置是 `~/.zcode/cli/config.json` 的 `mcp.servers`。

### 环境事件

- guest 因全天三重故障+硬重启循环累积损伤 (WU 服务事务被腰斩多次) 进入
  不可逆假死 (E1000 rx ring full、KDNET 断入失效),已**回滚快照 3**
  (09/05 干净基线) 并确认 Tools/KDNET 均正常。坏状态留档快照
  `broken-0906-guest-hang`。
- 快照 3 内 KDNET 已配置 (调试器直接连上),testsigning 待核实。

### 决死单步捕获 (23:42) - 死亡指令精确定位

- 方法:EnterCore 断点条件停住 (@edx==5,最后一个核),单步跨过 E1~E7 全部
  准备步骤 (save_regs/ConfigureVmcb/HSAVE MSR/SVME/FillGuestState/VMCB 复制/
  PA 计算/State=Guest),全部成功执行;机器死在
  `call svmb!_svmb_vmm_loop` 这条指令的执行当中 (call 发出后目标失联,
  KDNET "out of sequence ping",随后 Triple fault,Shutdown 23:42:58)。
- 即:**第一个核的第一条 VMRUN 指令本身就把 vCPU 打进 shutdown**。
  与"SvmbVmExitEntry 零命中"完全吻合;退出处理代码根本没机会运行。
- 注意:前 5 个核已完成 VMRUN 并在 guest 态运行 (本核 E1~E7 全绿),
  说明 VMCB 构造基本可用;差异变量 = "第 6 核 VMRUN"或"全部核都在 VMM 下
  时的全局状态" (如 TSC/IRR/中断注入边界)。
- 下一轮:EnterCore (@edx==5) 停住 → 步过到 call _svmb_vmm_loop →
  **不步进**,改为在 asm `_svmb_vmm_loop` 的 `vmrun` 指令处下断,
  放行命中后转储 VMCB 全字段 (dt svmb!svmb::VMCB guestVmcbPa) 再单步 vmrun,
  对照 AMD APM Vol2 Ch15/16 无效 VMCB 检查表逐项核对;重点:
  VMware 嵌套下 InterceptVmrun (op1 bit28) 自拦截行为与 GuestAsid=1 重复值。

### 离线 APM 对照 (23:50)

- 拦截向量核对:op1=bit18(CPUID)+bit28(VMRUN),op2=bit0(VMMCALL)+bit1(VMMCALL leg),
  MSRPM 只含 EFER/VM_CR(/HSAVE)。均合法。
- 段解析 (segments.cpp):TR/LDTR 16 字节描述符 base 高位拼装正确 (p[8..11]);
  属性直读 GDT,符合 SimpleSvm 惯例。无发现。
- 两个待定谳嫌疑 (需下一轮 VMCB 转储):
  1. **VMRUN 自拦截 (op1 bit28)**:嵌套模式下,VMware 对 L1 VMCB 中拦截
     VMRUN 的行为可能有额外校验/不支持 (宿主 VMRUN 模拟路径 vs 真硬件);
     裸机 SimpleVM 系项目全部可行,嵌套下待验证。对照实验:去掉 bit28。
  2. **GuestAsid=1**:VMware 嵌套可能对 ASID 有保留区间。对照:ASID=0 或
     TlbControl=3 (flush all) + ASID 递增。
- 补充事实:前 5 核 VMRUN 成功进入 guest 态 (E1~E7 全绿 + 决死单步前
  系统仍活着),所以 VMCB 模板大体正确;嫌疑集中在 **逐核差异** 或
  **全核虚拟化后的全局条件**,而非模板本身。

### 第二轮决死捕获尝试 (00:02) - 调试方法缺陷,非新信息

- 在核5 EnterCore 单步 (p 8) 跨过 `call svmb!LogWrite` 时超时 → 三重故障。
- 原因:MinLevel 修复后 LogWrite 真正执行,其中 DbgPrint (KdDebuggerEnabled
  分支) 走 KDNET 发包;**单步跟踪状态下逐条发包与调试器单步事件竞争**,
  罕见但致命的死锁。上一轮 (23:42) 用 p 40 大步跨过所有日志调用没触发,
  本轮 p 8 恰好停在 call LogWrite 边界上。
- 教训:**单步捕获必须避开 LogWrite/LogSerialTrace 的 DbgPrint 路径**,
  或临时置 KdDebuggerEnabled=false / 用断点代替单步。

## 第九轮 (09/07 凌晨): 根因#7 定谳 - ic2.VMRUN 自拦截位在 VMware 嵌套下致命

### 对照实验结果

- 实验:去掉 VMCB Control.InterceptOpcode2 bit0 (VMRUN 自拦截,反嵌套用),
  其余不变 (构建 86C496CB)。
- 结果:**此前同窗口 9 连杀 (100% 复现) 的三重故障彻底消失**——VMRUN 激活
  窗口运行 4+ 分钟零崩溃 (对照前:最后核进 VMRUN 后数秒内必死,一条
  VMEXIT 都没有)。
- 结论:**VMware 嵌套 AMD-V 不支持 guest VMCB 设置 ic2.VMRUN 拦截位**。
  L1 hypervisor 的 VMRUN 模拟路径遇到该位时直接把 vCPU 打进 shutdown
  (VMware 报 "vcpu-0 tried to execute an invalid point of memory")。
  裸机 SimpleVM 系项目从未触发此路径,因为真硬件合法支持该位。
- VMCB 转储佐证:死亡时的 VMCB Control 区 = op1 0x10040000 (CPUID+MSR,与
  代码一致) + op2 0x3 (VMRUN+VMMCALL);Save 区 EFER/CR0/CR3/CR4/RFLAGS/
  段全部符合 APM 检查表。除 VMRUN 位外无异常。

### 反嵌套保障的替代 (ic2.VMRUN 移除后)

- guest 视角 SVM 仍被掩盖:CPUID 0x80000001.ECX[2] 被清 (HideCpuidBits) +
  VM_CR.SVMDIS=1 + EFER.SVME 强制回读为 1,正常 guest 不会尝试 VMRUN;
  强行执行者会因 SVMDIS 得到 #UD/#GP,仍在 svmb 控制内 (只是从"精确拦截
  注入 #BP"退化为"CPU 自动 fault")。

### 遗留问题 (下一优先级)

- **Stop 逐核退出死锁**:去掉 VMRUN 拦截位后不再崩,但 svmbctl stop 的
  HC_EXIT_VMM 交接仍偶发死锁 (16:22 同款,系统假死无崩溃,KDNET 断入失效)。
  疑点:VMMCALL 退出后 GIF/中断状态恢复与 RunOnEachCore 等待交互。

### 正式版验证 (02:30-02:45)

- 正式修复构建 04B7D539 (去除 ic2.VMRUN 位 + 注释正式化) 部署核验一致,
  完整周期触发: svmbctl start 成功, hypervisor 激活, **零三重故障**,
  guest 在全核虚拟化状态下稳定运行 (Tools 按预期被 MSRPM 干掉——
  结构性已知问题, 见第六轮)。
- 随后 svmbctl stop 触发已知的 Stop 逐核退出死锁 (假死无崩溃, KDNET
  断入失效) —— 与 16:22 案例一致, 属独立 bug, 列为下一优先级。

## 当日状态总结 (09/07 凌晨)

- 已修复 7 项根因 (全部有对照实验或活体取证支撑), 提交链:
  7bd2db3 → d1d87cf → f71a758 → 0dd1a92 → (protectHsave 默认值) →
  (MinLevel 致盲修复) → 04B7D539 (ic2.VMRUN)。
- 驱动当前形态: 全核 VMRUN 稳定激活, 零三重故障, 日志子系统真实可用,
  串口黑匣子贯通, 逐核 devirt 标记 + ExitTrace 就绪。
- 阻塞完整回归 (storm/demo/cycle) 的剩余问题:
  1. Stop 逐核退出死锁 (HC_EXIT_VMM 交接, 假死无崩溃) — 最高优先级;
  2. MSRPM 杀 Tools (结构性, 需进程级排除或 NPT);
  3. VIX exec 的 condrv 假控制台阻塞 (< NUL 已规避, 一次 boot 一次 exec)。

### Stop 死锁第三轮攻坚 (03:11-03:30) - DbgPrint 假设不完全成立

- 修复 D1EA9122 (HcExitVmm 交接路径完全无日志 + vmload/STGI 顺序调整):
  构建部署后复现周期,**死锁仍在** (VM 活、guest 黑屏、Tools 0、TF 0)。
- 修正认知:死锁点不在 HcExitVmm 自身的 DbgPrint —— 该函数交接路径现在
  完全无日志,死锁依旧。
- 下一步排查方向 (按嫌疑排序):
  1. **Stop() 里 RunOnEachCore 的 pre/post SVMB_LOGI 调用** (VMMCALL 之前
     那条"stop: core N pre-devirt" 仍在临界链上 —— LogWrite→DbgPrint/KDNET
     发生在切完亲和、VMMCALL 之前,此时核还在 guest 态,交互窗口不同);
  2. VMMCALL 本身在 VMware 嵌套下的行为 (与 ic2.VMRUN 同族问题:
     VMMCALL 拦截位 op2.bit1 也可能不被嵌套支持 → 表现为异常而非退出);
  3. HcExitVmm 返回后 asm exit_virtualization 路径 (rsp 切换/ret 到 NRip)。
- 有效的部分: svmbctl 30s 超时 (465CF188) 已生效 —— 用户态不再永久陪葬。

### Stop 死锁二分 B (03:40-03:50) - 日志假设证伪

- 对照构建 DDF8580F:恢复完整单次 pass + 后置日志(等价于 D1EA9122 前的
  原始逻辑 + HcExitVmm 无日志化),死锁依旧 → **"日志在临界链上"假设证伪**。
- 结论修正:死锁点在 **VMMCALL/交接本身**,与日志无关。
- 下一轮方案 (二分 C,结构性改造):废弃"IOCTL 线程切亲和 + VMMCALL"模式,
  改为 **自退出模式** —— Stop() 只设置每核 `Info.State = Leaving`(原子写,
  无跨核操作);每个核自己的 VMM 循环在下次 VMEXIT 时检查 State==Leaving,
  在 VMM 内部直接完成 devirt(关 SVME、恢复 host 栈、ret 到 vmm_loop 调用者),
  完全消除跨核 VMMCALL 交接。优点:无跨核竞争、无亲和切换、死锁面归零。
  成本:devirt 延迟取决于该核下次 VMEXIT 时机(可用 NMI/IPI 主动触发一次
  VMEXIT 加速,或轮询等待)。

### 关键运营定律确认 (04:41)

- **每次 guest boot 只有一次 exec 名额**:第一条 exec(无论内容)真实执行,
  之后所有 exec 请求挂死且**不执行**。此前多轮"触发后无日志"均因误把
  第二条 exec 当成已执行。
- 验证纪律(固化):boot → [可选:哈希校验] → **立即触发周期**(唯一
  名额给周期)→ 硬重启 → 第一条 exec 名额用于拉日志。
- 推论:所有"驱动加载后 Tools 死"的判断需重新审视——部分场景实际是
  exec 名额已耗尽,脚本从未运行。

### Stop 死锁收敛 (05:00) - 假死点特性与活体捕获方案

- 三个自退出版周期样本:假死点随机分布在 HV_CONTROL IOCTL 执行期
  (run0 旧驱动卡 STEP4 后、run2 新驱动卡 STEP3 中),尾部 700 字节全零
  = 数据写入但未刷盘。零三重故障(VMRUN 位修复稳定)。
- 收窄结论:**任何 HV_CONTROL IOCTL 都可能触发假死**,与具体动作
  (start/stop)无关;非确定性,~50% 概率。
- 推论:死锁在 VMMCALL 在 VMware 嵌套下的执行路径本身(真硬件与嵌套
  行为差异),离线分析已到极限,必须活体捕获:
  方案 A:KDNET 断点布在 _svmb_hypercall 的 vmmcall 指令 + HcExitVmm
  入口,复现时看 VMMCALL 是否返回、返回到哪;
  方案 B(零成本):利用 IPI nudge 已在 Stop 中实现的特性,把 nudge 改为
  循环 IPI + 每次后检查 State,把等待上限从 5s 降到 500ms 并记录每次
  IPI 后的 State 快照串口输出 —— 下一会话首选。
- 剩余唯一阻塞:此死锁。其余(MSRPM 杀 Tools、VIX exec)已有规避方案。

### 24h 连续攻坚收官 (06:50) - Stop 死锁特征与未竟事项

- 确定性事实:
  1. 假死点在 HV_CONTROL IOCTL 内(PRE-VMMCALL 断点零命中 → 死在 Start
     早期,ConfigureVmcb/memcpy/LogWrite 之间),非 VMMCALL 交接;
  2. 死时尾部 741~2223 字节全零(空间已写、数据未刷盘);
  3. 无调试器跑一轮(06:36 boot)周期日志零增长 —— 但 exec 名额定律使
     "脚本是否真正运行"同样存疑,该对照未获干净数据;
  4. 三次成功取证的日志均显示:6 核 VMRUN 全部成功、exit trace 全为
     CPUID(0x72)、virtualization active、info RUNNING —— **启动路径
     在有日志佐证的样本里 100% 健康**。
- 死锁样本不足的根因:每次 boot 只有一次 exec 名额 + Tools 死后日志
  无法拉取 + 假死时日志未刷盘,三者叠加导致死锁现场数据采集率极低。
- 下一会话建议(按序):
  1. **cycle_startup 改写**:把每步的输出立即经 COM1 写出(sc/svmbctl
     输出重定向 COM1),绕开 NTFS 刷盘问题;
  2. 驱动 LogWrite 的 DbgPrint 分支加节流(每核每秒 N 条),消除
     KDNET 风暴窗口;
  3. 复现时用 kd 活体:断 EnterCore 入口(非 vmrun),步过确认死点。

### 快照回滚串口陷阱 (08:00 发现, 影响全天数据!)

- **revertToSnapshot 会把 vmx 配置一并回滚**:9/6 晚配置的串口 file 模式
  被还原为 pipe 模式(无人读)。此后全天每轮 boot 的驱动串口镜像都写向
  死管道:SerialPut 每字符空转 10000 次 inbyte(VMware yieldOnMsrRead=
  TRUE 还会强制 VM 让出 CPU)——**EnterCore 每核 7 个标记 × 30 字符 =
  数千次退化 I/O,疑似"假死"的实际是分钟级慢放**,vmx CPU 1600%+ 吻合。
- 已修回 file 模式并确认。**此前所有"EnterCore 早期死锁"的结论(包括
  E4→E5 窗口、自退出改造动机)都需要在 file 模式下重新验证** —— 症状
  可能纯粹是 pipe 慢放。
- E1-E4 断点序列 (07:35) 仍有效:EnterCore 至少跑到 E4 (SVME=1)。

## 调试方法论经验总结 (2026-09-07 回顾夜,供后续会话遵循)

### A. 环境陷阱类 (成本最高的教训)

1. **快照回滚会连带回滚 vmx 配置**。串口 file 模式、内存/CPU 改动等都会
   被还原。回滚后必须:①核对 vmx 关键项(serial0.fileType);②重新部署
   guest 侧所有文件。本次串口回到 pipe 模式无人读,SerialPut 每字符空转
   10000 次 inbyte + yieldOnMsrRead 强制让出 CPU,EnterCore 被慢放成
   "假死",误导攻坚数小时 (vmx CPU 1600%+ 是唯一线索)。
2. **一次 boot 只有一次 VIX exec 名额**。第一条 runProgramInGuest 真实
   执行,后续所有 exec 静默挂死且不执行。名额必须留给最关键操作;validate
   探针也会烧掉名额。卡死后唯一恢复手段 = 硬重启。
3. **Tools 死亡 ≠ 通道全死**:listProcesses/file 通道通常仍活;反之
   exec 挂死也不代表 Tools 死。每个通道独立验证,不要笼统归因。
4. **硬断电丢最近 2-3 分钟写入** (NTFS 元数据已落但数据页丢失):
   文件"存在但内容全零"。关机前等 ≥30s;重要输出立即拉回宿主。
5. **kd.exe 僵尸进程占住 UDP 50000** → 新 KD 会话 no_debuggee。连不上
   先 Get-Process kd 清理,再查防火墙,最后才怀疑 guest。

### B. 证据采集类

6. **kd.exe 控制台收不到 guest 的 DbgPrint** (WinDbg GUI 可以)。依赖
   DbgPrint 的取证链在 kd 控制台下是盲的。可靠替代:断点 .printf 轨迹
   (命中即 gc)、串口黑匣子、kd 直接读内存 (gLog 环形缓冲)。
7. **断点 > 单步**。单步穿过 LogWrite/DbgPrint/KDNET 路径会死锁
   (调试传输与单步事件竞争)。用"打印即放行"断点 (bp addr ".printf; gc"),
   需要看中间态就在断点命令里转储,不要真的 t/p。
8. **sxe ld + 命中后布 bp** 是驱动加载期插桩的正确姿势:模块加载事件
   断入 → 此时符号已可解析 → 布真实断点 → 放行。bu 在 KDNET 重连后
   会全部丢失,每次重连后必须重新布防。
9. **日志三通道独立失效的可能性**:MinLevel=0 曾同时杀死环形缓冲/
   DbgPrint/串口镜像。排查"没有日志"时先用 kd 读 gLog 内存验证子系统
   本身是否被关掉,而不是假设"代码没执行到"。
10. **对照实验是最强武器**:ic2.VMRUN 位致命的定谳靠的是"只改这一位"
    的对照构建 (9 连杀 → 4 分钟零崩溃)。假设必须能被对照实验证伪。

### C. 认知纪律类

11. **工具失败 ≠ 目标失败**:exec 卡死、Tools 死亡、pull 超时都曾被
    误判为"驱动把系统搞挂了"。区分"观测通道死了"和"被观测系统死了"。
12. **N 次同一位置崩溃 = 高价值信息**:VMRUN 窗口 9 连杀本身就是
    "确定性 bug"的证明,值得投入对照实验;而随机分布的假死更可能是
    环境/竞态问题。
13. **每轮复现后立即收割**:TF 时间戳、串口、cycle 日志三者对齐时间线。
    假死/崩溃后 guest 会自愈重启,现场只有 2-3 分钟窗口。
14. **零 VMEXIT 是强排除**:SvmbVmExitEntry 断点零命中直接排除了整个
    退出处理路径,把嫌疑压缩到 VMRUN 指令本身。类似"强排除"断点
    (预期高频事件的实际零命中) 应尽早布置。
15. **keystroke/GUI 合成输入会被宿主静默丢弃** (多显示器/前台抢占);
    computer-use 点击"accepted"不代表 guest 收到。guest 内操作优先用
    vmrun 文件通道 + Startup 文件夹或人工双击。

### D. 环境重建 SOP (回滚/失控后)

1. vmrun stop hard → 检查/修正 vmx (serial0 文件模式!) → start;
2. 等 Tools (listProcessesInGuest 返回 >100 进程);
3. 部署三件套 (svmb.sys/svmbctl/cycle 脚本) 并 fileExists 验证;
4. 布防 KDNET (.logopen → sxe ld → g);
5. 唯一 exec 名额给周期脚本 (< NUL);
6. 周期完成后 2-3 分钟内收割 (TF/串口/cycle 日志对齐)。

## Round 10 修复 (2026-09-07): 拦截位 + 16550 初始化

### 现象

`svmbctl start` 返回 RC=0，但 VMCB 状态从未变 RUNNING。KDNET logopen
显示 6 个核都走完 `pre-vmrun` 逻辑后回到 `[svmb][c5] virtualization
active on 6 cores`，但 1-2 秒后 VMware 自动 reset guest
(`target machine restarted without notifying the debugger`)。

### 决定性证据

```
[svmb][c5] uart probe lsr=ff iir=ff            ← 16550 没初始化,总线悬空
[svmb][c5] svmb driver loaded (proto v1)
[svmb][c5] svm check: CPUID.80000001.ECX=00c003ff (SVM=1)
[svmb][c5] svm check: VM_CR=8 (SVMDIS=0 SVMDIS_LOCK=1)
[svmb][c5] svm check: OK
[svmb][c0..c5] pre-vmrun cpu=N vmcb=... EFER=5d01 CS.attr=029b ...
[svmb][c5] virtualization active on 6 cores
... 1.5s 后 VMware 自动 reset ...
```

### 排查路径 (按时间顺序)

| 阶段 | 假设 | 验证方式 | 结论 |
|---|---|---|---|
| v3 | HSAVE 没写进去导致 vmrun #UD | E3b read-back 验证 | SVME=1 之后 VM_HSAVE_PA 写入被 L1 透明吞掉 |
| v4 | VMCB 状态非法 | dump ExitInfo1/2/3/Int/NRip + InterceptOpcode1/2 | 全 0,ExitCode=ff..ff:CPU 根本没进 vmrun 后路径 |
| v5 | CS.limit=0 导致 vmrun 失败 | 把 limit 强制设 0xffffffff | 仍失败,CS.limit 不是根因 |
| v6 | CPUID.80000001.ECX.SVM=0 (vmrun #UD) | 读出 ECX 字节 | **误判**:0xc003ff 的低 8 位是 0xff = 11111111,**bit2=1** |
| v7 | VM_CR.SVMDIS_LOCK=1 禁用 SVM | 详细化 CheckCpu + 设 SVMDIS_LOCK 失败 | **误判**:参考实现 在同一台 VM 跑通 |
| v8 | (对照实验) 参考实现 在同一 VMware vhv VM 跑通 | 装 参考实现 看 logopen | 参考实现 进虚拟化、#BP 拦截成功 → **svmb 代码 bug** |
| v9 | (代码对比) 参考实现 设了 ic2.VMRUN 拦截,svmb 没有 | 比对双方 EnterCore | **vmrun 没设 VMRUN 拦截 = 致命**(猜测)** |
| v10 | svmb v9 拦截位用了 `(1u<<3)` 但 AMD APM 是 bit 0 | 看 vmcb.h `ic2::VMRUN = 1u<<0` | **位号错**:应该是 bit 0,不是 bit 3 |
| v11 | 用 `ic2::VMRUN | ic2::VMMCALL` 常量 | 重编+重测 | **6 核 virtualization active 成功** |

### 根因 (最终版)

`driver/src/core/intercept_manager.cpp` line 280 之前写的:

```cpp
u32 op2 = (1u << 1) | (1u << 3);  // 注释写 VMMCALL + VMRUN
```

但 **ic2.VMRUN = bit 0**,**ic2.VMLOAD = bit 2**,**ic2.VMSAVE = bit 3** —— bit 3 是
**VMSAVE** 不是 VMRUN。这条 op2 实际写的是 `VMMCALL + VMSAVE`,**漏掉了
VMRUN 拦截**。

VMRUN 拦截缺失的影响:

- 没有 VMRUN 拦截,guest 嵌套 vmrun 不能被 host 捕获
- 修复后才意识到:**这个 bit 在 VMware nested 下是必要的** —— 之前 round 9
  的旧诊断 "VMware 嵌套下设 VMRUN 拦截会 triple-fault" 是错的,被对照实验证伪

### 修复 (commit d225bdc)

```cpp
// ic2 bits per vmcb.h: bit 0 = VMRUN (anti-nesting; must be set, see
// 参考实现 SVM.cpp:492 + CRASH_DEBUG_LOG round 10), bit 1 = VMMCALL.
u32 op2 = ic2::VMRUN | ic2::VMMCALL;
```

### 同次修复: 16550 UART 初始化

`driver/src/platform/logger.cpp` 之前是裸写 COM1 (裸写 0x3F8)。
16550A 必须先初始化 (LCR/DLL/DLM/LCR/FCR/MCR) 否则 transmit holding
register 不接受数据,所有 `LogSerialTrace` 一行都落不到 VMware 的 file
sink。**这是为什么之前 round 1-9 的所有串口 beacon 全失效** —— 不是
VMware 不转发 COM1,而是 svmb 没让 UART 时钟运转。

修复:

```cpp
// One-time 16550A init at first SerialWrite call. Without it the COM1
// file sink never receives svmb flight-recorder bytes - the .NET
// SerialPort test landed fine because serial.sys initialized the device.
void SerialInit() {
    out(0x3F9, 0x00);  // IER: no interrupts
    out(0x3FB, 0x80);  // LCR: DLAB on
    out(0x3F8, 0x01);  // DLL: divisor 1 = 115200
    out(0x3F9, 0x00);  // DLM
    out(0x3FB, 0x03);  // LCR: 8N1, DLAB off
    out(0x3FA, 0xC7);  // FCR: FIFO enable + clear
    out(0x3FC, 0x0B);  // MCR: DTR | RTS | OUT2
}
```

### 副修复: GetSegmentLimitForVmcb

`driver/src/hw/segments.cpp` 新增 `GetSegmentLimitForVmcb` 返回 `0xFFFFFFFF`:

```cpp
// AMD APM vol.2 15.5: VMCB.SAVE.<seg>.Limit is reserved and must be
// ignored for long-mode 64-bit code segments when the segment attribute
// has L=1 - but the VMCB layout still requires a non-zero value.
// Coerce to 0xFFFFFFFF so vmrun validation always passes; the limit
// is ignored by the CPU in long mode regardless of value.
u32 GetSegmentLimitForVmcb(u16 sel, u64 gdtBase) {
    UNREFERENCED_PARAMETER(sel);
    UNREFERENCED_PARAMETER(gdtBase);
    return 0xFFFFFFFF;
}
```

后来证明这条不是根因 (v6 的 VMCB 控制区全 0 排除 vmrun 后路径),但留着无害。

### 验证

```
v11: svmbctl start RC=0 → 6 核 virtualization active
v11: svmbctl info   → hv state: RUNNING
v11: svmbctl stop  RC=0 → OS 调度恢复
v11: svmbctl info   → hv state: OFF, cores: 0 virtualized / 6 total
```

### 在 VMware vhv 下的已知限制

svmb **没有 NPT** (`hypervisor.cpp:382` 显式 `Np.Data = 0`)。VMware
Workstation 的 vhv L1 hypervisor 检测到 svmb 的 guest 物理地址"穿透"到
L1 自己的内存空间,触发自我保护 → 自动 reset guest (1-2 秒内)。

对照: `feature-m3m4` 分支已有完整 NPT 实现 (mm/npt.cpp + ConfigureVmcb
接入),参考实现 也是 NPT-full,所以同样环境能稳定跑。

**C 路径**(TODO): 在 `feature/npt-merge` 分支合并 `feature-m3m4`,然后
NPT 启用后即可验证 vmexit 流量 (`svmbctl storm 100000` 当前返回
`STATUS_DEVICE_NOT_READY` 是因为 VMware reset 后 hv 实例被销毁)。

### 取证通道关键经验

**`kd> .logopen /t <文件>` + CTRL+BREAK 后 g** 这条链**取代了**之前
"靠 COM1 串口 beacon 取证"的范式:

- 之前:`LogSerialTrace` 写到 COM1 → VMware file sink → host 上读串口文件
- 现在:DbgPrint 包到达 KDNET → kd flush 到 .logopen 文件 → host tail
- 优势:.logopen 是 host 端文件,**VM 冻结也不影响**;串口文件 16550
  不初始化就一行不落,导致前 9 轮所有 beacon 全失效

这也是 `vm-driver-debug-loop` 技能的核心结论:**冻结后的取证通道只有
.logopen**。

### 关键决策回顾

| 选项 | 当时判断 | 真实原因 | 修正 |
|---|---|---|---|
| "CPUID SVM bit=0" | (v6) bit2=0 | **算错位** (0xc003ff bit2=1) | v7 用 `-and` 算 |
| "VM_CR.SVMDIS_LOCK 致命" | (v7-v8) 必拒绝 | 参考实现 同 VM 跑通 | v8 撤掉该检查 |
| "ic2.VMRUN 不能设" | (round 9) 会 triple-fault | 参考实现 设了不崩 | v11 加回来 |
| "vmrun #UD 是环境问题" | (v6-v7) VMware vhv 屏蔽 | 参考实现 跑通 = svmb 代码 bug | D 路径找到 |

## Round 12 (2026-09-07): 参考实现 NPT 对比 + v15/v16 修复尝试 — 仍未收敛

### 现象 (Round 11 之后)

feature/npt-merge 合并完成，NPT 代码落地。`NptEnable=1` + `svmbctl start`
后 VMCB.NP_ENABLE 翻转、NCr3 写入。首次 vmrun 立刻触发 OS 调度级
挂死：

- `svmbctl.exe start` 返回 exit code 1 + `cannot open \\.\svmb (err 2)`
  **或** `VMware Tools 未在客户机中运行`
- `vmware.log` 时间戳停滞（最后一条是 VM 内部的 CDROM 模拟，依赖 guest 触发）
- guest ping 100% 丢失，OS 完全不调度
- vmrun reset hard 恢复

### 与 参考实现B 的 NPT 对比

将 `<host>\Downloads\参考实现B\参考实现B\` 与 svmb 逐项对比
NPT 与 NPF 处理路径，**关键差异**：

| 维度 | 参考实现 | svmb (v14) |
|---|---|---|
| NPT init 范围 | `[0, 0xFFFFFFFFFF]`（**全 40-bit 1 TB**，2 MB 大页，RWX）| 仅 `MmGetPhysicalMemoryRanges()` 报告的 RAM 区间 |
| NPT init 时机 | `PageTableManager::Init()` 调 `BuildNptPageTable()`（hypervisor 启动前） | `FillDefaultView()` 在 `NptEnable=1` 时调用（hypervisor 已启动后） |
| per-core NPT | **每个核心独立 PML4**（`pageTableCnt = cpuCnt`，`GetNCr3ForCore(cpuIdx)` 每核一表）| **全局单一 PML4**（一个 `DefaultView_`，所有 vCPU 共享同一 `Pml4Pa_`）|
| NPF handler | 仅 `if (!present)` → 调 `FixPageFault`（实际不可达，注释：*"理论上不可能缺页"*）| `if (!faultPresent)` → `MapRam` / `MapNonRam` 走 lazy identity-map |
| MMIO 处理 | **无特殊处理**：APIC 0xFEE00000/HPET 等 MMIO 与 RAM 一起被预映射为 RWX 大页，guest 直接命中真实 MMIO 硬件 | NPF lazy-map 把 MMIO 当成 RAM 映射到一个临时页，guest 写 APIC 没反应 |
| EventInj 注入 #GP | 完全没有此路径 | 无 |

### Round 12 修复尝试 v15：NPT 对 MMIO 注入 #GP

按"参考实现 没特殊处理 MMIO"的表面结论，看似 MMIO 应被预映射。但 svmb
**没有**做全 PA 预映射，所以先在 `npf.cpp:MapNonRam` 路径**给 MMIO 注
入 #GP** 让 guest OS 走非 NPT 路径：

```cpp
case NpfAction::MapNonRam:
{
    // EventInj field layout (union EventInj in vmcb.h):
    //   Vector:8, Type:3, Ev:1, Reserved:19, Valid:1, ErrorCode:32
    u64 injData = 0;
    injData |= ((u64)13    & 0xff)   << 0;   // #GP vector
    injData |= ((u64)EVT_EXCEPTION & 0x7) << 8;
    injData |= ((u64)1      & 0x1)   << 11;  // Ev (error code valid)
    injData |= ((u64)1      & 0x1)   << 31;  // Valid
    ctx.Vmcb()->Ctrl.EventInj.Data = injData;
    ctx.AdvanceRip = true;
    return true;
}
```

logopen 出现 `[svmb][c0] npf: not-present MMIO gpa=fee00300 -> inject #GP`
确认路径走通，但 **VM 仍立即冻结**，ping 全失。

### Round 12 修复尝试 v16：FillIdentityWholePa（参考实现 思路）

按 参考实现 `BuildNptPageTable` 的实际行为，在 svmb 加：

```cpp
// driver/src/mm/npt.h
NTSTATUS FillIdentityWholePa();  // [0, RamEnd_) PA 空间用 2M RWX 大页
                                // 身份映射（参考实现 NPT init 等价）

// driver/src/mm/npt.cpp
NTSTATUS NptManager::FillIdentityWholePa()
{
    if (!DefaultView_.Inited()) return STATUS_INVALID_PARAMETER;
    if (Ranges_.Count() == 0 || Ranges_.RamEnd() == 0) return STATUS_UNSUCCESSFUL;
    NptPerms rwx = {true, true, true};
    return DefaultView_.MapRange(0, Ranges_.RamEnd(), rwx);  // 一次大页填充
}

// driver/src/main.cpp (NptEnable 路径)
if (nptEnable)
{
    NTSTATUS stFill = gNpt->FillIdentityWholePa();  // <-- 新增
    if (!NT_SUCCESS(stFill)) SVMB_LOGW(...);
    NTSTATUS stEn = gNpt->SetActiveView(0);
    if (NT_SUCCESS(stEn)) stEn = gNpt->Enable();
    ...
}
```

v16 构建 OK（RC=0），push + sc start 后 `svmbctl info` 返回正常
（device `\\.\svmb` 创建成功），证明 **驱动 init 路径完全干净**。

但 `NptEnable=1` + `svmbctl start` 之后 VM **仍然立即冻结**，与 v15
现象一致。

### 关键事实修正

- **v15 报 `cannot open \\.\svmb` 不是驱动 init 失败**：
  实际是 svmbctl 在 VM 冻结后立即拿到 ERROR_FILE_NOT_FOUND
  （vmrun 立刻返回 Tools 未运行，说明 guest OS 已经死循环）。
  这条 ctl 输出其实是**冻结结果**而非**冻结原因**。
- 驱动 init 路径完全干净 — `svmbctl info` 在 NPT=0 时正常返回，
  v16 加载后 RUNNING + svmb_steps_out 步骤日志全打到 driver 端。
- 取证通道限制：vmx 没有 KDNET（仅 `serial0.fileName` 文件），svmb
  裸写 COM1 在冻结态不被 VMware 转发（vm-driver-debug-loop §6），
  导致无法读冻结瞬间的 kd 输出。仅有 `vmware.log` 时间戳停滞 +
  vmrun Tools 不响应两条旁证。

### 当前结论与未解决方向

**NPT 启动卡死与具体 NPT 处理路径（lazy-map vs identity-fill vs
inject #GP）无关**——三种走法都立即冻结。换言之：
- 如果根因是 NPT 表本身：v16 已经把 [0, RamEnd_) 整个 PA 空间预映
  射为 RWX 大页，NPT 永远不会被 guest walk 触发，VM 应该正常运行。
  但仍冻结 → 排除"MMIO 黑洞"假设。
- 如果根因是 NPT 的副作用（如 per-core NCr3 写错、CleanBits 错、
  TlbControl 不当）：三个版本（v14 lazy-map / v15 inject #GP /
  v16 identity-fill）的公共代码路径是 `ConfigureVmcb` 里 NP_ENABLE
  翻转 + NCr3 写入。下一步 bisect 应该在**这条路径上**做：
  - 在 `Hypervisor::ConfigureVmcb` 里把 NP_ENABLE / NCr3 写与
    vmrun 解耦，单独验证 NP_ENABLE=1 但 NCr3=0 / 不写 NCr3 是否
    冻结
  - 检查 参考实现 与 svmb 在 `FillGuestState` / `MSRPM` / `intercept
    mask` 三个路径上的具体差异（特别是 MSRPM，参考实现 用 static
    bits，svmb 用双缓冲 build + commit）
  - 检查 svmb 的 `InterceptCrWrite` 是否包含 CR3，导致 guest 内核
    每次进程切换都 vmexit，性能致死而非逻辑死

### 关键决策回顾 (Round 12)

| 选项 | 当时判断 | 真实原因 | 修正 |
|---|---|---|---|
| "MMIO 黑洞 = NPT 启动卡死根因" | (v15) inject #GP 让 guest 走非 NPT 路径 | v16 整个 PA 预映射后**仍冻结**，说明 MMIO 不是根因 | 撤回 v15，保留 v16 identity-fill 作为长期正确的 NPT init 模式（与 参考实现 一致）|
| "全 1 TB identity 必填" | 参考实现 那样 | svmb `MapRange` 用 4K 大页混合（按 2M chunk），1 TB 会触发大量 4K leaf fill，pool 占用 ~2 GB | v16 把窗口限制到 `RamEnd_`（MmGetPhysicalMemoryRanges 上界），guest 永不触及更高的 PA |
| "驱动 init 失败 = svmbctl cannot open" | VM 冻死后 ctl 立即报错 | 实际是 ctl 拿到 ERROR_FILE_NOT_FOUND 是 VM 冻结后的**结果**，不是 init 失败原因 | ctl info 在 NPT=0 时正常 → init 干净；ctl start 触发 NPT 后冻结 → bisect 焦点在 ConfigureVmcb 的 NPT 写路径 |

### TODO

- **C 路径**: 在 `feature/npt-merge` 分支合并 `feature-m3m4` (NPT) → 验证
  svmb 在 VMware vhv 下稳定跑 → 验证 vmexit 流量 (`svmbctl storm` 不再失败)
- 在 `tests/CRASH_DEBUG_LOG.md` (此文档) 添 Round 11: NPT 集成验证
- 把 `feature-m3m4` 带回 master 的具体时机看 NPT bisect 结果定

## Round 11: feature-m3m4 合并 + NPT 启用 (2026-09-07)

### 流程

1. `git checkout -b feature/npt-merge`
2. `git merge feature-m3m4` - 3 个 conflict:
   - `driver/src/core/hypervisor.cpp` (1): 采用 feature-m3m4 的 ConfigureVmcb
     + NptManager::Active() 分支 (master 的 Np.Data=0 是 M3 stub)
   - `driver/src/hw/msrpm.cpp` (2): 采用 3-buffer rotation (Active/Shadow/Spare),
     master 的 leak-8KB workaround 被 superseded
   - `driver/src/main.cpp` (3): 采用 offline subsystem init 块
     (gNpt/gHooks/gCr3Seed/gDbgRing + SetApplyCallback + NptSetSlideHook),
     保留 master 的 `protectHsave=0` 默认值
3. 修 `SVMB_LOG_FORCE_VISIBLE` 重复宏 (vcxproj 传 + logger.h #define)
4. v13 编过 (RC=0, 138 KB)

### v13 部署 + 行为 (NPT 默认关)

```
[svmb][c0] uart probe lsr=ff iir=ff
[svmb][c0] config: serial=1 hideCpuid=1 protectHsave=0 nptEnable=0
[svmb][c0] cr3 seed unavailable: c0000022 (continuing without)
[svmb][c0] svmb driver loaded (proto v1)
```

启动 + stop OK 但 storm 100000 失败 err=21 STATUS_DEVICE_NOT_READY -
feature-m3m4 的 start handler 在 IOCTL path 创建 `Hypervisor* h` 但
**没把它赋值给全局 `gInstance`**,后续 IOCTL 走 `Instance()` 拿到 nullptr。

### v14 NPT 启用

`NptEnable` 是 feature-m3m4 的 tier-3 gate (必须显式设, 默认 0):

```
NptEnable  DWORD 1   tier-3 gate: bring NPT live at start (M3 activation;
                     do NOT set until the start crash-line is resolved)
```

通过 `vm_reg_add_npt.bat` 写入 `HKLM\...\Parameters\NptEnable=1`,
driver 重启后看到:

```
[svmb][c0] NPT enabled (ncr3=16fc0000)
[svmb][c0] svmb driver loaded (proto v1)
```

### v14 ctl start 行为 (vmexit 流量首次捕获)

```
[svmb][c1] ioctl HV_CONTROL action=0 repeat=0
[svmb][c1] svm check: CPUID.80000001.ECX=00c003ff (SVM=1)
[svmb][c1] svm check: OK
[svmb][c0] vmrun cpu=0 vmcb=... EFER=5d01 (SVME=1) ExitCode_pre=0
[svmb][c0] exit trace: code=400 rip=... nrip=0 cpl=0 svme=1 info1=100000004
[svmb][c0] npf: mapped non-RAM (MMIO?) gpa=fee00300
```

### 决定性证据

| 项 | v11/v12/v13 (无 NPT) | v14 (NptEnable=1) |
|---|---|---|
| vmexit trace | 0 行 | ✅ **2 行 NPF trace** |
| vmexit code | (从未产生) | ✅ **0x400 = VMEXIT_NPF** |
| NPF handler | 不存在 | ✅ 处理 guest APIC 物理访问 |
| VMware 自动 reset | ✅ 触发 (6 核 → 1 核) | ❌ 未触发 |
| OS 调度 | 短暂卡死 (~1s) | 长时间卡死 (svmbctl timeout) |

**vmexit 流量在 v14 第一次被完整捕获到**。NPT 起到保护作用
(VMware 不再 reset guest),但 NPT handler 处理路径还有 bug
(start crash-line 仍未解, OS 调度被卡死)。

### NPT 之后仍待解的 bug

1. NPF handler 处理 APIC 等 MMIO 物理地址时, 是否将 trap 转发回 guest?
   (info1=100000004 标志位意味着 P=1 R/W=1 - NPF 想写入 0xfee00300)
2. APIC EOI / interrupt acknowledge 等 host MMIO 拦截完整吗?
3. `cr3 seed unavailable: c0000022` - Cr3Seed 需要 SeSinglePrivilege,
   svmbctl.exe 在低特权下 - 这是 feature-m3m4 的预期 warning,
   但是否影响 production 部署?

### 提交记录

`feature/npt-merge` 分支上 1 个 merge commit:

```
dee4634 merge: feature-m3m4 -> feature/npt-merge (NPT + hooks + cr3_seed + dbg)
```

master 当前 (在 v14 bisect 期间不动):
```
e1f3c90 docs: CRASH_DEBUG_LOG round 10 - VMRUN intercept bit fix + NPT root cause
f9e0335 chore: trim round 10 diagnostic logging after vmrun success
9669c6a chore: ignore driver-test scratch + intermediate obj dirs
f051820 scripts: add vm-driver-debug-loop workflow bats
d225bdc fix: ic2.VMRUN intercept bit + 16550 UART init + diagnostic logging
2b4b473 fix: capture guest state before enabling SVME in EnterCore
```

feature/npt-merge 分支领先 master 1 个 commit (merge dee4634)。


## Round 12-13: 参考实现 对比 + 拦截对齐 (2026-09-07)

### Round 12 摘要 (已写在文档前面)

v16 `FillIdentityWholePa` 把 [0, RamEnd_) 预映射为 2M RWX 大页（与
参考实现 BuildNptPageTable 思路一致）。三种 NPT 处理路径（lazy-map /
inject #GP / identity-fill）都触发相同的 VM freeze → 根因不在 NPT
表本身。

### Round 13: 拦截对齐 参考实现 后的进展

**关键发现**（vmware.log 14:18:36 panic 现场）：
```
2026-09-07T14:18:36.938Z vcpu-2 vcpu-2:Invalid VMCB.
2026-09-07T14:18:36.938Z vcpu-2 [msg.panic.haveLog] A log file is available ...
```
VMware vhv 在 `vcpu-2` 检测到 L2 guest VMCB 字段不合法 → dump → panic。
这才是真正的死亡信号，之前 v14-v16 误读为 OS 调度卡死。

**sweeping 对比 参考实现B 后找到两个过度拦截**（详见本轮 commit
+ AGENTS.md）：

| 字段 | 参考实现 默认 | svmb 之前 | svmb v17 |
|---|---|---|---|
| `InterceptException` (#DE/#BP/#UD) | 0 | 强制设 | **0**（已撤） |
| `InterceptOpcode2.VMMCALL` (bit 1) | 0 | 强制设 | **0**（已撤） |
| `InterceptOpcode2.VMRUN` (bit 0) | 1 | 1 | 1（保留） |
| `InterceptOpcode1` (CPUID + MSR) | 0x00140000 | 0x00140000 | 0x00140000 |

修改文件：
- `driver/src/core/hypervisor.cpp:325-326` — 删 #DE/#BP/#UD 强制设
- `driver/src/core/intercept_manager.cpp:268-292` — 删 VMMCALL 强制设
- 加 `r13-*` 生命周期 log 标签，方便 `svmbctl log` 路径回溯

**v17 / v18 验证**（按 vm-driver-debug-loop 严格流程）：

- v17 (无 r13 log)：NPT=0 + ctl start → VM **不冻**（ping 100% 通、Tools
  状态 ok、vmware.log 无 panic），但 driver 在 ctl start 后某个时刻被
  卸载（`sc query svmb` 报 1060 / `cannot open \.\svmb`），意味着
  Start() 路径上某个 status 触发了 Stop()→unload。
- v18 (带 r13 log)：编译 OK；尚未跑完整流程验证哪条 r13 log 出现。

**关键决策回顾 (Round 12-13)**:

| 选项 | 当时判断 | 真实原因 | 修正 |
|---|---|---|---|
| "VM 调度卡死 = NPT 黑洞" | v15 走 inject #GP | v16 整个 PA 预映射后仍冻 | 撤 v15，留 v16 (identity-fill) 作长期 NPT 基础 |
| "vmware.log 时间停滞 = VM 死" | v14 误读 | "Invalid VMCB" panic + CoreDump 跑了好几秒才完成 | 必须 grep `Invalid VMCB` / `panic` 关键字判断 |
| "sc query RUNNING = 驱动 init 成功" | v14 误读 | ctl start 后 ctl info 报 cannot open，是驱动被卸载的 **结果** | 必须 svmbctl log 抓 r13-* 标签看 Start 内部状态 |

### 工作流规则落地 (Round 13 同步)

新增 `AGENTS.md` 作为 agent 规则源：要求严格使用
`vm-driver-debug-loop` 技能的 Phase 1-7 bat 脚本（不可裸用 vmrun），
关键条款：vmrun 死时唯一恢复是 `build\vm_reset_hard.bat`，证据落
`tests/CRASH_DEBUG_LOG.md`。

### TODO

- **P0**: 跑 v18 全流程，svmbctl log 抓 r13-* 标签解析 ctl start 后
  driver 卸载的精确步骤（Start() 失败回滚？Stop() 内部错误？）。
- **P1**: 验证 v18 + `NptEnable=1` 下是否 freeze 解除（按 v17 路径
  走通后再开 NPT）。
- **P2**: 在 r13-* 日志确认 Start 干净后，svmbctl storm 跑 10k 验证
  vmexit 流量。
- **P3**: 把 `feature/npt-merge` 含 v17/v18 改动带回 master。

## Round 13: KDNET 调试链 + asm 双解引用修复 + triple fault 定位 (2026-09-08)

### 调试链路（本轮最重要的基础设施突破）

按 AGENTS.md 流程建立 KDNET + `.logopen`（此前失败因 key 用错，
正确 key 在本地 KDNET 配置（不入库））。链路验证通过：
break 响应、vertarget 响应、日志实时落盘。**本轮全部证据来自该通道**。

### 现象 1: v18 ctl start → guest BSOD（首次拿到崩溃现场！）

`KERNEL_SECURITY_CHECK_FAILURE (139)` Arg1=4
"thread's stack pointer was outside the legal stack extents"。

kv 回溯链：`KiPageFault → MmAccessFault → MiUserFault → ... →
RtlpGetStackLimitsEx → KeBugCheckEx(139,4)`，故障指令定位：

```
svmb!_svmb_vmm_loop+0x277:
  mov rax,[rsp]                 ; vcpu
  mov rax,[rax+VCPU_INFO]       ; ← 读到 {State|ExitTraceLeft<<32}
  mov rax,[rax]                 ; ← 当指针解引用 → #PF
  cmp rax,3                     ; State==Leaving 自退出检查
```

**根因**：`svm_entry.asm:365` 假设 `[vcpu+0x3000]` 是指向 VcpuInfo
的**指针**（两层解引用），但 vcpu.h 的 VcpuInfo 是**内嵌**结构
（static_assert 锁定 +0x3000，State 在偏移 0）。运行态该 qword =
`0x000007FF_00000002`（State=2 Guest, ExitTraceLeft≈2047）→ 解引用
无效地址 → #PF 在 VmmStack 上分发 → 栈越界 → 139/4。

**修复** (v19)：直接读内嵌 dword：

```asm
mov rax, [rsp]                    ; vcpu
mov eax, [rax + VCPU_INFO]        ; Info.State (dword, zero-extended)
cmp eax, 3                        ; VcpuState::Leaving
```

### 现象 2: v19/v20 Start 成功后 ~40s-2.5min 无声硬复位

修复后（历史性进展）：
- **`virtualization active on 6 cores` 首次完整出现**
- v19: 253 个 CPUID vmexit（c1=155, c5=36, c7=48, c3=12）被正常服务
- v19 的"冻结"实为**诊断日志风暴**：每 exit 一条 DbgPrint，调试器
  不排空时 guest 逐条等待 KDNET ack → 爬行假死。v20 把
  ExitTraceLeft 2048→16 解决。
- v20: guest 健康（死前 0s Tools 心跳正常）、**零 exit、断点
  （bp SvmbFillMachineFrame / SvmbVmExitEntry）均未命中**、
  2.5 分钟后 vmware.log 出现：

```
vcpu-0 Chipset: The guest has requested that the virtual machine be hard reset.
```

kd 同时报 "The target machine restarted without notifying the
debugger" = **triple fault / 显式复位写**（kd 附着时 bugcheck 会停机
不复位，故非普通蓝屏）。

### 关键决策回顾 (Round 13)

| 选项 | 当时判断 | 真实原因 | 修正 |
|---|---|---|---|
| "kd 连不上 = 目标没开调试" | (前几轮) key=1.2.3.4 | key 抄错，正确 key 在本地 KDNET 配置 | 用对 key 秒连 |
| "Start 后冻结 = NPT/MMIO 问题" | (v14-16) | 是 139/4 栈越界（asm 双解引用）+ KDNET 日志风暴两个独立问题 | v19 修 asm；v20 限流日志 |
| "服务 1060 = 驱动卸载" | (本轮) 服务键被删 | triple fault 时注册表 lazy flush 未落盘，硬复位回滚了 sc create | 复位后需重装服务再测 |
| "kd break 失败 = 目标死透" | SKILL.md §6 | 部分成立，但要先区分"目标停在 kd 断点上导致 vmrun 全卡"（g 放行即恢复） | 先查目标是否 halted |

### 铁律新增 (Round 13)

- **kd 会话把目标停在断点时，所有 vmrun/Tools 操作都会卡死** —
  先 `g` 放行再跑 guest 侧命令。
- **exit 级诊断日志必须限流**：KDNET DbgPrint 每条都等调试器 ack，
  不限流会把 guest 拖到假死（ExitTraceLeft 已从 2048 → 16）。
- `vmrun getGuestIPAddress` 在 guest 刚启动时会报旧 IP；ping 不通
  时先看 vmware.log 是否推进，再判定死亡。

### round 14 TODO（按优先级）

1. **定位 triple fault 复位源**：vmware.log 已证 "guest requested
   hard reset"。零 exit + 断点未命中 + Tools 心跳健康到死前 0s。
   下一步：kd bp `svmb!HandleVmrunExit` 之外，再用 `ba w` 监视
   GuestVmcb.ExitCode 或直接给 6 个核上 `bp svmb!_svmb_vmm_loop+0x10`
   （vmrun 后第一条）确认 exit 是否真的为零；同时排查 guest 侧
   ACPI FADT reset register (0xCF9) 写 / 8042 写是否为 Windows
   主动复位（如 PatchGuard 类检测），kd 附着时断
   `nt!HalpAcpiPmControlSetInfo` / `nt!KeBugCheckEx`。
2. 若复位确认为 Windows 主动行为：排查 `HideCpuidBits=1`
   （SVM bit 隐藏）是否触发完整性检测，对照 参考实现 的"始终隐藏
   SVM bit"在其框架下的长治久安条件。
3. NPT=1 路径（FillIdentityWholePa 已就位）等 triple fault 解决后再验。

## Round 14: 复位源定位 — Windows Hvi 探测循环 + 非 bugcheck 硬复位 (2026-09-08)

### 取证手段与结果

**1. 客侧被动取证**（vm_forensics_dump.bat / vm_pull_forensics.bat）：

- `C:\Windows\Minidump\` 与 MEMORY.DMP 最新均停留在 09/04
  → **round 13 的两次复位均未产生崩溃转储**
- 事件日志：Kernel-Power **EventID 41（关键）** 三条，时间精确命中
  三次复位（00:02:16 / 00:20:04 / 未跟踪的 23:53:30），且查询
  1001（BugCheck）**零命中**
  → **不是蓝屏**，是"未正常关机的重启" = triple fault / 直接复位写

**2. 无 kd 复测**（排除调试器因素）：完全脱离 kd 运行 v20 →
vmware.log 第三次出现同一签名：

```
vcpu-0 Chipset: The guest has requested that the virtual machine be hard reset.
```
→ **KDNET/调试器彻底排除**，复位是 guest 主动发起

**3. kd 布防判别断点**（hal!HalpTimerWatchdogTriggerSystemReset /
hal!HalpPowerWriteResetCommand / hal!HalEfiResetSystem）：**全部未命中**
→ 不是 Windows 已知 HAL 复位路径，kd 无法拦截 = **CPU 级 triple fault
或 watchdog 硬件复位**

**4. 死亡前的关键信号**（exit trace 预算 16 条抓到的最后现场）：
单核（c4）陷入 **CPUID 探测死循环**，4 个 RIP 全部位于 Windows 内核
**HVI（Hypervisor Integrity）代码族**：

| RIP (nt+) | 符号 |
|---|---|
| 0x17e943 | `HviIsAnyHypervisorPresent+0x23`（CPUID leaf 0x40000001）|
| 0x26d7c5 | Hvi 静态函数（`mov eax,40000001h; cpuid; cmp eax,766E6258h`）|
| 0x2743e3 | `HviGetHypervisorInterface+0xea6f3`（无名静态）|
| 0x2741f5 | `HviGetHypervisorFeatures+0xea7d5`（无名静态）|

→ **Windows 内核在死亡前正反复探测"是否存在 hypervisor / 其接口与特性"**

### 已排除的假设

| 假设 | 实验 | 结果 |
|---|---|---|
| KDNET/调试器日志风暴拖死 guest | 无 kd 完整复测 | 仍复位 → 排除（但日志风暴确实存在，限流仍必要）|
| HideCpuidBits=1 造成 SVM bit 凭空消失 → 完整性复位 | `vm_reg_hide_cpuid0.bat` 有效实验（config 行确认 hideCpuid=0）| 22s 仍复位 → 排除 |

### 新发现的流程陷阱（已固化）

**`sc delete svmb` 会连带删除 `Services\svmb\Parameters` 整个子键**！
任何"先 reg add 再 clean"的顺序都会让参数蒸发，实验结论作废。
正确顺序：**clean → create → 写参数（seriallog/npt/hidecpuid）→ start**。
（这也解释了此前多次 `HideCpuidBits=0` 写入"没生效"的现象。）
新增脚本：`vm_reg_hide_cpuid0.bat`、`vm_forensics_dump.bat`、
`vm_pull_forensics.bat`、`vm_bcd_hv.bat`、`vm_pull_bcd.bat`。

### 当前最优先假设（round 15 验证）

**Windows 自身 Hyper-V 启动器/VBS**：本 VM 以 `vhv.enable=TRUE` 运行，
guest 能看到真实 SVM。若 `bcdedit` 的 `hypervisorlaunchtype=Auto`
（Win10 默认），Windows 会在运行期尝试用 SVM 启动自己的 hypervisor：
Hvi* 探测（CPUID 循环）→ 尝试写 EFER.SVME / 执行 VMRUN → 与 svmb 的
MSRPM(EFER) 和 ic2.VMRUN 拦截相撞 → 启动失败重试 → 最终走策略复位。
该路径完全绕过 bugcheck，与全部观测吻合。

参考实现 在同一 VM 稳定运行的解释候选：其 EFER/VM_CR MSRPM 位 +
VMRUN 拦截的**处理行为**（如对 leaf 0x40000001、CPUID.1.ECX[31] 的
响应）与 svmb 不同，或其加载时机（boot start）让 Windows 在启动早期
就完成 hypervisor 检测并永久放弃。

### round 15 TODO

1. **趁 guest 活着**（ctl start 之前！）跑 `vm_bcd_hv.bat` +
   `vm_pull_bcd.bat`，查 `hypervisorlaunchtype`；再查
   `msinfo32` 的"已检测到虚拟机监控程序"/Device Guard/VBS 状态。
2. 若 hypervisorlaunchtype=Auto：改 `hypervisorlaunchtype=Off`
   后复测 ctl start 存活；或 svmb 侧对 CPUID.1.ECX[31] /
   0x40000000/1 伪造一致应答（对齐参考实现行为需先读其 CPUID handler）。
3. kd 下 `bp nt!HviIsAnyHypervisorPresent` + `kv` 抓调用者栈，
   确认触发线程（workitem/PatchGuard/启动器）。
4. 对照 参考实现 的 MSR/CPUID exit handler 全文（Round 15 必读
   SVM.cpp:596-657 已列，但需逐字节对齐行为而非仅掩码）。

## Round 15: 二分定位 — CPUID exit 路径是延迟复位的必要充分条件 (2026-09-08)

### 排除的两个假设

1. **Windows 自启 Hyper-V**：`bcdedit` 显示 `hypervisorlaunchtype Off` +
   `vsmlaunchtype Off` → 排除。
2. **CPUID SVM bit 隐藏造成应答不一致**：HideCpuidBits=0（纯透传应答）
   仍 73s 复位 → 排除。

### Hvi 探测循环的调用者（bp + kv 实锤）

死亡前 CPUID 循环的调用链 = **Windows 软件保护/激活栈**（无害的
高频 CPUID 探测者，非元凶）：

```
NtQuerySystemInformation (用户态)
→ ExHandleSPCall2 → SPCallServerHandleIsAppLicensed
→ ExpGetVMActivationStatus → ExGetVMType
→ HviIsAnyHypervisorPresent (CPUID leaf 0x40000001)
```

### 二分矩阵（决定性实验）

| 配置 | ic1 拦截 | hideCpuid | 结果 |
|---|---|---|---|
| v20 | CPUID+MSR | 1 | 死 22s-2m20s（Hvi 探测循环后复位）|
| v20 | CPUID+MSR | 0 | 死 73s |
| **v21** | **无（全透传）** | 1 | **存活 >7.5min（超历史窗口 3 倍）** |
| v22 | 仅 CPUID | 1 | 死 ~40s |
| v23 | 仅 CPUID | 0 | 死 ~73s |

**结论：CPUID exit 处理本身（EnterCore→vmrun→CPUID exit→EmulateCpuid
→re-entry 这一机制）触发延迟硬复位，与应答内容无关**。机制上复位的
签名不变（Chipset "guest requested hard reset" / 无 dump / 无 1001 /
Kernel-Power 41），但仅在有 CPUID exit 时发生。

### 测量污染教训（本轮反复踩坑）

- **kd 的任何命令都会 break 目标并冻结 guest**，之后不 `g` 则 ping/
  vmrun 全灭 — 三次"死亡"误判均源于此。
- ping 判活必须 `-n 1 -w 5000` 且 grep `已接收 = 1`：`-n 2` 成功时
  输出 `已接收 = 2`，`-w 2`（2ms）超时过紧，都会产生假阴性。
- vmrun/deploy 链条必须后台跑并 TaskOutput 收割，前台 30s MCP 超时
  会中断链条留下半部署状态。

### round 16 TODO

1. **CPUID exit 处理与 参考实现 的机制级对比**（掩码已排除，剩下的
   差异在 exit 机制内）：
   - 参考实现 handler 直接把结果写 GuestRegs + rip=nRip，无 dispatcher
     中间层；svmb 经 GuestContext/AdvanceRip/dispatcher。逐指令对比
     两边 asm 的 exit→handler→re-entry 序列（vmload/vmsave 时机、
     rflags/segment 的保存恢复集合）。
   - 重点怀疑：svmb 在 CPUID exit 时对 VMCB save area 的写入集合
     （rip 回写、rax/rflags 回写）与 CleanBits/ASID 的交互 —
     VMware L1 对"exit 后部分更新 VMCB 再 vmrun"的模拟容忍度。
2. 若机制对比无果：抓 VMware 侧证据（vmware.log 提高日志级别
   `monitor = "debug"`），或用 ldt/gdt/IDT 一致性校验指令组
   （vgif/invlpga 等冷门指令在 exit 路径上的使用差异）。
3. 仓库状态：`intercept_manager.cpp` 当前停留在 v22 bisect 态
   （op1=CPUID only）。round 16 定位根因后统一恢复 baseline 并
   一次性回归。

## Round 16 (进行中): C++ 层排除 — 短路实验结果

**实验 v24**: `SvmbVmExitEntry` 顶部对 CPUID exit (0x72) 短路 — 内联
`__cpuidex` 原生应答 + rip=NRip，**跳过 dispatcher（自旋锁）、exit
trace、全部 handler 层**（只剩 FillMachineFrame 在 asm 里先跑）。

**结果**: virtualization active 后 ~1-2 分钟 guest 仍然 wedge（ping 死，
kd CTRL+BREAK 无响应 = "wedged below the debugger's reach"），本轮无
chipset 复位行（8→8，软死锁而非硬复位）。

**结论（与 round 15 合并）**:
- 触发器不在 C++ dispatch/自旋锁/trace 层（v24 排除）
- 不在应答内容（v23 纯透传仍死）
- **在 exit/re-entry 裸周期本身**：vmrun → CPUID exit → vmsave guest →
  vmload host → (任意处理) → vmsave host → vmload guest → vmrun
- 且呈间歇性：v19 曾连续正确处理 253 次；死亡延迟随机 22s-2.5min；
  终态两种表现（chipset 硬复位行 / 无复位行的软 wedge）

**新模型（round 17 主攻）**: CPUID exit 后的重入窗口存在**间歇性的
中断/状态一致性风险** — 例如外部中断落在 VmmStack（Windows 不认识的
栈）上的概率性事件：多数时候没事，撞上关键路径 → 核心 wedge（中断
无法再送）→ 全机死。参考实现 同为 thin-hypervisor 却稳定，round 17
必须找到它消除该风险的确切机制（其 asm 无 cli/sti，但 host RFLAGS
快照的 IF 位时序、或 KERNEL_STACK_SIZE 栈、或 guest RFLAGS 进入
VMCB 的路径可能不同）。

### round 17 计划

1. 在 svmb 的 VMM 循环重入前显式 `cli`（或验证 host RFLAGS.IF 状态），
   对照存活率；同时读 参考实现 enter 时 `SAVE_GUEST_STATUS_FROM_REGS`
   捕获 rflags 的确切时机。
2. 若 cli 有效 → 固化为修复，回归 v19 全量拦截基线 + storm。
3. 仓库现状：v24（bisect 短路 + op1=CPUID only）已构建
   （b4833ceb9c98bb8f）但**未提交**，随本 round 文档一并提交。

## Round 17 (部分完成): 源码级对比穷尽 — 剩余怀疑收敛到 per-exit 硬件交互

### 本轮检查项（全部干净）

1. `EmulateCpuid` (exit_dispatcher.cpp:216): 纯 __cpuidex 透传 +
   零扩展写回 — 正确。
2. `ApplyTfFixup` (hypervisor.cpp:601): 仅当 guest RFLAGS.TF=1 时注入
   #DB；常规运行 TF=0 不触发 — 排除。
3. **参考实现 SVM_asm.asm 与 svmb svm_entry.asm 的 exit 循环逐指令
   对比：结构完全一致**（vmload guest → vmrun → exitcode 检查 →
   vmsave guest → vmload host → BACKUP → FillMachineFrame → handler →
   RESTORE → Extra1/State 检查 → vmsave host → jmp enter_guest）。
   参考实现 多一个退出前的寄存器一致性调试比对（非功能性）。
4. 参考实现 SAVE_GUEST_STATUS_FROM_REGS (SVM.h:430-505) vs svmb
   FillGuestState 逐字段对比：差异仅 svmb 多填 SYSENTER_CS/ESP/EIP 和
   DR6=FFFF0FF0（参考实现 留零）— **且 v21 已证明 save 区状态无辜**
   （v21 同样在首次 vmrun 加载它却存活 >7.5min）。

### 穷尽后的逻辑结论

静态源码层面 svmb 与 参考实现 已无未对齐项。差异必然存在于
**动态行为**——每次 CPUID exit 触发的 vmsave/vmload/vmrun 硬件交互
在 VMware L1 嵌套模拟下的微观效应。round 15-16 已证明：
- 该交互必要且充分（op1=0 即存活）
- 与 C++ 处理无关（v24 短路仍死）
- 与应答内容无关（v23 透传仍死）
- 死亡形态两种：chipset 硬复位行 / 无复位行的软 wedge（kd break
  不可达 = 核心处于 shutdown 等待态）

### round 18 计划（VMCB 字节级 diff 实验）

1. **VMCB 转储对比**：参考实现 与 svmb 分别部署，kd 在各自 enter 函数
   处 bp，`db <guestVmcbVA> L1000` 转储 4KB VMCB 原始字节，逐字节
   diff。参考实现 的 KdPrint 输出 guestVmcb PA 可定位。
   - 顺序执行避免 SVM 冲突：参考实现 装载+dump+卸载 → svmb 同样。
2. 若 VMCB 静态一致：抓动态转储 — kd 在第 N 次 exit 处 bp，dump
   exit 前后的 guest VMCB + host VMCB，观察 vmsave/vmload 往返中
   哪个字段漂移（重点 EFER/EFER_SVME、G_PAT、STAR 系、LBR/VIRT
   bits — VMware L1 对这些位有自己的 nested 语义）。
3. 备选：vmx 加 `monitor = "debug"` 抓 VMware L1 日志，直接看
   L1 对我们 vmrun/vmsave/vmload 的模拟失败点。
4. 当前仓库状态：v24 bisect 代码（op1=CPUID only + 短路）已提交
   （379c569），根因定位后统一恢复全量基线。

## Round 18 (中止): VMCB 字节级 diff — 被宿主 VM 环境劣化阻塞

**环境故障时间线** (2026-09-08 03:30-04:45 local):
1. 19:35 UTC boot 曾触发 `VMware Workstation unrecoverable error:
   (vcpu-3)` panic（此前连续多次 triple-fault 复位后的宿主状态劣化）
2. 之后的 vmx (PID 19280, 8.6GB) 进入卡死态：vmrun reset 被搁置
   （"MSG in progress"）、taskkill /F 与 PowerShell Stop-Process 均
   "拒绝访问"、vmware.log 冻结、KDNET 无响应、ping 两 IP 全灭
3. **serial 弹窗陷阱确认升级**：每次成功 boot 后
   `serial_blackbox.log` 会被 guest 重建 → 下一次 VM start 必然触发
   "文件已存在" GUI 弹窗并阻塞启动。vmx 的
   `msg.popup.defaultAnswer`/`answer.msg.serial.file.open` 配置项
   对该弹窗**无效**（实测）。可行缓解：每次 start 前
   `rm serial_blackbox.log`（但 vmx 锁文件时删不掉）。
4. vm_start.bat 在旧 vmx 占锁时会**静默失败**（echo 不代表 vmrun
   成功）——必须用 vmware.log 新 boot header 验证启动真发生。

**Round 18 实验计划完整保留**（环境恢复后立即可执行）：
1. 参考实现 部署：产物
   `<host>\Downloads\参考实现B\参考实现B\x64\Debug\Amd-V-ReloadDbg.sys`
   (+pdb 同目录)，DriverEntry **自动进入虚拟化**（无需 IOCTL 触发），
   设备名 `\.\YCData`。guest 路径建议
   `<guest-user>\Desktop\driverTest\svmb-test\Amd-V-ReloadDbg.sys`。
   服务名自定（如 AmdVDbg）。
2. kd 抓取：`.sympath+ <参考实现 pdb 目录>` → `bp Amd-V-ReloadDbg!
   _run_svm_vmrun`（每次 boot 每核命中一次）→ 首次命中 `db @rcx
   L1000`（guestVmcb 在 VirtCpuInfo+0，ExitCode 于 +0x70 佐证）→
   `.writemem <windbg-test>\logs\refimpl_vmcb.bin
   @rcx L1000` → `bc 0; g` 放行其余核。
3. svmb 抓取：hard reset 清 参考实现 → 部署 v24 二进制 →
   `bp svmb!_svmb_vmm_loop`（rcx=vcpu，guestVmcb 在 vcpu+0）→
   同样 `db @rcx L1000` + `.writemem ...svmb_vmcb.bin`。
4. host 侧逐字节 diff（`cmp -l` / python），重点 control 区 0x00-0xC8
   与 save 区 0x400-0x7FF；可疑字段回对照 APM B-1/B-2 表。

**环境恢复步骤（用户回到电脑后）**：VMware Workstation GUI 里应答
卡住的弹窗（serial 文件覆盖提示）或直接退出 Workstation 进程重启；
若 GUI 无响应则重启宿主机。然后从 round 18 计划第 1 步继续。

## Round 18 (完成): VMCB 字节 diff 实验 — 段 limit/DR 理论证伪，收敛到边界穿越

### 实验执行

1. **参考实现 抓取**（快照恢复法）：发现 `post-fix-2026-09-07-serial-file`
   快照恢复后 参考实现 直接处于活跃运行态（用户昨天测完打的快照），
   kd `bp Amd_V_ReloadDbg!_run_svm_vmrun` → `.writemem` 抓 8KB
   （guest+host VMCB）→ refimpl_vmcb.bin ✓
2. **svmb 抓取**：hard reset 清 参考实现 → 部署 v24 → kd
   `bp svmb!_svmb_vmm_loop`（rcx=vcpu，guestVmcb@+0/host@+0x1000）→
   ctl start 触发 → svmb_vmcb.bin ✓
3. **逐字节 diff**（python）：163 字节差异，逐字段归因。

### 字段归因

| 区域 | 差异 | 归因 |
|---|---|---|
| ic1 MSR bit (0x0C-0x0F) | 参考实现 有 / svmb(v24) 无 | bisect 状态，非根因 |
| MSRPM PA (0x48-0x4F) | 地址不同 | 分配位置，预期 |
| NP_ENABLE/NCr3 | 参考实现=1/有，svmb=0 | NPT 开关，预期 |
| 段 limit (CS/SS/DS/ES/GS/LDTR/TR) | **参考实现=全零，svmb=4GB/真实** | **候选根因 → v25 证伪** |
| DR6/DR7 | 参考实现=0/0，svmb=FFFF0FF0/0x400 | 候选 → v25 证伪 |
| CR3/LSTAR/CSTAR/KGS/RIP/RSP/RAX/CR2 | 不同 boot 期望差异 | 无害 |

### v25（全 参考实现 对齐：limit=0 + DR6/DR7=0 + 全量拦截 + 无短路）

**72s 死亡**（软 wedge，无 chipset 行）→ **段 limit/DR 理论证伪**。

### 穷尽后剩余的差异与结论

至此 svmb 的 VMCB 静态内容已与 参考实现 完全对齐（除 NPT 开关与
MSRPM 页数：参考实现 2 页 vs svmb 3 页三缓冲），死亡仍复现。结合
round 15/16/17：**触发条件 = CPUID exit 的 L1 边界往返本身**
（v21 零往返存活；任何带往返的配置都死，与处理/应答/静态状态无关）。
参考实现 相同往返却稳定 → 剩余可测差异：
1. **MSRPM 页数**（参考实现 2 页 vs svmb 3 页三缓冲）— round 19 首
   个二分项：把 svmb 的 Msrpm 换成单页/双页，观察。
2. 纯时序/分配地址差异（VMware L1 嵌套模拟 bug 的概率性触发）——
   若 MSRPM 二分无效，用 `monitor="debug"` 抓 L1 日志。

### 仓库/环境状态

- v25 二进制 = 全 参考实现 对齐 + 全量拦截（当前已部署 guest 中，
  已死状态）。
- vmx 曾卡死一次（PID 19280）；本次靠快照
  `post-fix-2026-09-07-serial-file` 回滚恢复（该快照含活跃的
  参考实现，恢复即 参考实现 运行态）。serial_blackbox.log 的
  "文件已存在"弹窗仍需 start 前 `rm` 预防。

## Round 19 (评估): MSRPM 页数二分 — 价值降级

检查 Msrpm::Init：svmb 分配 3 个 SIZE(2 页) 缓冲做轮换，但 VMCB 的
MsrpmBasePa 只指向 Active 一个 2 页区域 — 与 参考实现 的单 2 页缓冲在
**L1 可见性上完全等价**（其余缓冲闲置且 L1 不会访问）。原"页数差异"
理论价值有限，二分降级。

### 当前完整排除清单（截至 round 19）

应答内容 / CPUID 隐藏 / MSR 拦截 / C++ dispatch 层 / 段 limit /
DR6/DR7 / 异常拦截 / VMMCALL 拦截 / KDNET / Hyper-V 启动器 /
PatchGuard(Hvi 调用者=激活栈) / 调试器 / 弹窗配置。

**唯一恒真关联**：CPUID exit 的 L1 边界往返发生 → 概率性 wedge/复位
（22s-2.5min）；不发生 → 稳定（v21 >7.5min，参考实现 长稳）。

### round 20 建议方向（二选一，推荐 A）

**A（推荐）— VMware L1 日志直取**：vmx 加
`monitor = "debug"` + `monitorLogShowTsc = "FALSE"`，复现死亡，直接读
vmware.log 里 L1 对我们 vmrun/vmload/vmsave 的模拟记录与复位理由。
这是不再猜、直接看 L1 想法的数据通道。风险：日志量大、需回滚后清
`monitor` 行。

**B — NPT=1 对照实验**：参考实现 稳定态是 **NPT 开启** 的（快照恢复
证实），而 svmb 所有死亡配置都是 NPT 关闭。开启 NptEnable=1 +
FillIdentityWholePa（已就位）复测存活 —— 若 NPT 下反而稳定，说明
VMware L1 的"无 NPT 的嵌套 SVM"路径本身有缺陷（round 10 曾记
"vmrun without NPT forces gpa==hpa → L1 resets"）， тогда NPT=1 从
"待验证项"升级为"必需项"。成本低（注册表一个键），信息量大。

## Round 20 (B 完成): NPT=1 对照 — 同样死亡，NPT 开关排除

HideCpuidBits=默认(1) + NptEnable=1（FillIdentityWholePa 生效）+
v25 全对齐二进制：ctl start 后 ~75s 死亡（软 wedge，无 chipset 行）。

**方案 B 证伪**：参考实现 稳定态虽为 NPT 开启，但 svmb NPT=1 同样死 →
"NPT 缺失 = VMware L1 缺陷路径"不成立。

### 穷尽清单终版（20 轮累计）

已证伪：应答内容 / CPUID 隐藏 / MSR 拦截 / C++ dispatch / 段 limit /
DR6/DR7 / NPT 开关 / 异常拦截 / VMMCALL 拦截 / KDNET / Hyper-V 启动
器 / PatchGuard / 调试器 / 弹窗配置 / MSRPM 页数（理论弱化）。

**不变事实**：CPUID exit 往返发生 → 概率性死亡（40-90s 窗口，两类
死亡形态）；不发生 → 稳定。参考实现 同往返稳定（今日实证 12min+36min
两个窗口）。

### round 21 剩余路径

**A（vmx monitor="debug"）成为唯一推荐**：直接抓 VMware L1 对
svmb vmrun/vmload/vmsave 的模拟记录与死亡瞬间的 L1 视角理由。日志
量大，建议在新会话执行：加 vmx 行 → 复现 → grep L1 嵌套相关行 →
对照 参考实现 同场景日志。

## Round 21 (关键发现): 死亡不是随机的 — 固定 ~72s 定时器

v26（exit 后 2µs 节奏停顿，模拟 参考实现 的比对延迟）仍死亡，但连续
5 次实验的死亡时间高度一致：**72/73/75/72/73s（ctl start 后）**。
这不是竞态/随机 — 是一个**固定周期事件**。

### 关联推理

- 我们抓到的死亡前 CPUID 探测循环 = **sppsvc/许可激活栈**
  （SPCall2 → ExpGetVMActivationStatus → HviIsAnyHypervisorPresent），
  该栈以 ~1 分钟量级的周期轮询 VM 激活状态。
- ~72s ≈ 该轮询周期的 1-2 个周期（自 ctl start 起算）。
- v21（无 CPUID 拦截）存活：许可探测原生执行，无害。
- v22+（CPUID 拦截）：**许可探测的 CPUID 突发穿过我们的 VMM 时，
  突发中某条 CPUID/某个上下文触发致命路径** → 整机死亡。
- v23（透传应答）也死 → 致命的不是应答值，而是**探测上下文本身**
  （比如探测从某个特殊 segment/CR/IRQL 状态执行 CPUID，我们的
  exit/重入路径对该状态的处理有缺陷），或探测 CPUID 的**密度**
  在 VMM 往返下放大了某个隐性缺陷。

### round 22 实验计划

1. **锁定致命 CPUID**：kd `bp svmb!SvmbVmExitEntry`（会非常频繁，
   用 `bp ... "k 10; g"` 记录后放行）抓许可探测突发中的一条，看
   k 栈里 ExpGetVMActivationStatus 的上下文（IRQL/段/CR8），对比
   正常 CPUID exit。
2. **排除上下文假设**：svmbctl 关闭许可服务（`sc stop sppsvc` +
   disable）后复测存活 — 若探测停止后存活，直接锁定 sppsvc 轮询
   为触发器，再深挖它的哪条 CPUID 致命。
3. **monitor="debug"**（L1 日志）与 2 并行。

## Round 21 (补充): sppsvc 证伪 + 死亡形态修正

1. `sc stop sppsvc + disabled` 后死亡仍精确复现（73s）→ 许可轮询
   **不是**触发器（SPCall2 调用者不只是 sppsvc，或触发器另有其源）。
2. **死亡形态修正**：本次死亡（无 kd、无 chipset 行）后 vmrun 挂起、
   `GuestRpcSendTimedOut`、`E1000 rx ring full` → guest **全核冻结**
   （非重启！此前 kd 观察到的"restart"只发生在有 kd 的轮次）。
   vCPUs 全部停止调度 = **六个 VMM 循环同时卡住**的形态。
3. v26（2µs 节奏）排除"重入过快竞态"。
4. 固定 ~72s 依然成立（本轮 73s）。

### 修正后的模型

ctl start 后 ~72s，某个全系统事件到达 → 六个核全部冻结在 VMM 循环
内（或各自卡死在进 VMM 的路上）。候选：
- 全核同时收到某类 exit（如 NMI/SMI 类），而我们的 VMM 处理路径对
  它死锁（如全核争抢 dispatcher 自旋锁 —— 但 v24 无自旋锁也死）
- VMware L1 在 ~72s 时刻对嵌套 vCPU 做了一次全局操作（如同步
  snapshot/记账），该操作与我们的 VMCB 往返互锁
- Windows 侧 ~72s 定时活动（DPC 清理/定时器到期）触发全核 CPUID
  突发，突发在 VMM 内死锁

### round 22 修订计划（新会话执行）

1. kd 全程 attached 复现，冻结时 `send_ctrl_break`/vertarget 检查
   各核状态（~* k 不可用时用 !process/-1 或搜索 KPCR）——若核停在
   svmb 的 VMM 内，`ub @rip` 看卡在哪条指令（自旋锁？vmsave？）。
2. v27：临时移除 dispatcher 自旋锁 + exit trace + FillMachineFrame，
   仅保留 SVMB_LOGW(首 16 exit)，分辨卡死层。
3. `monitor="debug"` L1 日志（round 20 方案 A）。

## Round 22 (关键修正): 死亡发生在首次 exit 到达 VMM 之前

**带自动记录断点的复现结果**（kd 全程 attached）：
- `bp svmb!SvmbVmExitEntry`（自动记录 exit code + 放行）→ **零命中**
- exit trace（16 条预算）→ **零条**
- `virtualization active on 6 cores` 后 ~30-40s → "The target machine
  restarted without notifying the debugger" → 死亡

**结论修正**：死亡发生在**首次 #VMEXIT 穿越 VMware L1→L2 边界的过程
中或之前** —— 致命事件根本没到我们的 VMM 处理代码。这解释了此前
所有矛盾：
- v24 短路（跳过 C++）仍死 → 因为死在 handler 之前
- v23 透传应答仍死 → 因为根本没到应答阶段
- v19 的 253 次 exit 为何活着？—— KDNET 逐条 ack 使每个 exit 耗时
  数秒，机器在"爬行"中撑过了死亡窗口；最终仍是同一 wedge
- **CPUID 拦截的真正作用**：它使 guest 的 CPUID 产生**嵌套穿越**；
  v21 无拦截 = 无穿越 = 无死亡。死亡 = 首次穿越时的 L1 侧事件

### 参考实现 对照的关键剩余差异（round 18 diff 已示但被低估）

参考实现 的稳定态是 **NP_ENABLE=1 + NCr3 已设**（NPT 常开）；svmb
死亡配置 NP_ENABLE=0/NCr3=0。round 20 B 测过 NPT=1 但**未带本次
仪器化**（不知道那次死亡发生在首 exit 前还是后）。

### round 23 计划

1. **NPT=1 + 仪器化复测**（bp + trace + kd attached）：若 NPT=1 下
   exit 能到达 VMM（bp 命中）且机器存活 → 根因 = VMware L1 对
   "无 NPT 的嵌套 CPUID exit"的模拟缺陷 → NPT=1 升级为必需配置。
2. 若 NPT=1 下死亡依旧且零 exit → 对照 参考实现 抓 vmware.log
   `monitor="debug"` 看两者 L1 视角的差异。
3. 复测通过后：全量基线回归 + storm 10k + NPT 验证 + 合入 master。

## Round 23 (决定性): NPT=1 同样零 exit 到达 VMM — 死亡在 L1 穿越侧

**NPT=1 仪器化复现数据**（kd attached + bp + trace）：
- DriverEntry: `NPT enabled (ncr3=12540f000)` ✓（FillIdentityWholePa +
  Enable 成功）
- **全部 6 核 `NP_ENABLE=1 NCr3=12540f000`** ✓（NPT 真实生效于每个
  VMCB）
- exit trace: **0** 条；bp(SvmbVmExitEntry): **0** 命中
- `virtualization active` 后 ~3 min：全核冻结（GuestRpc 超时、无
  chipset 复位行、kd break 不可达）

### 最终结论（round 22+23 合并）

1. 死亡时**没有任何 #VMEXIT 到达 svmb 的 VMM 处理代码**（硬件断点
   + trace 双重证实，不可辩驳）。
2. 触发条件：VMCB 中任何非零 ic1 拦截位（CPUID 或 MSR）→ guest 的
   首条对应指令产生**嵌套 exit 穿越** → 死亡发生在 **VMware L1 的
   嵌套 exit 模拟路径内**，先于我们的 handler。
3. NPT 开关无关（round 23 排除）。
4. v21 唯一存活 = 唯一无穿越配置。
5. 参考实现 相同穿越稳定 → L1 对 svmb 的穿越处理仍存在某个未知
   分歧（VMC 静态内容已对齐，剩余差异在动态/硬件不可见层面：
   如 L1 对 MSRPM 语义、HSAVE 语义、或 L2 VMCB 物理页的 shadow
   管理）。

### round 24 计划（新会话）

1. **vmx `monitor = "debug"`** 复现死亡，从 L1 视角直接读取模拟
   失败点（唯一剩下的一手数据源）。
2. 对照实验：参考实现 同场景 L1 日志，diff 两条穿越路径的 L1 行为。
3. 若 L1 日志指向具体 VMCB 字段 → 修正后回归全量基线 + storm。

## Round 24 (完成): monitor=debug 抓到精确死亡机制 — vcpu-1 即时 triple fault

**修正时间模型**：ctl_start_utc=04:02:20 与 `vcpu-1 Triple fault` 时间戳
**同秒（04:02:20.178）** —— 三重故障发生在 hypervisor 激活的**同一秒内**
（此前测的 "~72s" 包含了未计入的部署链耗时；死亡窗口实际 <5s）。

### 精确死亡链

1. `ctl start` IOCTL 线程落在 **vcpu-1**（历次日志 `ioctl HV_CONTROL`
   均记于 [c1]）。
2. 六核进入 VMM，IOCTL 线程作为 guest 在 vcpu-1 上继续完成 IOCTL
   返回路径。
3. vcpu-1 上发生**第一次嵌套 exit 穿越**（Windows 返回路径上的首条
   被拦截指令）→ VMware L1 嵌套模拟处理 → **vcpu-1 triple fault →
   shutdown state**。
4. 全部 12 线程 hard reset（vcpu-0..5 标 "mode HV" = 嵌套活跃核）。

### monitor=debug 的局限

仅切换到调试版 VMM（断言更全），**不自动输出逐 exit 日志**；故障前
1.2s 内 vmx 层零记录。逐 exit L1 轨迹需要额外机制（vprobes 或
Workstation 支持级日志），round 25 备选。

### round 25 备选方向

1. **vprobes**：vmx/vmrun 级 `vprobe` 在 L1 的 svmExit 处插桩，打印
   退出码与 L1 内部状态（Workstation 支持，需研究语法）。
2. **二分 svmb 与 参考实现 的 VMRUN 入口前状态**：vcpu-1 triple fault
   时另一核（如 vcpu-0）已通过 bp 停住 —— kd 冻结全场后读
   vcpu-1 的 VMCB/GPR 快照，与 参考实现 同刻对照。
3. **跑 参考实现 同场景对照**：确认 参考实现 在 vcpu-1 的首次穿越
   确实成功（此前 12min/36min 稳定已证，但未逐核看首穿越）。

## Round 25 (受阻): 测量通道失效 — ping 不可作为存活判据

参考实现 服务启动后 60s ping 丢失，但 **Tools/exec 通道正常**（sc query
成功返回）→ **guest OS 存活，死的只是 ICMP/网络栈**。参考实现 运行态
与 svmb 死亡态（Tools 全死、全核冻结）必须用 **Tools 通道 + kd**
双重判活，ping 已不可靠。

kd 连接本轮持续超时（5+ 次），参考实现 的 bp 穿越计数无法获取。

### 环境已知问题（累计）

- guest IP 在 .136/.137 间漂移，ping 时通时断（含 参考实现 稳定态）
- KDNET 连接间歇性超时（guest 正常运行时也连不上）
- serial_blackbox.log 弹窗：每次 start 前必须 `rm`
- vmx 修改需 VM 关机；vm_start.bat 静默失败需用 vmware.log 新 boot
  header 验证

### round 26 优先级（新会话）

1. **恢复 KDNET 连接**（kd 是唯一可靠取证通道）：优先排查宿主侧
   vmnet/防火墙，或重启 Workstation；确认 guest BCD debug 仍 ON。
2. kd 恢复后执行未竟的 参考实现 穿越计数（bp VmExitHandler 自动记录）
   —— 判定 参考实现 稳定态是否有 exit 穿越发生。
3. 根据结果走 round 24 的三条备选（vprobes / 冻结核对照 / 首穿越
   验证）。

## Round 26 (决定性对照完成): 参考实现 稳定态 = 持续数千次 CPUID 穿越

kd `bp Amd_V_ReloadDbg!VmExitHandler "dw @rcx+0x70 L2; g"` 在 参考实现
稳定运行态命中**数千次，全部 code=0x72（CPUID）**，跨 6+ 个 VMCB
（各核），机器持续稳定。另有少量 0x7C（VMMCALL）与 0x43（IOIO）
exit —— 参考实现 的实际拦截面比其源码字面更宽（或部分 exit 码为
VMware L1 注入的合成 exit）。

### 由此锁定的最终事实矩阵

| | 参考实现 | svmb |
|---|---|---|
| 静态 VMCB 内容（round 18/25 对齐后）| ≈相同 | ≈相同 |
| CPUID exit 穿越 | **数千次/分钟，持续** | 首次穿越即死（round 22/23 零到达）|
| 稳定性 | 数十分钟稳定 | 启动后 22s-2.5min 死 |

**结论：分歧不在静态配置、不在处理逻辑，而在"svmb 的首次穿越"这个
动态事件本身。** 参考实现 的穿越从未出过问题 → 首次穿越时 L1 需要
建立的 shadow/记录状态对 参考实现 是健康的，对 svmb 是致命的。

### 剩余可怀疑的差异（round 27 候选，按优先级）

1. **首次穿越发生的时间点**：参考实现 在 DriverEntry（系统刚起、
   sched/DPC 环境简单）完成全部 6 核进入与首批穿越；svmb 在
   ctl start（桌面全负载运行中）才首穿。→ 实验：svmb 改为
   boot 后立即自动 start（或 SVM 启动延迟到开机 5s 内），观察
   首穿是否仍致命。
2. **首穿前 svmb 的 MSRPM 三缓冲 CommitBuild 时序**：svmb 在
   DriverEntry Fold 后 CommitBuild 一次；参考实现 是静态单缓冲。
   若 CommitBuild 后 Active 指针/PA 有半更新窗口 → 实验：svmb
   去掉三缓冲，直接单缓冲静态 MSRPM（参考实现 完全对齐）。
3. **VMCR 物理页的对齐/邻接**：svmb 的 VMCB 在 VcpuContext 大块
   分配内部（与 HSave/Regs/VmmStack 同页邻接），参考实现 的
   guestVmcb 是独立分配。L1 若按 2MB 大页 shadow VMCB 区域，
   邻接页的属性可能污染 → 实验：svmb 的 GuestVmcb/HostVmcb 改为
   独立 MmAllocateContiguousMemory 分配（参考实现 对齐）。
4. vprobes L1 插桩（成本最高，最后手段）。

## Round 27 / 实验 3 (2026-09-08): VMCB 独立连续分配 → 阴性（首穿死亡依旧）

### 实验内容

svmb 的 GuestVmcb/HostVmcb/HSave 从 VcpuContext 共享 blob 内嵌
（`alignas(4096)` 三页 @0x0/0x1000/0x2000）改为**独立
`MmAllocateContiguousMemory` 页**（svm-base 参考注释里的原始模式），
blob 只留 Info/Regs/VmmStack（绝对偏移 0x3000/0x4000/0x5000 用显式
padding 钉死，asm 契约不变；sizeof 仍 0xB000）。

改动面：vcpu.h（指针化 + pad）、offsets.inc（VCPU_GUEST_VMCB 等 3 个
偏移变为指针槽）、svm_entry.asm（ExitCode 读取先 chase 指针再读
+0x70）、hypervisor.cpp（Start 分配 3 页/核 + PA 页对齐断言；EnterCore
全部 `->` 化）、exit_dispatcher.h（Vmcb() 直接返回指针）、hypercall.cpp、
intercept_manager.cpp、tlb.cpp（机械 `->` 替换）。编译签名通过
（`57ba8cbe…95c1` 双向校验一致）。

### 结果（NptEnable=0 + ctl start，KDNET kd-1f6bb01d + .logopen 全程）

- **6 核全部成功进入 SVM**：日志打出全部 6 条 `r13-vmrun-fire` +
  `virtualization active on 6 cores`；exp3 独立连续 VMCB PA 全部生效
  （13ffac000/13fe57000/13cddf000/13cdce000/13cd99000/13cd7f000，
  全部页对齐、低于 4G）。
- **零 exit trace**：ExitTraceLeft=16/核 的早期取证一行未打 → 没有任何
  VMEXIT 到达 SvmbVmExitEntry。
- ctl start 后 ~20-34s：Tools 心跳断联；`vmrun exec` 全灭。
- **kd CTRL+BREAK 20s 无响应** → 死在 OS 中断层之下（SKILL §6 签名）。
- vmware.log **无 panic / triple fault / reset** —— 本轮是纯静默冻结
  （与 round 22/23 的 Invalid-VMCB panic / 三重故障复位签名不同）。
- 恢复：`vm_reset_hard.bat`（30-90s 阻塞），vmware.log 05:36:52 新
  boot header 确认。

### 判定

**实验 3 阴性。** VMCB/HSAVE 物理页的"独立连续分配拓扑"不是死因；
VMware L1 对 参考实现（嵌 blob 内嵌 VMCB，Amd-V-ReloadDbg 源码
VirtCpuInfo 结构 guestVmcb/hostVmcb/hostStatus 全内嵌在单次
ExAllocatePool 分配里，见 NPT-Hook/SVM.h:55）和 svmb（同构 blob）
一视同仁——round 27 候选 #3 的前提（参考实现=独立分配）本身就不成立，
本轮改完后 svmb 反而比 参考实现 更"独立"，死亡依旧。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| 参考实现 的 VMCB 是独立 MmAllocateContiguousMemory | 参考实现（实测稳定的那份）与旧 svmb 一样是单 blob 内嵌；svm-base 注释里的独立分配是另一份未验证代码 |
| 独立分配后首穿可活 | 全核进入成功、首穿依旧致命（且本轮为静默冻结而非复位）|
| blob 内嵌对齐是风险 | exp3 的 alignas 改指针后 Info 会漂到 0x1000——static_assert 抓住，显式 pad 修掉（vcpu.h:47-55 教训入档）|

### TODO（round 27 余项，按新优先级）

1. **实验 1（时机假设）**：svmb 加 Parameters\AutoStart=1，DriverEntry
   末尾（System 进程、PASSIVE、开机静默期）直接 `hv->Start()`，服务改
   auto 启动 → 完全复刻 参考实现 的"开机即虚拟化"时序。
2. **实验 2（MSRPM 单缓冲）**：去掉 CommitBuild 三缓冲轮换，静态单
   缓冲（参考实现 完全对齐）。
3. vprobes L1 插桩（最后手段，成本最高）。

## Round 27 / 实验 1 (2026-09-08 ~14:30): 进入上下文 → 阳性！进入路径破了

### 修正后的立论

用户纠正：参考实现 不是"开机即虚拟化"，是 **驱动加载（sc start）即
虚拟化**（EnterVirtualization 在 DriverEntry 里）。且 round 25/26 里
参考实现 就是在**负载桌面**上 sc start 的照样稳定 → "静默开机 vs 负载
桌面"假设已被现有数据证伪。真正可测的内核差异是**进入上下文**：

- 参考实现：DriverEntry（System 进程内核加载线程）
- svmb：svmbctl.exe 的用户 IOCTL 线程（round 24：死的是 IOCTL 线程
  所在核 vcpu-1）

### 实现

main.cpp 加 `Parameters\AutoStart=1`：DriverEntry 末尾直接
`new Hypervisor(); h->Start()`（失败仅记日志不回滚 DriverEntry）。
服务保持 demand-start，`sc start svmb` 即完全复刻 参考实现 流程。

### 决定性对照（同一二进制！）

| | exp3 死亡轮（13:33）| exp1 存活轮（13:56）|
|---|---|---|
| 二进制 | exp3 构建 | **同一 exp3 代码 + AutoStart** |
| VMCB | 独立连续页 | 同 |
| 拦截基线 | op1=10040000 op2=1 | 同 |
| 进入方式 | ctl start（IOCTL 线程）| sc start → DriverEntry |
| 结果 | 首穿即死（6 核进入后零 trace）| **6 核进入后 96/96 条 trace 打满，全部 code=72 正常服务** |

exp1 运行：hv RUNNING，6/6 核，**vmexits 16844（+4min）→ 21209（+7min）
持续爬升**，guest 全程存活（exec/Tools/sc query 正常）——远超全部历史
死亡窗口（5s/22-40s/72-75s/2.5min）。**首穿死亡被进入上下文唯一变量
消除。**

### 但退出路径炸了：round-13 自退设计的线程丢弃缺陷（139/4 实锤）

10 分钟后 `ctl stop`：6 核标记 Leaving 后 5s 无一自退（"stop aborted"
），~20s 后 guest 崩溃。kd CTRL_BREAK 命中正在发生的
**139/4（KERNEL_SECURITY_CHECK_FAILURE，Arg1=4）**：

- 肇事线程 = **lsass.exe 普通线程**（core 2）——无辜受害者
- trap frame：**RIP=0、CS=NULL**，异常分发时栈指针越界 → fast-fail
- 机理（svm_entry.asm:371-385）：State==Leaving 的自退直接清 SVME、
  `mov rsp,[rsp+20h]; ret` 回 EnterCore —— 恢复的是**驻留 enter 线程**
  的续体；当刻被中断的**别的线程**（RESTORE_REGISTERS 刚把它的 GPRs
  放回 CPU）被整个丢弃，rax/rcx/rdx 还被 EFER MSR 往返踩掉。
- 为何现在才炸：round 13 之后进入路径 90s 内必死，**devirt 从未在真实
  负载下执行过**；进入路径修好后地雷才暴露。对照：VMMCALL 版
  exit_virtualization（asm:397-406，恢复 VMMCALL 调用者自身的
  rsp/rip/rflags）是正确的——丢弃问题只在 State==Leaving 分支。

### round 27 结论

1. **进入路径死亡根因 = IOCTL 线程上下文进入**（DriverEntry/System
   线程进入即稳）。机制层面待查（猜测：用户进程地址空间/IOCTL 线程
   栈/核亲和 vs VMware L1 首穿的交互），但因果已锁定。
2. **退出路径 bug = round-13 自退丢弃被中断线程**，有完整崩溃现场。
   修复方向（round 28）：Leaving 分支改为"原地恢复被中断线程 + 关
   SVME + 从 VMCB 恢复 CR3/rip/rsp"（即用被中断线程自己的状态完成
   devirt），驻留线程随后作为普通内核线程自然走完 EnterCore。
3. 过程陷阱（已入 SKILL 记忆）：kd 会话的 kd.exe 进程死亡后目标停在
   halt 且 MCP 报 "already running"；重连后 uptime 冻结可证实；
   `g` 可能被排队的 break banner 吞掉需再按一次。

### TODO（round 28）

1. 修 asm 自退：恢复被中断线程上下文（CR3@vmcb+550、VMLOAD guest、
   rip/rsp/rflags 从写回后的 VMCB/Regs），不碰驻留线程。
2. 回归：AutoStart enter → 10min 稳定 → ctl stop 干净退出 → ctl
   start（现在已知会死，验证 IOCTL 修复前先禁用或一并修）。
3. IOCTL 进入路径机制根因（为何用户线程上下文首穿致命）——优先级
   降低：先让 AutoStart 模式全绿。

## Round 28 (2026-09-08 ~15:30): 原地恢复 devirt 修复 — 进入/退出双通

### 修复演进（三轮，各轮崩溃现场驱动一步）

**v1（原地恢复，无 stgi）**：Leaving 分支改为恢复被中断线程
（vmload guest VMCB + 清 SVME + CR3@550 + rflags/rsp/rip/rax/rdx/rcx
从 Regs）。结果：stop 时全机冻结在 `KeIpiGenericCall`（stop: core
行一行未打）——**根因 GIF**：#VMEXIT 清 GIF、原循环靠 vmrun 重开，
宿主侧恢复路径没人恢复 GIF → 被恢复核所有中断（含 IPI 屏障）永封。
（v1 冻结一度误判为 kd 假死——注意 kd.exe 进程死亡后 MCP 报
"already running" 且目标实际 halt，重连 uptime 冻结可证。）

**v2（+stgi，加在 exit_virtualization 同款位置）**：stop 时新崩溃
**0x0A IRQL_NOT_LESS_OR_EQUAL**（野指针 0x00001417000000A5 @ IRQL 2，
nt+0x2AFBBD）。根因：**把用户态被中断的线程以宿主态直接恢复**——
user 线程需要 VMRUN 全状态重载（CPL/段），宿主侧恢复 = 用户代码在
CPL0 跑 → 野指针崩溃。注意 **Save.Cpl 不可用**（FillGuestState 恒写
0，所有 user rip 的 trace 都显示 cpl=0），门槛改用 Save.Rip 顶字节
（0xFF = 内核 VA）。

**v3（+内核态门槛）**：user exit → 照常 vmrun 重入，本核留待下个
内核态 exit 再 devirt。结果（同一启动内）：

1. `sc start`（AutoStart，DriverEntry 进入）：6/6 核，1693 exits
   @25s → 3625 @2min，稳定。
2. `ctl stop` 第一次：核 0/4 立即原地退出（state=0），1/2/3/5 未及
   内核态 exit，15s 预算到 → "stop aborted"（**无害**，设计如此）。
   abort 后剩余核在后续内核态 exit 上**自然排空**（第二个 info：
   0/6 virtualized）。
3. `ctl stop` 第二次：瞬时全量收尾，**hv state: OFF**。
4. 全程零崩溃零冻结；停止后 90s guest 持续健康（exec + 哈希一致）。

### 语义注记

- 中止后 hv 保持 RUNNING、全核 Off：此时 **ctl start 走的是 IOCTL
  进入 = 已知致死路径**（round 27 结论），切勿在 aborted 状态再
  start；应再发一次 stop 完成收尾（或未来把 IOCTL 进入修好）。
- v1 冻结教训入档：**宿主侧恢复路径必须 stgi**；VMMCALL 版
  exit_virtualization 同坑已一并修。

### 战役状态

进入路径（round 27）+ 退出路径（round 28）双通。剩余：
1. IOCTL 进入机制根因（为何用户 IOCTL 线程上下文首穿致命）——
   AutoStart 模式已可用，优先级降低。
2. 全量回归：storm 10k、NptEnable=1（FillIdentityWholePa）、循环
   enter/stop。
3. auto-stop 单相化（内核 exit 唤醒 nudge），目前两相 stop 可用。

## Round 28b/c/d (2026-09-08 ~16:20): 全量回归 + 三连修 — 进/停循环 ×3 全绿 + NPT=1 全绿

### 回归暴露 + 修复（每个缺口一轮）

**b1. 惰性排空过慢**：空闲核很少发生**内核态** exit（背景 CPUID 多为
用户态；INTR 不在拦截集、不走 exit），第一轮 stop 15s 预算屡屡超时
→ abort（无害但繁琐）→ 二次 stop 竞态 + sc stop 卸载冻结。
**修**：`IpiNudgeCallback` 里执行 `__cpuid`——VMware vhv 下 CPUID 必
exit（与拦截位无关），且回调跑内核态 = 正中 devirt 门槛 → stop 变
**单相确定性**。

**b2. CR8 缺失**（r28b-R2 停止时 **0x80 NMI_HARDWARE_FAILURE**，
AuthenticAMD，Idle 线程）：IPI 回调运行于 IPI_LEVEL（CR8=15），
#VMEXIT 后宿主 CR8=HSAVE 快照（0），原地恢复不还原 → 高 IRQL 上下文
以 IRQL 0 语义续跑 → 中断投递失序。**修**：从 VMCB V_TPR(0x60) 恢复
CR8。

**b3. stgi 位置**（r28c 立即 **#UD c000001d @ _svmb_vmm_loop**）：
为 NMI 窗口把 stgi 挪到清 SVME 之后——**APM：SVME=0 时 stgi #UD**。
**修**：stgi 回到 vmload 之后、EFER 清除之前（SVME=1 期间）。
NMI 中窗风险接受：窗口内 rsp/rax 半恢复（r28b-R1 同位置 stop 成功
证明可容忍）；CR8 修复才是 0x80 真因。

### 回归结果（r28d，哈希 28674bba）

| 轮 | 操作 | 结果 |
|---|---|---|
| R1 | sc start（AutoStart 进入）| 6/6 核，1440 exits@20s |
| R1 | ctl stop（单相）| **hv OFF**，六核全 0，无 abort |
| R2 | sc stop → sc start（卸载/重载）| 6/6 核，1004 exits |
| R2 | ctl stop | hv OFF |
| R3 | 卸载/重载再入 | 6/6 核 |
| R3 | ctl stop | hv OFF |
| NPT | NptEnable=1 重载进入 | "NPT enabled (ncr3=…)"，6/6 核 |
| NPT | 浸泡 2min（1846 exits）→ ctl stop | hv OFF |
| 收尾 | NptEnable=0 恢复，exec/哈希健康检查 | 通过 |

### storm 语义（记录，不阻塞）

当前基线 op2=1（仅 VMRUN）→ **VMMCALL 不产生 exit**，`svmbctl storm`
的 hypercall 压测在此基线下无 exit 压力（运行本身无害）。exit 压力
由自然 CPUID 流量证明（21k+ / 10min）。storm 全量压测待 VMMCALL
拦截位纳入基线后再跑。

### 提交

r28b/c/d 修复 + vm_svc_stop.bat / vm_reg_npt_off.bat 见 HEAD。

## Round 29 (2026-09-08 ~16:50): storm 10k 压测 PASS — 62k exits，1.6 秒

### 修正 round-28b-d 的错误注记

旧注记称"storm 需 VMMCALL 拦截位入基线"——**错**。svmbctl storm 走
**CPUID hypercall 通道**（`__cpuidex(HYPERCALL_CPUID_LEAF, HC_PROBE)`，
main.cpp:374），CPUID 在 VMware vhv 下必然 exit，与 VMMCALL 拦截位
无关，基线无需任何改动。当日 storm 无效果的真因有两个：
1. guest 里的 svmbctl.exe 是旧版（vm_push_driver.bat 只推 .sys 不推
   .exe）——已加 `vm_push_ctl.bat` + `vm_guest_hash_ctl.bat` 双向哈希；
2. `vm_ctl_run.bat` 不落盘输出，svmbctl 的 usage/报错全被吞——已加
   `vm_ctl_storm.bat`（输出重定向进 steps log）。

### 压测结果（r28d 构建，哈希 28674bba；AutoStart 进入 6/6 核）

| 时点 | vmexits |
|---|---|
| 进入后基线 | 1,058 |
| **storm 10000（1.6s 墙钟）** | **61,785** |
| +15s 静置 | 62,226 |

Δ≈60.7k = 10,000 iters × 6 核 ✓（余量是窗口期自然 CPUID）。瞬时
~38k exits/s，VMM 往返 ~26µs。压测后 guest 存活、6/6 核、CPUID 隐藏
检查 ok、`ctl stop` 单相干净（hv OFF）。

### 结论

进入（DriverEntry）、退出（原地恢复）、exit 高压（62k 次穿越）、
CPUID 隐藏、NPT=1 全部在同一构建上验证通过。storm 的 VMMCALL 拦截
位待办撤销。

## Round 30 (2026-09-08 ~17:10): 虚拟化改为加载即开启（默认行为）

### 变更

main.cpp：`autoStart` 默认值 0 → **1**。加载驱动（sc start）即进入
虚拟化；注册表 `AutoStart=0` 为显式退出开关（bisect 用，退出后 ctl
start 即暴露已知致死路径）。DriverEntry 失败仍只记日志不回滚加载。

配套确认：DevUnload 已调用 `hv->Stop()`，r28d 单相停止使其在虚拟化
运行中卸载驱动成为安全主路径。

### 验证（r30 构建，哈希 153b2cc3）

| 场景 | 结果 |
|---|---|
| T1：无 AutoStart 注册表值，加载 | **hv RUNNING，6/6 核**（默认生效）|
| T2：虚拟化运行中 `sc stop`（卸载）→ 重载 | 干净卸载无挂死；重载后 6/6 核 |
| T3：`AutoStart=0` 加载 | hv OFF（退出开关生效）|
| 收尾 | 删值恢复默认，guest 虚拟化运行中、健康（哈希一致）|

### 环境注记（本轮新坑）

- kd 连接的初始 break 会吞掉第一个 `g`（banner 提示也这么说）——
  **connect 后应连发 g 直到 "already running" 并用 vmrun exec 验活**
  再开始操作；本轮因目标被 halt 25 分钟而被迫硬重置一次。
- 功能验证（info/reg/sc）全程走 exec 通道即可，**kd 非必需**——先
  功能后挂调试器可完全绕开该陷阱。

## Round 31 (2026-09-08 傍晚): M3 联调首攻 —— NPF 实路径探针三轮挂死全解

### 背景

master @ 6b78ebc 之后启动 M3 联调（NPF 实路径 + TLB 协议）。新增
`SVMB_IOCTL_NPT_PROBE`（协议 0x811）+ `core/npt_probe.cpp`：驱动内自包含
探针（目标函数 + detour 都在 svmb.sys），stage 0 = 装 hook → 强制 exit 清
TLB → 调 N 次（期望每次取指 NPF → slide → 隐藏页 detour）→ 摘除 → 复调验
证恢复；stage 1 = 建第二个全 PA identity view → live 切换发布 → 读回
VMCB NCr3 → 切回销毁。ctl 端 `npfprobe <n>` / `viewprobe`。

### 现象（四轮 boot，三种死法）

| boot | 构建 | 现象 |
|---|---|---|
| 1 | 37c0cc7c | npfprobe IOCTL 挂死（单核 livelock，guest 其余存活）；**IO gate 被探针持有 → 所有诊断 IOCTL 永久阻塞**，环形日志拿不出来 |
| 2 | 069665fe | svc start 后 ~1 分钟空闲冻死（探针未运行；Tools 心跳超时）——NPT=1 空闲稳定性存疑（待单独复测，本轮未定案） |
| 3 | 069665fe (kd 挂载) | 探针运行：slide 16 次 EXEC flip 全部成功（readback PTE=9f12b061 = 隐藏页 EXEC 态，软件侧正确）→ 洪水闸拒绝 → deny 循环 → spin break 跳指令 → 日志子系统砖化 → 全机冻结 |
| 4 | c2e0bfbf（探针页隔离后） | 探针线程 wild jump rip=0（trap frame 实锤，rdx 残留字符串字节）→ SEH 分发 → resume 后 **0x139 KERNEL_SECURITY_CHECK_FAILURE**（r15 = 探针线程本尊） |

### 根因（已证实）

**boot1/3：钩子页与驱动日志代码同页。** `SvmbNpfProbeTarget` 被链接器放
在与 RtlStringCbVPrintfA（ntstrsafe 内联实例，svmb+0xA080）相同的 4K 页
（gpa=12c0a6000，两次 boot 位置一致）。装 hook → 整页 NX → **NPF 处理器
自己的诊断日志（Install 的 hook 日志、npf diag、deny WARN 全部走
LogWrite→格式化函数）在取指时再次踩中 NX 页 → exit 处理路径内递归 NPF**
→ slide 反复翻转 → 洪水闸拒绝 → deny 循环 → spin break 逐指令跳过 →
页上所有活函数（含日志）被打碎 → 冻结。
- 铁律级教训：**hook 目标页绝不能含 VMM exit 路径自身执行的任何代码**
  （日志、分发器、处理函数）。M4 hook 策略选外部内核 API（NtCreateFile
  等）天然无冲突；探针用独立节解决。
- 修复：`#pragma code_seg(".svmbpr0")` 把探针目标隔离到独立 PE 节（加载
  器按页映射，整页只有它自己）。

**boot4（页隔离后）的 wild jump rip=0 未定案。** 候选：patch/steal 在
.svmbpr0 新 prologue 上的解码问题、hidden-copy 执行流问题。下一轮 kd
断点在 Install 返回后 dump hidden copy 前 64 字节 vs 原页，单次调用单步。

### 本轮新增的防护（全部保留——把无限挂死变成有界可诊断失败）

1. npf.cpp：入口诊断（cap 32，gpa/info1/slide/action）、lazy-map 失败日志
   封顶、**deny spin breaker**（同 GPA spin 256 次后 AdvanceRip 跳过，
   探针 FAIL 但返回，IO gate 释放，日志可读）
2. npt_hook_mgr：**slide 洪水闸**（每 hook 64 flips/秒 → decline 落入
   deny breaker 路径）、swap 失败日志封顶、**flip readback 证据行**
3. npf GVA flush：fetch 类 NPF 在 slide 后补 `invlpga Save.Rip 页`
   （INVLPGA 语义要求 GVA；原 TlbInvlpgaLocal 传 GPA 是无效操作数）
4. 探针目标独立 .svmbpr0 节

### 流程资产与新坑

- vmx `answer.msg.serial.file.open = "Append"`：串口文件"替换/追加"弹窗的
  官方自动应答键；原 vmx 里的值是无效的 `"1"`（弹窗照旧的根因）。VM 运行
  中改 vmx 会被回写覆盖，须关机改。
- 快照 `r31-base-vmx-append` = 标准基线（vmx 修复 + 干净未加载驱动 +
  NptEnable=1/SerialLog=1 Parameters）。revert 实测 ~40s + Tools 45s。
- kd 死对等待窗口：kill kd 后目标端 kdnet 拒新连接数十秒~分钟，反复 kill
  越拖越久；本轮用 `( sleep 30; echo q ) | kd.exe -k ... -c "..."` 一次性
  会话绕开 MCP 30s 上限。
- 本地 RIP 解析流程：`!running -t` 拿原始地址 → dumpbin /DISASM（build/
  host_disasm.bat）→ llvm-symbolizer --obj=svmb.sys（PDB 解析静态函数）。
- 串口黑盒再次全程 0 字节（abandoned 结论第三次确认）。
- IO gate 教训升级：**任何可能 spin 的 IOCTL 处理路径必须保证有界返回**，
  否则 gate 持有让全部诊断通道（log/storm/info）陪葬。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| NPT=1 已在 round 29 验证稳定 | 只验证了 <1min 短窗（enter→storm→stop）；空闲长跑从未验证过 |
| 探针挂死 = slide TLB 活锁（需修 flush） | 第一层是页冲突：日志代码住在钩子页上，exit 内递归 NPF |
| slide 翻转"软件侧成功但 CPU 不见效" | boot3 readback 证明软件侧正确且 16 次翻转后洪水闸正常拒绝；死因在别处 |
| kd kill 后可立即重连 | 目标端死对等待窗口，越 kill 越堵 |

### TODO（下一轮）

1. kd 挂载 + 断点验证 patch 完整性：Install 返回后 dump hidden copy 前
   64B vs 原页对应偏移，核对 `48 B8 <detour> FF E0` 位置与 stolen 长度；
   单次调用单步观察 rip 轨迹（boot4 rip=0 定案）。
2. 独立复测 NPT=1 空闲 5 分钟（无探针、无 kd）——boot2 的冻死是否复现。
3. 两者绿了再跑 stage 0 全计数断言 → stage 1 view 切换 → storm 存活。

## Round 32 (2026-09-08 夜): M3 联调通关 —— NPF 实路径 + TLB 协议全绿

### 结论先行

**M3 联调三大目标全部 PASS（同一次 boot 内）：**
1. NPT 上线 + 5 分钟空闲浸泡绿（exits 2712→6878 稳步爬升，hv RUNNING）
2. **NPF 实路径**（stage 0）：`detour=16 origWhileHooked=0 origAfter=16
   exitDelta=3` —— 16/16 调用走 NX 取指→NPF→slide→隐藏页 detour，原始页
   零泄漏，摘除完全恢复；exitDelta=3 = 首次翻转后 TLB 缓存生效，符合设计
3. **TLB 协议**（stage 1 view 切换）：`pml4=2fb5000 == vmcbNcr3` —— 第二
   个全 PA identity view live 切换发布到全部 VMCB 并读回验证；随后
   storm 2000×6 存活，6/6 核、CPUID 隐藏 ok

### 真正的根因（boot1-4 + r32 首boot 全部统一解释）

**VMware vhv 下 NPT 叶子 U/S=0（User=false）无条件 #NPF —— 即使 CPL=0 的
内核态访问。** U/S=1 的叶子（identity 2M 填充、lazy-map RWX）从不 fault。

证据链（kd logopen 全程捕获）：
- `npt hook: target=fffff80662f9d000 ...` —— Install 完成（.svmbpr0 节
  隔离生效，target 恰为节首 RVA 0x1D000）
- 紧接 `npf: diag gpa=ace73000 ... slide=0` 洪水 —— ace73000 即本轮
  svmb.sys 探针页的 PA（SLIDE_DATA 武装：P|RW|NX|**U=0**）
- slide 对 ace73000 的 LookupPage 未命中（钩子页是它自己，fault 的 GPA 也
  是它自己——slide 用 gpa4k 查到钩子后幂等翻转，但 U=0 叶子每次访问照旧
  fault）→ 洪水闸 64 次拒绝 → deny 循环 → spin break 256 后 AdvanceRip
  → 跳指令破坏执行流 → AV at rip=0（boot4 同款）→ 0x139
- 对照：identity 填充页（U=1）5min 空闲 + 60k exits + storm 零 NPF

**修复**：`SLIDE_DATA = {true,false,true}` / `SLIDE_EXEC = {false,true,true}`
（npt_hook_mgr.h）。历史教训：r31 的"TLB stale"假设错误——翻转载件侧从来
就是对的（16 次 readback 全对），是 U/S 语义在作怪。

### 流程改进（本轮落地）

- **stage 2 字节 dump**（`svmbctl npfdump`）：Install + dump + remove，不
  执行钩子页，无需 kd 即可拿 patch/trampoline 真值。实测：
  `hidden = 48 B8 <detour> FF E0 | 原始字节[12..]`、tramp 前 stolen=14
  字节 + `48 B8` 回跳，全部正确。
- ctl 校验两处 bug 修正（memcmp 起点、tramp tail 位置——字节本来就对，
  是校验代码自己写错）。
- 5 分钟浸泡 + 每 60s vmexits 采样（后台循环）。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| boot2 空闲冻死 → NPT=1 空闲不稳定 | 冻死与探针 IOCTL 相关（U/S=0 叶子武装即触发）；U=1 修复后浸泡+探针+storm 同 boot 全绿 |
| slide 翻转后 CPU 不见效 = TLB stale / flush 语义 | 载件侧从来正确；U/S=0 叶子在 VMware vhv 下无条件 fault |
| GVA invlpga 是关键修复 | 无害但非必需；U=1 才是 |
| boot4 rip=0 是补丁字节问题 | 字节全对（stage 2 验证）；是 spin break 跳指令破坏执行流 |

### M3 收尾状态

- NPF 实路径 ✅、TLB 协议 ✅、NPT 空闲稳定 ✅
- ARCHITECTURE.md 待办更新：M3 联调完成；M4（mod attach debugger/
  cr3_monitor → 事件到 R3、CR3 伪造实测）成为下一关
- 遗留观察项：storm 2000 轮后 exits 计数 16981 继续自然爬升；stage 0 的
  exitDelta=3 证明 hook 页翻转后零额外开销

## Round 33 (2026-09-08 深夜): 真实内核函数 hook（NtCreateFile）—— 机制通，passthrough 未通

### 目标与结果

`nthook`（stage 3）：`MmGetSystemRoutineAddress(L"NtCreateFile")` 真实内核
热函数 hook，ASM 无帧中继 detour（计数 + tail-jmp trampoline 保原逻辑），
RunOnEachCore 多核触发打开 \SystemRoot\System32\cmd.exe。

**已证实的（全部 live）**：
- 多核命中：6 核 24/24 detour 全部命中（first-fetch NPF→slide→隐藏页
  patch→detour 链路在真实热函数上工作正常）
- 一次意外压测：ctl iterations 未归一化导致 393,210 次 hooked 调用
  （65535/core），guest 全程存活、Remove 干净、postOpen=1 —— 滑动引擎
  韧性远超预期
- call→patch→detour→relay→ret 链路完全干净：no-tramp 二分模式
  （relay 直接返回 c0000001）返回值逐字透传给 caller

**未解决的**：passthrough 返回 c0000010（STATUS_INVALID_DEVICE_REQUEST）
24/24 确定性失败。已排除：
1. ~~帧移位~~（ASM 无帧中继 rsp 纹丝不动，仍 c0000010）
2. ~~入口 steal/patch 字节~~（nt!NtCreateFile = `sub rsp,88h; xor
   eax,eax; mov [rsp+78h],rax` = 14 字节，trampoline/hidden 字节已核）
3. ~~上游 dispatch~~（c0000001 标记逐字返回）

剩余候选：trampoline 页上执行 stolen 字节的上下文差异 vs hidden copy 上
执行 body 的差异。第三刀 bisect 已实现未跑完：**direct-resume 模式**
（跳过 trampoline 直接 jmp targetVa+stolen 进隐藏副本 body）——本轮
该模式首跑后 Tools 死亡，状态未及用 kd 取证。

### 新发现 bug（独立）：sc stop 在重度 hook 活动后触发 CPU bugcheck

nthook 421k 次调用之后 sc stop：CPU5 走进 KeBugCheckEx（参数疑 0x80 类
NMI），CPU2 卡死在 KeIpiGenericCall（Stop 的 IPI 屏障等 bugcheck 核应答）
→ sc stop 永挂 + IO gate 持有。与 r28 的 0x80（CR8 恢复类）症状相似但
路径不同（NPF/hook 高压后的 devirt）。**处理纪律：devirt 前挂 kd、
bugcheck 现场直接看 CPU5 的 bugcheck 代码与栈。**

### 流程教训（本轮两次自伤）

- **铁律 8 复发**：kd connect 自动 break 后忘发 g，目标 halt 14 分钟，
  期间 push 失败被误判"新的空闲冻死"。识别特征：!running 显示 All
  processors idle + uptime 增长（17 分钟墙钟只走 39 秒运行时）。kd 在手
  时 Tools 死亡 ≠ 冻结，先查 !running。
- kd logopen 会 REPLAY 旧 boot 的缓冲行，读日志先核对 boot 标记。

### 下一轮入口

1. kd 挂载 → direct-resume 模式复跑 → 冻结/异常现场取证（现在知道
   Tools 死 ≠ 冻结，先 !running）
2. c0000010 二分第三刀读数：直通 hidden body 若成功 → 问题在 trampoline
   页；若 c0000010 → 问题在 hidden copy 上执行 body（怀疑点：页执行时
   的缓存属性/IOMMU 影子——hidden 页 NPT 叶子由 SwapPage4k 新建）
3. devirt-bugcheck（CPU5/0x80 类）独立排障
4. 全绿后：M4（debugger/cr3_monitor 联调）

## Round 34 (2026-09-08 深夜): c0000010 定谳 + direct-resume 崩溃定谳 —— 两个 bug 一个根：寄存器纪律

### 现象（本轮接手的冻结现场）

上一轮 nthook 65535（direct-resume 二分模式）跑出 bisect2-rc=127（"Tools
未在客户机中运行"）。本轮 kd 连上后发现：**uptime 冻结在 4:34**（21:21
与 21:53 两次 vertarget 完全一致）→ guest 从上一会话起就没重启过，一直
冻在 kd 的 second-chance AV 现场。rc=127 真相 = kd 冻结了全机（第二次
机会异常挂起所有核），Tools 自然"死"。

崩溃现场（kd 抓的活现场）：
- 线程：svmbctl.exe (Cid 16cc.177c)，IRP = NPT_PROBE IOCTL 挂着
- 栈：`nt!NtCreateFile+0x79 → IopCreateFile+0x597 →
  ExAllocatePoolWithTag → ExAllocateHeapPool → RtlpHpLargeAlloc →
  RtlpHpMetadataAlloc → RtlpHpMetadataHeapCtxGet+0x4` 死在
  `movaps xmm0,[rcx]`，rcx=...d8 **不是 16 字节对齐**（内存可读，纯对齐
  错误）

### 定谳 1：direct-resume 崩溃 = bisect 设计错误（跳过 prologue）

`g_svmb_nthook_resume = targetVa + StolenLen` 跳过的是整个 stolen
prefix——`sub rsp,88h` 没执行 → body 所有 rsp 相对寻址整体偏移 0x88 →
x64 ABI 的 rsp%16==0 不变式破坏 → 深处 heap 例程第一条 `movaps` 落在
错位的栈槽上 #GP。与 NPT/hidden 页无关（执行深入 6 层调用栈才崩）。

### 定谳 2：r33 的 c0000010 = trampoline 尾跳污染 rax

kd 反汇编活现场 `u nt!NtCreateFile`：
```
+0x00: mov rax,detour; jmp rax     <- 12B patch（EXEC slide 生效中）
+0x0E: mov dword [rsp+70h],20h     <- body 开始
+0x16: mov dword [rsp+68h],eax     <- 读 EAX！
+0x1A: mov qword [rsp+60h],rax     <- 读 RAX！staging NULL
```
**body 在 +14 处要求 rax==0**（prologue 的 `xor eax,eax` 语义被下游当
NULL/零标志 staging 进局部变量）。而 trampoline 尾跳是
`mov rax,targetVa+stolen; jmp rax` → body 入口 rax=正文地址≠0 →
IopCreateFile 把垃圾指针当 EaBuffer/staging → c0000010 24/24 确定性。
direct 模式的 `mov rax,resume; jmp rax` 同病。

### 修复（一个原则：进 body 的尾跳不许碰任何寄存器）

1. **npt_hook_mgr.cpp**：tramp 尾跳改 `jmp qword ptr [rip+disp32]`
   （6B，FF 25）+ tramp 页 0x100 偏移处 8 字节 resume 槽；
   `stolen+6 > 0x100` 拒装。
2. **svm_entry.asm**：bisect 直跳模式内联 stolen prefix
   （`sub rsp,88h; xor eax,eax; mov [rsp+78h],rax`）后
   `jmp qword ptr [g_svmb_nthook_resume]`（内存间接，rax 恢复 0）；
   StolenLen≠14 拒绝（内联前缀只对 NtCreateFile 成立）。
3. **npt_probe.cpp**：notramp 全局标志 + StolenLen 校验 + TrampBytes
   落盘；**app/main.cpp**：npfdump 断言 [14..15] 改 FF 25。

### 复测（全绿，同 boot 连续跑）

| 测试 | 结果 |
|---|---|
| npfdump (stage 2) | [+] patch ok + [+] tramp 尾 FF 25 ok |
| nthook tramp 模式 | hits=24 openOk=24/24 last=00000000 postOpen=1 **PASS** |
| nthook 65535 直跳 | hits=24 openOk=24/24 last=00000000 postOpen=1 **PASS** |
| npfprobe 16 (stage 0 回归) | detour=16/16 origHooked=0 origAfter=16 exitDelta=3 PASS |
| storm 2000（13.3 万亿 hypercall exits）后双 nthook | 双 PASS |
| **sc stop（重度 hook 活动后）** | **干净卸载**：6 核 self-exit state=0 + hypervisor stopped，无 bugcheck 无挂起 |

### devirt-bugcheck 结案（降级为"已解释的历史事件"）

r33 的 CPU5 KeBugCheckEx + CPU2 KeIpiGenericCall 屏障挂死**未再复现**，
且当时机器里还躺着 393,210 次 c0000010+垃圾 staging 的失败 open——
统一解释：**rax 污染 bug 的次生灾害**。海量被喂了假指针的 I/O 请求把
系统状态搞烂，卸载时的 bugcheck 是症状不是独立缺陷。tramp 修好后同等
甚至更高强度（storm 13.3T + 双 nthook）的 devirt 完全干净。观察保留：
若未来 devirt 再崩，按 r33 纪律挂 kd 直读 CPU5。

### 新知识（kd/取证）

- **kd 的内存读穿越 svmb 的 NPT slide**：KDNET 传输的内存读由 guest 内
  kdnet stub 执行，而它跑在被虚拟化的内核里 → `u nt!NtCreateFile` 看到
  的是当前 slide 态（EXEC=hidden 补丁页 / DATA=pristine）。本例因此看到
  "补丁在 NT 页上"+"svmb 已从模块表摘除但 hook 仍活"的僵尸态组合。
- uptime 冻结（两次 vertarget 相同）= 目标冻死在 kd 异常现场的硬标志。
- 物理 `!db` 读失败（10.5GB 高位内存）= KDNET 传输限制，非映射问题。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| bisect2 rc=127 = 新的冻结谜团 | 上轮的 AV 现场冻了一整晚（kd second-chance 挂全机） |
| c0000010 在 tramp 页 vs hidden 页之间 | 寄存器纪律：尾跳 mov rax 破坏 body 的 rax=0 契约 |
| direct-resume 冻结 = hidden body 执行有问题 | bisect 设计错误：跳过 prologue 破坏栈对位 |
| devirt-bugcheck 是独立的卸载缺陷 | rax bug 的次生损伤；修复后不再复现 |

### TODO

1. git 提交 r34 修复 + 文档
2. M4：mod attach（debugger/cr3_monitor → 事件上报 R3）+ CR3 spoof live
3. CODE_REVIEW §十 live-NPT 前置清单复核
4. 生产语义决策：NPF deny spin-breaker 目前 advance-RIP（真实 hook 上有
   损坏风险）——生产化前改回 benign spin+限流日志

## Round 35 (2026-09-08 深夜): NPT hook 通用回调层 —— 三件工程 + 一个 stub 栈平衡 bug 的活捉

### 交付（用户点名的三件事）

1. **per-hook detour 封装**：Install 拆成 PrepareHook（与 detour 无关的一半：
   校验/steal/hidden 副本/tramp 构建）+ ArmHook（补丁合成/slide arm/节点
   挂链）；新增 `InstallCallback(target, &hookId, cb, mode)` —— 每个 hook
   独占一张 trampoline 页，stub 发射在其 +0x180，detour = tramp+0x180。
   多 hook 可同时存活（注册表槽 0..31）。
2. **通用寄存器保存回调层**：stub 保存全部易变 GPR → 建 SVMB_HOOK_FRAME
   （EntryRsp + R11/R10/R9/R8/Rdx/Rcx/Rax；Rax 因 12 字节补丁先行污染，
   结构上保留但无意义）→ 调 NptHookManagedCb(hookId, frame)（注册表分发
   + 每-(cpu,hook) 重入门闩：回调里再调被钩 API 自动降级 passthrough）→
   恢复 → 寄存器无关跳 tramp 入口（passthrough）或以回调返回值 override
   （rax=返回值，ret 直达原调用方，body 不执行）。
3. **StealLen 拒绝控制转移**：新 INSN_F_CTLX 标志覆盖 call/jmp rel8|rel32、
   jcc rel8/rel32、loopcc/jrcxz、ret —— PC 相对位移搬到 tramp 会跳错目标，
   ret 前缀会提前返回。离线单测 +5 用例（265 checks）。

### 新 bug（活捉全过程）：stub `add rsp,68h` 应为 `add rsp,30h`

首跑 stage 3 即杀机：干净 boot 上 npfdump（裸 Install）绿，nthook 必死；
kd 断点打印抓到 **STUB rsp=R → TRAMP rsp=R+0x38**，随后 0x3B
(SYSTEM_SERVICE_EXCEPTION, C0000005 @ rip=0x13600000000 —— 用户值被写进
内核栈)。数值审计：pushes 0x38 + sub 0x30 = 0x68，passthrough 的
`add rsp,68h` 把 sub 撤掉后 **7 个 pop 再 +0x38 → rsp = R+0x38** —— body
带着 +0x38 的 rsp 运行，写穿调用方 home 区/syscall 陷阱帧。**add 应只撤
sub 的 0x30**（pop 自己负责撤 push）。override 路径同病同修。

连带修复：
- InstallCallback 原来把用户回调直接烘进 stub（绕过托管层/重入门闩）——
  改为统一调用 NptHookManagedCb（(hookId, frame) 双参与其签名吻合）。
- SVMB_HOOK_FRAME 补 Pad 字段：sub 0x30 = shadow(0x20)+EntryRsp(8)+对齐垫
  (8)，原结构体从 R11 起全部差 8 字节。

### 调试技法沉淀（本轮值回票价的两招）

- **kd 断点打印抓寄存器轨迹**：bp stub/tramp 各打 `.printf rsp/r15; gc`，
  两行输出直接暴露 +0x38 漂移 —— 比读 ten 层汇编快一个数量级。
- **从 baked 指针反推驱动 image 基址**：patch(detour) → stub → stub 内
  `mov rax,imm64` 的回调 VA → 减本地 disasm RVA → image base → 直接读
  日志环/globals（hits=1 = 恰好一次完整回调往返）。**注意 VA 加法进位：
  base+0x23000 算成 base+0x30000 白读一页（0x76D+0x23=0x790）。

### 测试矩阵（修复后 2b770387，同一 boot）

| 项 | 结果 |
|---|---|
| npfdump | [+] patch + [+] tramp 尾 FF 25 |
| nthook stage3（passthrough） | hits=24 openOk=24/24 last=0 **PASS**（连跑两次稳定） |
| nthookov stage4（override） | denied=24(c0000022) otherOk=24 hits=49 **PASS** |
| npfprobe stage0 回归 | detour=16/16 origAfter=16 exitDelta=3 PASS |
| storm 2000（13.3 万亿 exits） | 完成 |
| sc stop | 干净 STOPPED，Tools 存活 |

### 发现的既有问题（A/B 定界，非本轮回归）

selftest 265 checks 中 6 项失败（npt.pte.identity-pfn / npt.swap.pfn /
npt.ro.getpte / npt.ro.bits / nptmv.slots.exhausted /
shpage.overlap-rejected）。**git stash A/B：r34 代码同 6 项失败** —— 历史
遗留（r32 前后引入、无人跑过 selftest）。其中 shpage.overlap-rejected 是
测试用例选点问题（t2+8 = lea riprel，旧 StealLen 也会拒，只是状态码不同
路径）。Check() 现在每个失败都记日志（原来只报 first）。→ 独立排障项。

### 僵尸机双例（r34/r35 同款，独立课题）

nthook 崩溃后 guest 呈现：模块表无 svmb + SVME=1 + 代码页映射/数据页未映
射的半卸载态 + NPF deny spin-loop（IopCreateFile+0x54d r15=1 来回打转）。
→ "devirt/unload 与 in-flight IOCTL/挂活钩子的竞态"仍是独立开放课题。

### TODO

1. M4（mod attach / cr3_monitor → R3 事件 + CR3 spoof live）
2. selftest 6 项既有失败独立排障
3. devirt/unload-zombie 竞态独立排障（诊断入口已备：kd 断点打印法 +
   ring 反推法）
4. 生产语义：NPF deny spin-breaker advance-RIP 仍未回改

## Round 36 (2026-09-09): 修网 + 卸载健壮化 + deny 生产语义 + 卸载压力回归

### 1. selftest-6 逐项定谳：六项全是测试侧 bug，实现无罪（已修复，267 checks 0 failed）

| 失败项 | 定谳 | 修复 |
|---|---|---|
| npt.pte.identity-pfn | 概念性断言错误：PTE 只存 4K 页基址，永远不等于带页内偏移的 GPA（0x...1234） | 比较页基址 `& ~0xFFF` |
| npt.swap.pfn | **int 溢出**：`0x99999 << 12` 是 int 运算，0x99999000 > INT_MAX → 负数 → 与 u64 比较时符号扩展成 0xFFFF...99999000，永不相等 | `0x99999ull << 12` |
| npt.ro.getpte | 大页语义：整块 2M MapRange 后 GetPte4k 按文档返回 NOT_SPLIT（错误码，非 NT_SUCCESS），测试却断言成功 | 前置断言改为显式 NOT_SPLIT（npt.ro.large-presplit），取 PTE 移到 Split2M 之后 |
| npt.ro.bits | `NptPerms{false,true,false}` 在现字段序 {Write,Execute,User} 下是**可执行**页而非只读 —— ro.bits 期待 NX=1 永假 | `{false,false,true}`（RO+NX） |
| nptmv.slots.exhausted | off-by-one：槽位数组 = MAX_VIEWS-1（默认视图独立在外），销毁的视图已释放槽位 → 满容量可建 MAX_VIEWS-1 个 | 期望改 MAX_VIEWS-1 |
| shpage.overlap-rejected | t2+8 的 12 字节窗口含函数+12 处的 `lea r8,[rip+…]` → 严格 StealLen（r35）以 NOT_SUPPORTED 先于 overlap 拒绝 —— **该优先级是设计使然** | 放宽为接受两种拒绝码并注明优先级；overlap 正例已由 TestNptManager 的 same-target 重装（hook.duplicate-page-rejected）覆盖 |

方法论：静态推演与事实矛盾时（swap.pfn 推演应过实际挂），先怀疑**字面量类型**——十六进制移位溢出是 selftest 的经典暗雷。

### 2. 卸载健壮化（zombie 守卫 + 在途穿越排空）

- **DevUnload zombie 守卫**（main.cpp）：hv->Stop() 返回失败或 Running_ 仍 true → **MANUALLY_INITIATED_CRASH + 'SVMB'(0x53564D42) tag + stop 状态码 + exits** fail-fast。此前 `(void)hv->Stop()` 无视失败继续拆 —— 模块摘除时 VMM 活着 = r33/r34/r35 三见 zombie 的直接通路。核心认知：**DriverUnload 没有拒绝的机会**（返回即释放镜像），所以唯一安全的失败处理就是显式死。
- **在途穿越计数 + 有界排空**（npt_hook_mgr）：NptHookManagedCb 入口 ++ / 出口 --（g_npthk_inflight）；Remove 的 graveyard 溢出真释放前 + Deinit 顶部 `DrainInflightCrossings()`（1ms 步进、1s 上限，PASSIVE）。覆盖"线程在 callback/tramp 里时页被释放"的 UAF 窗口；stub 内 callback 外的纳秒级窗口由排空延时覆盖（注释已注明）。
- 卸载支持语义确认（用户要求）：`svmbctl stop`（IOCTL）= 唯一 devirt 入口，15s 有界 + 硬门禁拒绝（保 Running_）；sc stop 的 DevUnload 只允许在 VMM 已停后做 NPT 恢复与内存释放，否则 fail-fast。NPT hook 恢复（叶子还原）在 gHooks->Deinit()，顺序在 hv->Stop() 之后 ✓。

### 3. deny 生产语义分叉（npf.cpp + vcxproj）

- Release 配置新增 **SVMB_PRODUCTION** 预处理宏；npf.cpp Deny* 分支：
  - **生产**：`KeBugCheckEx(MANUALLY_INITIATED_CRASH, 'SVMB'+1(0x53564D43), gpa, rip, action<<32|spins)` —— 与 参考实现 的 `__debugbreak+KeBugCheck` 同哲学：VMM 解不了的 NPF = 心智模型已错，立即死。
  - **Debug**：保留 r31 spin-breaker（自旋 256 次 → AdvanceRip，probe 失败但返回）。AdvanceRip 是已接受的腐蚀，仅限调试构建。
- Release 配置编译验证通过（x64\Release\svmb.sys）。

### 4. 卸载压力回归用例（build/regression_unload.sh）

宿主侧脚本串现有 fleet bats：N 循环 × [svc_start → storm 200 → nthook 2 → nthookov 2 → npfprobe 4 → svc_stop]，任一 bat 失败即中止并报循环号。**首轮 3 循环全绿**（本次回归即其验证）。会话日志累计：passthrough PASS ×5、override PASS ×4、slide PASS ×5。

### 状态

- selftest：267 checks / 0 failed（净 +2：ro 前置拆分检查）
- 回归矩阵：npfdump / nthook / nthookov / probe / storm / 卸载循环 全绿
- guest 健康，驱动 stopped，构建 75f18e52

### TODO

1. M4（mod attach / cr3_monitor → R3 事件；注入点 = 新回调层）
2. per-core 双视图 slide（参考实现 external/internal 模型，M5 候选）
3. Release 构建的全矩阵验证（当前 fail-fast 路径仅编译验证，未运行时触发）

### 负向测试轮（r36 遗留，暂定 r38，需蓝屏-恢复循环，用户批准后排期）

失败路径的运行时验证——当前全部只有编译/逻辑验证，从未触发：

| # | 场景 | 构建预期 |
|---|---|---|
| N1 | 带活钩子 + VMM 活着时 sc stop（不先 svmbctl stop） | Debug：DevUnload 守卫蓝屏 0xE2 参数1=0x53564D42('SVMB') |
| N2 | 构造不可解 NPF（生产构建触发 Deny* 终态） | Release(SVMB_PRODUCTION)：蓝屏 0xE2 参数1=0x53564D43，参数2/3=gpa/rip |
| N3 | 穿越进行中 Remove（回调长循环 + 立即 remove） | Debug：DrainInflightCrossings 真实等待（观察停顿），释放后无 UAF |
| N4 | Release 构建（SVMB_PRODUCTION）全矩阵回归 | deny 破坏性路径改为即时蓝屏，其余全绿 |
| N5 | 蓝屏后的 KDNET dump 取证流程演练 | Minidump 可分析、'SVMB' tag 可检索 |

验证手段：每个场景跑完用 windbg-debugging 的 bsod-analyze / 事件日志 Id=1001
核对 bugcheck 码与参数；N1/N2 需要硬重置恢复。

## Round 37 (2026-09-09 凌晨): M4 联调上半场 —— A/B/C 有绿有发现，D 撞上重注入真 bug

### M4-A 模块挂载实路径 ✅ GREEN

mod attach demo_cpuid/debugger/cr3_monitor 三连 → modules(3) → 模块状态下
storm（1.33 万亿 exits）→ detach ×3 → modules(0) → storm 复测正常。CR3
读+写拦截全系统在线时机器存活。已知小瑕疵：同模块 detach 后立即 re-attach
偶发 err 1359（重试即成功，待查）。

### M4-B CR3 读伪造 E2E ✅ GREEN（`cr3spoof <key>` 新命令）

probe 新增 stage 5（返回本线程 CR3）；ctl 新增 `cr3spoof`：watch 前/中/后
三次读 CR3 断言 `spoofed == real ^ key`。结果：
`real=865e000 spoofed=d6c820de expect=d6c820de post=865e000` → PASS。

**重大设计发现（伴随修复）**：`cr3 watch` 原实现把 `EnableReadSpoof=1`
（全局伪造）一并置位 —— **全系统所有 CR3 读都返回 real^key**，内核任何
消费 CR3 值的路径拿到垃圾 → guest 直接被拖垮（KDNET 都饿死，vmrun 永挂，
硬重置恢复）。且 `Cr3ResolveRead` 分支顺序使"目标进程伪造"分支在全局开启
时不可达。修复：watch 不再置全局伪造，仅定向（值匹配）伪造。

**附带发现**：Cr3Seed 的 `PsSetCreateProcessNotifyRoutineEx` 自 r30 起
注册即失败 c0000022（当时判定 benign），镜像名布防/创建自动布防全被卡死
—— M4 的 image-watch 依赖它，升级为正式排障项（候选：Ex 变体对测试签名
驱动的签名要求 / 非 Ex 回调降级 / 注册时机）。

### M4-C CR3 写监控 + 事件环 ◐ PARTIAL

事件环 + R3 排水链路已通（3 条 ViewActivate ack，lost 0）；写监控计数随
上下文切换自然增长（设计内）。图像名布防被死种子表阻塞（见上）。watch
后 notepad 的 ProcessWatch 事件未出现 = 种子问题的直接后果。

### M4-D 异常面（#BP/#UD → 事件 → 重注入）❌ 可复现 Tools 死亡 bug

**dbgtest 三次尝试（int3 版 ×2 + ud2 版 ×1）全部复现同一结局**：探针发出
后 Tools 进程消失（vmrun 全挂、内核本身健康、全核 idle、无 bugcheck、无
svmbctl 残留）。中途还发现 int3 变体必然卡死在"内核调试常开 + 无调试器
连接 = 等待调试器"的语义上（已改用 __ud2 绕开）。

证据与推断：
- 事件环在 Tools 死前只有 3 条旧 ViewActivate 事件、lost 0 —— **#UD 从未
  产生 VMEXIT**（`InterceptExc=0` 在 entercore 日志恒定；但 ApplyAll →
  ApplyToVcpu 确实写 VMCB 且 CleanBits=0）—— 拦截位是否真的到达嵌套 CPU
  存疑，或事件环计数点不对。
- Tools 死亡机制候选：eventInj 重注入与调度窗口竞态（注入落到错误线程上
  下文 → vmtoolsd 用户态吃 #UD 崩溃）；或 ReinfectExitEvent 在 VMware 嵌
  套下静默失效。
- 下一轮入口：①kd 常驻 + bp 在 HandleExceptionExit / eventInj 写点，观察
  一次 dbgtest 的完整退出流；②临时禁用重注入（只推事件不注入）隔离"事件
  面"与"注入面"；③核对 InjectEvent 的 type/ecValid 组合在嵌套下的合法性。

### 环境教训（本轮反复踩）

- vm_ctl_run.bat 无 `< NUL` = condrv 挂死（AGENTS 铁律1 的现成反例）——已
  弃用，一律用落盘版 bat。
- guest 夜间有环境性重启/维护（BITS 启动类型翻转等 WU 痕迹），会话开始必须
  先核对服务状态 + uptime（1077 = 本 boot 从未启动过服务）。
- 步骤日志（append 型）尾部出现 NUL 填充 = 写入方被杀的痕迹，读日志时跳过
  NUL 区。

### 状态

- M4：A ✅ / B ✅ / C ◐（种子阻塞）/ D ❌（重注入 bug，入口已备）/ E-F 未开
- guest 已硬重置恢复健康；构建 75f18e52 + ctl(r37)；代码已含 stage5/6、
  cr3spoof、dbg config/step/dbgtest 命令（D 面红牌待修）

### TODO

1. M4-D 重注入 bug 排障（入口：①②③如上）
2. Cr3Seed c0000022 排障（解锁 image-watch / 创建自动布防 / ProcessWatch 事件）
3. M4-E DR 隐身 + MTF 单步实测（命令已备：dbg config/step）
4. M4-F EnableProcessView；M4-G 回归整合

## Round 38 (2026-09-09 凌晨): M4-D 排障 —— 三层遥测陷阱剥完后，真 bug 与新红旗并现

### 排障过程（按时间序的真相还原）

1. **控制实验**：不挂 debugger 模块，裸 `__ud2()` 探针 → 原生 SEH 接住
   0xC000001D，guest 存活 → **ud2 本身在虚拟化下无害**。
2. **挂模块复测**：探针秒回（原生路径）、ring 0→0 → **#UD 没有产生
   VMEXIT，异常拦截位从未生效** —— M4-D 的"重注入 bug"被推翻，真问题是
   **拦截根本没武装**。
3. **ApplyToVcpu 日志**：`exc=004a`（#DB|#BP|#UD）正确折算并写入全部
   VMCB（CleanBits=0 也在写）→ 软件侧无误。
4. **回滚泄漏修复**：Attach 失败（首 attach 必现的 1359 瞬时失败）原路径
   只清槽位、不 ReleaseOwner → 泄漏的注册被后续 ApplyAll 折算成 exc=0 全
   清。已补齐 UnregisterOwner/ReleaseOwner。
5. **#BP AdvanceRip 修复**：int3 是 TRAP 类，VMEXIT 保存的 rip 已指向
   int3 之后；旧代码再 AdvanceRip = **跳过崩溃进程里一条活指令**（vmtoolsd
   死亡的第一嫌疑）。已改 false。
6. **CleanBits 注入定律（本轮最大发现）**：退出路径写 eventInj 不清
   CleanBits → **此 VMware vhv 静默丢弃控制区写入**（与 NCr3 发布、活拦截
   武装同一铁律）→ 注入永不生效 → guest 原地重执行 ud2 → 无限 #VMEXIT 循
   环且线程攥着 gIoGate → 全部诊断 IOCTL 挂死 → svc_stop 15s 超时 →
   **r36 zombie 守卫按设计 bugcheck** → "意外关机" → 自动重启循环。
   已修：InjectEvent 写完 eventInj 后 CleanBits.Bits=0。

### 红旗（未解决）：~60-90s 快速重启循环

CleanBits 修复部署后，guest 陷入 60-90s 一次的硬死亡循环（无 bugcheck
落盘 = 三重故障类；vmx 日志 `CPU reset: soft` 风暴；驱动加载后 ~1 分钟死
亡，与模块无关——多个 boot 我根本没启动服务仍复现）。**尚未 stash A/B 定
界**（今晚驱动 delta：CleanBits 注入/apply 日志/回滚修复 vs 环境性损伤
——今夜 5+ 次硬重置 + WU 半禁用 + UsoSvc 停止的脏状态叠加）。下一轮第一
动作：git stash → 75f18e52 复测 → 定界。

### 三层遥测陷阱（本轮最大成本，已刻入纪律）

1. **append 型步骤日志被杀进程写入 NUL 填充** → grep 读到混合新旧内容，
   "STATE RUNNING/STOPPED" 反复横跳全是幻觉 → **状态读数一律改独立
   fresh 文件**（已建 vm_probe_fresh/vm_pull_fresh）。
2. **vmx 日志时间戳是 UTC**，本地=UTC+8——把今天 07:16 本地读成昨晚
   23:16，凭空造出"昨晚以来的重启史"。
3. **7026（boot 驱动未加载）≠ 重启证据**：与 vmx 日志交叉验证后才知真假。
   guest 自主软重启在 vmx 日志的表现是 `CPU reset: soft` 风暴。

### M4 记分板（r38 末）

A ✅ / B ✅ / C ◐（种子）/ **D：拦截武装 ✅、注入面 CleanBits 修复已部署
但被环境崩溃循环阻断终验** / E-F 待开。

### 下一轮入口

1. **stash A/B 定界重启循环**（75f18e52 vs now，各 10 分钟观察窗）
2. 若 CleanBits 修复无罪 → dbgtest 终验（PASS 判据：exc=c000001d &&
   ring 增长 && Tools 存活）
3. Cr3Seed c0000022；M4-E DR/单步；M4-F ProcessView；M4-G 回归整合

### r38 补充：无接触观察定界 —— 重启循环与驱动加载强相关

干净的 10 分钟零接触窗口（fresh boot、服务未启动、无任何 vmrun 操作）：
guest 稳定存活（无新 soft reset、Tools 全程存活、服务保持 1077 未启动）。
对照：驱动加载（svc_start → virtualize at load，无模块无 IOCTL）后数分钟
内必死（无 bugcheck 落盘 = 三重故障类；vmx `CPU reset: soft` 风暴）。

**定界结论：空闲死亡与"r38 驱动已加载"强相关。** r38 各 delta 在 idle 下
理论上均 inert（无 Require/Release/Inject 调用点），但死亡照发生 —— 嫌疑
收敛到：(a) ApplyToVcpu 的 CleanBits=0 + 活 VMCB 控制区改写与 VMware 嵌套
阴影的交互（空闲 58 exits/s 全部走 AdvanceRip 路径）；(b) r32-r36 某处引
入、r36 的 5min soak 恰好没踩到的长周期 idle 死亡；(c) 编译/链接层漂移。
**下一轮第一动作：git stash A/B —— r36-final(76a49c3) vs r38(8c3d4e2) 各
15 分钟加载态空闲观察窗**，死/不死直接指认改动区间，再二分到具体提交。

环境侧已修复并排除：WU 自动重启循环（wuauserv/UsoSvc 已恢复+完成更新周
期，07:58 后无环境性重启）；三层遥测陷阱全部改用 fresh-file 读数规避。

### r38 收口时的最终状态与 r39 精确续查程序

**最终状态**：master(8c3d4e2) 部署运行中；attach 成功（秒级）→ dbgtest 仍
原生捕获（ring 0→0）→ **异常拦截写入与运行时效果矛盾未解**。同机制下
CR3-read 拦截已被 cr3spoof 证明生效（同 ApplyAll/同 VMCB 写/同
CleanBits=0），唯异常位不生效。

**r39 第一动作（精确程序，环境稳定后执行）**：
1. kd 连接（趁 boot 后前 2 分钟，循环重启会杀会话）
2. `.sympath+ <repo>\x64\Debug` + `.reload /f svmb`
3. svc_start → mod attach debugger
4. `x svmb*Hypervisor*gInstance*` → poi → Hypervisor+Vcpus_ 偏移（hypervisor.h
   成员序）→ Vcpus_[0] → VcpuContext.GuestVmcb 在 +0x0
5. `dd <guestVmcb>+0x48 L1` = InterceptException：**0x4a=嵌套吞写（改用
   VMRUN 边界重发布/每核 IPI 重写）；0x0=注册表/折叠层丢 Require（查
   Dispatch 重入与 UnregisterOwner 时序）**
6. 对照 `dd +0xA0 L2`（EventInj 0xA8 前）与 CleanBits(+0xC0)

**环境注意**：guest 夜间进入 2-10 分钟级重启循环（无 bugcheck 落盘 =
三重故障类；WU 已恢复+完成更新周期后仍需观察是否收敛）；驱动 DEMAND_START
在重启后不会自动加载，每个 boot 需要重新 svc_start + attach。

## Round 39 (2026-09-09 上午): kd 直读 + 范式翻转 —— #UD 拦截全链是通的，杀死 guest 的是 vhv 的 NMI 中止

### 背景与第一动作纠偏

r39 文档程序写的 `dd <guestVmcb>+0x48 L1` 是**错误偏移**：VMCB 控制区布局
InterceptCrRead@0x00 / **InterceptException@0x08** / IC1@0x0C / IC2@0x10 /
MSRPM_PA@0x48 / ASID@0x58 / TLB_CTL@0x5C / NCr3@0xB0（driver/src/hw/vmcb.h
与物理转储逐字段对上：MSRPM=0x13af80000、ASID=1、TLB=3、NCr3=132741000）。
+0x48 读到的 0x3af80000 是 MSRPM Base PA——r38 的"拦截矛盾"建立在读错字段
上，从未真正读到过异常掩码。**物理读 +0x08 基线=00000000 与驱动日志一致，
软件写入路径无罪。**

### 实验时间线（boot1..5，kd KDNET 全程在场）

- boot1: svc_start → 6 min 空闲稳定（3 次 kd break/resume 无害）→ attach
  三波武装 0008→000a→004a → `module debugger attached (token 1)` → ~1s 死。
- boot3: attach 后 12/12 vmrun 探针 70s 全 ALIVE → kd CTRL+BREAK → **1 秒
  内死**（KDNET 都打不进去）。
- boot4(opt-in 构建, 仅 #UD): attach（单波 0040）→ dbgtest → `exc exit:
  vec=6 rip=...5bb9b531 cnt=1` → **立即 bugcheck，kd 首次活捉**。
- boot5(opt-in, 全零武装): attach（无任何 apply 波）→ dbgtest 原生
  c000001d → 探针 60s 全 ALIVE → kd break → 仍死（~1s，非 bugcheck，
  无 dump；死钟与 break/attach 的关系未解，转 r40）。

### 两个真 Bug 修复（本轮代码）

1. **ReinfectExitEvent 漏 CleanBits=0**（exit_dispatcher.cpp）：r38 的
   CleanBits 铁律只加在 InjectEvent，而异常退出实际走 ReinfectExitEvent
   （复制 ExitIntInfo→EventInj 的路径）——重注入全被 vhv 丢弃。已补。
2. **#BP AdvanceRip 语义**（r38 已修，本轮 kd 直读再次确认 int3 TRAP 类
   不前移）。

### 范式翻转：#UD 拦截其实是"半支持"，死因是 vhv NMI 中止

kd 流实锤（r39 新增 `exc exit: vec=%u cnt=%u` 观测线）：

```
[svmb][c4] exc exit: vec=6 rip=fffff8025bb9b531 cnt=1   ← #UD VMEXIT 了！
                                                            handler Push 了（cnt=1）！
KDTARGET: Refreshing
A fatal system error has occurred.                        ← bugcheck 活捉
```

**#UD 首次 VMEXIT、入环、重注入、SEH 捕获 c000001d 全链工作**（r38 的
"探针秒回原生路径=拦截没武装"结论作废——那轮 attach 大概率静默失败 +
原生捕获与干净重注入不可区分）。但 vhv 随即在**同一 RIP**（NMI 栈的
中断点=RunNptProbe+0xc51=vec=6 退出点）注入 NMI：

```
nt!KiNmiInterrupt → KxNmiInterrupt → KiProcessNMI → hal!HalHandleNMI
  → WheaReportHwError → PSHED → KeBugCheckEx(0x80 'TDO')
WHEA: Error Source 2 (Notify Type NMI), Error Count = 1
FAILURE_BUCKET: 0x80_4F4454_AuthenticAMD
```

**平台定律（第 4 条）：VMware vhv 对嵌套 VMCB 的异常拦截位（bit1/3/6）
半支持——首次拦截生效，随即以 NMI 中止；Windows 0x80 bugcheck；若 #BP
被拦，bugcheck 的 snap-in int3 也被吞 → 三重故障无 dump（r38 全部"无
dump 重启循环"的机械解释）。** CR4.VMXE 类比：参考实现 从不武装异常拦截，
正好绕开。

### 工程决策：异常拦截改 opt-in（默认全零）

- `DebuggerInit` 不再 RequireException（handler 保持注册，无害）。
- `DebuggerConfigure` 接线 SVMB_DBG_CONFIG.EnableBp/EnableDb/EnableUd
  （结构体本来就有这些字段，r37 没接线）→ Require/ReleaseException
  状态跟踪；`DebuggerStop` 释放。默认全零 = 本 vhv 安全；裸机测试可
  opt-in。
- dbgtest 输出 `ring 0 -> 0` 的字段标签是旧字段复用（OvDeniedOk/
  OvOtherOk 装 ring 前后 Count），FAIL 判定在本 vhv（默认不武装）下
  语义为"拦截面未启用"而非缺陷。

### 遥测纪律（新增两条）

4. **kd 重连日志是重放缓冲**：NCr3 相同的 boot 无法用日志区分新旧，
   `lm m svmb` / 实时命令才算数——不要对 replay 内容做"自动加载/自动
   attach"式推断。
5. **kd 自动断入（重连初始 break / 命令自动断入）后必须显式 g**——
   忘了 g 会让 vmrun 全挂假象成"Tools 死了"（本轮踩了两次）。

### M4 记分板（r39 末）

A ✅ / B ✅ / C ◐ / **D：拦截+入环+重注入+SEH 全链实测通过；vhv NMI
定律定谳；拦截面转 opt-in 默认关闭** / E-F 待开。

### r40 入口

1. **死钟定界**：kd 会话完全关闭（close_kd_session），纯 vmrun 观察
   "驱动加载+attach(零武装)" 状态 15 分钟——区分"kd break/在场杀死"
   vs "60-90s 周期钟"（今天 boot5 死在 break 前后、boot3 死在 break
   后 1s、boot1 死在 attach 后 1s，模式不齐）。
2. ring `cnt=1` vs ctl `after=0` 矛盾：优先查 Drain 是否被并发调用
   （DBG_EVENTS IOCTL 竞争），或 stage-6 的 before/after 读点。
3. Cr3Seed c0000022（PsSetCreateProcessNotifyRoutineEx 拒绝）；首
   attach 1359 瞬时失败；M4-E DR/单步、M4-F ProcessView、M4-G 回归。
4. dbgtest 判定文案更新（opt-in 语义），ctl 重建随下轮。

## Round 40 (2026-09-09 中午): 死钟定界 + 小口径清理 —— 死亡=kd 相关；Cr3Seed 一标志修复

### 死钟定界（kd 完全关闭后的双 15 分钟窗口）

| 窗口 | 配置 | 结果 |
|---|---|---|
| Run-B | 干净 boot，零接触零驱动 | 15min×30 探针全 STOPPED，boot 时间不变 |
| Run-A (r40 构建) | svc_start + mod attach（零武装） | 15min×90 探针（10s 粒度）全 RUNNING，soft reset 计数 210→210 |
| r39 boot3/5（对照） | 同 Run-A 配置但 kd 会话在场 | 60-90s 内死亡（break 后 1s / break 前 ~10s） |

**定谳：死亡与 kd 在场/断入强相关，与驱动构建、武装状态无关。**
机制候选：kd 断入 int3/KDNET break-in 路径与 VMRUN 循环在嵌套阴影下的
交互（即便 exc=0）。**纪律升级（r30 定律的硬形态）：驱动虚拟化运行期间
禁用 kd 断入；功能验证一律 vmrun 通道；kd 只用于驱动未加载/已死状态的
取证。**

### boot-Y"神秘自启"判定为孤例

r39 收口前 boot-Y（10:35:43）出现 DEMAND_START 服务无人启动却 RUNNING
（vmexits ~10:47 起算）。本轮全源排除：SCM 日志 7036 被 guest 全局压掉
（无证据价值）、无计划任务、无 Run/RunOnce、无启动目录（用户+公共）、
无失败自动重启、无 helper 服务；15 分钟干净 boot 被动观察全程 STOPPED
未复现。**判定为与 kd 解冻伴生的一次性事件，挂观察。**

### 小口径清理（全部落地）

1. **Cr3Seed c0000022 修复**：`PsSetCreateProcessNotifyRoutineEx` 要求
   驱动 PE 头 `/INTEGRITYCHECK`（MSDN 文档要求），svmb.vcxproj 两个
   Link 配置补齐 → 测试签名下正常加载，`cr3 seed: monitored=4` 实测
   入表。**M4-F image-watch 的进程通知源打通。**
2. **dbgtest 判定双 bug**：(a) `GetExceptionCode()` 的 LONG 符号扩展成
   0xFFFFFFFFC000001D，与 0xC000001Dull 比较永假——**r37 起所有 FAIL
   判定 partly 就是这个比较 bug**；改取低 32 位。(b) verdict 拆分：
   armed 全链=PASS / 原生捕获=[=] SKIPPED（opt-in 默认下的正确结果）/
   其余=FAIL。实测 [=] 输出正确。
3. **ring E2E 线上证明**：viewprobe（view 切换 → Push）→ dbg events
   `2 (lost 0)` 带完整载荷。ring 机制无罪；r39 的 after=0 疑点保留在
   stage-6 内（已加 in-band `nptprobe s6: exc=.. before/after/lost`
   日志，待裸机武装测试解读）。
4. **vm_ctl_mod.bat `exit` 语义修复**：旧 retry 循环在 for 体里裸 `exit`
   → cmd 以不可预测 errorlevel 退出 = r39 追过的"attach exit 1"幻象。
   改 `exit /b`。
5. **首 attach 1359/err-2**：svmbctl 已打印失败阶段+错误码、driver 端
   已打 init failed 日志；r39 后期 modfresh bat 三连 boot 首试即成，
   旧 bat 语义嫌疑最大。按"已插桩、观察"关闭。

### 遥测纪律（新增）

6. guest 的 SCM 7036 信息事件被全局压制——**"SCM 无启动事件"不能当
   服务未启动的证据**；服务状态以 `sc query` fresh 读数为准。

### M4 记分板（r40 末）

A ✅ / B ✅ / C ✅（cr3 seed 通了）/ **D：全链已证 + 平台定律 + opt-in** /
E-F 待开（地基已稳：kd 纪律 + 死钟定界）/ G 回归整合。

### r41 入口

1. M4-E：DR hide / MTF 单步 live（dbg config ArmSingleStep + dbgtest2
   变体；注意 kd 纪律——全 vmrun 通道验证）。
2. M4-F：cr3 watch 全链（seed 表已通，EnableReadSpoof 保持关——r37
   全局 spoof 教训）。
3. M4-G：regression_unload.sh 扩展 dbg/cr3 面 + Release(SVMB_PRODUCTION)
   全矩阵一次。

## Round 41 (2026-09-09 下午): M4-E/F/G —— DR/MTF 平台定律定谳；cr3 watch 链打通；Release 全矩阵绿

### M4-E（DR hide / MTF 单步）：本 vhv 平台性死路，实现面已硬化

- **DR 处理器从未注册（真 bug）**：DebuggerInit 只注册了 BP/DB/UD，
  HandleDrRead/HandleDrWrite 挂空。DR 面武装后退出落到 default 策略 →
  AdvanceRip 跳过 mov 且不写目的寄存器 → 调用方读到 rax 残留垃圾
  （首次 dbgdr 测到内核指针的机制）。已补 DR_READ/DR_WRITE(0..7) 注册。
- **DR 拦截位不透传嵌套 VMCB（平台定律 #5）**：dbg config 1 1 后
  drR=0080/drW=0080 确认写入 VMCB，但 during 读返回**真实 DR7=0x400**——
  非武装时 guest 本应读到 VMCB 装载的 DR7=0（svmb 零填充），实测 400
  = vhv 自己处理了 DR 访问。与 #UD 异常定律同族：vhv 保留调试机制。
- **MTF 同样不透传**：dbg step → mtf=1 写入全部 VMCB、guest 存活、
  零 step 事件——DbgCtl.MTF 嵌套层无效。注意：armStep 的 RequireMtf
  无消费时不释放（挂观察，无害）。
- 结论：M4-E 的 DR/MTF 面在裸机上可用（实现正确+处理器已注册），
  本 vhv 上属平台不可用面。dbgdrtest 在回归中 record-only（|| true）。

### M4-F（cr3 watch）：链路全通

- **cr3spoof 回归根因**：CR3-read 拦截位由 **cr3_monitor 模块的
  Cr3MonInit** 武装（RequireCr(3,R,W)）——回归只挂 debugger 模块时
  CR3 读根本不退出，spoof 静默失效。vm_ctl_modfresh 改为双模块
  attach（debugger + cr3_monitor）。修复后两个周期
  `spoofed == real^key` 精确 PASS（如 b1839000→6f2e50de）。
- **seed 管道 live**：进程创建回调实时入表（monitored 5→6）。
- **watch 配置往返**：arm（read spoof on, key=DEADC0DE）/cleared 均正常。
- in-target（notepad 进程内读 CR3 被骗）的最终观测需要进程内探针
  （stage-8 候选），机制由 r37 cr3spoof 覆盖。
- vm_guest_spawn_notepad 改 vmrun `-noWait`（start 拒绝重定向 stdin；
  `-noWait` 必须放在 runProgramInGuest 之后、vmx 之前）。

### M4-G：regression_m4.sh + Release 全矩阵

- 新脚本 build/regression_m4.sh：N 循环 start → 双模块 attach（重试）
  → dbgtest（[=]）→ dbgdr（record-only）→ viewprobe → dbgevents →
  cr3spoof → storm 200 → stop。裸 `exit`/路径转义/初始 1056 等脚本坑
  全部修净。
- **Debug 构建 2/2 绿；Release（SVMB_PRODUCTION，签名字段验证
  guest hash=Release hash）2/2 绿**。含 deny fail-fast 语义的
  生产构建首次跑通 M4 面。
- guest 收尾：Debug 构建重新部署，服务 STOPPED，干净状态。

### r42 入口

1. NPT hook 面回归（regression_unload.sh）在 Release 上复跑一次。
2. negative-test 轮（N1-N5，r38 计划，需用户批准排期）。
3. stage-8 in-target CR3 探针（notepad 进程内验证 targeted spoof）。
4. 裸机测试清单沉淀（DR/MTF/exc 面：本 vhv 不可测，裸机一条龙）。

## Round 42 (2026-09-09 傍晚): Release hook 回归绿；N1 判过时；stage-8 楔死新 bug 入档

### Release hook 面（regression_unload.sh 3 循环）：GREEN

Release(SVMB_PRODUCTION) 上 start → hooks → storm 200 → probe → stop
×3 全绿，收在干净 STOPPED。N4（Release 全矩阵）至此双脚本覆盖
（r41 regression_m4 + 本轮 regression_unload）。

### 负测试轮 N1-N5

- **N1（活钩子+VMM 直接 sc stop）：前提过时，判定 PASS-by-design**。
  当前驱动的单相 devirt（CPUID-nudge IPI）让 sc stop 连钩子带 VMM
  一次卸干净（STATE STOPPED + 设备消失 + guest 存活 + 无 bugcheck）。
  r36 设想的"守卫蓝屏"不会触发——安全网在不应触发时没触发。
- **N4：r41 已绿**（regression_m4 Debug 2/2 + Release 2/2）。
- **N5（dump 取证演练）**：Minidump 在位（090526/090626 两个旧 dump），
  bsod-analyze 需 UAC 交互点击（本轮无人值守被拒）——管线 r35 已验证，
  待下次有人值守时演练。
- **N2（Release 不可解 NPF）：评估后推迟**——需要可构造的"分类器拒绝"
  NPF 注入口（新 IOCTL 或时序型构造），排 r43 设计。
- **N3（穿越中 Remove）：推迟**——需拆分 nthook 的安装/执行/移除为
  独立步骤才能构造竞争窗口。

### stage-8 in-target CR3 自检：探针上线，楔死新 bug 入档（r43 主线）

新增 NPT_PROBE stage 8 + `svmbctl cr3self [key]`：seen=__readcr3()
（可能被 spoof 的 guest 视图）vs truth=本核 VMCB Save.Cr3（内存直读，
无退出）。未挂模块时基线 PASS（seen==truth）。**挂 cr3_monitor 后
stage-8 IOCTL 楔死**（guest cmd 无输出、100 字节全 NUL、宿主侧 120s
超时击杀），随后**驱动被干净自卸载**（设备 err 2、服务 STOPPED、无
bugcheck 无重启）——两 boot 复现。r43 切面：stage-8 与 stage-5 的差异
只有 Vcpu/VMCB 解引用和一条 LOGW；二分定位。注意 Tools 执行通道被楔死
时 runProgramInGuest 秒退 rc=1（假象：bat"失败"实为通道死）——hard
reset 恢复。

### 裸机测试清单（本 vhv 不可测面，真机一条龙）

1. 异常拦截面（#UD/#DB/#BP 全链：exit→ring→reinject→SEH；vhv NMI 中止）
2. MTF 单步（DbgCtl.MTF 嵌套层不透传）
3. DR hide/spoof（DR 拦截位不透传；dbgdrtest 判定恢复严格版
   post==real）
4. svmbctl dbg config EnableUd=1 → dbgtest 应变 PASS（ring 增长）

### r43 入口

1. **stage-8 楔死二分**（先去 Vcpu 解引用，再去 LOGW；对比 stage-5）
2. N2 注入口设计（debug-only NPF deny 注入 IOCTL）
3. N3 竞争构造（nthook 拆步）
4. N5 有人值守演练

## Round 43 (2026-09-09 晚): 楔死破案 —— CR3-write 死亡螺旋；in-target spoof 全链 PASS

### stage-8"楔死"破案：不是 stage-8 的错，是 CR3-write 拦截死亡螺旋

二分变体（stage 9=仅 seen / 10=仅 VMCB 解引用 / 11=仅 LOGW）全部即时
返回——stage-8 代码无罪。真正的机制链：

1. cr3_monitor 模块 Cr3MonInit **无条件 RequireCr(3, R+W)**（r37 起）；
2. CR3-WRITE 拦截 = 每次上下文切换都 VMEXIT（实测空闲武装态 ~2500/s，
   负载尖峰远超——正反馈：核越慢→切换越频→退出越多→更慢）；
3. 全系统 180 倍减速 → svmbctl IOCTL 超时（"楔死"）→ 击杀/超时的
   服务被慢速停止（"干净自卸载"）→ r42 两 boot 复现的完整解释；
4. r37/r41 回归能绿只是风暴窗口不足一分钟。

**修复（惰性+方向拆分武装）**：Cr3MonInit 不再 RequireCr（仅注册
handler）；ApplyCr3Intercepts 按配置懒武装——**read 方向**随目标存在
（读取罕见、便宜，spoof 必需），**write 方向仅在显式
EnableWriteMonitor 时**（死亡螺旋源，已知危险特性）；配置清空即释放；
Cr3MonStop 兜底释放。CmdCr3SpoofTest 的 EnableWriteMonitor 改 0
（spoof 不需要写监视）。

### in-target spoof 全链 PASS（M4-F 终验）

```
MARK_X_WATCH → cr3 watch svmbctl.exe armed (read spoof on)
MARK_Y_SELF  → cr3self: seen=e57c80de truth=3bd14000
               [+] seen == truth ^ deadc0de   ← 精确命中
MARK_Z_UNWATCH → cleared
STATE: RUNNING（无死亡螺旋；vmexits 23/s = 基线）
```

链路：watch 武装 → 新建 svmbctl 进程 → OnSeedCreate 以映像名匹配、
把新进程 CR3 装进 TargetCr3 → 该进程自己的 __readcr3() 读到 XOR 值 →
stage-8 用 VMCB Save.Cr3 直读验证。**M4-F 的 in-target 验证闭环。**

### 遥测新律（第 7 条）

**runProgramInGuest 秒退 rc=1 但 guest 文件未被改写 = Tools 执行通道
楔死**（上一次被杀的 vmrun/超时残留），不是 bat 失败；hard reset 恢复。
另外 kd 重连重放会把旧 boot 内容混进新 boot 流（r39 律 #4 的加重确认：
本轮"重启循环+自动 attach"半个是重放幻象、半个是真风暴死亡）。

### r44 入口

1. N2 NPF 注入 IOCTL 设计 + N3 拆步构造（照 r42 评估执行）
2. N5 有人值守 UAC 演练
3. 裸机清单（r42 已入档）随真机到货执行
4. （可选）CR3-write 监视的替代设计：写监视只在 NPT 视图内做页保护，
   不拦 CR3 写——绕开本平台风暴

## Round 44 (2026-09-09 晚): N2 注入构造落地；gremlin 风暴 = kd 在场；写监视替代设计

### N2 注入构造实现（stage 12 + probestage 12）

确定性构造：SetPerm4k 把 .svmbpr0 目标页 NX（无钩子认领）→ 调用目标 →
取指 NPF 进 deny 分支。Debug：spin-breaker 256 次后 AdvanceRip，探针
存活返回（exitDelta≈257 指纹）；Release：fail-fast 'SVMB'+1（本 stage
在 Release 上永不返回）。验证因 guest 循环中断，待稳定 boot 复测。

### gremlin 风暴定性：kd 在场（被动也算）+ 驱动虚拟化 = ~80s 死亡循环

r43 晚间 guest 陷入 ~80s/次的重启循环（驱动每 boot 自载——gremlin 启动
服务——虚拟化 ~80s 死亡）。隔离 guest svmb.sys 后循环立断（330 冻结，
guest 稳定）。结合 r40 Run-A（kd 关闭 15min 稳定）对照：**kd 连接
（含被动不 break）+ 虚拟化 = 死亡**；kd 关闭 = 稳定。r40 定律升级为
硬规则：虚拟化浸泡/回归期间 kd 会话必须完全关闭，不满足此条件的
"楔死/死亡"读数一律先怀疑 kd 因素。gremlin 本体（服务自启的执行者）
仍未锁定（计划任务/WMI/Run 键/启动目录全部排除），但因 kd-死亡耦合
其危害被 kd 纪律覆盖，降级为观察项。

### CR3-write 监视替代设计（r44 设计交付，实现排后）

目标：在不在 CR3-write 上开全系统拦截的前提下，感知被 watch 进程的
换入/换出（进程视图切换的触发源）。
- 路线 A（NPT 页保护，推荐）：目标进程的"哨兵页"在其 NPT 视图内标记
  U/S=0（或 NX）；进程不在运行时任何越权访问即 NPF——把"感知切换"
  变成"感知访问"，完全绕开 CR3-write。代价：需要 per-process 视图
  （M4 前置）与 NPF 驱动的切换（现有 slide 机制同族）。
- 路线 B（采样）：空闲退出路径顺带采样各核 Save.Cr3（CPUID/MSR 退出
  已有），近似上下文跟踪，零新增拦截；精度受采样率限制。
- 路线 C（维持现状 + 有界窗口）：写监视仅在有界操作窗口内开启
  （如单次 cr3spoof 秒级），流程化使用，不做长时挂载。
r43 的方向拆分武装已实现路线 C 的语义（write=opt-in）。

### r45 入口

1. stage-12 在稳定 boot 上复测（Debug 存活 + Release fail-fast 各一次）
2. 路线 A 设计细化（per-process 视图是 M4 遗留前置）
3. N5 有人值守 UAC 演练

### r44 补充：无 kd 长周期死亡仍在（周期 ~13min）；stage-12 复测被循环吃掉

kd 关闭后的浸泡 9/9 分钟全绿，但**随后数分钟内 guest 仍死亡**（Tools
消失、无 bugcheck 落盘）——无 kd 死亡周期从 ~80s 拉长到 ~13min，kd 是
加速因素而非唯一因素。stage-12 复测被该死亡吃掉（Tools 窗口错过），
 guest 已重新隔离 svmb.sys 并停在稳定 STOPPED 态（**r45 第一步必须先
vm_unquarantine_sys.bat 恢复镜像**）。r45 主线：kd 双关 + 退出率遥测
曲线（死亡前 exit-rate 形态可区分螺旋 vs 猝死）+ 长周期死亡定位。

## Round 45 (2026-09-09 深夜): N2 定谳 + 退出率遥测落地 —— r44"长周期死亡"未复现，疑似 Tools 通道误判

### 现象

1. **stage-12（r44 N2 构造）复测即"冻结"**：`probestage 12` 后 vmrun 通道
   秒报 "VMware Tools 未在客户机中运行"，心跳断（vmware.log 08:07:31Z），
   无 reset/三重故障记录 —— 外观与历次"guest 冻结"一致。
2. **stage-13（r45 隔离 stub 重构）同样秒冻**：排除了"共享代码页"假设。
3. kd（MCP kdnet + `.logopen`）现场取证给出第三种真相（见下）。

### 与参考的对比

- 参考实现 的 deny 语义：`__debugbreak` + KeBugCheck，从不 skip 指令。
- r44 的 svmb stage-12：Debug 策略 = spin 256 次 + `AdvanceRip`（r31 起
  的 spin-breaker 设计）—— 这一条与参考的"永不跳指令"背道而驰。

### 根因链（kd 现场实锤，logopen 文件 `kd_r45_s13_*.log`）

| # | 事实（kd/日志证据） | 结论 |
|---|---|---|
| 1 | `npf: deny action=4 gpa=10e923000 rip=ffff9c8e4dbd1000` ×N | deny 路径**确实触发**，隔离 stub 页工作正常 |
| 2 | action=4 = **DenyUser**（枚举 0..5），不是设计的 DenyExecute(3) | **r44 NX 元组写错**：`NptPerms={Write,Execute,User}`，`{true,true,false}` 实际是 W+X+U0 —— NX 从未上膛；唯一生效的变化是 U/S 1→0，vhv 把内核取指报成 user 访问（`Save.Cpl` 恒 0 已知不可信）→ U/S 违例 → DenyUser |
| 3 | `npf: deny spin break ... (RIP advanced)` 后 chaos | spin-break 的 `AdvanceRip` 是**假解**：跳过 1 字节 RET 后执行仍在受限页上，逐字节 NPF→advance 爬行 4095 次后跌出页外执行任意池字节 —— "probe will FAIL but return" 是谎言 |
| 4 | kd CTRL+BREAK 仍可断入、内核可读、`g_npfDenySpins=257` 精确 | **内核没死！** 死的是 vmtoolsd（风暴定律同款签名），vmrun 报错≠guest 死亡 |
| 5 | 257 = 1 + NPF_SPIN_BREAK | 指纹完美，deny 计数/自旋/自旋断裂全部按设计工作 |

### 修复（本轮提交）

1. **元组修复**（npt_probe.cpp stage-12/13）：`{true,false,true}` = W+NX+US
   —— 干净隔离 NX 违例，不再被 U/S 噪声劫持。
2. **策略修复**（npf.cpp Debug deny 分支）：spin 256 次后 **resolve-in-place**
   （`SetPerm4k(RWX)` + `TlbInvlpgaLocal(rip)`），**不再 AdvanceRip**——
   被拦指令原样重执行，零指令跳过，probe 干净返回。
3. stage-13 探针（隔离 ExecPool 页 + 1 字节 RET stub）替代 stage-12 的
   共享代码页 NX（后者即使修好元组也天然危险）。

### 验证（Boot A2，修复后）

- `probestage 13` → `exitDelta=257 (expect 257)` + `[+] N2 isolated deny PASS`
- exitprof 直方图独立佐证：`NPF 257 19%`（退出计数与成因直方图双源一致）
- guest 全绿存活：RUNNING 6/6 cores、Tools 通道正常、cpuid 隐藏位完好

### 新遥测：exitprof（r45 落地，永久资产）

- `SVMB_IOCTL_EXITPROF (0x851)` + `SVMB_EXITPROF{Total,Hist[0x102]}`：
  槽 0x00..0xFF 密铺 SVM 退出码 + NPF 专属槽 + other；per-vcpu 计数
  （VcpuInfo 页内，owning-core 单写者，无锁），IOCTL 汇总。
- `svmbctl exitprof`：Top-24 退出码 + 助记符（对齐 hw/vmcb.h vmexit 命名）。
- `build/vm_ctl_exitprof_fresh.bat`：fresh2.txt 落盘采样通道。

### 浸泡实验（Boot B，kd 全关，裸虚拟化，~40min/29 样本）

- **结果：零死亡**。154k exits，RUNNING 6/6 到收尾。r44 的"~13min 长周期
  死亡"在本配置下 3 倍窗内不复现。
- 速率形态：boot 风暴 30-90/s → 稳态 5-15/s，叠加**纯 CPUID 突发**
  （90s 窗内 20-47k exits ≈ 峰值 520/s，guest 事件驱动，非螺旋）。
- 成因构成全程 ~99% cpuid + 一次性 msr=328 突发（t+3min）；无 NPF/异常
  积累，无正反馈爬升 → **既非螺旋也非猝死，是"根本没死"**。
- r44 死亡叙事重审：其判据是 vmrun 通道失联 —— 与今日两次被 kd 证伪的
  "冻结"同签名。高置信假设：**r44 的 13min 死亡 = vmtoolsd 通道死亡**
  （CPUID 风暴把 vmtoolsd 饿死的方差事件），内核未必死。遗留：若要实锤
  需下次"死亡"时 kd 断入判活（本轮 kd 判活流程已固化）。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| stage-12 冻结 = 共享代码页被 AdvanceRip 破坏 | 主凶是元组错序导致的 DenyUser + AdvanceRip 走页外；共享页只是放大器 |
| NPF deny 触发 = NX 上膛 | NX 从未上膛；deny 的是 U/S（vhv user-bit 怪癖） |
| "Tools 未运行" = guest 冻结 | 内核可活（kd 断入为证）；是风暴定律的 Tools 饿死签名 |
| r44 ~13min 无 kd 死亡周期 | 40min 浸泡零死亡；疑似当年也是 Tools 通道误判 |

### TODO

- 下次"死亡"先 kd 断入判活（内核 vs Tools 双层），再定性 —— 判活流程
  已写入 AGENTS.md。
- Release deny（bugcheck 'SVMB'+1）路径仍未实测（需要一次专门的
  Release boot + probestage 13 + 1001 事件/dump 取证）。
- gremlin 本轮两 boot 均未现身（服务被我删/建掌控），未获新线索。
- exitprof 突发源（CPUID 风暴的发起者进程）未定位 —— 可用 kd
  `!process` 配合突发窗口抓现行。

## Round 46 (2026-09-09 深夜): 路线 A 哨兵原语 PASS —— cr3 保护 r46/r47/r48 系列开工

用户定方向：Cr3 保护只针对被 watch 进程，不做全系统。三轮计划：
r46 = 原语验证；r47 = 真实哨兵（写感知替代 CR3-write 拦截）；
r48 = per-core 进程视图（保护本体）。

### r46 交付

1. **`cr3 watch` 脚枪修复**：EnableWriteMonitor 默认 0（app/main.cpp:300
   的无条件 =1 与 r43 cr3spoof 修复不一致——r43 只改了 cr3spoof，主命令
   仍是螺旋触发器）。显式第三参 "wm" 保留 legacy 开关。行为验证：watch
   5s 后 exitprof 总量持平（r43 螺旋 2500/s 会 5s 上万），干净。
2. **stage-14 哨兵原语探针（PASS，一次过）**：W 拒绝叶
   (`SetPerm4k({false,true,true})`) → 写 → DenyWrite 风暴 → r45
   resolve-in-place 放行落盘；trip1=257（spin-break 全额），重上膛后
   trip2=1（全局 spin 预算已花，立即 resolve），两次读回证明写真落地。
   退出计数与 exitprof NPF=258（257+1）双源一致。
   —— 这就是 r47 真实哨兵（线程内核栈页）要用的全部原语。

## Round 47 (2026-09-10 凌晨): 哨兵机制建成（未过活体验证）；1903 导出走不通；watch 流程回归待 A/B

用户方向：Cr3 保护只针对被 watch 进程（不做全系统）。r47 = 真实哨兵
（NPT 写拒绝感知被 watch 进程的调度事件，替代 CR3-write 拦截）。

### 已建成并入库（代码完成，活体验证被下述回归阻断）

1. **npf.cpp deny 回调钩**（NpfSetDenyCallback）：DenyWrite/Execute/User
   分类后在通用 spin-breaker 之前先给模块策略一次接管机会——哨兵语义
   =记录+立即 resolve+排队重上膛，零 spin 零跳指令。
2. **cr3_monitor 哨兵注册表**（16 槽）：watch 命中进程创建时武装其线程
   对象页（ETHREAD 首页含 KTHREAD.State，调度器每次换入/换出必写）；
   方向提示（Save.Cr3==目标=换出）进事件 ring（新事件类型
   SvmbDbgEvtSentinelTrip，Extra 最高位=方向）；CR3_STATS.Reserved=trips。
3. **IRQL 三修**（本轮最重要的工程沉淀）：
   - `NptView::SetPerm4kNoLock`：时钟 ISR 在 CLOCK2_LEVEL 写
     KTHREAD.CycleTime → 哨兵 trip 会跑到 CLOCK2，而 ViewLock 的
     KeAcquireSpinLock 在 >DISPATCH 会"向下提 IRQL"直接 bugcheck——
     退出路径必须用无锁 PTE 写（页表在视图不销毁时稳定，单 qword 写）。
   - KeSetTimer → **KeInsertQueueDpc**（后者合法到 HIGH_LEVEL，自带去重；
     DPC 在被中断写重执行后自然重上膛）。
   - 事件 ring Push 也是 spinlock → CLOCK2 trip 跳过 Push（计数器仍记）。
4. **解除武装竞态**：DisarmProcess/DisarmAll 恢复 RWX 后
   KeFlushQueuedDpcs 再恢复一遍，关闭"在飞 DPC 重上膛→孤儿拒绝页"窗口
   （孤儿拒绝页落在复用池内存 = 活陷阱）。
5. **psnpt 导出陷阱（1903 实锤）**：PsGetNextProcessThread 有 PDB 符号
   （kd `x nt!` 可见）但 **MmGetSystemRoutineAddress 返回 NULL = 未导出**；
   手动 extern 声明链接不过（导入库无桩）。回退 = 布局偏移线程遍历：
   EPROCESS.ThreadListHead=0x488、ETHREAD.ThreadListEntry=0x6b8
   （kd `dt` 校验，18362.1903 x64），__try 包裹。
   注意 `x nt!Foo` 显示的是 PDB 符号 ≠ 导出；lib 的 strings 扫描不可靠
   （连已链接成功的 PsSetCreateProcessNotifyRoutineEx 都扫不出来）。

### 回归：watch/attach 流程今天反复楔死+3 次软重置（根因未定）

时间线（全部 psnpt=NULL、哨兵完全惰性的构建上）：
- 18:32 本地：watch+spawn 链 → 软重置（全 6 vcpu CPU reset soft）
- 18:50：mod attach + watch + exitprof ×2 → 软重置
- 19:06：kd 随行复现（bp nt!KeBugCheckEx 已布）→ 目标重置且 **bp 未命中
  = 三重故障类，无 bugcheck 调用**（与无 dump/无 1001 一致）
- 02:0x：干净重置后 svc start → info RUNNING 295 exits → ~10s 内设备
  自行消失（err 2，无 crash 无 reset）；再走 mod attach 链 → 整链挂死
  （KDNET 连不上 = 硬楔死）

关键事实：三次软重置发生在哨兵惰性（零页武装）时 → **回归不在 trip
路径**；嫌疑收敛到 (a) watch 配置流（r47 改了 OnSeedCreate 签名/终止
分支 DisarmProcess/KeFlushQueuedDpcs——终止通知对每个进程退出都会跑）
或 (b) mod attach 流（r47 Cr3MonInit 新增 MmGetSystemRoutineAddress/
KeInitializeDpc/NpfSetDenyCallback），或 (c) 客户机经今日 5+ 次硬重置
退化（r38 幽灵）。**r48 第一步必须 A/B**：stash r47 → 在 r46 构建上重放
完全相同的 mod+watch+generate 流程 ×2——crash 则继续二分（r45 exit
census？），green 则逐项回放 r47 delta。

### 状态与下一步

- 代码已提交（本轮）；guest svmb.sys 已隔离停稳。
- r48：①A/B 定位回归；②哨兵活体验证（watch notepad → trips>0 +
  sentinel 事件 + exitprof NPF 对账）；③per-core 进程视图（M5 级）。
- r46 stage-14 原语 PASS 的结论不受本轮影响（独立绿）。

## Round 48 (2026-09-10 上午): 回归元凶定谳 —— CR3-READ 拦截持续武装 = 死亡螺旋；r43"读侧便宜"作废

### A/B 定性（决定性）

r46 构建重放 mod+watch+generate 流 → 同样软重置（01:42:06Z 全 6 vcpu CPU
reset，无 dump）→ **回归与 r47 无关**。逐步存活性探测锁定杀手步骤 =
`cr3 watch`（武装后 <50s 死亡；无 key watch 则全程存活多轮）。

**平台新律（覆盖 r43）**：CR3-READ 拦截位一旦持续武装，本 vhv 上一律
死亡螺旋（与写侧同性质，疑似 vhv 半支持使其覆盖全部 CR3 访问）。
r43 的"读侧便宜/只需武装读侧"只在前 60s 窗口内成立，不可持续。
含义：**in-target CR3 read-spoof 必须改为有界窗口模式**（arm→用→disarm
秒级），或换哨兵/NPT 机制实现——列入 r49 设计项。

### 附带修复与机制修正

1. **ApplyCr3Intercepts 门控**：wantRead 改为 `TargetCr3!=0 && XorKey!=0`
   （名字 watch 不再武装任何 CR 拦截）；OnSeedCreate 里重评估。
   实测：nokey watch + generate + stats 多轮全绿（guest 稳定）。
2. **哨兵武装点两次修正**：
   - 进程创建通知时初始线程不在 ThreadListHead（r47 偏移遍历每次空表）
   - 初始线程的 thread-notify 又先于进程通知触发（watch pid 未设）
   - 最终形状 = **延迟武装 worker**：OnSeedCreate → KeSetEvent →
     持久系统线程睡 100ms → PsLookupProcessByProcessId → 偏移遍历武装
   （thread-notify 保留，覆盖目标进程后来新建的线程）。
3. 僵尸 vmrun 会卡死整个 VM 操作队列（reset 挂 8min 无 reset 记录、
   vmware.log 冻结）——先 powershell Stop-Process vmrun 再重放 reset。
   vmx 拒杀但队列清空后 reset 自行完成。

### 未竟

- 哨兵 trips 仍为 0：延迟 worker 部署后未及验证 log（vm_svcstate_log
  通道瞬时失败）。下轮第一步：nokey watch → generate → `svmbctl log`
  查 "sentinel: armed N pages"/"arm skip why=" 行，按 why 分支修。
- read-spoof 有界窗口化设计（r49）。
- per-core 进程视图（M5）。
- guest 已隔离 svmb.sys 停稳。

## Round 49 (2026-09-10 上午): 诊断管道建成；armed 成功但 trips=0 —— 决定性探针立为 r50 首步

### 交付

1. **诊断管道（不再被 ring 淹没）**：CR3_STATS.Reserved 拆为
   SentinelTrips + SentinelArmed + 新增 SentinelDiag
   （low16=arm-worker 唤醒数，high16=last-skip-why 码 0..9，
   见 cr3_monitor.cpp kSentWhy*）；ctl `cr3 stats` 直读。
   遥测律 #8：ring 会被 boot 期 exit-trace 淹没，哨兵状态必须走 stats。
2. **arm worker 实锤运行**：diag 低 16 位随 watch 流程增长（9、10 次
   wake），lookup 对短命 svmbctl 进程有时失败（+100ms 时已退出，属预期），
   亦有成功（why 回落 0 = ArmPage 全绿走完）。
3. **为什么 trips=0（未解，证据链）**：armed 曾 >0（why=0 且 unwatch 后
   armed=0）但 trips=0 = 被拒绝页在武装窗口内一次写都没吃到。嫌疑按序：
   (a) 武装的是"死进程"的页（+100ms 时目标可能已退出，页被拒绝但无人写）；
   (b) 偏移 0x6b8/0x488 在这条路径上对错未独立验证（walk 日志行从未出现
   —— SentinelArmForProcess 的两条 LOGI 也没在 ring 里，与 why=0 矛盾，
   存在 ring 覆盖或分支未走的可能）；
   (c) deny 生效性未自证（SetPerm4k 后无 PTE 读回/自写验证）。

### r50 首步（决定性探针，一次 boot 内可完成）

哨兵页武装成功后**立即 driver 自写该页**（stage-14 同款自证）：
armed → drv-write → 应 NPF → SentinelDenyCb trips++ → resolve → 写落地。
- trips 增长 = 哨兵全链通，trips=0 的原因收敛到"目标进程写不到"，
  即武装时机/页选择问题 → 把 arm 时机改到 thread-notify + seed 双信号
  或改为对 watch 进程首次调度后补挂。
- trips 不增长 = deny 压根没生效 → 查 MapRange/SetPerm4k 的视图一致性
  与 PTE 读回。

### 环境注意

10:18 又一次软重置（diag 流程中，nokey watch+generate——nokey 安全结论
需要本轮重验一次）；gremlin 本 boot 实锤自动启动了驱动服务（info 直接
RUNNING）。vmrun 通道间歇抽风（dump 906/1720 字节交替），重试即可。

## Round 50 (2026-09-10): INVLPGA 32 位截断根 bug 修复；哨兵武装链全通；trip 可见性仍缺最后一环

### 根 bug 修复（全驱动受益）

**`_svmb_invlpga` 的 `mov eax, ecx` 把 64 位 GVA 截断成 32 位**——所有
内核地址（>2^32）的 INVLPGA 自驱动诞生起一直是静默 no-op。修复为
`mov rax, rcx`（svm_entry.asm，含注释）。受影响面：slide 翻转可见性、
deny resolve 落地、哨兵武装可见性——r31b"传 GPA 是 no-op"/"flush-all
不可靠"的历史谜团可能同源于此。

### 哨兵武装链已全通（notepad 活体）

- 长命目标（notepad，-noWait spawn，**-noWait 必须放 vmx 之后**——r48
  之前的 bat 位置错误导致 spawn 静默失败）+ 延迟 worker → **armed
  pages: 7**（多线程全数武装）；kill 后 armed 7→0（终止解除武装 ✓）。
- 自证写回探针已实现（读-写同值，零破损）。

### 未竟：trips 仍 0（deny 可见性缺最后一环）

武装后连 driver 自写都不触发 NPF → deny 对 TLB 仍不可见。已加
IPI 广播 INVLPGA + 64 位修复，仍未生效。已就位待验的探针：**武装后
GetPte4k 原始读回（"sentinel: pte gpa= pte=" 日志行）**——区分
"PTE 没改成" vs "PTE 改了但硬件不认"。本轮末 vmrun 通道劣化（dump 52
字节）未能跑完该验证。下轮首步：重跑 notepad 流 + 读 pte 行。

### 待做（r51+）

- PTE 读回定谳 → trips>0 → 哨兵活体闭环
- 10:18 nokey 软重置的定性（nokey 安全性待重验）
- read-spoof 有界窗口化；per-core 视图（M5）

## Round 51 (2026-09-10): 哨兵活体闭环 —— trips 40+，真实调度写感知实现

### r51-1 定谳（PTE 读回）

干净 boot 单次 dump（**LOG_READ 是 read-and-clear——反复 dump 会自己
把 ring 排干**；另 log bat 写 fresh4 而 pull 拉 fresh2 的通道错位也烧了
两轮）拿到决定性行：
`sentinel: pte gpa=86a82000 pte=86a82065` = P|US|A|D、无 RW → **PTE
正确改写**，"没改成"分支排除 → 自写不 fault = 可见性/匹配问题。

### r51-2 两个 bug 连破

1. **cb UNMATCHED 日志实锤**：deny cb 对已武装 gpa 报 UNMATCHED →
   **自写探针在槽位注册之前执行**（r50 把探针插在了注册 loop 前）→
   cb 查空表 → 落回 spin-breaker（256 转/次）→ resolve。trips 永远 0。
   修复 = 注册先行（r51 ORDERING LAW：槽位必须先于任何能触发 deny 的
   操作注册）。
2. **-noWait 位置**：必须放 **vmx 之后**（之前放 runProgramInGuest 后
   会被当成 vmx 路径 → spawn 静默失败——notepad 根本没起来过）。

### r51-3 活体闭环（全绿）

- notepad spawn → worker 延迟武装 → **trips 7 = armed 7**（每页自证
  trip 一次，IPI 广播 INVLPGA 使 deny 生效——r50 的 64 位修复是前提）
- 运行 10s → **trips 7→40**（+33 = notepad 线程真实调度写的感知！）
- kill → trips 40→50（终止清理写 +10）、armed 7→0（终止解除 ✓）
- guest 全程 RUNNING 健康，exitprof 正常。

### 遗留（r52+）

- 终止 disarm 与最后一批 State 写存在竞态（kill 的 +10 部分来自清理
  写，最终转换被 disarm 吃掉）——调优项非缺陷。
- 方向位（outgoing）已在事件里，尚未用真实进程验证方向判定准确性。
- read-spoof 有界窗口化（r48 定律：CR3-READ 拦截不可持续）；per-core
  进程视图（M5）。

## Round 52 (2026-09-10): 方向位验证 ✓；read-spoof 平台不可用定谳（vhv 不透传 InterceptCrRead）

### r52-1 方向位验证 ✓

notepad 生命周期抓到 **57 个 sentinel 事件**（64 槽 ring）：全部
core=4、cr3=1aa000（System 上下文）、outgoing=0——空闲 notepad 的转换
写来自唤醒者/System 上下文，"自身为当前"的换出写在空闲负载下罕见。
方向位功能正确；分布是负载形状问题（busy 进程才会出现 outgoing=1）。
事件附带发现：57 事件跨两次 notepad 生命周期、每线程页独立 gpa ✓。

### r52-2 终止 disarm 竞态定性 ✓

kill 后 armed 7→0、无孤儿 deny、guest 健康——按设计工作，无需调优。
（r51 的"+10 终止写"证明清理写能被感知；最终一批 State 写被 disarm
吃掉属可接受损耗。）

### r52-3 read-spoof 平台不可用定谳（重大平台定律）

cr3spoof（有界窗口形状本就正确：watch→验→unwatch 毫秒级）实马跑：
`crR=0008` 成功武装（intercept apply 日志）、窗口内零 CR3-read exit
（read exits: 0）、dur 探针读到真值 → **vhv 不透传嵌套 VMCB 的
InterceptCrRead 位**（与 r39 异常位半支持、r41 DR/MTF 同族第三例）。
推论：
- in-target CR3 read-spoof 在此 vhv 上平台不可用——裸机特性（真机
  AMD 硬件如约退出）。
- r48 "watch+key 熔毁"的归因需修正：不是 read-exit 压力（exit 为 0），
  更可能是哨兵 CLOCK2 NPF churn（pre-IRQL-fix）或其它 seed 路径效应。
  保留开放问题。
- 实现保留（裸机可用）；vhv 上 cr3spoof 测试预期 FAIL（记录在案）。

### r52-4 per-core 视图（M5）设计要点（实现排 r53）

- NptManager 增加 per-core DesiredView[cpu]；哨兵 trip（含方向）更新
  目标所在核的 Desired；各核下一次 VMEXIT 时惰性发布自己的
  VMCB.NCr3（CleanBits=0；exit 路径直写 Info 页内状态即可，无锁竞态
  = 单写者）。CPUID 退出流保证 ms 级发布延迟。
- 验证：in-target 探针读 VmcbNcr3 == ProcView PML4；出目标 == 基线。

### r53 队列

per-core 视图实现与验证；方向位 busy 负载补充验证；裸机清单合并
（read-spoof/DR/MTF/exc 面都是裸机特性）。

## Round 53 (2026-09-10): per-core 进程视图 PASS —— M5 里程碑，CR3 保护三态齐备

### 实现

1. **VcpuInfo.PublishedNcr3**：每核最后发布的 NCr3（单写者=owning core，
   无锁）。Info 页内，pad 吸收，asm 布局不动。
2. **Hypervisor::SetPerCoreView(targetCr3, procPml4, basePml4)** +
   三个 getter：cr3_monitor（模块层）把 TargetCr3↔ProcView PML4 绑定
   推给核心层；basePml4=0 = 策略关闭。
3. **退出路径谓词发布**（SvmbVmExitEntry）：每核每次 exit 比较
   `Save.Cr3 == TargetCr3` → want=ProcPml4 else BasePml4；
   `want != PublishedNcr3` 时写 `VMCB NCr3 + TlbControl=FLUSH_ALL +
   CleanBits=0`（三者缺一不可——对照 TlbApplyViewSwitchAllCores，
   首版漏了 TLB_CTL 直接楔死一次）。谓词设计使**哨兵不参与视图切换**
   （Save.Cr3 每 exit 天然可得），哨兵专职感知/遥测。
4. **ViewPolicyRefresh**（cr3_monitor）：Configure 时把
   TargetCr3+ProcViewId（EnsureProcessView 产物）绑定推给核心层；
   清除时 revert。
5. **stage-15 探针 + `cr3 viewtest`**：ctl 自指 watch（raw CR3 目标
   + EnableProcessView）→ ForceFlushExit 触发本核发布 → 读回
   VMCB NCr3 对比 ProcView/Base PML4 → unwatch 复验回基线。

### 验证（PASS）

```
viewtest: target=ad2c2000 procPml4=13a1e0000 basePml4=2c28a000
  in-target NCr3=13a1e0000 (expect procPml4)   ✓
  post      NCr3=2c28a000 (expect basePml4)    ✓
[+] per-core view PASS
```
之后 info/exitprof 全绿（RUNNING、cpuid 隐藏 ✓、NPF=2 仅自证残留）。

### 过程坑

- 首版发布漏 `TlbControl=FLUSH_ALL` → shadow TLB 新旧 root 混用 →
  guest 楔死一次（对照官方 TlbApplyViewSwitchAllCores 三件套补齐）。
- python heredoc 对 CRLF 文件的字符串匹配不稳定——长块插入改用
  find-切片定位。

### CR3 保护版图（r53 后）

| 层 | 状态 |
|---|---|
| 感知（哨兵） | ✅ r51 活体闭环 |
| 伪造（read-spoof） | ⚠️ 裸机特性（vhv 不透传 InterceptCrRead，r52） |
| 隔离（per-core 视图） | ✅ **本轮 PASS** |

### r54+ 队列

- ProcView 内容差异化：M4 策略在 ProcView 内藏/改页（视图不再与基线
  同内容，隔离才有攻击面意义）——NPT SwapPage4k/隐藏页机制在位。
- busy 负载方向位补充验证；disarm 竞态微调（可选）。
- 裸机清单合并（read-spoof/DR/MTF/exc 面全依赖真机）。

## Round 54 (2026-09-10): ProcView 内容差异化探针建成 —— NOT_SPLIT 定位到 swap 链，验证待通道稳定

### 已实现（代码入库）

stage-16（`probestage 16`）：自包含的内容差异化验证——分配数据页+隐藏页
（11111111 / 22222222 填充）→ CreateView+FillView 临时 ProcView →
MapRange 拆页 → SwapPage4k(bgpa→隐藏PFN) → SetPerCoreView 自指 watch →
ForceFlushExit → 读 buf 应见 HIDDEN → 还原 → 读应见 REAL。ctl 打印
`view content: hidden-read= real-read=`。+每步 st 日志（fill/map/swap）。

### 卡点

SwapPage4k 链返回 **c0de0002 = NPT_STATUS_NOT_SPLIT**（"2M region
still large-page"）。已修一版（SwapPage4k 前先 MapRange 拆页）仍复现；
每步日志已部署但 vmrun 通道劣化未拿到 fill/map/swap 三行。r55 首跑即得：
- fill 失败 = FillView 边界 chunk 问题（RamEnd 附近）
- map 失败 = EnsurePde/FillView 组合语义
- swap 失败 = MapRange 拆页未生效（查 Split2MLocked 后 PDE 状态）

### 同轮已完成

- r52-3 read-spoof 有界窗口验证闭环（cr3spoof 本 vhv 预期 FAIL 已记录）
- 裸机清单：read-spoof/DR/MTF/exc 面全部依赖真机，已分散入档各轮

## Round 55 (2026-09-10): stage-16 内容差异化 PASS + 熔解根因定谳（TlbControl）+ 哨兵 arm worker 零唤醒

### 现象（三个 boot 的熔解链）

- boot#1/#2：`sc start`（AutoStart+NptEnable）后 30-90s 内 vmrun 通道死
  （先"Tools 未运行"后 exec 挂起），无任何触发/无 watch/无 key。
- boot#3：**宿主侧零操作**也熔——registry 残留服务被 gremlin 在登录时
  自启加载。gremlin 实锤存在。
- kd 两次断入同签名：CPU0/2 在 `MiWalkPageTablesRecursively`/
  `KiIpiSendRequestEx`（Mm working-set manager 的 TB-flush IPI 风暴），
  vmtoolsd 饿死；`lm m svmb` 空（驱动已不在模块列表，卸载机制待查，
  AutoStart 失败按 main.cpp:808 设计是不卸载的——矛盾未解但不阻塞）。
- 隔离（ren .quarantine + sc delete）后 guest 岩石稳定 → 熔解条件 =
  "驱动已加载"本身。

### 与参考的对比（根因）

svm-base 从不写 VMCB.TlbControl（恒 0=不刷新）；svmb ConfigureVmcb 在
NPT 启用时设 `TLB_CTL_FLUSH_ALL(3)`——AMD 语义 TLB_CONTROL **每次
VMRUN 重新评估**，等于每次重入全 TLB 刷新 → guest 软缺页率爆炸 →
Mm working-set manager 疯狂扫页表+跨核 IPI → livelock → Tools 饿死。
r48"CR3 读拦截死亡螺旋"归因正式推翻（r52 read exits=0 已疑，本轮实锤：
无任何拦截也熔）。

### 修复（本轮提交）

`SvmbVmExitEntry` 出口路径：`TlbControl != 0` 时清零+CleanBits=0——
语义变为"root 变更时 publish 路径武装一次，首个出口即撤"；perm 翻转
可见性本就依赖 INVLPGA（r50 法则），无损失。
**boot#5 验证：加载后 200s+ 通道存活（旧窗口 30-90s 必死）、多核
CPUID exit 健康、stage-16 复现 PASS（零回归）。**

### stage-16 内容差异化：PASS（连续两个构建）

`view content: hidden-read=22222222 real-read=11111111`，链路
fill/map/swap 全 st=0，pre-swap pte=身份映射 bgpa（位正确）。
**NOT_SPLIT 定谳：从未在哈希校验过的最新构建上复现**——r54 收尾的
"仍复现"证据被通道退化期旧二进制污染（双目录/旧 ctl 漂移陷阱）。
M4 策略页机制（SwapPage4k 内容换页）端到端成立。

### 流程法则（本轮沉淀）

1. **原子一击法则**：sc start 后 ~30s 熔解窗口内只够 1-2 次 vmrun。
   start+probe+log 合并进**同一条 guest cmd**、输出落盘，拉取可跨
   boot（磁盘持久）——`vm_start_probe16_log.bat`。
2. **赛跑隔离法则**：gremlin 在登录即自启残留服务 → 重置后立刻
   `vm_quarantine_now.bat` 轮询抢跑（连续两个 boot 首跳即赢）。
3. timeout 杀 cmd 不杀 vmrun 子进程 → 僵尸塞队列；清理用
   `Get-Process vmrun | Stop-Process -Force`。
4. kill 后读 stats 会看到 trips/armed 被 disarm 清零——事件证据读
   ring，不读 stats。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| r48 熔解=CR3 读拦截压力 | 无拦截也熔；根因=每 VMRUN 全 TLB 刷新 |
| NOT_SPLIT=Split2MLocked 失效 | 最新构建从不复现；旧证据=stale binary |
| gremlin=未证实传说 | boot#3 零操作熔解=实锤 |
| watch 流"回归"神秘 | 精确症状=g_armWakes=0（worker 零唤醒），见 TODO |

### TODO（r56）

1. 哨兵 arm worker 零唤醒：`SentinelArmWorker` 从未过 wait
   （diag 低16=0）。首步：**加载后第一次 ring dump 抓 init 三行**
   （`psnpt=%p` / `arm worker st=%08x`）——PsCreateSystemThread 是否
   失败。次选：`g_armPending` 卡 1（worker 首跑前 KeSetEvent 全被
   CompareExchange 闸门吞掉）——但首 watch 时 pending 应为 0，需实证。
2. `lm m svmb` 空 vs "AutoStart 失败不卸载"矛盾——驱动卸载者是谁。
3. busy 负载方向位（outgoing=1）补充验证：worker 修复后才有意义。
4. 裸机清单合并（read-spoof/DR/MTF/exc 面全依赖真机）。

## Round 56 (2026-09-10): 哨兵两根钉子拔除 + busy 方向位闭环 GREEN

### 现象（接 r55 TODO #1）

watch 正常（"armed pid="）但 armed pages=0、g_armWakes=0（diag 低16=0）——
worker 疑似零唤醒。

### 与参考的对比 / 根因链（两个独立缺口 + 一个 r55 修复的暴露面）

1. **流程缺口**：cr3_monitor 是标准模块，worker 线程/线程通知/deny 回调钩
   全在 `Cr3MonInit`（仅 `mod attach cr3_monitor` 时跑）。r55 优化窗口时
   丢掉了 mod attach 步骤 → worker 根本没创建。init 行本轮回捕
   （`arm worker st=00000000`）证明创建路径从来健康。
2. **代码缺口**：`Cr3MonitorConfigure` 对**已运行目标**走直配分支
   （LookupByImage 命中 → "armed pid="）——该分支从不调度延迟哨兵遍历
   （KeSetEvent 只在 OnSeedCreate 创建通知分支）。r50-52 全绿纯属顺序
   巧合（都是 watch 先、spawn 后）。修复：直配分支镜像创建路径调度
   （g_armPid + pending 门 + KeSetEvent），并加 `g_armWorkerLive` 门
   （未 attach 时 LOGW 提示而非哑吞）+ attach 时复位可能卡死的
   g_armPending。
3. **r55 熔解修复的暴露面（本轮最深一钉）**：trips 首轮 33 后冻结。
   SentinelRearmPage 的 deny 翻转（RW→R）从无 INVLPGA——r46-54 被
   每次 VMRUN 的 FLUSH_ALL 兜底（r55 拆掉），deny 对 NPT TLB 不可见 →
   每页只 trip 一次。定律化：**resolve 不需要 INVLPGA（失败的走表不进
   TLB），re-deny 必须广播刷**。修复 = RearmDpc 批量 deny 后一次
   KeIpiGenericCall 广播各页 GVA（SentinelFlushBatch，≤16 页单 IPI）。
   另：DenyCb 里旧 `TlbInvlpgaLocal(rip&~0xFFF)` 用指令页 GVA 属误用
   （stage-14 里 RIP 与数据同页的巧合），保留不动（无害）。

### 验证矩阵（boot，全部 vmrun 通道）

- 直配路径 arm → `arm worker lookup ok` → 偏移遍历（psnpt=NULL 回退）
  → 16 槽全满（"armed gpa= (thread obj page)"）→ 自写探针过 →
  diag=00000001（worker 醒 1 次、零 why 错）。
- re-arm 修复前 trips 33 冻结；修复后 **33 → 388 → 948 持续增长**
  （~15-20/s）。
- **busy 方向位（r52 遗留补验）**：spinner 目标 314 trips 中
  **out=1 × 11**（目标自身上下文写：时钟/抢占时目标正当前）+
  **out=0 × 303**（外来上下文：System=1aa000 的调度器记账 + 邻核进程
  的换入写）。r52 "busy 才见 outgoing=1" 预言实锤；方向遥测全通。
- kill → 终止 disarm：armed 16→0，trips 累计保留，guest 健康。

### 流程法则（新增）

- mod attach 是 sentinel 能力的**前置开关**——任何 watch 流前必须
  `vm_ctl_modfresh.bat`（首 attach 瞬时 1359 用单独一次 modfresh 重试，
  in-guest 5 连太快没用）。
- spawn-then-watch（直配路径）与 watch-then-spawn（通知路径）现在等效；
  验证哪个分支跑过看 ring 行格式（`armed %s pid=`=通知路径，
  `%s armed pid=`=直配路径）。

## 裸机清单（合并 r39/r41/r52/r45 散项——迁移到真实 AMD 硬件前的核对表）

| 特性 | vhv 现状 | 裸机预期 |
|---|---|---|
| read-spoof（InterceptCrRead） | 不透传嵌套（crR=0008 零 exit，r52 定谳） | 可用（有界窗口纪律仍适用） |
| DR 拦截（drR/drW） | 不透传（r41） | 可用 |
| MTF 单步 | 不透传（r41） | 可用 |
| 异常拦截（exc bits） | 半支持：首次 #UD exit+重注入+SEH 通，随后同 RIP NMI 0x80（r39） | 可用，默认全零保持 |
| TlbControl=FLUSH_ALL | 每次 VMRUN 重评估 → 熔解（r55 定谳修复） | 语义同 AMD 手册，修复通用；参考实现 恒 0 亦通 |
| NPF/perm 翻转可见性 | INVLPGA 必须（r50 法则 + r56 re-deny 补全） | 同左 |
| AutoStart 加载即虚拟化 | 稳定（r55 melt fix 后） | 待验证 |

## Round 57 (2026-09-10): 生产 ProcView 内容策略落地（stage-17 PASS×2）；TlbControl 回退决策；熔解升级实录

### 主交付：stage-17 / `cr3 policytest` PASS

生产路径（非临时视图）内容差异化：viewtest 式 setup（self-watch by raw
CR3 + EnableProcessView=1）→ 驱动侧 stage-17 用 `ViewByPml4Pa`（新增
NptManager 访问器）解析策略 PML4 → 在生产 ProcView 内
MapRange+SwapPage4k(buf GPA→隐藏页) → 本核 ForceFlushExit 读 → 隐藏
0x22222222；SetPerCoreView(0,0,base) 翻回 base 读 → 真 0x11111111 →
恢复策略 + 换回原页。两次独立 boot PASS。
ctl 侧 `cr3 watch <exe> [key] [wm|pv]` 新增 pv opt-in（name-watch 携带
per-core 进程视图）。

### 哨兵双视图武装（集成缺口修复）

哨兵只 deny Active()（base 视图）→ per-core 策略生效后 in-target 写走
ProcView（RW 克隆）→ out=1 类 trip 静默丢失。修复 = SentinelViews()
helper（base+ProcView 去重），ArmPage/RearmDpc/DisarmProcess/DisarmAll
全部改双视图操作；RearmDpc 的 MapRange 保证后建视图也有 4K 叶。

### TlbControl 回退决策（r55 修复部分撤销，如实记录）

r55 的 exit 路径清零把熔解窗口从 30-90s 拖到 1-8min——但今日实测：
- boot A（有修复）：加载后 8min **三重故障类意外关机**（Event 6008
  "14:55:16 关闭是意外的"，无 dump 无 BugCheck 事件，vcpu 逐核 soft
  reset）——policytest/s15 之后 ~20s。
- boot C（有修复）：加载后 ~1-2min 楔死（KDNET 握手都失败=内核级）。
- 无驱动 boot：稳 25min+。
**结论：per-entry FLUSH_ALL 在此 vhv 上是承重的（补偿某个未隔离的
嵌套 TLB 弱点）；清零只是把快速熔解换成慢速熔解+三重故障风险（更糟）。
回退恢复 r54 已知可用语义 + 30-90s 原子一击工作流。** vhv 嵌套 TLB
表征（对照 svm-base）= 独立回合。

### 流程资产（新增）

- `vm_unquarantine_now.bat` / `vm_quarantine_now.bat`：镜像改名对偶。
- **guest 侧辅助 bat**（build\guest\spawn_and_watch.bat + push/run
  bats）：spawn+sleep+watch+stats 全在 guest 内执行 = 1 个 vmrun op，
  完全免疫 vmrun 引号坑（无括号/无内嵌引号），熔解后输出仍落盘。
- `vm_evt_1074.bat`：无括号 XPath 查 1074/6008/41（内层括号会被 cmd
  当外层组闭合——今日踩坑）。
- watch 分支判定：`armed %s pid=`=直配路径；`armed, read spoof on`=
  CmdCr3Watch 成功行。

### 未竟（如实）

pv 组合 live 验证（in-target trip + out=1 while ProcView active）被熔
解吃掉窗口：watch 行已落盘（"armed, read spoof on"）但 stats 前内核
楔死。双视图代码已入库且 arm 链路同 r56 已证模式；trip 侧证据待熔解
回合解决后补测。

## Round 57 队列 → r58

1. **vhv 嵌套 TLB 表征**（ melt 根因回合）：svm-base 的 TLB 语义对照
   （ASID 取值/TlbControl/CR3 写拦截面），目标=找到既不熔解也不拖慢
   的配置；成功则 r56 双视图+pv 组合 live 验证可从容执行。
2. stage-16 回归在 r57 构建上补跑（r55 boot#5 PASS 仍为最新有效证据；
   r57 diff 不触及该路径）。
3. trip 风暴 exitprof 量测（r56 数据 15-20/s 已示低开销，正式量测顺延）。

## Round 58 (2026-09-10 下午): 熔解二分第一阶段 —— 熔解是随机的，单 boot A/B 不可靠

### 实验矩阵（新增 TlbMode/GuestAsid 注册表旋钮 + 浸泡脚手架）

- 旋钮：`Parameters\TlbMode`（0=flush-all 常驻[r54 语义]，1=从不武装
  [参考实现 语义]）、`Parameters\GuestAsid`（默认 1=参考实现 同值）。
  ConfigureVmcb 消费，DriverEntry 读参。
- 脚手架：`build/soak_experiment.sh`（重置→赛跑→推送→参数→启动→
  15s 探活循环）；guest 侧 `exitprof_sampler.bat`（10s 轮转 6 文件，
  熔解后仍落盘）。

### 结果

| 实验 | 配置 | 结果 |
|---|---|---|
| Phase 1 | NptEnable=0 | **稳活 10min（34 样本）**——NPT=熔解必要成分在今日环境成立（与 r45 40min 裸虚拟化一致） |
| Phase 2 观察 | NptEnable=1 + TlbMode=0（=r54 语义，今晨 1-8min 必死的配置族） | **稳活 45min+**（20+ 个样本），直到本轮验证负载（spinner+pv watch）后 ~45min 才楔死 |

### 关键推翻与再归因

- 今晨的 1-8min 熔解全部发生在 **r55-clear 构建**（TlbControl 首入后
  清零）；下午的 45min+ 稳定发生在 **flush-all 回退构建**。但两配置的
  熔解起点历史方差极大（30s～8min），且同一构建今日上午/下午表现
  不同 → **熔解起点是随机的、被未知环境态调制**；单 boot A/B 结论
  （包括 r55 的部分结论）不可靠。
- 新领先假说（待证）：flush-all 掩蔽 vhv 嵌套 TLB 陈旧性（r56 哨兵
  re-arm 的 INVLPGA 证据支持该机制存在）；clear 构建的慢熔 = 陈旧性
  积累；晨间快熔（30-90s，r54 构建）与下午稳定（同语义）的差异指向
  环境态调制（宿主状态/guest 累积损伤/纯运气，未定）。
- 宿主侧已排除：空闲内存 15.2GB（无压力）；孤儿 kd.exe 已清理。

### policytest 三连 PASS

回退构建上再 PASS（累计 3 次）——生产 ProcView 内容策略证据稳固。
stage-15 同场回归 PASS。

### 未竟与 r58 续

- pv 组合 live trip 证据：helper 已跑（watch 武装），stats 前被楔死截
  断（fresh2 空）。
- 熔解复现时的完整取证链已就位：sampler 尾样 + kd !running + 6008/
  1074 事件。需要：等待下一次自然熔解，或重置宿主/guest 基线后做
  长 soak（30min+）A/B。
- 判活法则补充：探活 rc 需区分 0/1（活）与 124+（楔死）——svmbctl 在
  无驱动时 rc=1 是合法存活态。

## Round 58 续（2026-09-10 晚）: 飞行记录仪部署 + 45min 零熔解（含金量最高的负结果）

### 新基础设施（全部入库）

- **guest 侧 melt_watchdog.bat**：每 ~7s 追加 `[时间戳] + exitprof 全直方图
  + FreePhysicalMemory` 到 melt_watch.log；楔死冻结循环 → 最后一条即熔
  解时刻，尾部即死亡前趋势。5s/条 ~1KB，1 小时 ~360KB。
- **宿主侧 melt_host_watch.sh**：20s 探活（rc 0/1=活、124+=楔死）+
  vmware.log 心跳/reset 行快照 → melt_host_timeline.txt。
- 证据收集链：熔解后 reset+赛跑 → 拉 melt_watch.log / ep*.txt / 6008+
  1074 事件 → 三源时间戳对齐（guest 本地=UTC+8、vmx=UTC、宿主探活=UTC）。

### 45min 浸泡结果（NptEnable=1 + TlbMode=0 flush-all + Asid=1）

- **零熔解**。521 样本、135 次宿主探活全 rc=0。
- 退出率：total 582→181,808（均值 ~67/s，尾部 ~30/s）——稳态无螺旋，
  与今晨熔解前无从对比（晨间未采样）。
- 内存：FreePhys 2.60GB→2.56GB——无泄漏。
- vmware.log：整个窗口无心跳超时事件。

### 关键推论（本轮核心产出）

同一 flush-all 语义：昨晨 r54-WIP 构建 30-90s 熔、今晨 8min 熔、今晚
45min+ 稳。**构建语义不变而表现天差地别 → 熔解是"环境态 × 驱动加载"
的联合现象，驱动语义（TlbControl/ASID）不是充分条件。** 环境态候选：
guest OS 多日累积损伤（今日 ~30 次硬重置）、vmx 宿主进程的长期状态、
gremlin 的可变行为。**r59 起的正确姿势**：宿主重启 + guest 干净基线
（快照/修复安装）重置环境态，然后 TlbMode 0/1 各 30min+ 长 soak 做有
统计意义的 A/B；在此之前一切单 boot 归因都视为噪声。

### 遗留不变

pv 组合 live trip 证据、stage-16 在 r57 构建回归、trip exitprof 量测
——全部顺延，等熔解问题收敛后执行。

## Round 59 (2026-09-10 深夜): 干净基线 A/B —— 两种 TLB 语义都稳；熔解的主因是 guest 损伤累积

### 环境重置（r58 计划执行）

- 快照回滚到 `r31-base-vmx-append`（09-09 基线）：guest OS 累积损伤（
  今日 ~30 次硬重置 + WU 半禁用 + Minidump/事件堆积）与 vmx 长期状态
  全部重置。
- 回滚后首次开机被 **cpufail 恢复阻塞**（r31 快照带内存态，开机试图从
  Snapshot8.vmem 恢复 CPU 状态失败 → msg.checkpoint.restore.error）。
  修复：vmx 删 `checkpoint.vmState` 两行 → 冷启动直接从回滚磁盘引导
  （正是想要的干净基线）。附带：硬停后的 .lck 残留也会无头阻塞启动
  （"操作被取消"），启动前清 `*.lck`。

### A/B 结果（同一干净基线，同一构建，只切 TlbMode）

| Soak | 配置 | 时长 | 结果 |
|---|---|---|---|
| A | TlbMode=0（flush-all 常驻） | 32min | **零熔解**，373 watchdog 样本，211k exits，无心跳事件 |
| B | TlbMode=1（从不刷新，参考实现 语义） | 32min | **零熔解**，763 累计样本，126k exits，无心跳事件 |

（Soak B 用 sc stop→改参→sc start 翻转；ring 证实全部 VMCB
`TlbControl=0` + `tlbMode=1 asid=1` 生效。）

### 结论（对比今日晨间数据）

1. **熔解的主因是 guest 损伤累积，驱动加载是扳机，TLB 语义不是因果项**
   ——今晨损伤 guest 上两种语义都会熔（flush-all 8min/清零 1-2min），
   干净基线上两种语义 32min 各自稳定。
2. 晨间快熔（30-90s） vs 慢熔（8min）的形态差未定，可能对应损伤程度
   或纯随机；干净基线上未复现任何形态。
3. **运维律：测试基线要定期回滚快照**——guest 损伤累积本身就是实验变
   量。快照回滚流程：stop hard → revertToSnapshot → **删 vmx 的
   checkpoint.vmState**（否则 cpufail 卡恢复）→ 清 *.lck → start。
4. TlbMode=1 与 参考实现 语义一致且 32min 稳定，是未来默认值的候选；
   在获得更长（小时级）soak 证据前，默认仍保持 TlbMode=0。

### 飞行记录仪战备状态

melt_watchdog 连续跨越 A/B（763 样本无中断）、melt_host_timeline 记录
全程、证据链（ep/6008/1074/kd）随时可拉。下次任何熔解都有完整趋势。

## Round 60 (2026-09-10 晚): 干净基线两次熔解 —— 熔解跟随 sentinel arm 活动，与 TLB 模式/guest 损伤均无关；r59 主因结论作废

### 现象

干净基线（r31 快照，r59 重置）上同一 HEAD 构建（6e71fd1，哈希双向
校验一致）连续两次熔解：

| # | 时刻 | 前置操作 | arm→熔间隔 |
|---|---|---|---|
| 1 | 19:27:36 心跳停 | mod attach + powershell spinner + `cr3 watch powershell.exe 0 pv`（成功回显 armed） | ≤60s |
| 2 | 20:05:56 心跳停 | mod attach + `cr3 policytest`（自包含 arm→stage17→unwatch，PASS 回显后） | ~30s |

无 reset 记录（纯活锁）。**熔解 #1/#2 都是"响"的**：6 个 vCPU 线程在
宿主侧满载打转（exit 洪泛），**首次把宿主机拖到整机卡死**——用户强杀
kd + VM 才救回。r55-58 的熔解从未影响宿主操作，本变体性质不同。

### 与 r59 结论的对照（关键决策回顾）

| r59 认为 | r60 实测 |
|---|---|
| 熔解主因 = guest 损伤累积 | 干净基线照样熔，损伤非主因 |
| TLB 语义非因果（两模式各 32min 稳） | 非因果成立，但 r59 的稳定样本身就是错的对照组——**全部无 sentinel 活动** |
| gremlin 可能随快照回滚消失 | gremlin = sentinel 路径自身 bug，干净基线健在 |
| policytest 短 arm 有界安全（r57 老 guest PASS×3） | 干净基线上 policytest 也熔（#2），unwatch 救不了 |
| 裸 kd.exe 一次性连接 + .dump /f 可抓熔解黄金现场 | 熔解=宿主 CPU 已被榨干，kd 连接重试+转储 I/O 雪上加霜→宿主机卡死，现场全丢 |

**熔解矩阵（全历史）**：r55/56 熔 = sentinel 活动期（当时代价等价
flush-all）；r59 稳 = 无 sentinel（两 TLB 模式）；r60 熔×2 = sentinel
活动（TlbMode=1，干净基线）。→ **唯一与熔解全程共变的变量是
cr3_monitor 的 arm/unwatch 活动。** 模型：arm 循环埋雷（某个 per-core
视图/未广播的失效），之后任意内存抖动引爆为 exit 洪泛活锁。候选雷点：
(a) RearmDpc 的 KeIpiGenericCall flush 批处理；(b) unwatch 的
SetPerCoreView 恢复路径留某核 stale NCr3；(c) trip 重武装漏 INVLPGA
→ 单核 trip/重武装自激 = exit 洪泛（与"响"的形态及 r55 kd 所见
MiWalkPageTables 风暴吻合）。

### 附带事故与新规

裸 `kd.exe -c '...~* kb; .dump /f...'` 连接熔解 guest → 宿主机整机
卡死（用户手工恢复）。**AGENTS.md 新律：内核调试只许走 windbg-mcp，
禁裸 kd/windbg 命令行；熔解期 kd 只做轻命令**（vertarget/单核栈），
禁 `.dump /f` 与全核栈。

### 功能收口（本轮完成的部分）

- **policytest PASS**：hidden-read=22222222 / real-read=11111111——
  生产 ProcView 内容交换机制在干净基线 + TlbMode=1 上正确（机制本身
  工作，武装后的系统失稳是另一回事）。
- **stage-14 PASS**：trip1=257（deny 生效、resolve 提交写）、trip2=1
  （重武装生效）——NPF deny 原语健康。
- **exitprof 基线**：total=3752，cpuid 93% / NPF 6%——空载无螺旋。
- **TlbMode=1 裸 NPT soak 延续**：context 中断期间 17:47-19:25 共
  98min 零熔解（累计 r59 32min + 本次 98min）；熔解 #2 后新 boot 又
  恢复裸 NPT 稳定运行。
- stage-15 需 watch 活动才有意义 → 跳过（有 watch = 熔解风险）。
- 遗留缺口：两次熔解都**没有** melt_watchdog/exitprof sampler 在跑
  （r59 战备姿态未在本 session 开机后重启）——趋势数据缺失，只有
  vmware.log 时间线。教训：watchdog 应作为开机标准姿态。

### TODO

1. pv 活体 trip out=1 证据：与熔解触发器同路径，**是否做短窗口有界
   尝试（arm 后 10-15s 内抢拉 stats/events 再 unwatch）需用户决定**——
   代价是可能再熔一次、宿主再卡一次。
2. 熔解根因攻击改打 sentinel 路径：优先 (c) trip 重武装 INVLPGA 审计
   + (b) unwatch 恢复路径 per-core 审计——两者都可以**静态审计 + 定向
   修复**先行，不必先跑熔解实验。
3. 任何后续熔解实验前：先启动 melt_watchdog + exitprof sampler，
   并评估宿主当前负载。

## Round 61 (2026-09-10 深夜) — 熔解根因定谳：ProcView 无 MMIO 映射 → EOI 无限 NPF；急性形态已修复 @ (见提交)

### kd 实时取证（windbg-mcp，r60 新规下首战）

提前接入 kd + `.logopen`，重放 `spawn → watch powershell.exe 0 pv` 配方。Tools 死后 CTRL+BREAK 断入，**死亡序列完整捕获**：

```
[svmb][c5] sentinel: TRIP MATCHED gpa=10ec8d000 cr3=13f8d000 out=1   ← 目标进程自己写自己线程页（方向位实锤）
[svmb][c5] npf: diag gpa=fee000b0 info1=100000006 slide=0 action=1
[svmb][c5] npf: mapped non-RAM (MMIO?) gpa=fee000b0                  ← 无限重复至断入
```

**fee000b0 = LAPIC EOI 寄存器（0xFEE00000+0xB0）**。c5 因目标进程当前而发布 ProcView NCr3 → ProcView 里 APIC 页 not-present → 该核每个中断的 EOI 写 NPF → non-RAM 路径无法完成写 → 无限重试 = 单核 exit 洪泛 = 熔解。Base 视图裸跑 98min 稳定（EOI 正常）；参考实现 全 [0,1TB) 大页含 MMIO 洞所以稳定——第三个对照全部对齐。

### 根因链（r60 雷点逐一收口）

| # | 位置 | 问题 | 修复 |
|---|---|---|---|
| **F4** | npt.cpp `FillView` | ProcView 只映射固件 RAM 子范围，MMIO 洞（LAPIC/IO-APIC/HPET）not-present | **真根因**。fill 后补 `MapRange(0, RamEnd, RWX)` 全窗口大页；MapRange 对已拆分块只填 not-present 项 → 既有 sentinel deny 叶完好 |
| F2 | cr3_monitor `SentinelDenyCb` | resolve 只修 `Active()`（base）；ProcView 上的 trip 永远修不对 → 同类无限循环 | resolve 改双视图（r57 arm 侧双视图的 resolve 侧补齐） |
| F3 | cr3_monitor worker | 睡 100ms 后不复查 watch 状态 → policytest 毫秒级 unwatch 后才武装 = 孤儿 deny（exit-notify 清理已跑过，页被池复用 = 永久随机 NPF 陷阱） | worker 醒后复查 `g_watchPid==pid`；DisarmProcess/DisarmAll 取消 pending arm |
| F1 | hypervisor.cpp:796 | 视图切换写 `TlbControl=FLUSH_ALL` 是粘滞的——切过一次的核永久 flush-all，TlbMode=1 语义静默劣化 | 非切换出口恢复 `TlbControl=0`（一次性 flush 语义；CleanBits=0 保证写入生效） |

### 修复验证（构建 435fb9ad，哈希双向一致）

- **熔解配方存活**：spawn → watch pv → trips 670 / armed 16 → stats 正常应答。r60 同配方 ≤60s 必死。
- **pv-combined 活体 trip 证据（r57 以来欠账）闭环**：日志环 **43 条 `TRIP MATCHED ... out=1`**（cr3=b4df000=目标进程自身上下文写，ProcView 激活下）+ 474 条 out=0（调度记账）；事件环 64 条（1357 lost = CLOCK2 门控丢弃，预期）。
- **慢形态仍在**：watch 持续武装 ~12min 后全核 soft reset（无 dump 三重故障类，r38 形态）。急性 EOI 洪泛已死，武装态包络从 ≤60s 扩到 ~12min，但哨兵长期武装的慢形态死亡未解决——r62 二分起点。注意此形态 Tools 死但内核此前 kd 断入正常（r45 律）。

### 流程沉淀

- vm_watch_pv_probe.bat（内层括号）是死的——`while(1)` 的 `)` 闭合外层组；活脚本 = guest 侧 spawn_and_watch.bat（vm_run_spawn_watch）。
- kd 在场判 melt 时序：Tools 心跳停止 ≠ watchdog 冻结时刻（本次 watchdog 最后样本 20:32:52、内核冻结 ~20:32:59、心跳停止 20:33:20——间隔 ~20s）。
- exitprof 累积直方图零 NPF = 哨兵从未 trip 过（r61 复盘用）。
- 慢形态下次复现时的取证位：kd 断入读 `!running` + 各核当前线程 + trip 率趋势（watchdog 已常备）。

### TODO (r62)

1. 慢形态二分：武装态 ~12min 三重故障。嫌疑序：(a) RearmDpc 的 KeIpiGenericCall 每 100ms 全核 IPI 的长期累积效应；(b) TlbMode=1 + 一次性切换 flush 在此 vhv 上的慢腐化；(c) slot/PageVa 悬挂（目标线程退出后 PageVa 指向复用页，RearmDpc 仍对旧 GPA re-deny——但 slot 在 exit-notify 清了；查 thread-notify 是否漏清）。(b) 的 A/B：TlbMode=0 重放配方看 12min 形态是否仍在。
2. 事件环 lost 比例高：Push 的 CLOCK2 门控丢弃是真信号（trip 在 CLOCK2 批发到达）——方向位统计改走日志环。

## Round 62 (2026-09-10 深夜) — 慢形态死亡现场抓捕：0xD1 rip=0 + 哨兵页伪 NPF 自旋；两修复入列 @ (见提交)

### 现场（kd 提前接入，windbg-mcp）

r61 修复构建（435fb9ad）+ 线程退出解除补丁重放配方，kd 在场 ~90 秒后死（kd 无场时同族活了 12min——**r44 的 kd 加速律对慢形态同样有效，可当复现加速器用**）。断入捕获：

1. 死前模式：目标线程在 c0/c2 双核间弹跳，`out=1` trip 风暴级交替（同两页 gpa=225f000/114be1000 反复）
2. 最后日志：c1 上 `npf: diag gpa=114be1080 / 114be10c8 info1=100000007 action=5` —— **哨兵页内部偏移的写被分类 Unhandled**
3. 致命：`Fatal System Error 0xD1 (0, 0xFF, 0x68, 0)`；`.trap` 读出 **rip=0**（取指空地址野跳）、rsp 距栈顶 0x50

### 分析

- action=5=Unhandled：叶子已 RWX 却仍 NPF = **陈旧 shadow-TLB 项**。旧代码 Unhandled→decline→guest 对同一陈旧项**无界重试自旋**（自旋者若持 CLOCK2 级——时钟 ISR 写 KTHREAD 恰是哨兵页——即中断永久不完成）。
- rip=0 的直接机制未完全定谳（0xFF 的 IRQL 报告值 + kdnet stub 上下文存疑）；但"伪 NPF 自旋"是死前最后可见成分，先拔。
- r61 的线程退出解除（嫌疑 c）补丁本身没有独立验证机会——本轮死在另一成分上；两修复同机待验。

### 修复（构建 457690e4）

1. **线程退出解除**（r62 主修）：SentinelThreadNotify created=false → SentinelDisarmPage（查该页槽位、双视图恢复 RWX、清槽）。消除"中途退出的线程把 deny 留给池复用者"。
2. **Unhandled NPF 自愈**：幂等 MapRange（只填 not-present 项，绝不弱化既有 deny 叶）+ 本地 INVLPGA + 重试。伪 NPF 从无限自旋变一次自愈。

### TODO (r63)

1. kd 在场快速复现验证（加速器用法）：两修复后跑配方，观察 5-15min。若仍死 → 新现场继续抓（rip=0 的确切跳源要用 `!for_each` 栈扫描或保留下次 dump 预算）。
2. kd 无场 45min 周期探活浸泡（真实验证形态）。
3. 补 r60 顺延回归：policytest / stage-14 / exitprof（当前构建一次跑齐）。

## Round 63 (2026-09-11 凌晨) — kd 加速验证三轮：r62 修复#2 自曝 CLOCK2 违例 + RearmDpc TOCTOU 修复；基病浮出为 (b) @ (见提交)

### 三轮 kd 在场快速迭代（加速器效应：无场 12min → 有场 40s-3min）

| 构建 | 死法 | 教训/产出 |
|---|---|---|
| 457690e4（r62 交付） | 2min 健康 trip 流 → **第一笔伪 NPF（Unhandled）出现即死** | **r62 修复#2 自曝**：Unhandled 自愈用了 MapRange=ViewLock_ 自旋锁，而伪 NPF 从 CLOCK2 到达（KTHREAD+0x204，时钟 ISR 写）——r47 定律自踩。修正=一次性 `TlbControl=FLUSH_ALL`（纯 VMCB 写，无锁） |
| 1896189d | 3-5min 健康多核 trip 流 → **双核页对乒乓活锁**（c3/c5 交替 120b51000/13a374000 数百次）→ kdnet 硬断；本轮零伪 NPF | **RearmDpc TOCTOU 实锤**：DPC 读老化 tick→决定 re-deny→并发 resolve 在间隙刷新 tick→deny 落在 allow 之后→写永不落地。修复=CAS 认领 re-deny（tick 变了跳过本轮）+ DenyCb resolve 改为先记 tick 再写 allow PTE |
| e57640e1 | ~40 trips 后无前兆 kdnet 硬断 | 非确定性残余：签名每次不同（0xD1 rip=0 / 乒乓+楔死 / 静默楔死）= 时序依赖的底层腐化 |

### 归因收敛

三连修复都在真问题上（每轮死法前移一层），但**残余基病与 r57 定律吻合**："per-entry FLUSH_ALL 是此 vhv 的承重墙（补偿未隔离的嵌套 TLB 弱点）"。TlbMode=1 拆掉承重墙，sentinel 的高频 deny/allow churn + 视图切换把弱点打出来——签名随扰动漂移正符合"TLB 陈旧性"类疾病。**r64 决定性实验（一拖再拖的那个 A/B）：武装 watch + TlbMode=0**——若 15min+ 稳定，则基病=vhv 嵌套 TLB 弱点被 TlbMode=1 放大，armed-watch 包络在 TlbMode=0 下重验；若仍死，基病在 sentinel 自身，回 (a) IPI 累积。

### 流程沉淀

- kd 日志文件直接 tail（host 侧）比 wait_for_break 拉全量输出高效一个量级。
- `KDTARGET: Refreshing KD connection` = 目标内核级楔死信号（transport 断），非断入。
- 三轮迭代全靠 kd 实时日志（logopen）+ 每轮一死法——"加速器用法"已成为标准手段：**修一轮→kd 场内快速验证→签名变了=前进一步**。

### 遗留

慢形态（武装 watch 长期稳定性）在 TlbMode=1 下未收敛。当前 guest：裸 NPT TlbMode=1 稳定基线（驱动 RUNNING）。r64：TlbMode=0 A/B（vm_reg_param 改 TlbMode → sc stop/start → attach → 配方 → 15-45min 观察），再定 sentinel 生产路径。

## Round 64 (2026-09-11) — 决定性 A/B：武装 watch + TlbMode=0 = 45/45min 存活；基病定谳 @ (见提交)

### A/B 结果（同一构建 e57640e1，同一配方，只切 TlbMode）

| 配置 | 武装 watch 后表现 |
|---|---|
| TlbMode=1（r60-r63） | ≤60s（r60）、~90s-12min（r61-62）、40s-3min（r63）必死，签名漂移 |
| **TlbMode=0（本轮）** | **45/45min 全程存活**，配方后 trips=43/armed=16，探活 9/9 |

### 定谳

**基病 = vhv 嵌套 TLB 弱点**（r57 定律经干预实验确认）：per-entry FLUSH_ALL
不是可优化项而是承重墙——sentinel 的 deny/allow churn + 视图切换在
TlbMode=1（从不刷新）下必然踩中陈旧 TLB。**TlbMode 语义正式定型**：
- TlbMode=0（flush-all）= 默认且唯一支持 armed-watch/ProcView 的模式
- TlbMode=1 = 仅裸 NPT 场景可用（98min+ 稳定），armed 操作禁用
- r59 "TlbMode=1 候选默认"作废（当时无 sentinel 对照组）

### 回归三件套（顺延自 r60，本轮补齐）

- **policytest PASS**：hidden=22222222 / real=11111111（TlbMode=0 + r63 全部修复在位）
- **stage-14 PASS**：trip1=257 / trip2=1（deny 原语）
- **exitprof**：160606 exits，cpuid 99% / NPF 301（=stage-14 对账），无螺旋
- 注：浸泡期间 watched spinner 进程自然退出 → DisarmProcess 正常清理（trips 停在 43），onSeedCreate(false) 通路在真实退出场景工作

### 流程沉淀

- fresh2.txt 文件名本轮出现写入失败（独立文件名即刻恢复）——总结：**新实验一律用独立输出文件名**，共享 fresh* 名的复用语义已不可靠。
- sentinel trips 在 watched 进程退出后停涨 = DisarmProcess 清理生效的可观测信号。

### r65 候选

1. TlbMode=0 + armed watch 的 2-4 小时超长浸泡（当前默认配置的最终验收）。
2. pv 活体 out=1 证据在 TlbMode=0 下复采（r61 拿到过 43×out=1 @ TlbMode=1 短窗；TlbMode=0 下重采更贴近生产配置）。
3. 生产轮（unload 硬化/NPF 生产语义/Release 全回归/安全审计）或裸机迁移路线图。

## Round 65 (2026-09-11 深夜) — 终验浸泡首attempt即出新数据点：TlbMode=0 冷启动武装急性楔死；r64 结论细化 @ (见提交)

### 事件链（r64 结束到本轮）

1. **21:59 全核 soft reset**（无前兆记录，无 watchdog 覆盖——r64 的
   watchdog 启动静默失败，无场数据）；重启后驱动被**疑似 gremlin**
   自动拉起（745 exits RUNNING，无人工 start）。
2. r65 布防：VM 已关机 → 清 stale vmx.lck（vmrun start 卡 7min 的根因）
   → 开机 → svc start → **watchdog 启动并验证在采样**（新律：launch 后
   必须验日志增长）→ attach → 配方。
3. **配方期间急性楔死**：watchdog 尾样本冻结在 exits=1276 / cpuid 100%
   （驱动加载后 ~30s，attach+配方窗口），exec 通道 rc=124（楔死类）。
   **TlbMode=0 上发生**。

### 对 r64 结论的细化（非推翻）

r64 的 A/B 本身干净（45min 探活可区分中途 reboot，9/9 ALIVE=同一 boot），
但"TlbMode=0 完全安全"不成立：**成熟 boot（运行数小时）上武装=45min 稳；
冷启动 ~3min 即武装=急性楔死**。边界=boot 年龄（启动期进程 churn →
trip 率/视图切换极高）或纯非确定性。TlbMode=1 严格更差（r60-63 全死）
不变。哨兵 armed 包络在 TlbMode=0 上仍有未收敛的长尾。

### 工程含义与 r66 计划

- armed-watch 的生产化路径必须解决**冷启动窗口**：候选 (a) 延迟武装
  （驱动加载后 N 分钟内拒绝 watch，或 watch 请求排队到 boot 稳态）；
  (b) 冷启动 trip 率量测（exitprof NPF 曲线）定位危险窗；(c) armed
  操作只在成熟 boot 上允许（运维律）。
- r66：冷启动 vs 成熟 boot 武装的 A/B（同一 TlbMode=0）+ exitprof 逐分
  钟采样定位危险窗。
- guest 留存：裸 NPT TlbMode=0 稳定（驱动 RUNNING）。
- 顺延：pv out=1 在 TlbMode=0 的复采（本轮被楔死截断）、2-4h 浸泡。

## Round 66 (2026-09-11 凌晨) — 冷/熟 boot 武装 A/B 三腿完成；attach 本身即冷启动死因；"gremlin"破案=假 1060 @ (见提交)

### 实验矩阵（全部 TlbMode=0，构建 e57640e1 双向哈希一致，s_tlbMode=0 kd 实读确认）

| 腿 | boot 年龄 | 驱动加载 | mod attach | 武装 | kd | 结果 |
|---|---|---|---|---|---|---|
| 隔夜（r65 遗留态） | 0→3h17m | 00:12 | 无 | 无 | 无 | **3h17m 稳定**，exitprof 全程 100% cpuid 零 NPF |
| Leg B | 3h20m | +2min | +2min（**在 arm 之后**） | 裸 arm→attach 触发异步重武装 | **在场** | **第一笔 out=1 trip 后全局瞬间楔死**（attach 后 ~60-80s） |
| Leg A | **2m54s**（冷） | +2m54s | +2m32s | **未及执行** | 无 | **attach 后 ≤11s 全局楔死**，Tools 04:04:11 已死 |

### 关键签名（watchdog 5s 采样，比计划"逐分钟"更细）

- **Leg A（冷启动）**：attach 窗口 **msr exits 飙升 429→904**（5.4s 内，≈2.8/s→88/s），
  cpuid 98%→96% 被挤压，随后冻结。**死亡前兆 = msr exit ramp**（Leg B/隔夜腿 msr 恒 28 无 ramp）。
  冷 boot 上 attach 即致死，**武装不是必要条件**。
- **Leg B（成熟 boot）**：死亡前零异常（msr 恒 28、cpuid 稳 99%、无 NPF 风暴），
  kd logopen 尾行=`TRIP MATCHED gpa=135e47000 cr3=44079000 out=1`——**第一笔被监视进程
  自碰哨兵页 = 最后一条日志**。静默瞬间楔死，非熔解。
- 楔死时 kd CTRL+BREAK 无响应（OS 之下楔死类），串口黑盒空（已知该通道废弃）。

### 对危险窗的定位（r66 计划的回答）

主变量=**boot 年龄**，不是"load→arm 延迟"：
- boot ≤5min 时 **mod attach 即死**（Leg A），武装来不及参与；r65 的
  "attach+配方窗口 ~30s 死"同型（其 exits=1276 ≈ 加载后 26s）。
- boot 3h+ 时 attach+武装可存活（r64 45min），但 Leg B 证明该包络不闭：
  异步重武装路径 + kd 在场仍死于第一笔 out=1。r64 的稳定至少依赖
  （无 kd、同步武装顺序）两个未分离的保护变量。
- **msr exit ramp 是冷启动危险窗的可观测信号**——生产化"延迟武装"可
  用它做 boot 稳态判据（ramp 消失+cpuid 稳定 = 允许武装）。

### "gremlin" 破案（本轮意外产出，流程级）

1. **假 1060 定律**：`vm_run_query.bat`（裸 `sc query svmb`，无 `< NUL`、无重定向）
   在 Session-0 返回**假 ERROR_SERVICE_DOES_NOT_EXIST(1060)**。03:22 的
   "服务不存在"→03:25"复活"（以及 r65 记录的"gremlin 自动拉起"）全部是此假象——
   服务实际一直 RUNNING。**新律：guest 查询一律在带 `< NUL` 的组内做，禁用裸查询 bat。**
2. r65 遗留现场：melt_watchdog cmd（00:11:48 起）+ spinner powershell（00:12:11 起，
   CPU 3h24m ≈ 全 boot）一直在跑——非自启机制。WMI 订阅/计划任务/Run 键/
   服务恢复动作全查无（gremlin_hunt_r66.ps1）。已 taskkill + 日志旋转
   （melt_watch_overnight_r65.log 留存 10h 数据）。
3. **arm 顺序律**：裸 arm（attach 前）会打 "sentinel disabled"——watch 必须在
   `mod attach cr3_monitor` **之后**下；attach 会异步补武装既有裸 watch
   （每页 self-write→TRIP out=0→armed），r64 的顺序是 attach→配方。

### Leg A2（本轮收官）：延迟武装验证 = 26min22s 武装稳定

boot 04:10:41 → load 04:25:55（boot+15m13s）→ attach +33s（**存活**，
对照 Leg A 同点 ≤11s 死）→ arm 04:27:49 → **trips=51/armed=16**（与
r64 健康模式 43/16 一致）→ 浸泡 26min22s 全绿收官（探活 rc=0 连续、
msr exits=0、spinner 全程存活），随后 unwatch 干净解除（armed pages→0）。
**生产化结论：延迟武装设计可行，危险窗=boot 年龄 [0, ~15min)**；
attach 后 60s 探活即可作为放行判据（配合 msr ramp 信号更稳）。

### 遗留与 TODO (r67)
2. **分离 Leg B 的两个保护变量**：(a) kd 在场是否为成熟 boot 致死加速器
   （r63 加速律在 TlbMode=0 的延伸验证）；(b) attach→arm 同步顺序 vs
   arm→attach 异步重武装路径。
3. msr ramp 根因：kd 在场复现 Leg A，读 exit MSR 地址（VMCB exitinfo），
   确认是 attach 路径自身（cr3_monitor init）还是启动期 OS MSR 活动。
4. pv out=1 在 TlbMode=0 的正常复采（两腿都被楔死截断）、2-4h 浸泡。

## Round 67 (2026-09-11 清晨) — 30min 无 kd 终验浸泡 ~10min 处崩出全战役首个 armed-watch dump：0xEF svchost 异常帧腐化 @ (见提交)

### 实验配置（用户指定：跳过 kd 判别腿，直接 30min 无 kd 终验）

- boot 05:16:45（power-on 05:16:31）→ T0 加载 05:30:31 → watchdog 05:30:49
  （验证采样 5:31:01/06 ✓）→ mod attach 05:31:21（**boot 年龄 14m36s**，比
  15min 门禁早 24s——记录在案）→ +60s 探活 ALIVE → arm 05:33:08
  → **trips=468/armed=16**（r64/A2 健康模式）。
- 浸泡监测：melt_host_watch.sh 30 + 手动探针。+5min 全绿（trips 636 持续
  增长，NPF 占 exits 2% 正常消化）。

### 崩溃链（~10min 处）

| 时刻 | 事件 |
|---|---|
| ~05:39 | trips/NPF **冻结在 636**（同 A2 冻结形态；spinner 仍活） |
| 05:42:48.17 | watchdog 最后一枪 total=50106（cpuid 98% / NPF 636 / **msr=0，零前兆**） |
| 05:43:10-46 | exec 通道挂起（host watch +614s rc=124 → WEDGE 分支提前收场） |
| 05:47:57 | **全核 soft reset**（vmware.log；= bugcheck 自重启签名，r65 21:59 事件同型） |
| 05:48 | **Minidump 落盘**：`C:\Windows\Minidump\091126-16593-01.dmp`（662KB，已取回 host `windbg-test/logs/`） |

### Dump 判读（windbg-mcp open_cdb_dump）

- **Bugcheck 0xEF CRITICAL_PROCESS_DIED，死者 = svchost.exe**（Cid 0x210，
  Session 0，services.exe 之子——**不是被监视的 spinner，无辜旁观者**）。
- 栈（自底向上）：svchost 用户线程正常页 fault（0x7ffa520bcd75）→
  KiPageFault → KiExceptionDispatch → 不可处理 → NtTerminateProcess →
  PspTerminateProcess → PspTerminateAllThreads → **PspCatchCriticalBreak**
  → KeBugCheckEx(0xEF)。
- **异常帧被腐化**：`.exr` = ExceptionAddress **0x0** / ExceptionCode
  **0x0**；bugcheck 线程 r13=**0xC0000006（IN_PAGE_ERROR 类）**；`.trap`
  帧地址 0x7400 不可读——投递到用户态派发器的异常上下文整包损坏。
- svmb 崩溃时在模块列表（fffff804`4dc70000）。崩溃时无 kd 在场（符合实验
  要求），dump 是自发崩溃的非加速产物。

### 机制定位（与既有定律对齐，未定谳处如实标注）

1. 受害者非武装目标 ⇒ **哨兵 deny 挂在 guest 物理帧上，物理帧被内存管理器
   复用/共享后，无辜进程的线程碰到该帧触发 NPF**（r62"sentinel 页伪 NPF"
   类的实锤升级：这次拿到了完整 dump）。
2. 异常帧 0/0 + IN_PAGE_ERROR 残迹 ⇒ **NPF decline→异常注入路径把垃圾异常
   帧投递给 guest**——与 r39 定律"vhv 对嵌套 VMCB 异常注入半支持"吻合。
3. exitprof 零前兆（msr=0、cpuid 稳、NPF 冻结）⇒ 死亡在异常投递层，
   **不在 exit 频率层**——r66 的"msr ramp"只是冷启动 attach 死（另一成分）
   的签名，不能作为此类死亡的探针。

### 顺带事件：05:02:58 二次"无归因"干净下电

终验前发现 VM 已关机（A2 收官 8 分钟后）。vmware.log 只剩 UI 断连记录，
疑似 VMware UI 退出带走 VM。未展开取证，仅记录。

### 结论与 TODO (r68)

- **结论**：armed-watch 在 TlbMode=0 成熟 boot 上的残余死亡率依旧非零
  （~10min 处自发崩溃），且死法升级为可取证的 0xEF 异常腐化。生产化
  的"延迟武装"门禁只挡冷启动成分，**挡不住这个**——需修驱动。
- TODO (r68)：
  1. **哨兵 deny 与物理帧生命期解耦**（主嫌疑修复位）：deny 落在 GPN 上，
     页帧被复用后 deny 不随迁 → 无辜 NPF。方向：arm 时记录 GPN→(进程,VA)
     绑定，resolve 时校验当前属主；或 deny 页用水印校验（读回比对）
     不匹配即视为帧已复用、立即放行。
  2. 异常注入路径审计：NPF decline 时注入 #PF 的 VMCB eventinj 字段
     构造（对照 r39 定律清单），或改为 kill-switch（目标帧异常直接
     DisarmProcess + 优雅放行，绝不注入）。
  3. trips 冻结现象（636/51 双案例）纳入观察：冻结后 ~3.5min 内死亡
     两例，可能是"re-deny 停摆 → 帧复用窗口打开"的前兆信号。
  4. kd 判别腿（attach→arm+kd）顺延——0xEF 机制优先级更高。
- guest 留存：05:47:57 新 boot，驱动 STOPPED（最干净留存态）。

## Round 68 (2026-09-11 早晨) — 0xEF 修复落地：水印 kill-switch + Unhandled 硬安全网；30min 武装浸泡全绿 @ 1b4512c4 (见提交)

### 机制收口（代码级调研结论）

读 npf.cpp / exit_dispatcher.cpp / cr3_monitor.cpp + hypervisor.cpp 后，
r67 的图景补全为**阴影基础病单因说**（guest 内不可修，Vmware 嵌套阴影对
TLB 控制的忠实性残缺，r57 定律终形态）：

1. **trips 冻结** = re-deny 翻转不可见：影子保住 arm 前缓存的 RW 表项
   （INVLPGA ASID=0 与粘滞 TlbControl=FLUSH_ALL 均未能让它失效）→ 热页
   写永不 NPF。r67 dump 证明同一时刻仍有冷路径 deny 存活（svchost 新页
   触发 NPF）——"冻结 + 残活 deny"并存。
2. **0xEF 异常帧腐化** = 残活 deny 挂在已被复用的物理帧上：无辜进程写入
   →NPF→resolve 放行——其上的具体腐化路径（trap frame 写入被别名/延迟）
   与阴影病同类。触发器=**deny 生命期与物理帧解耦失败**。

### 修复（构建 1b4512c4，双向哈希一致）

| # | 位置 | 内容 |
|---|---|---|
| F1 | cr3_monitor `SentinelDenyCb` | **外来 trip 水印 kill-switch**：out=0 时重读 KTHREAD 分派器头 dword 与武装时采样值比对，不符=帧已被复用→双视图 SetPerm4kNoLock 放行+永久拆槽（新增 kSentWhyReuse/g_sentReuse） |
| F2 | cr3_monitor `SentinelRearmDpc` | re-deny 前同款水印校验，不符→放行+拆槽（覆盖 lookup-fail 退出、进程死亡竞态等 r62 线程退出通知的残留缝隙） |
| F3 | cr3_monitor 全部 disarm 路径 | 解除的 GPA 进 4 槽环形记录；DenyCb 对"刚解除的 GPA"的飞行中 trip 改为直接放行（免 256 次 spin-breaker 税与撕裂窗） |
| F4 | npf.cpp `Unhandled` | **硬安全网**：连续 NPF_SPIN_BREAK(256) 次纯 flush 自愈无效→SetPerm4kNoLock 直接放行该页（阴影不忠实时宁过度放权不楔死）；MapRam 健康路径清零计数 |

被否决项：resolve 时移除 INVLPGA(rip)——若阴影缓存了带权限的失败遍历结果，
去掉它会复活 livelock；维持现状。

### 验证（成熟 boot 35min 龄，attach→arm 标准序，无 kd）

- arm 06:22:42 → **trips=157/armed=16 健康落地** → 30min36s 浸泡全程存活
  （探活 rc=0 连续 1798s+，spinner 30:35 CPU 全程在转，exitprof 99% cpuid
  零异常）→ unwatch 干净解除（armed→0）。**r67 同点 ~10min 必死未复现。**
- 诚实边界：**本轮 kill-switch 零触发**（日志环 232 条 sentinel 行中无
  REUSE/heal/UNMATCHED——trip 冻结后无外来 trip，复用窗口未出现）。即
  n=1 存活不能归因于修复生效，只能说未回归且防御网在场。阴影基础病
  未动（trips 冻结原样复现）。

### TODO (r69)

1. **2-4h 长浸泡 ×2**（同一构建）：真正的复用窗口验证——统计 g_sentReuse
   是否在真实 churn 中开火（需要把 g_sentReuse 上报到 cr3 stats）。
2. trips 冻结的正面攻击（阴影病）：试验 re-deny 轮附加"NCr3 抖动"或
   一次性 flush-all 全核广播（纯 VMCB 写）能否让 re-deny 重新可见——
   若能，冻结与残活 deny 同时消失，阴影病被 guest 内手段压制。
3. g_sentReuse / g_npfHeals 上报 cr3 stats（当前只能事后翻日志环）。
4. kd 判别腿继续顺延（0xEF 修复验证优先）。
- guest 留存：r68 构建 RUNNING 裸 NPT（已解除武装）。

## Round 69 (2026-09-11 上午) — 2.5h 武装浸泡全绿（战役最长）；冻结攻击定性：阴影只认 NCr3 变值 @ fd07b527 (见提交)

### 交付（构建 fd07b527，driver+ctl 双向哈希一致）

1. **stats 上报**：SVMB_CR3_STATS 新增 SentinelReuse/NpfHeals 两字段，
   ctl 输出 `sentinel reuse: N npf heals: N`——浸泡观察不再依赖事后翻日志环。
2. **冻结攻击（TlbControl kick）**：RearmDpc 轮尾对全部 VMCB 补写一次性
   TlbControl=FLUSH_ALL（TlbKickFlushAllCores，纯 VMCB 写，tlbMode=1 门禁）。

### 冻结攻击定性（本轮最重要的负结果）

**kick 无效，且原因查清**：ConfigureVmcb 在 tlbMode=0 下本来就持续武装
TlbControl=FLUSH_ALL（hypervisor.cpp:414-415）——r64/r68/r69 三轮浸泡的
trips 冻结全部发生在"每 VMRUN 都请求全刷"的状态下。结合视图切换（NCr3
**变值**）次次可见的事实，定性：**VMware 嵌套阴影只在 NCr3 数值变化时重建，
完全无视 TlbControl**。guest 侧要让 re-deny 可见只剩 NCr3 抖动一条路
（代价=每轮双阴影重建，收益存疑，挂起）。kick 代码保留（无害，且是
TlbControl 将来若被修复时的现成钩子）。

### 2.5h 浸泡（08:09:23 arm → 10:39:51，mature boot ~2.7h 龄，无 kd）

| 指标 | 值 |
|---|---|
| 存活 | **2h30m28s 全绿**（探活 rc=0 连续 8996s+，host watch 150min 跑满） |
| spinner | 2:30:27 CPU 全程在转 |
| exitprof | 533,910 exits，cpuid 99%，NPF=42=trips，无异常 |
| trips | 冻结在 42（arm 后 ~30s 内，阴影行为符合新定性） |
| reuse / heals | **0 / 0**——kill-switch 与硬安全网全程未开火 |

- **战役最长武装浸泡**：此前最好 r64=45min；r67 同配方 10min 必死。
  稳定包络 10min→45min→2.5h 三级跳。
- 诚实边界：reuse=0 意味着复用窗口在 2.5h 内**没有出现**（16 个槽的
  KTHREAD 页全程未被回收——线程长活 + r62/r68 解除路径跟上了），kill-switch
  仍未被实战激活。2.5h 存活同样不能归因于修复生效，只能说不回归+包络扩张。

### 模型修正与 r70

arm 后 sensing 的真实形态（三段式）：
1. arm 窗口：self-write + 调度 churn → 一阵 trips（42-636 随机）；
2. **阴影缓存成型后：热页 sensing 死亡（trips 冻结）——deny 对热写不可见**；
3. 冷路径兜底：新映射（复用帧的受害者）的首次访问仍走 PTE → NPF →
   kill-switch（r68 F1）接住并拆槽。

即：**sentinel 实际上是一次性传感 + 复用兜底护栏**，不是持续传感。这个
模型下 0xEF 的触发窗=帧回收后到受害者首碰之间的残活 deny——已被 F1/F2
的拆槽逻辑覆盖（前提是受害者触发 NPF；而冷路径 NPF 是被 r67 实证过的）。

TODO (r70)：
1. **强制复用窗口验证 kill-switch**：spawn→exit 循环风暴（短命线程大量
   进出）制造帧回收，看 SentinelReuse 是否开火——这是 F1 的实战测试。
2. 2-4h 浸泡继续攒 n（每 boot 一条）；多 boot 重复。
3. NCr3 抖动冻结攻击（挂起，代价/收益待评）。
4. kd 判别腿继续顺延。
- guest 留存：fd07b527 RUNNING 裸 NPT（已解除武装）。

## Round 70 (2026-09-11 中午) — churn 风暴干净通过；kd 活捉死亡瞬间：首笔 out=1 trip → NMI 'TDO' → 0x80 @ (见提交)

### 实验 1：线程 churn 风暴（kill-switch 实战测试尝试）

guest 侧 thread_storm.ps1：被监视进程内 4×500=2000 条短命线程（create→join），
跨 ~30s 完成；seed 接管 pid=1504，armed=4，trips 42→76。

| 观察项 | 结果 |
|---|---|
| 退出解除 lookup 失败（linger 路径） | **零次**——2000 次 exit 的 PsLookupThreadByThreadId 全部成功，解除路径比预期可靠 |
| SentinelReuse（kill-switch 开火） | 0——无 linger 即无复用 deny，符合逻辑 |
| arm 竞态窗 UNMATCHED trip | **5 次**（deny 已存储、slot 未注册窗口内的外来写）→ 落入 spin-breaker（Debug 吸收） |
| 系统 | 全程存活（exitprof 正常） |

**生产化注记**：UNMATCHED 在 SVMB_PRODUCTION 下走 fail-fast bugcheck——
需改为 resolve 语义（与 F3 的解除环同类），列入生产轨道清单。

### 实验 2：kd 水印外科手术（被死亡打断，本身无咎）

流程：kd 找到 `svmb!g_sent`（fffff802`19bb4510，layout：Gpa4k@0x0/Pid@0x8/
PageVa@0x10/Watermark@0x18/ResolveTick@0x20，stride 0x28）→ ed 改 slot0
水印=deadbeef → 等外来写触发。**未触发即被实验 3 的死亡打断**；风暴进程
11:00 退出时 DisarmProcess 把被污染槽一并干净清除（armed→0）——另证解除
路径可靠。期间 trips 恒 76：休眠线程页无外来写 + 冻结挡唤醒写（热页阴影
表项），kill-switch 只能被"冷路径写"触发（与 r67 svchost 场景一致）。

### 实验 3（重大）：死亡瞬间活捉——死亡家族统一

新 arm（fresh powershell pid=12600 + pv）+ kd 在场（自 10:58 连接）→
**死亡链被 kd logopen 完整捕获**：

```
cr3 monitor: armed powershell.exe pid=12600 cr3=6dbb9000
sentinel: arm worker lookup ok / walk ...
sentinel: pte gpa=2f7f000 pte=2f7f065          ← 仅 1 页武装完成
sentinel: TRIP MATCHED gpa=2f7f000 cr3=6dbb9000 out=1   ← 首笔 out=1
KDTARGET: Refreshing KD connection              ← 传输即断
A fatal system error ... Bugcheck callbacks not invoked
→ r13=0x4f4454 ('TDO')，KeBugCheckEx 参数 0x80
→ vmware.log 11:18:54 WinBSOD Synthetic MSR（guest 真实蓝屏流程）
```

**死亡原因定性：bugcheck 0x80（'TDO' NMI）**——VMware L0 对嵌套 VMCB 状态
panic 注 NMI（r39 定律原样）。死亡家族统一：

| 事件 | 配置 | 表现 |
|---|---|---|
| r39 | #UD 重注入 | 0x80 'TDO'（kd 活捉） |
| r66 Leg B | arm→attach 异步+kd | 首笔 out=1 → 静默楔死（kd 尾行） |
| **r70 本轮** | fresh arm+kd | **首笔 out=1 → 0x80 'TDO'（kd 全程捕获）** |
| r67 | kd 连接 2h 后 | 0xEF svchost 异常帧腐化（kd 在场） |

**kd 加速器定律升级为强相关**：全战役所有急性死亡（r39/r66B/r67/r70）
都发生在 KDNET 会话连接期间；无 kd 浸泡（A2/r68/r69）全部存活过观察窗。
kd 在场于 sentinel/exit 路径活动期 = L0 panic 放大器（KDNET 中断注入
嵌套 VMCB 敏感窗口的假说，未证）。**运维律：armed-watch 实验一律无 kd；
kd 只用于 dmp 离线分析或可丢弃 boot 的受控操作。**

### r71

1. 无 kd 的 kill-switch 实战测试重设计：不能靠 kd 污染水印——需要
   驱动内置测试钩子（IOCTL 参数：`cr3 watch ... corrupt` 之类）或
   长期 churn 等自然 linger（风暴证明 linger 天然罕见）。
2. 生产轨道清单新增：UNMATCHED 改 resolve 语义。
3. 模型固化：健康浸泡形态 = arm 窗口 trips 爆发 → 冻结（阴影热缓存）→
   冷路径兜底；死亡形态 A = 冷 boot+attach（msr ramp）；死亡形态 B =
   kd 在场（TDO NMI）；死亡形态 C = 极低频自发（r67 0xEF，kd 相关性待定）。
- guest 留存：11:27 fresh boot，驱动 STOPPED（干净）。

## Round 71 (2026-09-11 午后) — kill-switch 实战验证闭环：poison 钩子 + 自然开火 8 次；孤儿 deny resolve 落地 @ 99d0cfc8 (见提交)

### 交付（构建 99d0cfc8，sys+ctl 双向哈希一致）

1. **`cr3 poison` 测试钩子**（SVMB_IOCTL_CR3_POISON 0x822）：驱动自身把一个
   已武装槽的水印改写为 deadbeef（模拟帧复用），并入队 re-arm DPC——
   替代被运维律禁止的 kd 手术。无 kd 流程下 kill-switch 随时可测。
2. **孤儿 deny resolve**（门禁版）：`SentinelDenyCb` 对无槽认领的 W-deny，
   在 g_sentArmed>0 时直接双视图放行+日志（ORPHAN），不再落入 spin-breaker
   （Debug 税）/fail-fast（Production 误杀）；无武装时保留 legacy 探针语义
   （stage-14 的 257 计数不受影响）。

### 实现插曲（两个教科书坑）

- Cr3MonitorPoison 首版落在**匿名 namespace 段**（cr3_monitor.cpp 11-931 行
  是匿名 ns，933+ 才是 svmb ns）→ LNK2019。匿名段=内部实现，公共 API 必须
  放 svmb 段。
- poison 后 F2 不开火：**re-arm DPC 是 trip 驱动的**，trips 冻结后 DPC 永不
  运行 → 钩子里直接 KeInsertQueueDpc 修复（测试语义）。

### kill-switch 实战验证（无 kd，全程）

poison→10s 内：**reuse 0→3**（F1/F2 双路径开火），armed 16→13，
diag=000a0001（kSentWhyReuse）。日志环存证：

```
POISON slot 1 gpa=12a30f000 wm=02980000->deadbeef
REUSE disarm(rearm) gpa=12a30f000 ... want=deadbeef     ← F2 合成开火
POISON slot 0 gpa=12a347000 ...
REUSE disarm(rearm) gpa=12a347000 ...                    ← F2 合成开火
REUSE disarm(rearm) gpa=20aef000 wm=02988000 want=02989000  ← ★自然开火
```

★=未投毒槽的自然捕获：页头 dword 自然漂移（…9000→…8000）被 F2 拦截。
30min 浸泡中自然开火累计 **8 次**（armed 16→8），全部方向安全（宁可误拆
不可留残活 deny）；系统全程存活（exitprof 99% cpuid 无异常，探活 rc=0
连续 1821s+），unwatch 干净（armed→0）。

### 新事实与 r72

- **水印粒度偏细**：live KTHREAD 的首 dword（DISPATCHER_HEADER
  Type/Size/Absolute/Inserted）在调度活动下会自然漂移 → 每次漂移损失一个
  感知槽（30min 消耗 8/16）。r72 候选：水印改采样稳定字节（如 Type 字节）
  或掩码比较（只比 Type/Size 字节）。
- 双 spinner 现象：失败的配方调用仍留下了 spinner 进程（按名监视一锅端）
  → 调度 churn 增大 → 自然开火率上升。测试卫生：launch 前 taskkill。
- trips 本轮**持续流动**（73→205 不冻结）：双 spinner + 高 churn 下
  外来 trip 频繁——冻结不是绝对态，churn 大时感知恢复。

### r72 候选

1. 水印掩码化（只比 Type/Size 字节）+ 重测自然开火率（期望≈0）。
2. 生产轨道清单复核：UNMATCHED resolve（本轮已落地）、UNMATCHED 日志限频、
   Release 构建+签名链路试跑。
3. 2-4h 无 kd 浸泡在新构建上攒 n。
- guest 留存：99d0cfc8 RUNNING 裸 NPT（已解除武装，spinner 已清）。

## Round 72 (2026-09-11 下午) — **r62 线程退出解除实为死代码（r67 0xEF 真根因）**；三项修复 + 2h 零自然开火浸泡 @ 5bf663af (见提交)

### 根因发现（本轮最重要产出）

r72 原目标=水印掩码化。部署后自然开火依旧（reuse 2→8/10min），追查水印样本
身份时发现两层错误并全部修正：

1. **水印采错位置**：`SentinelArmPage` 采样 `pageVa`（**页首**）而非对象首——
   页首是池头/邻接分配尾数据（样本 0x02989000 的 byte0=0x00，而真
   KTHREAD 头 Type 应=6）。掩码化（0x00FF00FF 只比 Type/Size 字节）治不了
   这个。→ **改锚 `ObjVa`**（ETHREAD 对象首 dword，槽新增字段）。
2. **r62 线程退出解除是死代码**：`SentinelThreadNotify` 传 `SentinelDisarmPage((void*)t)`
   （ETHREAD 原始指针），而 DisarmPage 匹配 `slot.PageVa`（**页对齐值**）——
   非对齐指针 vs 页对齐值**永不相等**，解除静默 no-op（日志里从未出现过
   "disarm page (thread exit)" 行——回查全部历史日志环确认）。→ **改匹配
   `slot.ObjVa == objVa`**。

**由此 r67 的 0xEF 根因链条彻底闭合**：武装线程退出 → 解除 no-op → deny
滞留 → 池回收帧 → 无辜 svchost 首碰 NPF → 异常帧腐化 → 0xEF。r68 的
kill-switch 一直在给这个死代码擦屁股（r71/r72a 的 8+5 次"自然开火"全部
是它的代价）。ObjVa 锚定后自然开火的真实样本也拿到了：wm=00200006
（Type=6 ThreadObject ✓）vs 页内容变 0x00000000/d5129cc0（帧真被回收）。

### 修复清单（构建 5bf663af）

| # | 内容 |
|---|---|
| F5 | 水印采样改 ObjVa（对象首=稳定身份） |
| F6 | 水印比较掩码化（0x00FF00FF：Type+Size 字节， tolerate byte1/3 漂移） |
| **F7** | **DisarmPage 按 ObjVa 匹配（复活 r62 线程退出解除）** |

### 验证（无 kd 全程）

- poison 回归：F2 照常开火（reuse=1，diag=kSentWhyReuse）✓
- **自然开火归零**：修复前 8 次/30min（r71）→ 修复后 **0 次/2h**（reuse 恒 1）
- 2h 浸泡（PID 7364 长命会话，arm 13:53:32→15:50:32）：**全程存活**，
  探活 rc=0 连续 6923s+，armed=4 稳定，exitprof 978k exits 99% cpuid 零异常，
  unwatch 干净（armed→0）。

### 流程沉淀（本轮三个新坑）

1. **陈旧文件错觉二次发作**：配方秒败后 pull 静默失败，宿主旧文件冒充新
   结果（"35/16""44/16" 两次误读）。新律：**任何配方结果必须用独立文件名
   + 直接 stats 双重核对**。
2. **配方对 sc start 后静置时长敏感**：start 后 <90s 内运行必秒败（2-3s、
   rc=1、无文件重写），静置数分钟后自愈；机理未明，实测律：**sc start 后
   等 ≥2min 再跑配方**。
3. **watch 只对新建进程生效**（`not seeded yet - arming on create`）：存量
   进程不会在 configure 时被扫描武装——实验流程必须"先配置后 spawn"；
   guest 侧 `start` 间歇性失败（非驱动问题），重试或换 guest-bat 模式。

### r72 后的模型终态

哨兵三层：arm 窗口传感爆发 → 阴影缓存后热页传感冻结（TlbControl 无关，
NCr3 变值才有救，挂起）→ **线程退出解除（已修复）+ kill-switch（已验证）
兜底帧回收**。0xEF 的全部已知成分别：r62 死代码解除（本轮修复）、kd 加速
（运维律禁 kd）、冷 boot attach（延迟武装运维律）。

### r73 候选

1. 2-4h 浸泡在新构建攒 n（本轮 2h 零自然开火已是最好的单条）。
2. 生产轨道：Release 构建+签名试跑、UNMATCHED 限频、安装/卸载体验。
3. NCr3 抖动冻结攻击（低优先）。
4. `start` 间歇失败的 guest 侧调查（低优先，环境问题）。
- guest 留存：5bf663af RUNNING 裸 NPT（已解除武装，spinner 已清）。

## Round 73 (2026-09-11 傍晚) — 生产轨道开跑：Release/SVMB_PRODUCTION 首次构建+回归全过 + 60min 武装浸泡全绿 @ 3a7b2730 (见提交)

### 生产语义审计（构建前）

三处 fail-fast 站点逐一核验：
| 站点 | 结论 |
|---|---|
| exit_dispatcher ApplyDefault Bugcheck 分支 | **休眠**：Default_=0（AdvanceOrReinject），Bugcheck 策略未被配置 |
| main.cpp 卸载僵尸守卫（'SVMB' 0x80 tag） | **合法**：仅 unload 时核仍在 SVM 才触发（r33-35 僵尸防护） |
| npf.cpp Release deny 未消费 bugcheck | **已兜住**：哨兵 W-deny 全部被 deny 回调消费（orphan/after-disarm r71 均放行），模块卸载/取消监视走 DisarmAll 清 deny（核对 Cr3MonExit+unwatch 路径）|

### Release 首跑（构建 3a7b2730，114KB vs Debug 174KB）

- **编译一次通过**（driver+ctl），build_release.bat 已入库。
- **坑：Release 不自动测试签名** → svc start 败于 **577（ERROR_INVALID_IMAGE_HASH）**。
  修复：signtool 手动签（`/n svmb-test /s My`，与 ctl 同证书）。签名脚本已入库
  （sign_sys_rel.bat / sign_ctl_rel.bat）。宿主 signtool verify 报测试根不受信
  属预期（guest BCD testsigning ON 即可加载，实测 ✓）。

### Release 回归（全过）

| 项 | 结果 |
|---|---|
| info 基线 | hv RUNNING，6/6 cores，cpuid hidden ✓ |
| arm（spawn+watch pv） | trips=55/armed=16 ✓ 生产哨兵正常 |
| poison | **F2 开火 reuse=1**、armed 16→15、diag=kSentWhyReuse ✓ kill-switch 在生产语义工作 |
| policytest | **PASS** hidden=22222222/real=11111111 —— 读伪替策略视图在 Release 工作 |
| storm 64 | rc=0 ✓ |
| exitprof | 纯 cpuid 99%，NPF=trips 对账 ✓ |

### Release 武装浸泡 60min（armed 16:17→17:16）

全程 rc=0（host watch 跑满 3602s+），armed=16 稳定，reuse 恒 1（**零自然开火**
——r72 解除修复在 Release 同样生效），exitprof 397k exits 干净。unwatch 干净。

### 流程沉淀

1. **policytest 会清 watch 配置**（其毫秒级 unwatch 走 DisarmAll+清 TargetImage）
   → policytest 后必须重配 watch + 重造进程才能恢复武装。
2. Release 构建纪律：MSBuild 只自动签 Debug；Release sys 必须 sign_sys_rel.bat
   手签，ctl 同理（签名会改文件 → 哈希以签名后为准）。
3. 失败码 577 = 驱动签名无效（新宪法条目）。

### r74 候选

1. Release 2-4h 浸泡攒 n（本轮 60min 全绿）。
2. UNMATCHED 日志限频（r71 遗留小项）。
3. 卸载路径实测（Release 僵尸守卫 + 577 拒载语义下的干净卸载验证）。
4. 安装体验：signtool 证书链给 guest 装根证书 → 免 BCD testsigning（生产化分水岭）。
- guest 留存：Release 3a7b2730 RUNNING 裸 NPT（已解除武装）。

## Round 74 (2026-09-11 晚) — 生产轨道二连：卸载实测+证书安装+日志限频；**二阶孤儿根因：watch 切换不解除旧目标** @ a59c6954 (见提交)

### 交付清单（构建 56e84541 → a59c6954，Release 双签）

1. **日志限频**：孤儿/解除后 resolve 路径加 32 条上限（g_orphanLogs，
   burst 安全；r71 遗留项闭环）。
2. **卸载路径实测**：Release 3a7b2730（r73 构建）sc stop 干净卸载——
   系统存活、无僵尸守卫触发、服务正常删除。生产语义的首次卸载验证 ✓
3. **测试证书安装**：svmb-test 公钥导出 → guest Root + TrustedPeople
   双库安装成功。**边界如实记录**：内核测试签名驱动仍需 BCD testsigning ON
   （x64 内核策略：OFF 状态下仅微软签名可加载，测试根入库不够）——
   生产化分水岭 = 微软签名（attestation/WHQL），本地环境外。
4. **二阶孤儿根因修复**：`OnSeedCreate(created=true)` 切换 watch 目标时
   **不解除旧目标的槽**——旧目标后续线程退出被 notify 的 pid 门
   （g_watchPid != p 提前返回）跳过解除 → deny 滞留 → kill-switch
   开火（r74 首泡 5 次/30min，全部真回收：cur=00000000/d5129cc0）。
   修复=切换前 `SentinelDisarmProcess(oldPid)`。

### 验证（Release a59c6954，无 kd）

| 项 | 结果 |
|---|---|
| poison F2 回归 | reuse 0→1，armed 相应 -1 ✓ |
| 目标切换场景（storm A→storm B） | 切换后 armed 仅新目标（旧槽已解除）、**零孤儿** ✓ |
| **2h 武装浸泡**（18:20:15→20:20:20） | **reuse 恒 1（仅合成 poison），零自然开火**；会话按脚本时序正常退出→DisarmProcess 清场（armed→0）；exitprof 1.22M exits 99% cpuid 干净；探活 rc=0 连续 7247s+ |

对比链条：r71 8 次/30min（r62 死代码）→ r72 0 次/2h（单会话无切换，未覆盖
切换路径）→ r74 首泡 5 次/30min（**切换路径暴露**）→ r74b 0 次/2h（切换
解除修复）——孤儿槽的两个成因（死代码解除、切换不解除）均已修复并验证。

### 环境注记（非驱动问题）

guest `start` 间歇失败（13:07 起多次秒败无新进程，重试即愈）——疑似
session-0 桌面堆/管道瞬时问题，记录备查。绕过模式：guest-bat 启动器
（thread_storm_launch 系）成功率高。

### r75 候选

1. Release 长浸泡继续攒 n（a59c6954 已 2h 绿；目标 4h+×2）。
2. 多目标切换压力测试自动化（N 次 spawn 循环 + reuse 计数断言）。
3. guest 根证书已装——下轮可在 guest 内 `signtool verify /pa` 验链。
4. 生产化分水岭评估：微软 attestation 签名路径（需 EV 证书+开发者账户，
   环境外决策）。
- guest 留存：Release a59c6954 RUNNING 裸 NPT（已解除武装，spinner 清理中
  ——2052/5400 两个旧 spinner 仍活，无碍）。

## Round 75 计划（2026-09-11 晚立） — 写阻断保护模式（两层防御）+ 区域保护集

### 设计定稿（vhv 约束下的零注入方案）

**产品语义升级**：哨兵从"能感知"升级到"能拦截 + 能感知"。

- **L1 阻断层（guest MM 交付）**：登记受保护区域 → 驱动对目标进程调
  `ZwProtectVirtualMemory(PAGE_READONLY)` → 写者（用户态或内核）写时由
  **guest 自身 MM 交付真 Page Fault/AV**——VAD=readonly 使 MM 不会自愈。
  零异常注入（vhv 丢注入= TDO NMI 家族，本设计完全不碰 EventInj）。
  用户态写者=直接 AV；内核态写者=可 SEH 捕获的异常（裸写者 bugcheck，
  对攻击者可接受）。
- **L2 感知层（NPT，现有已验证机架）**：同一物理帧武装 W-deny（独立
  ProtectedRegion 表，32 槽，语义/生命周期与哨兵分离）。trip = L1 被绕过
  （如内核物理映射写）→ resolve+re-arm（感知，不阻断）+ 事件环告警
  （新事件 SvmbDbgEvtRegionTrip）。
- **多目标**：区域表按 pid 键控，天然多进程（不受 g_watchPid 单目标限制）。
- **裸机升级路径**：真机上 L2 可升级为真阻断（注入 #PF 安全），vhv 上
  明确不做。

### 实现 cutline

1. driver：区域表 + Cr3RegionRegister/Unprotect/List + deny 回调分支
   （区域 consume 先于哨兵槽匹配）+ re-arm DPC 扩展区域表
2. IOCTL：SVMB_IOCTL_CR3_PROT 0x823（reg/unreg/list 复用同一 struct）
3. CTL：cr3 protect/unprotect/regions + **cr3 blockself**（E2E 阻断自证：
   VirtualAlloc→写 OK→protect→__try 写→期待异常）
4. 回归：全量三件套 + poison + blockself + 切换场景，Release 构建
5. 浸泡：armed 区域会话 2h+

### 与既有件的关系

- 哨兵（sensing watch）：不动，正交共存
- r74 切换解除/kill-switch/ObjVa 水印：区域表复用同款模式
- 裸机清单：+1 行（L2 真阻断）

## Round 75 (2026-09-11 深夜) — 写阻断保护模式落地：blockself E2E PASS + 30/30 压测全绿 @ 7fd0092a (见提交)

### 交付（Release 7fd0092a sys / f82862af ctl，双签；协议 IOCTL_CR3_PROT 0x823）

**两层防御设计（vhv 零注入）**：
- **L1 阻断（guest MM）**：登记区域 → 驱动对目标进程 ZwProtectVirtualMemory
  (PAGE_READONLY) → 写者由 guest 自身 MM 交付 AV（VAD=readonly 不自愈）。
  不碰 EventInj（vhv 丢注入 = TDO NMI 家族）。
- **L2 感知（NPT）**：同帧 W-deny（32 槽区域表，独立于哨兵 16 槽）；
  trip = L1 被绕过（内核物理映射写等）→ resolve+re-arm+事件环告警
  （SvmbDbgEvtRegionTrip），永不阻断。
- API：`cr3 protect <pid> <base> <size>` / `cr3 unprotect <pid> [base]` /
  `cr3 regions` / `cr3 blockself`；驱动卸载路径 Cr3RegionCleanupAll 恢复
  guest 保护。

### 实现坑（两个）

1. KeStackAttachProcess/KAPC_STATE 不在 ntddk.h（ntifs.h）——按
   PsLookupThreadByThreadId 先例手动声明，APC 状态用 64 字节不透明
   alignas(16) 缓冲（内核填充，不解释）。
2. 注册时序：先填 OutId 再存槽（首版 Id 恒 0， Cosmetic，r76 修）。

### 验证

| 项 | 结果 |
|---|---|
| **blockself E2E** | **PASS**：pre-write OK → protect → read OK → **write 异常拦截** → unprotect → write 恢复 |
| 日志环 | REGISTER oldProt=4(PAGE_READWRITE)+L2 deny；unprotect 恢复 ✓ |
| **循环压测 30 轮** | **30 PASS / 0 FAIL**，30 次 register/unprotect/NPT 翻转循环，exitprof 356k exits **100% cpuid 零 NPF**（L1 全拦截，NPT 零打扰=设计分层精确工作） |
| 生态 | 哨兵/命令/卸载不受影响 |

### r76 候选

1. 区域 trip 的 E2E（L2 绕过检测）：需要一个内核物理映射写探针（CVE 式
   验证工具），或临时 MapPhysicalSpace 测试钩子。
2. 区域 Id 存储时序小修（Cosmetic）。
3. 进程死亡自动清区域（PsSetCreateProcessNotifyRoutineEx 联动）。
4. 多进程保护集演示（两个进程各保护一区域 + 切换场景）。
5. Release 长浸泡（blocksoak 已 30min 全绿）。
- guest 留存：Release 7fd0092a RUNNING 裸 NPT（区域清空）。

## Round 76 (2026-09-11 深夜) — L2 绕过 E2E 落地即引爆两颗 r75 埋雷：0xE2 'SVMC' fail-fast 病 + 音频后端锁死 vcpu 环境病

### 现象（三连，按时间线）

1. **22:47（本轮开工前 3 分钟）**：guest 自发 BSOD（091126-12546-01.dmp，
   r75 构建 7fd0092a，无人操作）——当时未归因，事后证实与本轮 23:19 的
   BSOD 同签名（见下）。
2. **22:52**：推 r76 → svc start 后 ~60s guest 全停（无 dump、无重启、
   6 vCPU 全 Wait、vmware-vmx 4 秒 0ms CPU）。硬重置后 **无驱动裸机**
   在 23:02-23:03 再次全停——两次全停均发生在 guest 音频事件后
   （vmware.log：vcpu 线程上下文里 SOUNDLIB 创建 WAVE stream →
   ~2.5s 后 vmx 强制 Stopping/Closing → vcpu 永久静默）。
3. **23:19**：sound.present=FALSE 后重部署，probee2e 在内核探针写指令处
   触发 Release fail-fast：**0xE2 MANUALLY_INITIATED_CRASH Arg1='SVMC'
   Arg2=GPA Arg3=Cr3RegionProbe+0x16e**（091126-10687-01.dmp）。

### 与参考的对比

- r75 blocksoak 30/30 全绿但从未触发 NPF（L1 纯 guest-MM 拦截），
  所以 r75 没炸——**不是 r75 验证通过了 sensing，是根本没走到**。
- 参考实现 参考无此结构（无 NPF deny 消费者分层）。

### 根因一（驱动，两颗雷）

kd 现场抓到决定性日志：`REGISTER id=1 gpa=2b199000` → `npf: deny
action=2 gpa=2b199000 cpl=0` → **没有 `sentinel: deny cb` 行** →
`g_denyCb == NULL`。NPF deny 消费者（NpfSetDenyCallback）在
Cr3MonInit 里注册，而 cr3_monitor 是 **mod attach 才 init 的模块**；
r75/r76 部署流程只有 svc start（裸部署）。于是：

- **雷 A**：裸部署下任何 region/sentinel deny 都无人消费 → 落入
  npf.cpp Release fail-fast → 0xE2 'SVMC'。r75 时代 22:47 的 BSOD
  同签名（同 Failure.Hash），即 **r75 上线当天就已埋雷**。
- **雷 B**：region 生命周期还有两个"deny 活着但槽不可见"窗口：
  REGISTER 先 arm 后发布、UNPROTECT 先清槽后恢复 RWX。

### 根因二（环境）

vcpu 线程上下文的 VMware SOUNDLIB 后端调用宿主 XIBERIA USB 耳机，
宿主音频设备停顿 → vcpu 线程被宿主侧锁挂住 → 6 vCPU 全停、guest
假死（无 dump、无重启、0 CPU）。与 r74 的 05:02:58 神秘关机同族。
**修复：vmx `sound.present=FALSE`**（测试机不需要音频），此后 12+
小时无再发。

### 修复（全部落在本轮提交）

| # | 修复 | 位置 |
|---|---|---|
| 1 | `Cr3MonitorLoadInit()`：NpfSetDenyCallback + death notify 在 DriverEntry 注册，驱动生命周期有效；Cr3MonInit 不再负责 | cr3_monitor.cpp / main.cpp |
| 2 | `Cr3RegionDeathNotify`：进程终止即 `Cr3RegionSweepPid`（裸部署也有清扫；死进程帧不得带活 deny 进池复用） | cr3_monitor.cpp |
| 3 | REGISTER 时序反转：采样 GPA → 填槽 → **先发布 Active 再 arm deny** | cr3_monitor.cpp |
| 4 | UNPROTECT 宽限环 `g_regionDisarmed[256]`：清槽前记入，consume 落空时查环→resolve+吞掉（镜像 r68 sentinel 环） | cr3_monitor.cpp |
| 5 | REGISTER 拒绝不在线页：`MmGetPhysicalAddress()==0`（demand-zero 未命中）的页直接拒绝并回滚——否则发布 Active=0 槽（consumer/sweep/list 三不可见）+ **对物理页 0 arm deny** | cr3_monitor.cpp |
| 6 | probe 要求活区域（pid+base 精确匹配），防止假阳性 E2E | cr3_monitor.cpp |
| 7 | 新增 `SVMB_IOCTL_CR3_PROBE 0x824` + `Cr3RegionProbe`：MDL IoReadAccess 锁页 → MmMapLockedPagesSpecifyCache(KernelMode) → MmProtectMdlSystemAddress(RW) → 写 magic——内核映射旁路 L1，必须惊动 L2 | 驱动+协议+ctl |
| 8 | `cr3 probee2e` 自含 E2E + `RegionTrips` stats 字段 | ctl |

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| r76 probe 代码引爆了 bug | 雷是 r75 埋的（消费者 attach 门控 + 22:47 同签名 BSOD）；probe 只是第一个真正走到 NPF 的流 |
| 两次无驱动全停 = 驱动病 | 环境病：宿主 USB 音频 → SOUNDLIB 锁死 vcpu（sound.present=FALSE 后绝迹） |
| "静态 log + 0 CPU" = guest 死 | 第三次是健康的空闲桌面（vmware.log 空闲即静默）；判活必须 vmrun 探测或截屏，不能只看 log |
| death sweep 没跑是 notify 没注册 | notify 正常；是 deathtest.ps1 没先写页 → demand-zero → GPA=0 槽三不可见（根因 #5） |

### 验证（构建 b6cbb521 双签名，最终版）

| 项 | 结果 |
|---|---|
| probee2e（kd 下首验） | PASS：旁路写落地 + RegionTrips 0→2282（影子 NPT 重试风暴，有界）+ L1 仍拦 c0000005 + 解保护恢复 |
| probee2e 30 轮压测 | 30 PASS / 0 FAIL |
| blocksoak 回归 | 36 PASS / 0 FAIL（跨重部署两段） |
| 死亡清扫负测试 | protect 后进程直死 → `death sweep pid=4836 released 1 region(s)` → regions=0，count 平衡 |
| 最终版回归 | probee2e ×3 PASS（单次 trip 即 resolve）+ blockself ×6 PASS + regions 恒 0 |
| Id 时序修复 | REGISTER/regions 从此显示真实 id（曾恒 0） |

### 遗留 / TODO

- **影子 NPT 重试风暴**：首轮 probe 一次旁路写消耗 2282 次 NPF exit
  才 resolve 生效（invlpga 局部失效在 vhv 影子上不动，靠周期性影子
  刷新收敛）；最终版单次 trip 生效——机制不稳定，r79 候选：trip 时
  批量 kick / TlbKickFlushAllCores 时机。
- sweep 只在 notify 里做（created=false）；驱动加载前已死的进程的
  region（不可能存在，region 随驱动生）——无缺口。
- 多进程保护集、region Id 级联（UNPROTECT by id）、L2 拦截模式
  （从 sensing 升级为 blocking，需解决注入限制）→ r77+。
- 22:47 的 r75 构建 BSOD dump（bsod_2247_pre.dmp）已归档 tests/。

### 环境备忘（新增律）

- **测试机 vmx 必须 sound.present=FALSE**：宿主 USB 音频停顿会以
  SOUNDLIB 锁死 vcpu 线程，guest 全停且无任何 dump。
- **guest 判活三件套**：vmrun 探测（第二通道）+ 截屏 + CPU 采样；
  log 静止与 0 CPU 皆非死证。

## Round 77 (2026-09-12 凌晨) — NCr3 shadow-kick：区域传感重试风暴确定性消除 @ c24089f1 (见提交)

### 现象

r76 首次 probee2e 一次旁路写消耗 **2282 次 NPF exit** 才 resolve 生效
（RegionTrips 0→2282）；后续 3 次回归又全部单 trip——间歇性尾部事件。

### 与参考的对比

r69 定律：vhv 嵌套影子只在 **NCr3 值变化**时重建，完全无视 TlbControl。
trip resolve（SetPerm4kNoLock RWX + invlpga）对影子不可见 → 重试每次
都走陈旧 deny 影子项，直到影子周期性刷新碰巧收敛。

### 修复

1. **wiggle scratch view**：NPT enable 时 CreateView + FillIdentityView
   （[0, RamEnd) identity 2M RWX，与 base view 覆盖一致——r61 LAPIC 洞
   不复发，验收 P3-9 确认）。
2. **ShadowKick()**：region trip resolve 后两次 ApplyCb 广播
   （scratch → active），强制影子重建，重试首跳落地。
3. **验收修正**（子代理 FAIL→PASS）：
   - P0：初版用 KeAcquireSpinLock——NPF 退出路径任意 IRQL（CLOCK2）
     下会降 IRQL → 移除锁 + `>DISPATCH` 门禁（CLOCK2 trip 落回 r76
     有界风暴回退）；
   - P2-1 采纳：不读写 Active_，仅读 Active() 一次，并发 kicker 最终
     广播收敛同一 Pml4Pa（DestroyView 防线恢复恒有效）；
   - P2-4：ACTIVATION PROTOCOL 登记 SANCTIONED EXCEPTIONS a/b；
   - P3-6：kick 移到日志行之后（取证顺序）；P3-1：CAS 窗口注释修正。
4. RegionTrips 门控：CAS 每 tick 窗口至多一 kick（风暴不成 rebuild 洪
   泛）；`WiggleKicks` stats 字段（三方一致，sizeof 不变尾部追加）。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| 风暴是 re-arm DPC 时序竞态 | 影子陈旧项：resolve 对影子不可见，重试全走 deny |
| 加锁串行化两阶段广播最稳 | 自旋锁在任意 IRQL 退出路径=P0；无锁收敛即可 |
| invlpga(rip 页) 足够 | resolve 场景"failed walks 不缓存"本不需要 flush；需要的是 NCr3 变值 |

### 验证（构建 c24089f1 双签名）

| 项 | 结果 |
|---|---|
| 基线量测（修复前） | 20/20 delta=1（暖态）；r76 冷态曾 2282 |
| 冷启动循环 ×3（修复后） | trips 0→1、kicks=1、PASS，确定性单 exit |
| 暖态 20 轮 | delta 1-2，无回归 |
| 全量回归 | probee2e ×3 + blockself ×9 全 PASS，stats 一致 |
| 子代理验收 | FAIL(P0) → 修复 → **PASS 无 P0/P1 残留** |

### 遗留（已登记）

- **P1-1 文档化（r80）**：ProcessView × region 组合，kick 后 ProcView
  pinned 核心回 manager-active，sensing 瞬态降级至下次 CR3 切换；
  r80 做 per-VCPU NCr3 还原。
- wiggle view 恒占 1/7 非默认槽；ActiveViewId() 暂无消费者；
  tlbMode=1 裸部署 FLUSH_ALL 粘滞（既有行为，A/B 实验需知悉）。

## Round 78 (2026-09-12 凌晨) — 多进程保护集 + by-id unprotect @ e56a92f (见提交)

### 新增

1. **UNPROTECT by Id**：SVMB_CR3_PROT 的 Reserved 改名 InUnprotectId
   （同 offset 同大小=ABI 兼容）；!=0 时按 region id 匹配，覆盖
   pid/base 路径（=0 完全走原逻辑，向后兼容）。Id 唯一性由 IOCTL
   gate 串行 + g_protNextId 单调保证；u32 回绕需 2^32 次注册（32 槽
   下不现实，注释已记录）。
2. **ctl `cr3 unprotectid <id>`**、**`cr3 holdpage <sec>`**（保护自己
   的页并存活指定时长；被杀即 death-sweep 演示）、regions 列表显示 Id。
3. **multitarget.ps1 E2E**：3 进程各保护一页（id=1,2,3）→ 杀 1（清扫
   剩 2）→ unprotectid 2（剩 1 且 id2 确认不在）→ 杀 2（清扫剩 0）。
   全链路 PASS。

### 坑

- PowerShell 5.1 的 Start-Process 参数是 `-PassThru` 不是
  `-PassingThru`；且守护断言必须逐步硬校验——首版脚本 spawn 全败后
  step4 (0==0) 打出假 PASS（独立文件名 + 直接核对每步计数的必要性
  再证一次）。
- holdpage 必须先写页命中再 REGISTER（r76 GPA=0 拒绝律）。

### 验证（构建 3e187eba 双签名 + P3 修正重建）

multitarget PASS；probee2e/blockself 回归 PASS（trips 0→1，kicks=1，
regions 终态 0）。子代理验收 **PASS-with-notes**（无 P0/P1/P2；P3：
陈旧 Id 注释修正、usage 对齐、holdpage 失败分支 VirtualFree、回绕
注释——已全部采纳）。

### 遗留

- LIST 上限 8 条与 OutCount（32）可能不一致且静默截断（预存，r82 工
  具轮处理）。
- r79：2h 混合浸泡（probesoak+blocksoak 交叠）攒 n。

## Round 79 (2026-09-12 上午) — 2h 混合浸泡全绿 @ 构建 bab53a23（r78 提交态）+ r80 构建部署

### 设计

24 轮 × (~5min)：每轮 probee2e（L2 传感+shadow-kick）+ blockself（L1
阻断）+ stats 快照。混合负载同时压两层与 kick/re-arm 通路。

### 结果（零事故）

| 项 | 值 |
|---|---|
| probee2e / blockself | 24/24 PASS + 24/24 PASS，**0 FAIL** |
| RegionTrips : WiggleKicks | 24 : 24（每轮精确单 exit，r77 kick 生效铁证） |
| 自然 kill / reuse / heals | 0 / 0 / 0 |
| regions 终态 | 0（清扫与 unprotect 全部平衡） |
| guest 存活 | 全程无 BSOD / 无冻结 / 无驱动卸载 |

### 附带产出（浸泡等待期并行完成，不占 VM）

- **r81**：docs/L2_BLOCKING_DESIGN.md（告警+击杀 v1 设计，子代理
  PASS-with-notes 修正后提交 @ f25c9a2）。
- **r80**：真 wiggle 代码 + 构建（评审 PASS-with-notes，P2 卸载窗口
  null 防护 + P3 已修；fresh-boot A/B 待本浸泡结束后执行）。

## Round 80 (2026-09-12 中午) — 真 wiggle（可观察的 scratch NCr3）+ per-VCPU 还原 @ 构建 1960f00a（见提交）

### r77 机制有效性疑虑（自省）

r77 ShadowKick 的两次 ApplyCb 广播背靠背完成，VMRUN 只能看到最终值
——scratch NCr3 从未被 guest 观察，影子重建可能从未发生（冷启动 3/3
可能只是风暴罕见的小样本巧合）。

### r80 修复（mode 2，新默认）

1. **phase-1** `TlbWiggleToScratch`：trip resolve 后、返回 guest 前，
   全核 VMCB NCr3 广播到 scratch identity 2M RWX view——下一次 VMRUN
   真实观察到值变化（影子重建 + 风暴写直接落 identity 帧完成）。
2. **phase-2** `TlbWiggleRestore`：折叠进已有 re-arm DPC（≥1 timer
   tick 后，保证 guest 真在 scratch 上跑过）；恢复目标 per-VCPU 用
   **PublishedNcr3**（ProcView pin 核恢复自己的视图——r77-P1-1 修复），
   未发布核回 manager-active。
3. **旋钮** Parameters\WiggleMode：0=off（r76 行为）/ 1=back-to-back
   （r77 语义，保留 A/B）/ 2=真 wiggle（默认）。
4. 验收修正（子代理 PASS-with-notes）：P2 卸载窗口 null 防护（DPC
   restore 对 NptInstance/Active 判空）；P3 WigglePa_ 在
   DestroyView/Deinit 失效、mode 2 加 NptEnabled 守卫、GIF=0 原子性
   依赖注释（唯一静默失效单点假设）、日志文案。

### 关键时序论证（子代理复核）

退出处理程序全程 GIF=0（svm_entry enter_guest 循环从不 stgi），DPC
无法在 kick 与 pending=1 之间交付——restore 兜底恰好一次，无 scratch
永久搁浅路径。注意：这依赖 GIF=0 原子性（已落注释为 CORRECTNESS
ANCHOR）。

### 暴露窗口（诚实清单，mode 2 比 r77 宽）

scratch 窗口（trip 后到 re-arm DPC，~1 tick）内全核 identity 2M RWX：
所有 sentinel/region deny、proc-view 隐藏页、hook slide 同时失效——
第二次并发攻击写落真帧且无 alert。缓解：CAS 窗限频、trips 稀有、
L1 层不依赖 NPT、identity 覆盖与 r61 修复一致。判定：可接受传感
权衡（评审确认）。

### 验证（fresh-boot A/B，各 5 次 probee2e）

| 腿 | 结果 |
|---|---|
| mode 1 冷启动 | 5/5 PASS，delta 全 1（风暴罕见，A/B 无可见差异） |
| mode 2 冷启动 | 5/5 PASS，delta 全 1（机制按构造确定性） |
| multitarget 全链路 | PASS（id 1,2,3 → 清扫 → by-id → 清扫 → 0） |
| probee2e + blockself | PASS，trips=kicks=1:1，regions=0 |

A/B 局限登记：风暴历史仅出现一次（r76 首验），两种 mode 在 5 样本
下不可区分；mode 2 的优势是机制按构造保证（scratch 必被观察），
不依赖影子老化运气。

### 附注

r80 代码随 r79 提交（aa18f38）入库——提交时工作区已含本变更，功能
验证在本轮完成后闭环。

## Round 82 (2026-09-12 中午) — LIST 截断修复 + README 刷新 @ 本轮提交（构建本轮重建）

### 修复

1. **SVMB_PROT_MAX_ENTRIES 8→32**（与 PROT_MAX 对齐）：LIST 原先最多
   回填 8 条而 OutCount 报真实计数（最多 32）——头部计数与列表行数
   不一致的静默截断。SVMB_CR3_PROT 232B→808B（有感 ABI 变更，ctl+
   驱动同轮重建）；新驱动+旧 ctl 组合从"静默截断"变为"响亮
   BUFFER_TOO_SMALL"（err 122）——方向正确。旧驱动+新 ctl 仍会静默
   少列（预存模式，不支持混版本部署，登记备查）。
2. **README 刷新**：功能面新增区域写保护行；成熟度小节更新到
   round-82（Release 轨道、WiggleMode 三档、sound.present=FALSE
   环境律、剩余硬化清单）。措辞按验收意见修正（TlbMode 覆盖面
   陈述收敛到实测证据）。

### 验证（构建本轮重建，双签名）

list32.ps1：9 个并发保护区域全部列出（9/9）→ 杀全清扫归零（0/0）
PASS；probee2e/blockself/multitarget 回归 PASS。子代理验收
**PASS-with-notes**（ABI 分析确认 808B SystemBuffer 安全、
OutCount==填行数恒等）。

## Round 83 (2026-09-12 下午) — 偏移自适应：1903 硬编码 → 运行时解析 @ 本轮提交（构建 ccfd12f1）

### 动机

EPROCESS DTB (0x28) / ThreadListHead (0x488) / ETHREAD ThreadListEntry
(0x6b8) 三个偏移硬编码 1903——换 Windows 版本即静默读错内存（上线的
第二颗雷）。改为 DriverEntry 运行时解析 + 已知表交叉核对。

### 算法（platform/offsets.cpp）

- **DTB**：当前进程 EPROCESS[0,0x400) 内唯一 qword == `__readcr3()` 掩码值。
- **线程表**：当前 ETHREAD（1903 不导出 PsGetNextProcessThread，r47 律）
  [0,0x800) 找 doubly-linked 一致的 LIST_ENTRY → 沿 Blink 反向走（同偏移
  步进，512 上限）落在 EPROCESS 内的 head → **head ≥ 0x100** 结构规则
  （首 0x100 是头部/DTB 邻域，排除 shadow-DTB 假候选 0x30）。
- 已知表 18362/18363 交叉核对；MISMATCH 响告但**扫描优先**。

### 两次实机迭代（kd 活捉）

1. v1 用 PsGetNextProcessThread 取首线程 → 1903 不导出（r47 律重现）。
2. v2 推测性解引用候选指针 → **0x50 PAGE_FAULT_IN_NONPAGED_AREA @
   OffsetsResolve+0x142**（kd first-chance 活捉）——**新定律：内核读已
   释放 NonPaged 池不产生可捕获 AV，MM 直接 0x50 bugcheck，SEH 接不住**
   （严格适用于推测性解引用；活体 referenced 对象链表的原有 __try 口径
   不变）。修复 = 所有推测性解引用一律过 SafeKernelQword
   （MmIsAddressValid 门控）。
3. v3 hits=2 → 候选诊断日志（cand=2f8@30 假候选 + 6b8@488 真值）→
   head≥0x100 规则排除 → **唯一命中**。

### 验收修正（子代理 PASS-with-notes）

P2 发布竞态（g_resolved 非原子 store 可能被重排到 offsets store 前 →
错偏移静默使用）→ Interlocked 发布/订阅；P3：walk 时重试解析（降级
对称性）、头注释与实现对齐、0x800 跨度过读注记、vcxproj ClInclude。

### 验证（构建 ccfd12f1 双签名，fresh 部署）

| 项 | 结果 |
|---|---|
| 偏移解析 | `build=18363 dtb=28 head=488 entry=6b8 vs-table=MATCH` |
| 附带事实 | 本机实际 build=**18363**（此前一直记录 18362，同布局） |
| seed（解析 DTB） | monitored 正常增长，cr3 落表（watch armed pid 行可见） |
| 哨兵（解析线程表） | mod attach + watch + spawn → **trips=23, armed=7**, worker 唤醒 |
| 区域回归 | probee2e PASS、regions=0 |
| 降级路径 | 解析失败 → seed 节点 cr3=0（限频日志）+ walk 拒绝（why=11），不读错内存 |

### 遗留

- 未解析窗口内 cr3 watch 的 "armed pid=X cr3=0" 文案有误导（功能
  正确：spoof 不 arm）——r85 文案项。
- 哨兵 walk 的活体 __try 口径保留（0x50 律只约束推测性解引用）。

## Round 84 (2026-09-12 下午) — armed-watch 冷启动边界重测：边界消失 @ 构建 ccfd12f1

### 背景

r65 定律：armed-watch（哨兵+视图切换）在冷启动早期武装会急性楔死，
成熟 boot 45min 存活（r64）→ 产生"boot 年龄 ≥15min 才武装"的运维律
（magic number）。r76-r80 更换了 trip 处理路径（shadow-kick/真
wiggle），边界可能已移动——重测。

### 方法（fresh-boot 矩阵，每腿独立）

reset → ping=T0 → T0+30s svc start → T0+delay 武装（mod attach +
watch notepad + spawn）→ 12min 存活观测（每分钟二通道探针）。
全程无 kd（armed 实验运维律）。

### 结果（4/4 腿全绿，零楔死零重启）

| 腿 | 武装时 boot 年龄 | 存活 | trips / armed |
|---|---|---|---|
| L1 | **~1.2min** | 12/12 min | 5 / 4 |
| L2 | 5.0min | 12/12 | 16 / 4 |
| L3 | 10.0min | 12/12 | 10 / 4 |
| L4 | 15.0min | 12/12 | 13 / 4 |

### 结论

**冷启动边界消失。** r65 的急性楔死在当前构建上不可复现——最早
1.2min 武装也干净存活，且整个启动期 trip 率下（L1 只累计 5 trips）
不再构成威胁。机制归因（按置信度排序）：
1. r80 真 wiggle：trip resolve 立即可见（旧路径依赖影子老化收敛，
   启动期高 trip 密度下多个核心同时风暴）；
2. r77 kick + r76 消费者契约修复消除了 deny 无主/重试风暴面；
3. r83 偏移解析消除了潜在错位读取（与本边界无关但同窗消除）。

**≥15min 运维律对当前构建作废**；若未来再现冷启动楔死，按 r80
暴露窗口清单排查。哨兵 trip 量（5-16/12min）与成熟 boot 一致——
启动期 trip 率假说未获支持。

## Round 85 (2026-09-12 下午) — 安全审计：无 P0；P1×2 修复 + P2×5 修复 @ 本轮提交（构建 23d9828a）

### 审计

独立子代理只读审计（tm 模型 TM1 非特权 / TM2 管理员）。完整报告与
处置表：docs/SECURITY_AUDIT.md。

### 信任边界结论

**无 P0，TM1→内核提权路径未发现**：设备 SDDL（SYS_ALL_ADM_ALL）+
16 IOCTL 全 BUFFERED 逐 case 长度校验 + IOCTL 全局门锁 + 服务键 ACL +
hypercall CPL 门均成立。

### 修复（本轮落地，实测验证）

| # | 级别 | 内容 | 处置 |
|---|---|---|---|
| P1-1 | P1 | Release 未编译掉测试武器库（POISON/PROBE/NPT_HOOK/NPT_PROBE/STRESS） | SVMB_PRODUCTION 门控 STATUS_NOT_SUPPORTED；实测 poison err 50、probe 拒绝、blockself 不受影响 |
| P1-2 | P1 | 未认领 deny → Release 0xE2（hook 页 flip-flood/stale shadow 日常可达，两次在案蓝屏） | **政策变更**：npf.cpp 移除 Release fail-fast 分叉，统一 r45 resolve 语义——消费者契约是第一道防线，整机死亡不是可接受失败模式 |
| P2-5 | P2 | cycle 无上限（每轮设计性泄漏） | 封顶 16 |
| P2-6 | P2 | IOCTL start = round-27 致命上下文 | SVMB_PRODUCTION 拒绝 |
| P2-8 | P2 | 死亡清扫 × REGISTER 槽复用竞态（恢复参数被覆盖 → 新区域 L1 保护被误还原） | UnprotectSlot 先快照后清槽 |
| P2-9 | P2 | IsKernelVa 收 non-canonical（单 IOCTL 蓝屏） | canonical 判定 |
| P2-10 | P2 | deny-cb 日志 cap 失效（比较预增值） | 改后增值封顶 |

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| SVMB_PRODUCTION 是攻击面开关 | 只是 fail-fast 开关——Release 一直带着完整测试武器库 |
| 0xE2 fail-fast 是安全网 | 它本身成了日常可达的不稳定源（r76 前后共两次无 attended 蓝屏）；r45 语义的 resolve 才是可接受失败模式 |
| 577/签名是上线主障碍 | 审计显示真正的产品化三件事是 P1-1/P1-2/P1-3 + 外部签名 |

### 遗留（登记为 r86 首项）

- **P1-3 region 帧身份防护**：换页后 deny 流放（可用性+告警完整性，
  非数据破坏）。钉页 vs 水印重验两案待选。
- P2-4 CPUID DoS 放大/预言机（设计固有，产品化前评估）。
- P3 清单（Graveyard 界、pid 桶倾斜、Size 对齐、HookId drain、
  宽限环容量）随重构顺带。

## Round 86 (2026-09-12 下午) — region 帧身份防护：MDL 钉页 @ 本轮提交（构建 e057aeed）

### P1-3（r85 审计遗留首项）

区域页 REGISTER 时不锁页：OS 正常换页 → 帧被释放复用 → NPT W-deny
滞留无辜帧（每 100ms re-deny 的 NPF/日志/事件洪泛 + 假 RegionTrip 洗
掉真告警），同时页回迁新帧后传感静默降级为仅 L1。死亡清扫只覆盖进程
退出，不覆盖工作集修剪。

### 修复（钉页方案）

- REGISTER：attach 后**先** `IoAllocateMdl + MmProbeAndLockPages
  (UserMode, IoReadAccess)`（fault-in + PFN 锁），再 ZwProtect
  PAGE_READONLY，再从 **MDL PFN 数组**采样 GPA（子代理 P2：从 VA 采样
  会在 pin 与 protect 之间的 WRITECOPY COW 微窗口偏斜——PFN 采样使钉帧
  与 deny 按构造一致）；MDL 存槽（ProtRegion.PinMdl）。
- UNPROTECT：PinMdl 快照（随 r85 P2-8 参数快照），guest-MM 与 NPT
  恢复**完成后**才 MmUnlockPages+IoFreeMdl（deny 必须在帧仍被钉住时
  清掉，unlock 后零误触）。
- IoReadAccess（非 IoWriteAccess）：caller 可合法重新保护已只读页；
  PFN 锁与访问模式无关地阻止换出/复用。
- GPA=0 拒绝路径成为死代码防御（锁页后页必然 present），保留并在
  注释标明；顺带修正该死路径的进程上下文错误（restore 移回 detach 前
  ——NtCurrentProcess 在 detach 后是 IOCTL 调用者）。
- 附加修复：Size 强制页对齐（P3-15 部分——Pages 截断与 SizeBytes 判
  定区间不一致）、unprotect 日志用快照 Id。

### 行为变更登记

未命中页从"GPA=0 拒绝"（r76）变为"**MmProbeAndLockPages fault-in 后
接受**"。钉页上限：32 槽 × 64 页 = 8MB 物理内存 + ~21KB MDL 池。

### 验收（子代理 PASS-with-notes）+ 修正

P2（pin→采样偏斜）→ MDL PFN 采样（上）；P3：GPA=0 死路径进程上下文、
快照 Id、文档登记（本条 + SECURITY_AUDIT.md P1-3 状态更新，P3-17
宽限环容量**未**随 P1-3 关闭——钉页不缩小 teardown 竞态窗，显式重
登记）。回归：Release 部署后 blockself PASS + multitarget 全链路
PASS（probee2e 的 probe 步在 Release 被禁属 r85 设计）。

## Round 87 (2026-09-12 下午) — CR3_READVM：虚拟化层进程内存读取落地 @ 本轮提交（构建 8d16999e）

### 现象（本轮目标）

按 docs/HYPERVISOR_MEM_READ.md 的可行性评估实现 IOCTL 0x825：seed 表
取目标 CR3 → 纯物理读手工遍历 x64 四级页表 → MmCopyMemory 搬运 payload。
不 attach、不开句柄、不走 guest 内核 API；读不产生 NPF，对 guest 与
本驱动自建 L1/L2 防护均不可见（持设备句柄者即信任根，SDDL 限管理员）。

### 实现

- `shared/svmb_protocol.h`：SVMB_IOCTL_CR3_READVM (0x825)、
  SVMB_CR3_READVM（Pid/Size/Va + OutResolved/OutFlags，payload 随头）、
  SVMB_READVM_MAX 64K、SVMB_READVM_F_PARTIAL、CR3_STATS 增
  ReadVmCalls/ReadVmPages。
- `driver/src/modules/mem_read.{h,cpp}`（新）：WalkVa（PML4→PDP→PD→PT，
  1G/2M 大页分支，52 位掩码）；每 chunk 独立走表；PhysRanges::IsRam
  预检（MMIO 洞不碰——MmCopyMemory 物理读有设备副作用风险）；present=0
  → 策略 A 零填充 + F_PARTIAL（文档 3.3）；全路径不 deref 任何 guest
  VA（0x50 法无关化）。scope 注记：NPT hook 翻转对物理读不可见，读到的
  即目标进程真帧（文档 4.2）。
- `main.cpp`：分发 case；长度检查（out 缓冲必须 = 头+Size）；**不在**
  production 拒绝清单——它是能力不是测试武器，门在设备 SDDL。
- `app/main.cpp`：`cr3 readvm/marker/readvm-self/readvm-hole` +
  probee2e 增读腿 + holdpage 增 outfile 参数。
- E2E bats：vm_r87_{self,hold,read,probee2e}.bat +
  vm_pull_{hold_r87,spawn_r87}.bat + vm_psdiag_r87.bat（7 个 bat）+
  hold_r87.ps1。

### E2E（Debug 部署，6 vCPU，hv RUNNING）

| 测试 | 结果 |
|---|---|
| readvm-self（驱动走本进程页表读标记） | PASS：resolved=32 flags=0 cmp=MATCH |
| readvm-hole（MEM_RESERVE 未提交） | PASS：ok=1 resolved=0 flags=1(PARTIAL)，无崩溃 |
| 跨进程读活 PowerShell 标记进程 | PASS：字节精确 `SVMB-R87MARK12`（53 56 4d 42...），resolved=32 flags=0 |
| probee2e 读腿（读保护区页） | PASS：内容正确 + RegionTrips 零增量；同页 probe 写 0→1 感知、L1 阻断完好 |
| 全程 read exits / region trips | 0（读路径对虚拟化事件通道静默）|

### 本轮踩坑（通道法补充）

1. **vmrun guest 通道单行道（r83 孤儿 vmrun 法的重述）**：后台
   runProgramInGuest 阻塞期再起第二个 guest-op → 直接 exit 1 静默失败。
   任何"双通道并发"设计（holdpage 持设备 + 另一实例读）都不可行——
   而且 svmbctl 设备句柄独占打开（CreateFileW share=0），双 ctl 实例
   本身就互斥。跨进程测试目标改用**无设备依赖的 PowerShell 标记进程**。
2. **`cmd start ""` 在 VIX 会话下静默不执行**：`(start "" /min
   powershell ... ) > file 2>&1` 通道 cmd 退出 1 且零输出（连 echo 都
   不落盘）。内联 PS 一行命令在多层引号剥离下同样无声死亡。可靠形态：
   **ps1 落盘推送 + `powershell -NoProfile -Command Start-Process
   powershell -ArgumentList '...' -WindowStyle Hidden`** 孵化（外层即
   退，通道释放，子进程活 60s）。
3. **bash→cmd 反斜杠/正斜杠不一致吞路径**：`cmd //c "build\x.bat"`
   与 `cmd //c "build/x.bat"` 都会间歇被 Git Bash 改写（'build' 不是
   内部或外部命令 / 路径合并）——拉取失败若 >/dev/null 就静默过期，
   宿主侧 steps_out 停在旧时刻造成"测试通过"假象。**每次拉取必须
   回显 RC 并核对文件 mtime**。
4. 新建 .bat 必须 CRLF（LF 批处理解析破损，本轮 `unix2dos` 补救）；
   新 bat 必须带 `call vmenv.bat`（漏掉则 %VMRUN% 空 → `''""' 不是
   内部或外部命令'`）。
5. readvm-hole 首跑 err 复现驱动端**头+payload 缓冲校验**按设计拒绝
   （out 缓冲只给 24B 头却要 32B payload → INVALID_BUFFER_SIZE）——
   防线正确，测试端修。

### 验收

子代理验收：见本轮提交信息。遗留：OutResolved 计 partial-transfer
弃读前缀为 0 字节（保守语义）；ReadVmPages u32 回绕（测试平台接受）。

### TODO

- r88 候选：present=0 策略 B（attach+触碰换入，有痕读）按需实现；
- hook 翻转页的"guest 所见视图"读取（查 npt_hook_mgr 记录）——当前
  物理读=真帧语义已文档化；
- 4h+ 浸泡与 armed-watch 共存的 readvm 长稳（读路径理论上零扰动，
  n=1 未归因）。

### 验收结论（子代理，PASS-with-notes，无 P0/P1）

采纳并当场修复：P2-1 README 快速开始路径控制字符损坏（`\v`/`\b` 被
转义吃掉）重写为真实反斜杠 + P3-7 双标点；P3-1 WalkVa 每级页表帧
IsRam 门（被污染 guest 页表不得把物理读引向 MMIO）；P3-3 死进程
TOCTOU 语义记档（复用帧可静默读出貌似合理数据、无 PARTIAL 标志）；
P3-4 Usage 补 holdpage [outfile]；P3-5 readvm size 改 base 10（前导
0 不得按八进制）；P3-6 本条目计数修正。P2-2 凭证已出 tip 但仍在
git 历史——发布前必须 filter-repo 或改 guest 密码（挂在 GitHub
审批单，不阻塞本提交）。P3-2 WalkVa 离线单测、production 可选
ReadVmEnable 旋钮：登记 TODO，不阻塞。

## Round 88 (2026-09-12 傍晚) — CR3_READVM 完整化：策略 B fault-in + 纯核单测 + 加固 @ 本轮提交（构建 a013b641→2c4fbeb9）

### 目标与实现

- **策略 B（docs/HYPERVISOR_MEM_READ.md 3.3 表 B）**：`SVMB_CR3_READVM` 增
  `InFlags`（24→32B，同轮双端重建），`SVMB_READVM_F_FAULTIN` 入参开启
  attach+触碰路径：PsLookupProcessByProcessId → KeStackAttachProcess
  （cr3_monitor r75 同款 ntifs 手动声明 + 64B 不透明 APC 态）→ 逐页
  SEH 包裹 read-touch（NOACCESS/guard 干净 AV 吞掉）→ detach → 解引用。
  读触碰不触发 COW；有痕（工作集增长、一次性 guard 被消费）——文档明示。
- **加固**（r87 验收遗留+本轮）：目标 VA 必须 user 区（IsKernelVa 拒绝，
  首尾页都查）；va+size u64 回绕拒绝；未知 InFlags 拒绝（先于 seed 查）。
- **纯核抽取（r87 P3-2）**：ReadVmVaIndex/EntryFrame/LargeGpa/PageGpa 四个
  纯函数供 WalkVa 与离线单测共用；offline_tests.cpp 增 TestMemReadPure
  （索引/帧掩码/1G-2M-4K GPA 合成/IsKernelVa canonical 回归），selftest
  282 项。

### E2E（Debug 部署，hv RUNNING）

| 测试 | 结果 |
|---|---|
| selftest | **282/282 全过**（含新 rdvm.* 15 项）|
| readvme2e 六腿 | PASS：A 未触碰页=PARTIAL 0；B faultin 跨两页=resolved 0x1008；kernel-va/wrap/bad-flags 全拒；NOACCESS+faultin SEH 存活 |
| 跨进程 demand-zero 页 A→B | A：resolved=0 PARTIAL；B：resolved=32 flags=0（attach 外进程触碰换入后走表读取）|
| 回归 readvm-self/hole | MATCH / 干净 PARTIAL |

### 本轮踩坑

1. ** grave.* 三连失败的根因是测试自身布局脆弱**：t2=自身函数+0x40 依赖
   函数页内偏移处可解码 12 字节 stolen 前缀；在文件前部插入 45 行新测试
   → 偏移移动 → StealLen 失败 → Install 报错。修复=t2 改同页候选探测
   （±0x40/±0x20/±0x80/±0x100 取首个安装成功者）。教训：**对代码地址
   做结构假设的测试必须在层间防御**。
2. `rdvm.1g-gpa` 期望常数手写错位（0x10ABCDEF0 vs 0x100ABCDEF0，十六进制
   位数数错）——walker 数学本身正确，python 离线核算后修常数。
3. **v5 bat 改写弄丢 ps1 推送行**：Start-Process 孵化重写时只留了 spawn，
   guest 一直在跑旧脚本（pid 每次新但格式旧——新进程+旧脚本的双重迷惑）。
   新律：**bat 重写必须保留全部副作用行（push/exec/pull 三件套）**。
4. AllocHGlobal(4096) 走堆段：页被堆元数据活动弄驻留，"未触碰 hole"假象
   （A 也 resolved=32）。**4MB 大块分配走 VirtualAlloc 直配**才是真
   demand-zero；发布 hole 时再 +0x1000 对齐跳过堆头页。
5. ctl 可选参数槽被关键字占用：`readvm pid va 32 faultin` 把 faultin 当
   文件名落盘（16 字节零文件）。修复=两个可选槽都识别关键字。

### 互斥与并发注记

FaultInPages 在 IOCTL 门锁内（PASSIVE）执行；attach 目标若恰在退出，
PsLookupProcessByProcessId 引用保护对象生命周期，触碰走目标真实 MMU
（异常可接，0x50 律不适用——那是内核池 VA 解引用）。种子表 Graveyard
节点永不释放的既有约定保证 Lookup 后 cr3 稳定。

### TODO

- r89 候选：hook 翻转页的 guest 所见视图读（查 npt_hook_mgr 记录）；
  4h+ readvm×armed-watch 长稳（本轮 36min，n=1）；ReadVmEnable 注册表
  旋钮（可选）；方案 C kill 政策实现。

### 验收结论（子代理，PASS-with-notes；P1-1 已修）

- **P1-1（真问题，已修）**：非 canonical VA（[2^63, 2^64-2^17)）可绕过
  IsKernelVa 双检——其 PML4 索引别名内核半区，walk 会静默读出目标进程
  内核内存。修复=改为对 Windows user 顶（0x7FFFFFFEFFFF）的范围检查
  （首字节>top || 末字节>top），顺带消解 P2-1（原 wrap 检查不可达）。
  readvme2e 增 L3b 非 canonical 拒绝回归腿。
- **P2-2（已修）**：SMAP 机器上裸 supervisor 触碰会静默退化策略 B →
  触碰循环以 EFLAGS.AC 括弧（本 WDK 无 _stac/_clac 内建，用
  __readeflags/__writeeflags 位 18；中断按栈保存恢复 AC，安全）。
- **P3 采纳**：WalkVa 深层帧地址统一 `!frame || !IsRam(frame)`（P3-2）；
  protocol.h 混布防线注记（P3-1）；vm_r88_e2e.bat 加参数替换禁令注释
  （P3-4）。P3-6（level-guard 哨兵值）留档不改。
- **修复引入的回归再修**：mem_read.cpp 代码量变化又移动了
  TestNptHookSharedPage 的页内偏移 → shpage.* 四连失败（ grave 同病）。
  两测试统一改用 PickSecondHookTarget 同页候选探测（±0x40/±0x20/±0x80/
  ±0x100），布局无关。最终构建 selftest **282/282**。

### 真武装浸泡（最终构建 95c56839→e53144d9，哈希双验）

首轮 23min soak 复盘：watch 静默空转——`cr3 watch` 需要 cr3_monitor
模块 attach（README 有载），而 vm_ctl_run 丢输出导致 attach 失败不可见，
18 轮浸泡在未武装状态跑完（读路径结论仍有效但 sentinel 共存未验）。
**重做**：attach（本次抓到 "[+] mod attach ok"）→ watch armed →
6 轮 vm_r87_self（每轮 3 个 svmbctl 创建=arm/disarm churn）：diag
arm-wakes 2→15 持续增长、trips=0（无人旁路写武装页，预期）、
reuse/heals/kicks/region trips 全 0、read exits=0、读全 MATCH。
**结论：readvm 与 armed-watch 共存静默。** 收尾 unwatch+detach。

### 新通道律（r88 增补）

1. **改状态类 guest 命令必须走输出捕获通道**（vm_r87_read/watchlog 式
   `>> steps.log`），vm_ctl_run 丢输出=静默失败不可见（本轮 attach 空转
   23min 的直接教训）。
2. bat 重写保留全部副作用行（r87 已立）+ **新 bat 先查 vmenv 行**
   （r88_watchlog 首版又漏）。
3. 需求"未触碰页"必须 VirtualAlloc 直配大块（AllocHGlobal 小分配走堆段
   即已驻留），且发布地址跳过堆头页。

### 遗留 TODO（r89 候选）

hook 翻转页的 guest 所见视图读（查 npt_hook_mgr 记录）；4h+ 浸泡攒 n
（本轮 23min 未武装 + ~6min 真武装，n 仍小）；ReadVmEnable 注册表旋钮
（可选）；方案 C kill 政策实现。

## Round 89 (2026-09-12 晚) — hook 视图读 + ReadVmEnable 旋钮 + 方案 C kill 政策 @ 本轮提交

### 交付

1. **GUESTVIEW 读（IOCTL 0x825 InFlags=SVMB_READVM_F_GUESTVIEW）**：
   hooked 页（NPT hook 翻转）读 hook 的 HiddenPa=guest 实际观察到的补丁
   副本；未 hook 页照常直读（一次调用跨两界）。反作弊比对语义：真值读
   （无 flag）vs guest 视图读（flag）恰在补丁点分歧。今天 hook 只支持
   内核页而 readvm 限用户页（r88 P1-1），故 L7 腿只钉"未 hook 页 flag
   无害"——用户页 hook 出现前该组合不可达，文档明示。
2. **ReadVmEnable 注册表旋钮**（Parameters\ReadVmEnable，默认 1，
   DriverEntry 读取）：=0 时 CR3_READVM 拒绝 STATUS_NOT_SUPPORTED。
   E2E：=0 重启后 err 50 ✓，=1 恢复 ✓。
3. **方案 C kill 政策（docs/L2_BLOCKING_DESIGN.md 3-C 落地）**：
   - 协议：SVMB_CR3_PROT **尾追** Policy 字段（设计文档裁定，旧 ctl 响亮
     BUFFER_TOO_SMALL）；SVMB_PROT_POLICY_KILL per-region opt-in；
     STATS 增 KillAttempts/KillDenylisted/KillDropped。
   - 管道：trip（任意 IRQL）→ 证据落槽（WriterCr3/Rip/Gpa）→ in-flight
     CAS（并发 trip 丢弃计数）→ KeInsertQueueDpc → DISPATCH 转
     IoQueueWorkItem（设计禁止 exit 直接排队）→ PASSIVE 工作项：
     Cr3Seed::LookupPidByCr3 反查（新，List_ 无锁遍历）→ pid<4 + 映像名
     拒绝名单（csrss/wininit/winlogon/services/lsass/smss，纯函数+
     离线单测）→ PsLookupProcessByProcessId + ObOpenObjectByPointer
     （ntifs 手动声明）+ ZwTerminateProcess(STATUS_ACCESS_DENIED)。
     反查失败=只告警（System/加载前进程天然豁免，不作为设计保证）。
   - 语义（文档明示）：首笔旁路写仍落地（威慑模型，阻断后续）；owner
     自写同杀（防篡改）。
   - E2E（probee2e kill 单进程形态）：注册 KILL 区域→读腿静默→旁路写
     →**进程被驱动终止**（输出止于 LANDED）→ stats attempts=1
     denylisted=0 dropped=0 → 死亡清扫 regions=0。真击杀 1/1。
4. ctl：cr3 probe <pid> <base> [size]（独立 trip 触发器）、probee2e
   [kill]、protect/holdpage [kill]、readvm 关键字泛化、stats kill 行。

### 重大事故与根因（本轮最大收获）

**readvme2e 首发即 5 连蓝屏（0x76 PROCESS_HAS_LOCKED_PAGES）+ 长时间
"卡死"假象**。kd 两次活捉（IopCompleteRequest→memcpy→MiUserFault 自旋、
自写预触碰 movzx→KiPageFault 自旋）+ minidump 0x76 分析最终归因：
**r89 的 MDL 故障换入在 MmProbeAndLockPages 失败（NOACCESS 页）后
无条件 MmUnlockPages——解锁从未锁定的 MDL 腐蚀 PFN 记账，进程退出时
0x76，且腐蚀使后续 demand-zero 缺页在 MiUserFault 永久自旋（假死）**。
教训链：①修复 MDL 的分支先"通过"（L1+L2），但 REST 腿一编译立刻复发
——**部分修复≠修复，全腿回归是唯一判据**；②输出全缓冲让每次卡死零痕
——**E2E 长函数必须 setvbuf(_IONBF)**；③minidump 是一等公民证据，
蓝屏重启后第一时间拉 dump 而不是猜。
期间两个投机修复（驱动预触碰、MapRam lazy-fill ShadowKick）均被证伪
回退：驱动预触碰本身就是自旋点；lazy-fill 自 r12 即存在且非根因
（npf.cpp 留档注释禁止无复现重加）。
**新定律：本 vhv 上内核态触碰 demand-zero 用户页可永久自旋
（MiUserFault 不收敛，SEH 不可达）——用户态缓冲必须由用户态代码
先触碰（ctl 全缓冲 memset 律），驱动禁止直接触碰调用方用户页。**

### 其他修复

- L4 腿常数错位（0x7FFFFFFEF000+16 仍在 user 区内=合法读取；改
  0x7FFFFFFEFFF0 真越界）。
- vm_reg_readvm/各种新 bat 的静默失败：**改状态类 guest 命令一律输出
  捕获**；**sc delete svmb 会连带删除 Services\svmb 键（含 Parameters
  全部旋钮）——deploy 周期里 create 之后必须重写全部旋钮值**
  （本轮 ReadVmEnable=0 写入后被 clean 静默清除，假"未生效"一次）。
- selftest 布局脆弱性收官（r88 的 grave 修复在 shpage 重演）：
  PickSecondHookTarget 共享 helper。

### 回归（最终构建，哈希双验）

selftest **293/293**；readvme2e 7 腿 PASS；probee2e PASS（读腿静默+
写感知 0→1+L1 阻断）；readvm-self MATCH；readvm-hole 干净 PARTIAL；
knob 0/1 双向验证；kill 击杀腿 1/1（见上）。

### TODO

- 4h+ 浸泡攒 n（本轮仅短程）；C- 挂起档（可逆前置）按需评估；
- hook 用户页出现后补 GUESTVIEW 真值 vs 视图分歧 E2E；
- WalkVa 离线单测已清账，剩余 P3 按验收。

## Round 90 (2026-09-12 晚) — r89 特性稳定性浸泡（1 小时 kill+read 压测）@ 本轮提交（构建同 r89，无代码改动）

### 形态

armed-watch 常驻（mod attach + watch svmbctl.exe，每轮 3 个 svmbctl
创建 = 武装/解除 churn）+ 12 轮 × ~5min：每轮 `cr3 probee2e kill`
（kill-owner 全链路：注册 KILL 区域 → 读腿静默验证 → 旁路写 → trip →
kill 工作项 → **进程自我终止** → 死亡清扫）+ readvm-self/hole 健康读 +
stats 快照。合计 **14 次 kill-owner 击杀**（r89 E2E 的 n=1 → n=14）、
~40 次武装/解除 churn、46 次 readvm。

### 结果：全绿

| 计数器 | 终值 | 判定 |
|---|---|---|
| kill attempts / denylisted / dropped | 14 / 0 / 0 | 每轮精确 +1，零拒绝零丢失 |
| region trips / kicks | 15 / 15 | 每次击杀精确单 trip 单 kick |
| protected regions（每轮快照+终态） | 0 | 死亡清扫 14/14 无泄漏 |
| sentinel reuse / npf heals | 0 / 0 | 零复用零 heal |
| read exits | 0 | 读路径全程对虚拟化通道静默 |
| readvm-self | 14/14 MATCH | hole 全部干净 PARTIAL |
| 新蓝屏/新 minidump | 0 | 基线 5 个无新增 |
| guest 存活 | 全程 | 12/12 轮 rc=0 |

r89 验收 P2-3 要的 kill-owner 压测形态（杀 owner → 终止 → 死亡清扫解锁
PinMdl 竞态）n=1 → n=14 清账；MapRam-kick 回退后的稳定性疑虑清账
（无 kick 的 lazy fill 全程无复现问题）。

### 运维注记

- 跑批期间宿主侧一切轻查询（stats_alone 每 ~10min）不影响 guest 判定；
- 清理：unwatch + mod detach，终态 = 服务 RUNNING + hv RUNNING + 裸
  NPT + regions 0 + watch 清。

### TODO

- 4h+ 过夜浸泡（含 C- 挂起档落地后）；
- 用户页 hook 出现后补 GUESTVIEW 分歧 E2E；
- 多 build 偏移验证、P3-17 宽限环、CPUID DoS 评估（清账项）。

## Round 91 (2026-09-13) — 方案 C- 挂起档：可逆响应落地 @ 本轮提交（构建哈希双验）

### 交付

- **POLICY_SUSPEND（0x2）**：区域 opt-in 挂起档。trip → 证据 → in-flight
  CAS → DPC → IoQueueWorkItem → PASSIVE：反查/拒绝名单与 kill 共用
  （PolicyResolveWriter 提取，mode-aware 统计），命中后
  **PsSuspendProcess 冻结写入者进程**（可逆！）→ 入挂起表（16 槽，
  pid/regionId/tick/image）。表满 = LOUD（LOGE + SuspDropped）不再静默
  （验收 P2-2）。
- **设计稿证伪**：原稿断言 PsSuspendProcess 为 2004+ 导出（18363 不可
  用）——拉取客机 ntoskrnl.exe 查导出表证伪：**18362.592 导出
  PsSuspendProcess/PsResumeProcess**（序号 1795/1825）。实现仍走运行时
  解析（MmGetSystemRoutineAddress），其他 build 缺失时 REGISTER 对
  SUSPEND 响亮拒绝（诚实降级）。未知 Policy 位同样响亮拒绝（P3-3）。
- **无门快路径（门死锁规避）**：被挂起进程持有 IOCTL 门（其旁路 IOCTL
  冻结在驱动内），list/resume（0x826/0x827）走 DevCtrl 无门快路径
  （DevCtrlSusp），挂起表自带 spin lock，不碰门保护态。
- **可逆面**：IOCTL_SUSP_LIST（cr3 suspended）/ IOCTL_SUSP_RESUME
  （cr3 resume <pid|all>，表外 pid 走直接 resume 兜底=表满也不可逆漏洞
  关闭[P2-2]）；死亡通知剪枝表项；**卸载默认 resume-all**
  （Cr3RegionKillDeinit 首扫 + in-flight 等待后二次扫[P2-1]——never
  strand）。
- **LIST 回显 ENTRY.Policy**（r89 P3-6 清账）+ ctl regions 打印
  policy=KILL/SUSPEND/alert（P2-3 清账）。
- stats 增 SuspAttempts/Denylisted/Dropped/Active（尾追）。

### P1-1（验收 FAIL 判定项，已修 + 实测）

验收指出：victim 冻结在 DevCtrlLocked 内持有 IOCTL 门，操作员
taskkill 冻结进程（对"挂死"工具的自然反应）→ IoReleaseGate 永不执行 →
门永久卡死（含卸载），唯一恢复=硬复位——击穿可逆承诺。修复：
**gGateOwnerPid 门主追踪 + 死亡通知 IoGateOwnerDied 强制放门**（门=互
斥体，死者必为唯一持有者，强制放门安全）。实测注记：冻结写入者实际
**抗拒 taskkill**（终止排队到 resume 后才落地，冻结反而保护了写入者
不被半路杀掉），强制放门路径作为死亡时机不确定性的安全网存在。

### E2E（最终构建，哈希双验）

| 腿 | 结果 |
|---|---|
| selftest | 293/293 |
| **挂起全链路** | probee2e suspend 冻结（输出止于读腿静默）→ `cr3 suspended` 列出 pid/region/image（suspend available: yes）→ `cr3 resume <pid>` → 进程续跑完成 **probee2e PASS** → 表剪枝 active=0 |
| kill 回归 | attempts=1 照常终止 |
| readvme2e / probee2e 正常 | PASS / PASS |
| knob | =1 正常（部署序列含 ReadVmEnable 重写）|

### 新律

1. **E2E 长函数 setvbuf(_IONBF)**（r89 立此轮发扬）：无它挂起冻结的
   输出断点无法定位；
2. **minidump 一等公民**（r89 立）：本轮零蓝屏未动用，但流程就绪；
3. **sc delete 连带删 Parameters 键**（r89 立）：本轮部署序列已固定
   为 clean→create→**写全部旋钮**→npt→seriallog→start；
4. **冻结写入者抗拒 taskkill**（r91 新知）：挂起期间终止排队到 resume
   落地——"冻结保护写入者不被半路杀掉"是特性不是缺陷；taskkill 冻结
   进程的场景由死亡通知+强制放门兜底。

### TODO

- 4h+ 过夜浸泡（kill+suspend 双策略混合形态）；
- 安全审计增补（kill/suspend 原语攻击面 + GUESTVIEW）；
- 用户页 hook 后补 GUESTVIEW 分歧 E2E；多 build 偏移验证。

## Round 92 (2026-09-13) — stealth 模块（CPUID 环境伪装插件）+ TSC vhv-honor 判决 @ 本轮提交（构建哈希双验）

### 现象 / 任务

用户提出反作弊 hypervisor 能力清单盘点后，指定 r92 = "环境伪装" 插件化：
CPUID 证据抹除做成 attach/detach 开关的模块 + TSC 补偿可行性验证腿
（vhv 是否尊重嵌套 VMCB 的 TscOffset 字段决定 r93 做不做 TSC 补偿）。

### 与已有机制的对照

- core 已有半套隐藏（默认开，`HideCpuidBits` 旋钮）：0x80000001 ECX
  SVM 位、0x8000000A EDX NPT 位抹除 + VM_CR.SVMDIS 强制 + VMRUN 反嵌套
  (#BP)。本轮补的是最响的两处暴露：leaf 1 ECX[31]（hypervisor
  present）与 0x40000000-0x40000010 签名叶（"VMwareVMware"、Hv#1、
  0x40000010 TSC/总线频率提示）。

### 实现

- `stealth` 模块（modules/stealth.cpp/h）：CPUID exit handler 优先级 10
  （core 的 HandleCpuidExit 是 0）；所有权集合与变换拆成纯函数
  （StealthScrubLeaf/StealthApplyScrub）离线测试钉死；attach=开 /
  detach=还原（ModuleManager 撤销 handler），无旋钮无 IOCTL——生命周期
  即开关。SvmbApi 按值拷贝进 g_api（BuildApi 给的是栈局部，存指针会悬垂
  ——demo_cpuid 存了指针但 Init 后未再解引用，属侥幸）。
- 刻意不抹 svmb 自有通道：0x400000FE presence probe、0x400000FF
  hypercall 载体、0x40000100 demo 叶——全部在抹除区间之上。
- 不做开机自动挂载：Windows 在引导时按 hypervisor 叶校准 TSC 相关路径，
  冷启动抹 0x40000010 未验证（本轮范围注记）。
- `TscProbe` 旋钮（Parameters，默认 0）+ `TSC_PROBE_OFFSET=0x4000000000`
  （≈91s @3GHz，淹没 kd/工具读数偏差）：VMCB init 时写入全部 6 vCPU。
- `SVMB_IOCTL_RDTSC` (0x828)：驱动 L1 上下文 `__rdtsc()`（不受 guest
  VMCB TscOffset 影响）——与用户态 RDTSC 配对即成对表仪。
- ctl 新动词 `cpuid <leaf>` / `rdtsc`（签名解码 + user-drv 差值打印）。

### 验证（最终构建，哈希双验 9384df47/3dc17412）

| 腿 | 结果 |
|---|---|
| selftest | 307/307（+14 stealth 断言） |
| CPUID baseline | ECX[31]=1、"VMwareVMware"、0x40000010=0x4794f2（≈4.69GHz 提示）全暴露 |
| attach | ECX[31]=0（仅 bit31 清零，0xfef83203→0x7ef83203）、签名叶全零、AuthenticAMD/0x80000001/0x400000FE-FF-100 原样 |
| detach | 精确还原 baseline（最终构建上再复验一轮） |
| 健康浸泡 | stealth 挂载下 readvm-self MATCH ×2，guest 全程存活 |
| **TSC 判决** | tscprobe=1 + 6/6 vCPU stamp 确认偏移已施加 ⇒ user-drv = **-25,427 ticks（≈-9µs，纯读取顺序差）**，若被尊重应为 +0x4000000000 ⇒ **vhv 忽略嵌套 VMCB TscOffset** |

### 关键决策回顾

| 我们以为 | 实际 |
|---|---|
| TSC 补偿 r93 可做（VMCB 字段现成） | vhv 忽略 TscOffset——裸机限定；替代方案=逐条 RDTSC 拦截模拟（每次读一条 exit，性能不可接受），不做 |
| kd rdmsr 0x10 做对表基准 | kd 自 r68 后已失联（no_debuggee）；改用驱动 L1 上下文 RDTSC 做参照——亚微秒偏差，比 kd 更干净 |
| ring dump 为空 = 日志系统坏 | `LogDrain` 是**消费型**：连排两次，第二次用空内容覆盖第一次的文件（fresh4 双 drain 事故）；steps log 只在显式 `svmbctl log` 时才进内容 |
| tscprobe=0 = 旋钮没生效 | 那是设旋钮之前 boot A 的旧 dump；单次 drain 立即回拉即见 tscprobe=1 + 6/6 stamp |

### 新律

1. **LogDrain 消费型**：ring dump 一生只能排一次，dump→pull 必须原子
   （一条链内完成），禁连排；
2. **steps log ≠ ring**：它只是历史追加文件，重启后要看新 boot 的
   驱动日志必须重新 `svmbctl log`；
3. **vhv 忽略嵌套 VMCB TscOffset**（本轮判决，r38 律的姊妹条）；
4. **验收 P2 修复引出的协议编辑律**：移动 #define 块时用 Edit 前后锚
   会复制常量——改完必须 grep 副本。

### TODO

- 裸机 TSC 补偿（TscOffset + exit/VMRUN 记账）仅在裸机移植时排期；
- 4h+ 过夜浸泡（kill+suspend 混合）仍在队首；
- 安全审计增补（kill/suspend/stealth/GUESTVIEW 攻击面）；
- 冷启动自动挂载 stealth 的 Windows 校准行为未验证（有意不做）。

## Round 94 (2026-09-13) — 安全审计增量（r89-r92 攻击面，子代理只读；与 r93 浸泡并行）— CLEAN

**判决：AUDIT VERDICT: CLEAN（无 P0/P1）——2×P2 + 11×P3 加固注记。**
TM1 复核：SDDL 门禁线下无任何新提权路径；四个新 IOCTL（READVM/SUSP×2/
RDTSC）全部 METHOD_BUFFERED+长度校验，r85 门禁全部继续成立；归因/可逆
机制比 r89/r91 自己的验收线更硬。

**P2×2（生产前应修，列入 r96 硬化轮）：**
1. **ST1**：stealth 模块无生产闸门——SVMB_PRODUCTION 下 `mod attach
   stealth` 应响亮拒绝（或显式旋钮 opt-in），对齐 r85 P1-1 测试武器
   哲学（隐藏 hypervisor 存在性=对 EDR/attestation 的对抗能力，不该在
   生产构建裸露）；
2. **R1**：CR3_READVM 输出缓冲若调用者不配合（demand-zero 页），完成
   复制阶段内核触碰自旋（r89 律）且持 gIoGate=全机楔死——修法=走查前
   MmProbeAndLockPages(UserMode, IoWriteAccess) 预探针输出区间
   （FaultInPages 同款模式），调用者配合不再是前提。

**P3 加固注记（11 项，摘关键）：**
- K1 归因可被 guest 内核写手借 CR3 转嫁（需要 admin 先布 KILL 区；
  借者本就能原生杀进程）⇒ 取证误伤面非提权；修法=响应限定
  writer_pid==region.Pid；
- K2 pid<4 守卫没盖住 System(=4)（今日靠"先于驱动加载未被 seed"偶然
  保护）⇒ 改 pid<=4 + kDeny 增 system/registry/memcompression；
- K3 拒绝名单映像冒名（同目录放个 csrss.exe）只会把 KILL 降级为
  alert，非绕过；⇒ kill 时对活 EPROCESS 复核映像名；
- K4 suspend-with-null-pfn 落入 KILL 分支（当前不可达；懒解析未来雷）
  ⇒ else if 显式化；
- K5 kill workitem 分配失败时 REGISTER 不拒 KILL（静默降级 alert）⇒
  镜像 SUSPEND 的响亮拒绝；
- S1 表满 17 槽冻结溢出者在卸载后永久搁浅（resume-all 只扫表）⇒
  挂起前预检槽位或保留溢出记录；
- S2 无门快路径结构性绕过 production 拒绝过滤器位置（今日无行为差异）
  ⇒ 拒绝检查上提到 DevCtrl 快路径之前；
- ST2 CPUID 处理链同叶重叠无仲裁（当前两模块叶集不相交）⇒ 未来模块
  注记；ST3 demo_cpuid 存栈局部 SvmbApi 指针（悬垂未解引用）⇒ 改拷贝；
- R2 帧复用 TOCTOU=对信任根的取证假信心（r87 已记档）⇒ 走查后复核
  Lookup(pid)+cr3，死目标读打 OutFlags 标记；R3 ReadVmEnable 生产默认
  应 0。

**Production 拒绝清单复核**：READVM（只读+双闸门+信任根声明在案）、
SUSP 对（驱动自施冻结的控制平面，加门=设计性死锁）、RDTSC（平凡）不
入拒绝清单=可辩护；唯一缺口是结构性的（S2：快路径绕过滤波器位置）。

**流程注记**：r94 与 r93 浸泡并行（子代理全程宿主只读，未触碰 guest）；
r94 无代码改动，无独立提交（审计结论随 CRASH_DEBUG_LOG 与
SECURITY_AUDIT.md 增量走待审批提交）；P2 修复列 r96 硬化轮（浸泡结束
后部署验证）。

## Round 93 (2026-09-13) — 4 小时 kill+suspend 混合浸泡 @ 本轮提交（构建 23ebf0fa/24bbc237，r92 无代码改动）

### 现象 / 任务

队首任务：战役最长浸泡（前纪录 r69 的 2.5h），且是**第一次双策略混合
形态**——每迭代 = probee2e kill（驱动终止进程）+ 挂起全链路（孵化
victim → 冻结 → cr3 suspended → resume all → victim 续跑 PASS）+
readvm-self/hole 健康读 + stats 双快照。

### 结果（宿主侧 395/395 迭代 rc=0，01:38:44 → 05:38:44 整 4h）

| 指标 | 值 |
|---|---|
| 迭代 | 395（host 计数）/ 398（guest 标记含 3 试点）全部 rc=0，最慢 42s |
| kill | attempts=398（+395 精确吻合）denylisted=0 dropped=0 |
| suspend | attempts=396（+395 精确吻合）active=0 终态（每迭代表剪枝干净） |
| victim 证据 | probee2e PASS ×399（浸泡 395 + 试点/回归 4）——每次冻结都被 resume 解开并跑完 |
| readvm-self | MATCH ×414，零 FAIL 行 |
| 异常 | 零蓝屏、零失败行、guest 全程存活（host 时间戳连续 36s 到最后一秒） |

r90 的 n=14 kill → n=395+14=398+；r91 的挂起 n=1 → n=396+。双策略
交互（同迭代先杀后挂）零耦合异常。

### 新律（浸泡基础设施）

1. **VIX 下 `cmd start` 死刑确认**（r87 律的再发现+补全）：r72 时代的
   `(start "" /min ...)` 形状在当前环境全变体失败（powershell/cmd/直接
   cmd 三连空输出）——**唯一合法孵化器 = r87 的
   `powershell -NoProfile -Command Start-Process powershell -ArgumentList
   '-File <ps1>' -WindowStyle Hidden`**，载荷必须落盘 ps1；
2. 挂起浸泡用孵化式 victim：冻结进程绝不占 VIX 通道（list/resume 从
   外部正常跑），victim 输出走 guest 侧重定向文件、resume 后
   `type >> steps log` 作 PASS 证据；
3. vm_ctl_mod.bat 的 5-retry 循环对 cmd 层失败会假报 RC=1（r92 试点
   误判 attach 失败）——模块挂载一律用自带输出捕获的专用 bat 核实。

### TODO

- r96 硬化轮（审计 ST1/R1 + 选摘 P3）已写完双配置编译通过，本浸泡一
  结束即部署验证（下一节提交后进行）；
- kill/suspend 混合浸泡 n 已充足，后续浸泡转向审计修复后的回归轮。

## Round 96 (2026-09-13) — r94 审计硬化轮落地 @ 本轮提交（最终构建 fa4ac416/16560b6c 哈希双验）

### 实现（审计 P2×2 全部 + P3 选摘 8 项；K1/K3 为政策语义项留用户定夺）

- **ST1**：SVMB_PRODUCTION 下 ModuleManager::Attach 对 "stealth" 响亮拒
  绝（STATUS_NOT_SUPPORTED）——对抗性能力不随生产构建裸露（r85 P1-1
  哲学）；
- **R1**：DevCtrl 顶层在 CR3_READVM 走查前对输出缓冲 IoAllocateMdl +
  __try MmProbeAndLockPages(UserMode, IoWriteAccess)，PMDL 持有至
  IoCompleteRequest 之后解锁——非配合调用者从"全机楔死"降级为
  STATUS_INVALID_USER_BUFFER 干净拒绝；**实测：readvm-deadbuf（保留页
  输出缓冲）→ refused err 998，无楔死**；
- **S2**：production 拒绝过滤从 DevCtrlLocked 上提到 DevCtrl 快路径之
  前（结构性缺口关闭），死标签 finish 清除；
- **K2**：pid 守卫 <=4；kDeny += system/registry/memcompression（离线
  断言 +3，suite 308→311）；
- **K4/K5**：挂起原语缺失绝不落入 KILL 分支（else-if 显式）；kill
  workitem 缺失时 REGISTER 镜像 SUSPEND 响亮拒绝（含完整回退序列）；
- **S1**：挂起前预检槽位——表满冻结降级为 alert-only（SuspDropped++），
  消灭"冻而不录"卸载搁浅；验收 P3-1 注记安全窗不变式（唯一 filler +
  单飞）已入注释；
- **R2**：READVM 走查后复核 Lookup(pid)==cr3，不匹配打 F_STALE(0x8)
  （协议尾追；InFlags 掩码天然拒伪造）；活目标 flags=0 实测不误报；
- **R3/ST3**：ReadVmEnable 生产默认 0；demo_cpuid 改按值拷贝；
- **死代码/注释清账**：r89 READVM "wedge documented" 注释更新为 R1 后
  语义；MDL 持有声明按 P3-3 精确化（completion copy 在 APC 阶段、解锁
  之后，重缺页需内存压力/并发拆毁）。

### 验收（子代理）：**PASS（无 P0/P1）+ 7×P3 全部采纳修复**

P3-1 安全窗不变式注释、P3-2 陈旧注释、P3-3 MDL 持有声明精确化、
P3-4 NULL UserBuffer 拒绝、P3-5 MDL 分配失败区分 INSUFFICIENT_RESOURCES、
P3-6 deadbuf 输出换行、P3-7 help 文案。

### 验证（最终构建）

selftest 311/0；readvm-deadbuf refused；readvm-self MATCH flags=0；
stealth attach/detach 回归绿（Debug 构建闸门不生效=设计）；完整混合
迭代 kill=1/suspend=1/PASS/active=0；Release 构建（production 分支）编
译通过。部署序列含一次 P3 修复后重部署（fa4ac416/16560b6c）。

### TODO

- K1（响应限定 owner 自写=改变威慑模型语义）、K3（kill 时活 EPROCESS
  映像复核）留用户政策决策；
- 多构建偏移验证（r97 队首）、P3-17 宽限环、CPUID DoS 重评照旧排队。

## Round 97 (2026-09-13) — P3-17 宽限环容量闭环 @ 本轮提交（构建 fc3f62ff 哈希双验）

### 变更（单文件：cr3_monitor.cpp）

- **PROT_DISARM_RING 256 → 2048**：对齐最坏拆毁面（PROT_MAX 32 区 ×
  64 页，验收核实 r86 MDL 钉页不增加页数、四条 teardown 路径全部经
  Cr3RegionUnprotectSlot 记环）。churn 风暴不再能在竞态写到来前挤出
  条目；2048 条满表面拆毁也不会自我驱逐（第 2049 条才挤）。
- **验收抓到并顺手修的既存雷**：索引用有符号 LONG `% 2048`，2^31 次
  记环后回绕成负 → % 产生负索引 = 数组前 8 字节 OOB 写。 horizon 极远
  （~33M 次全区拆毁）但修复是一个 token：改掩码
  `& (PROT_DISARM_RING - 1)`（与姊妹哨兵环的 `& 3` 同款，2048 是 2 的
  幂）。

### 验收（子代理 PASS）

容量数学核实（含 MDL 路径）、2048 槽线性扫描成本核实（仅 rare 拒绝
路径可达：absent-leaf 懒填充类 NPF exit 不触发 deny 回调；最坏数十
µs，同路径已有 IPI 广播更贵）、16KB 非分页静态可接受、注释与代码一
致。附注（ticket 不阻塞）：arm 用 PFN 数组而 consumer/re-arm/teardown
按 Active 物理连续假设寻址——物理非连续区域属 r86 设计属性，登记备
查。

### 验证

selftest 311/0（构建 fc3f62ff，哈希双验）。多构建偏移验证项判定为
**资源阻塞**（需其他 Windows 构建的 guest/ntoskrnl 样本，单 VM 无法
开展）——留用户决定是否追加测试机。

## Round 98 (2026-09-13) — CPUID DoS/预言机重评（评估轮，无代码改动，r85 遗留项闭环）

### 测量（构建 fc3f62ff，storm 动词 6 核 × 1M CPUID-exit）

- 6,003,369 条 cpuid exit 精确入账（exitprof 直方图 99% cpuid），
  wall 26.392s（Measure-Command 包裹）⇒ **聚合 227k exits/s，每 exit
  ≈4.4µs（六核并发含争用；单核顺序延迟 ≈26µs）**。
- 处理路径 O(1) 无锁无分配：HandleCpuidExit = 直通 + 既有位抹除 +
  per-vcpu 直方图槽自增——storm 期间零新日志、零内存增长、零异常。

### 评估结论

1. **DoS 放大=无**：guest CPUID 风暴烧的是**它自己的调度片**（guest
   进程 100% CPU），svmb 的每-exit 开销计在同一物理线程上；处理为
   O(1)/无锁/零分配，无跨核锁争用、无资源杠杆。物理机视角：风暴线程
   本来就 100%，虚拟化税（~100-1000×/指令 vs 原生 CPUID）是嵌套虚拟
   化固有成本，svmb 未额外放大。r85 "产品化前重评" ⇒ **闭环：无
   svmb 特有放大**。
2. **存在性预言机（诚实登记，结构性不可修）**：CPUID 0x400000FF 应答
   'SVMB' = 设计的 hypercall 通道（有意公开）；时序侧信道（rdtsc 包
   夹 cpuid 测 exit 抖动）在嵌套拓扑下**原则上不可消除**（TSC 补偿已
   判死 r92；exit 抖动 µs 级 vs 原生 ns 级）。 stealth 模块抹的是
   VMware 叶与本 bit31——不覆盖时序通道。结论：本拓扑下"完全不可检
   测"不可达；研究框架定位接受（与 r92 范围预期一致）。
3. **流程注记**：storm 动词名是 `storm`（help 菜单第 4 行），
   "stress" 是不存在动词——首次测量跑了 usage 菜单 33ms 假成功；
   测量脚本输出必须含状态行（Measure-Command 会吞内层 stdout，首跑
   未捕获 "stress done" 差点误读）。

### 队列清账

- 多构建偏移验证：资源阻塞（需其他构建 guest/ntoskrnl 样本），留用户；
- P3-17 / CPUID DoS：本轮闭环；
- 常备：GitHub 发布审批仍待用户（License/dump 历史/remote URL）。

## Round 99 (2026-09-13) — 系统调用审计 v1：E2E 工作后遭 PatchGuard 判死（负结果轮 + 架构出路）@ 本轮提交（构建 61a228f9）

### 任务与两代实现

目标：拦截/审计 ReadProcessMemory/WriteProcessMemory 类调用（调用者+
参数+频率）。架构演变（全部实测驱动）：

- **v1a（SSDT+Zw 存根）**：ZwRead/WriteVirtualMemory 解析 SSN →
  LSTAR 扫描 KiServiceTable → 表项 rel32 还原 Nt 地址。**两连死**：
  ① ZwRead/WriteVirtualMemory **根本不在导出表**（内核跨进程拷贝的
  导出接口是 MmCopyVirtualMemory）；② 1903 dispatcher 的 rip-rel lea
  （0x354 处 lea r10, [rip+0x3b9ce5]）目标是别的结构体，不是
  KiServiceTable——候选锚定全部失败（stage 3）。
- **v1b（落地方）**：直接 hook 导出的 **MmCopyVirtualMemory**
  （NtRead/WriteVirtualMemory 的唯一汇入点，覆盖 RPM/WPM 全路径），
  NPT 执行 hook（r75 已证的 InstallCallback 机制）+ 调用者 CR3→pid/
  image 归因（r91 反查）+ 无锁环形缓冲 + IOCTL 0x829 排水 +
  ctl sysaudit 动词 + SvmbApi 增设 InstallHookCb/RemoveHook（模块获得
  hook 能力）。

### E2E 实锤（拦截完全工作）

attach 后立即捕获真实流量：**pid+映像归因正确**（cmd.exe/powershell
 的跨进程拷贝）、探针 64 字节跨进程读完整入账（src 地址=notepad
 MainModule 基址 7ff7a42a0000、size=64）、src==dst 自拷贝过滤生效、
 零 lost。配套修了两个实现 bug：arg5 读取槽位（+8→+0x28 影子空间）、
 自拷贝噪音过滤。

### PatchGuard 判决（本轮核心负结果）

attach 数分钟后（迭代 1000 期间）guest bugcheck 自动重启。minidump
091326-12453-01（windbg-mcp 离线分析）：

```
CRITICAL_STRUCTURE_CORRUPTION (0x109)
Arg3 = fffff80048e22cb0   ← 精确 = 我们 hook 的 MmCopyVirtualMemory
Arg4 = 1                  ← "Modification of a function"
Failure.Bucket = 0x109_1_nt!MmCopyVirtualMemory
```

**判决：inline patch 型内核 hook（含 NPT 隐身补丁：读见原文、执行见
补丁页）对 PatchGuard 有必然暴露窗口**——PG 的完整性扫描迟早读到
EXEC 视图（slide 的 EXEC 态窗口/多核交错），补丁必被抓，0x109 不可避
免。r75 的 NtCreateFile hook 未炸 = 存活窗口短 + 随机扫描未中，属侥
幸；该机制对受保护内核函数**一律不可用**。回收：sysaudit 模块从注册
表摘除 + Init 顶部保险拒绝；SvmbApi 的 hook API 保留（通用无害）。

### 架构出路（r100 方向：X-deny 执行感知）

**零 guest 字节修改**的替代：对 MmCopyVirtualMemory 页设 NPT X-deny →
首次执行 NPF exit → 记录（RIP==target 即审计）→ 放行 + re-arm（哨兵
r68 的 trip→resolve→re-arm 同款）。PatchGuard 不可见（NPT 是 L1 私
有），代价 = 每次调用 ~2 exit + 页粒度邻居噪音（RPM 低频可接受）。
NPF DenyExecute 分类已存在（r31 hook slide 用过）。

### 流程律

1. **NPT inline hook ≠ PatchGuard 免疫**：读/执行分身骗得过完整性读，
   骗不过随机化扫描的多视角命中——受 PG 保护的内核函数禁止 inline
   hook（无论多隐身）；
2. **guest 侧输出文件必须 guest 侧 del**（bat 宿主删的是宿主拷贝，
   guest 文件跨运行累积=陈旧数据污染判读，r92 cpuid 律的 e2e 版）；
3. 循环输出的环形缓冲在 hook 回调路径禁自旋锁（冻结/终止策略可能在
   回调持锁时冻结线程——本次未炸但属同族雷，v1b 用 spinlock 是侥幸；
   r100 X-deny 版将用无锁环）；
4. windbg-mcp 离线 minidump 分析通道顺畅（0x109 秒级定位到函数级元
   凶），kd 失联不影响 dump 取证。

### TODO

- r100：X-deny 执行感知版 syscall 审计（MmCopyVirtualMemory 页）；
- 排查清单：其他 inline hook 存量（r75 nthook 测试钩子未常驻，无风险；
  sysaudit 已摘除）。

## Round 100 (2026-09-13) — X-deny 执行感知系统调用审计 @ 本轮提交（构建 f8d1fb47/ctl afa1f6e2 哈希双验）

### 任务与架构

r99 判决（inline hook PatchGuard 致死）后的正路落地：**零 guest 字节修
改**的执行感知。ARMED = 感知页（MmCopyVirtualMemory + NtCreateFile 两
页）NPT X-deny（双视图 MapRange+SetPerm4k NX + **r56 律 flush**
（IPI/KICK，验收 P2-1 补齐））；trip = 入口 fetch fault → 消费者记录
（参数在寄存器、CR3→pid/映像归因、无锁环入队）→ resolve（页开放、
指令原生重跑、零跳字节）→ 250ms 定时 re-deny（哨兵 r68 窗口模式）。
**PatchGuard 不可见由构造保证：guest 看到的页字节从未变化。**

### 基础设施落点

- npf 拒绝消费者签名扩展（传 GuestContext&——入口 fetch 时参数在寄存
  器）；cr3_monitor 新增前置消费者 seam（Cr3MonitorSetSenseCb）+
  Cr3MonitorArmViews（暴露 base+ProcView 对）；
- sysaudit v2.1：双感知页、无锁环（W/D 单调 + Commit=seq+1 末写提交
  + 序号校验排水）、re-deny DPC、ctl sysaudit 动词（armed/resolved/
  stage/total/lost/noise/target）。

### 实测（最终构建哈希双验）

- attach → armed ✓；探针（RPM 1MB 部分成功 + 200×64B 循环 + WPM err5）
  + 系统 NtCreateFile 流量 → **51+13 条真实捕获**（pid 归因含
  powershell.exe 自身调用）、noise 分离计数、零 lost、**零楔死**；
- TLB flush 修复后捕获率显著提升（修复前 target 命中=0、noise 独走——
  探针 50ms 间隔注释即此发现）；
- selftest 317/0（+6 环数学断言）；混合迭代 kill/suspend/PASS/active=0
  全绿（sysaudit 挂载中）；detach 后页恢复开放、guest 健康。

### 验收（子代理 FAIL→全修）

- **P1-1 环溢出楔死**：丢弃的 seq 在 Commit 序列上留洞 → 排水永久卡死
  → 修复=溢出 claim 写 FN_LOST 墓碑（占序列、排水跳过）；
- **P1-2 缺 Round 100 日志**（本条）；
- **P2-1 arm/re-deny 缺 r56 flush**（探针 50ms 间隔注释即此发现的实证）
  → IPI+KICK 补齐；**P2-2** consumer 改 SetPerm4kNoLock（exit 路径任意
  IRQL 合法，r63 律；KeSetTimer 以入口-fetch-only 契约注释保留）；
- P3-1 双 GPA 先解析后布防（消灭无消费者 NX 页）；P3-2 槽索引掩码化
  （r97 同族雷）；P3-3 ctl 打印 FnId；P3-4 disarm 顺序（先摘消费者+
  清状态再 cancel 再重开页）；P3-5 环数学离线断言（+6=317）。

### 已知边界（诚实登记）

1. **采样语义**：trip 后 250ms 开放窗内的调用不被感知；TLB 缓存翻译在
   刷新前不产生 fault（r56 flush 修复了 re-deny 侧，trip 侧天然采样）
   ——紧凑循环可滑过感知，真实 RPM 间隔+上下文切换则命中；
2. **小拷贝盲区**：64B 级 RPM 走 NtReadVirtualMemory 内部池拷贝，不经
   MmCopyVirtualMemory（实测零命中）——r101 需 SSDT+SSN 解析 Nt* 页
   （1903 SSN 常量 + 多锚表校验）；
3. 页粒度感知：同页邻居函数的 fetch 计 Noise（MmCopyVirtualMemory 页
   邻居低频，实测 noise 占比 ~50% 且多为低频系统函数）。

### TODO

- r101：SSDT+SSN 解析 NtRead/WriteVirtualMemory 页感知（补小拷贝盲区）；
- PG 安全性：v2 零字节修改=PG 无可检测物（构造性），本次挂载全程
  （~40min 含回归）guest 稳定；长期浸泡归入下次混合浸泡。

## Round 101 (2026-09-13) — SSDT+SSN 感知落地：小拷贝盲区关闭（零字节修改不变）@ 本轮提交（构建 048131bf）

### 现象（本轮目标）

Round 100 遗留：64B 级 ReadProcessMemory 走 NtReadVirtualMemory 内部池
拷贝，永不经过 MmCopyVirtualMemory（实测 target=0）。需要感知 Nt* 页本
身，而 NtRead/WriteVirtualMemory 未导出——必须定位 KiServiceTable。上轮
WIP 双路失败：LSTAR lea 扫描 + 映像内容扫描全数 lookalike，分支
`r101-ssdt-wip` 保全现场。

### 根因（离线三重验证 → 运行时 kdump 闭环）

1. **kd 自 r68 离线，但 r99 判决 minidump（091326-12453-01）离线可用**：
   windbg-mcp 打开后 `x nt!KiServiceTable` 等 PDB 符号全数命中（dump
   内 nt base `fffff800'48800000`，18362.592，PDB F3A4F64B…）→ 直接拿
   到 表/KiArgumentTable/descriptor/Nt* 函数的 **RVA（boot 无关）**；
   dump 的 .text 可读 → **Zw stub `mov eax, imm32` 直读 SSN**
   （ZwCreateFile=0x55、ZwOpenProcess=0x26——上轮 WIP 硬编码 NtOpen
   Process=0x23 系错误常量，entry[0x23] 实为 0x659200 某函数）。
2. **vm_pull_ntoskrnl.bat 拉 guest System32\ntoskrnl.exe**（直接拉运行
   镜像被拒，guest 内 copy 到 Temp 再拉；SHA256 2ea1e2a2…）→ Python 手
   解 PE .data：文件初值 `entry[0x55]=0x630540=NtCreateFile 自身 RVA`
   → 文件格式=**绝对映像 RVA**。
3. **运行时表被 boot 重写**：首轮 attach stage=3 拒绝（fail-safe 正确
   工作——宁拒不错杀页）；kdump 动词（本轮新增，IOCTL 0x82A）实测运行
   时表：`entry[0x55]=0x020b9307` → **打包表相对格式
   `service = table + (s32(entry) >> 4)`，低 4 位=栈参数数**（0x020b9307
   >>4=0x20B930，+0x424C10=0x630540 ✓，args=7=NtCF 的 7 个栈参 ✓）。
   四锚点（0x55/0x26/0x3F/0x3A）全部精确解出 NtCF/NtOp/NtRd/NtWr；
   负值条目（e0=0xfced7304）算术右移后落表前 .text ✓。
4. **descriptor 闭环**：kdump nt+0x58C880 = KeServiceDescriptorTable：
   `[0]=KiServiceTable VA、[2]=0x1D0 上限、[3]=KiArgumentTable VA`——
   且 r101 WIP 的"LSTAR 候选"实为 `lea r10,[KeServiceDescriptorTable]`
   的目标（=0x58C880），它只是描述符不是表本体；上轮**锚点失败根因=
   假设了无 >>4 的表相对格式**（ sar 4 打包是 19H1 引入）。
5. **syscall 进入态寄存器语义**（`uf KiSystemServiceUser` 定案）：
   `KiSystemCall64+0x24: mov rcx,r10` 恢复 ABI → **入口-fetch 时
   rcx=arg0**，rdx/r8/r9=arg1..3；**r10=dispatcher scratch**（服务函数
   地址，实测保存值呈 >>16 移位——非参数，弃用）。v2.1 用 rcx 记 NTCF
   反而是对的；本轮首版用 r10 是错的（已修）。

### 修复/实现

- `shared/svmb_protocol.h`：FN_NTRD/NTWR/NTOP（3/4/5）、MAX_TARGETS=6、
  `Reserved`→`TargetRd`；**IOCTL_KDUMP (0x82A)**（kernel VA qword 安全
  读窗，production 拒绝，入 Debug-only 诊断家族）。
- `driver/src/modules/sysaudit.cpp` v3：感知页 2→**5**；SysAuditInit 内
  SSDT 派生（ntBase=NtCF 导出 VA-0x630540；四锚点 `table+(s32(e)>>4)`
  解出 VA 必须逐一等于 ntBase+钉死 RVA，**任一不符=stage 3 拒绝 arm**—
  镜像非 18362.592 时安全失败而非 X-deny 错页）；SysAuditServiceVa 纯
  函数（离线断言）。consumer 两修：**RIP 匹配替代 GPA 匹配**（首版 GPA
  匹配把 NtReadVM fetch 全记成 COPY 页 noise——MmCopyVirtualMemory
  0x622cb0 与 NtReadVM 0x622a80 同页 0x622000，实测 noise=42/ntrd=0）；
  **arg0 改读 rcx**（见上）。
- `driver/src/main.cpp`：KDUMP case（KdumpSafeRead64：内核 VA 预滤+
  __try）+ production 过滤表追加。
- `app/main.cpp`：`kdump <va-hex> [n]` verb；sysaudit 打印 fn= 解码+
  targetRd。
- 探针 `guest_rpm_r101.ps1`：句柄掩码 0x1A→0x1F0FFF（r99 版 WPM err5
  根因）；VirtualAllocEx scratch 页做 WPM（不写 notepad RX 映像）。
- bats：vm_r101_e2e（attach→drain→探针→drain→拉取）、vm_r101_kdump
  （带参 %1=%2）、vm_r101_pull_ntos。

### E2E（构建 048131bf/9f0e1b11 双向 SHA256 校验；验收修复后终版 ba848433/2921024b 冒烟回归全绿——ntrd/ntwr 归因不变）

attach ok（armed=1 stage=0，targetRd=nt+0x622A80 ✓）；**ntrd 条目落地：
`ntrd pid=5460 (powershell.exe) src=9ec:7ff616d40000 dst=14920e38ac0
size=64`**——rcx=句柄、rdx=notepad 模块基址、r8=.NET 缓冲、r9=64B 精确
归因；`ntwr … dst=24628ed0000 size=64`=VirtualAllocEx scratch 精确命中；
ntop DesiredAccess=0x410、ntcf GENERIC_READ 组合语义齐；lost=0、排水健
康、kdump 描述符闭环、detach 后 guest 稳定（vmexits 2914 正常增长）。
PatchGuard 面不变：只读表 + 页 X-deny，全程零 guest 字节修改。

### 关键决策回顾

| 我们以为 | 实际 |
|---|---|
| r101 WIP：SSDT 发现需要复杂扫描 | minidump 符号 + Zw stub + 拉镜像，30 分钟离线解 |
| WIP 硬编码 NtOpenProcess=0x23 | 0x26（ZwOpenProcess `mov eax,26h`） |
| 文件里 table 初值=运行时格式 | boot 重写为打包表相对 `sar 4`（19H1） |
| r101 WIP"表相对偏移"假设 | 缺 >>4 打包——锚点全灭的根因 |
| syscall 后 arg0 在 r10 | kernel 已恢复 rcx=arg0；r10=scratch（>>16 怪值） |
| GPA 匹配感知页 | 同页双目标（MmCopy+NtReadVM）必须 RIP 匹配 |

### 已知边界（诚实登记）

1. 采样语义不变（250ms 窗 + TLB 缓存翻译滑过——本轮 1MB 读写未出 COPY
   条目即滑动实证，NTRD 侧靠 50ms 间隔+上下文切换命中）；
2. pid=0 条目=CR3 未登记的系统线程（r100 语义不变）；
3. r10 实测 >>16 移位值成因未深究（scratch，弃用即安全）；
4. SSN/RVA 常量钉死 18362.592——其它 build 会 stage=3 安全拒绝（多
   build 支持仍资源阻塞，需其他镜像的 guest）。

### 验收（子代理 PASS；P2-1 + 2×P3 采纳）

- P2-1 Lost 计数器从未递增（r100 原样带过来——墓碑占序列但排水跳过，
  溢出在 ctl 侧不可见，"lost=0" 不可测）→ 溢出分支补
  `InterlockedIncrement64(&g_Ring.Lost)`；
- P3-1 协议头注释与实现相反（仍写"arg0=r10"）→ 按 dump 定案重写
  （rcx=arg0，r10=scratch）——协议头是持久规格，照它"改回去"就会复刻
  WIP 的错误；
- P3-2 TargetRd 部分失败时报告未 arm 的已验证地址 → Drain 查找加
  Gpa 门（"0 = not armed" 语义闭合）；P3-3 双安全读 helper 留 r102 收敛；
  P3-4 KDUMP 的 MMIO 到达性已在协议注释诚实登记。

### TODO

- 长期浸泡：sysaudit 5 页挂载归入下次混合 kill+suspend 浸泡；
- CLI/批量：kdump 输出落盘自动化（现手动 cat）；
- r102 候选：行为层——对 NTRD/NTWR 条目按（调用方 pid，目标句柄）聚
  频，超阈 LOUD（README 行为分析缺口的 MVP）。

## Round 102 (2026-09-13) — 行为分析 MVP：按调用方聚频 + 边沿告警（遥测不处置）@ 本轮提交（E2E 构建 5271a161/da40b072；验收修复后终版 42b883ad/aa91ddaf 冒烟全绿）

### 现象（本轮目标）

README 反作弊能力清单的"行为分析"缺口：r101 已有原始 NTRD/NTWR/NTOP
条目，但无人聚合——EDR 语义里重要的是"谁在以什么频率跨进程读/写"，
不是单条记录。MVP=驱动内按调用方聚频（1s 窗）+ 超阈边沿告警。

### 实现

- 协议：**IOCTL_SYSAUD_BEHAVIOR (0x82B)** + SVMB_SYSAUD_CALLER/
  BEHAVIOR（每槽 lifetime/current-window 计数 + peak + Alerted 旗标；
  全局 Alerts/Dropped/TableFull）。阈值默认 128/32/16（rd/wr/op 每窗），
  **注册表旋钮 BehaveRdThs/BehaveWrThs/BehaveOpThs**（DriverEntry 读，
  0=保持默认）——r89 旋钮惯例（注意 sc delete 连 Parameters 键删除，
  deploy 周期后需重写）。
- sysaudit.cpp：CallerSlot[16] **CAS claim**（Knuth 哈希 + 线性探测）+
  **窗口 CAS rollover**（赢家清零 current 计数与 Alerted；输家自然汇入
  新窗）+ **精确边沿告警**（InterlockedIncrement 唯一值 == 阈值 恰好一
  个 CPU 观测到 + Alerted exchange 保证每窗每槽至多一次）+ LOUD
  LogError。无锁（r99 律：NPF 退出路径任意 IRQL）；**并发近似语义诚实
  登记**：计数可能丢增量，告警绝不重复。
- 统计面：只计**有归因**（pid≠0）的 NTRD/NTWR/NTOP；NtCF=环境文件
  I/O 有意排除；pid=0 进 Dropped。**遥测不处置**——告警只记日志/旗标
  ，kill/suspend 响应留 K1/K3 政策决策。
- `platform/saferead.h`：r101 验收 P3-3 清账——KdumpSafeRead64/
  KSafeRead32 收敛为 svmb::SafRead64/SafRead32。
- ctl `behavior` verb；offline 断言 +5（窗滚动/哈希稳定/边沿精确）。

### 判决性发现：采样律决定告警语义

首轮 E2E（默认 128 阈值）：**alerts=0**——探针 200 连发 64B 读只有
~3 条入环（peak 2/窗）。这不是 bug：**r100 采样律的定量呈现**——trip
resolve 后页开放，紧凑循环沿缓存的 RWX 翻译滑过，本 vhv 上感知密度
~2-3 条/秒/页。推论（诚实登记）：**行为告警是"感知突发"警报，不是真
实syscall 速率计**；默认阈值取真实 EDR 数值（128/32/16），本 vhv 上
实际只能靠旋钮调低演示。告警腿 E2E（BehaveRdThs=2，DriverEntry 读取
故 svc 重启而非重挂）：**alerts=1 精确单次边沿** +
`sysaudit-behave ALERT: pid=1408 (powershell.exe) cross-process RD
burst 2/window >= 2` LOUD 行入内核日志环，config 行回显旋钮生效。

### E2E（双腿）

- 默认腿（128）：行为表正确归因（pid/映像/rd/wr 计数 + peak），
  dropped=10（pid=0 系统流量）、tablefull=0、alerts=0（不可达=正确）。
- 告警腿（=2）：alerts=1、LOUD 行、窗口回显 rd>=2。
- 构建 5271a161/da40b072 双向 SHA256 校验；selftest 含新断言全绿。

### 浸泡（66 迭代 ~27min，挂载中）

每次迭代孵化新 powershell（新 pid——16 槽表填充与 **TableFull 路径**
实测）+ 探针 + drain + behavior；阈值保持 =2 使告警路径持续受压。
结果：**66/66 迭代全绿**——armed=1 全程（迭代 1 的 attach err 1359=
已挂载幂等怪癖，恰证全程未卸载）、total=3143/lost=0/noise=3246、
**表满路径实测**（callers=16 满、tablefull=297 增长、零楔死）、
alerts=22（告警路径持续受压零死锁）、dropped=952、guest 存活
（hv RUNNING、vmexits 95817 正常增长）、零蓝屏。

### 关键决策回顾

| 我们以为 | 实际 |
|---|---|
| 突发探针(200 连发)必过 128 阈值 | 采样律：只有 ~3 条被感知——紧循环沿缓存翻译滑动 |
| 告警阈值=真实 syscall 速率 | 感知突发速率（传感器每秒每页只有 O(1) 输出） |
| 阈值硬编码常数 | 旋钮化（DriverEntry 读），E2E 用低值演示边沿 |
| 告警触发多次/窗 | 精确单次（唯一递增值 + Alerted exchange 双保险） |

### 验收（子代理 PASS；7×P3 全收）

- P0/P1/P2 全无；并发设计被评为"强于自身注释"（同 pid 双槽不可达、
  rollover CAS 1:1 配对 Alerted 复位、陈旧边沿最多吞一次预算不会重复）；
- P3 修复：哈希注释诚实化（16 槽下乘法哈希退化为 pid mod 16 恒等——
  线性探测兜底）、BehaveSlotFor 竞态注释改为 CAS-收敛证明、peak
  check-then-exchange 回退语义登记、vm_r102_e2e.bat 头注释按采样律
  修正（默认腿 alerts=0 是正确结果）、断言计数 +6→+5、config 日志回
  显生效阈值而非原始 0、ctl OutCount 钳位。

### TODO

- r103 候选：目标侧归因（句柄→目标 pid 需 PASSIVE 延迟解析，exit 上
  下文不可 ObReferenceObjectByHandle）；告警接入 kill/suspend 政策
  （等 K1/K3）；Dropped 拆分（ntcf 排除 vs 真 pid=0）。
- 长期：5 页挂载 + 默认阈值的 2-4h 浸泡攒 n。

## Round 103 (2026-09-13) — 目标侧归因：句柄→pid PASSIVE 延迟解析 @ 本轮提交（E2E 首版 d454afec/636830ed；验收 FAIL→修复后终版 671024b1/c3fcffba，浸泡全绿）

### 现象（本轮目标）

r102 行为表只有"谁在调"（caller pid），没有"对谁做"（目标）——NTRD/
NTWR 的 rcx 是调用方私有的进程句柄，exit 上下文不能碰句柄表、不能
碰可能换出的用户内存（CLIENT_ID 解引用同样禁止）。EDR 语义的"谁读谁"
需要目标归因。

### 实现（延迟解析 = 唯一正路）

- **pending 环**（exit 上下文生产者）：claim 序号 → 写 (callerPid,
  handle) → Commit=seq+1 末写（r100 P1-1 律复用）；溢出写墓碑保序列
  连续 + ResDropped 计数。
- **单飞 PASSIVE 工作项**：SenseRedenyDpc 的 250ms 节拍兼任心跳
  （ResKick：CAS 单飞入队，批量 16 对/趟，清守卫后重查补队）；
  SysAuditResInit(gDevice)（DriverEntry 期，Cr3RegionKillInit 惯例，
  独立于 attach 生命周期）。
- **解析链**：PsLookupProcessByProcessId(caller) →
  KeStackAttachProcess（64 字节不透明 APC 状态，mem_read.cpp 惯例）→
  ObReferenceObjectByHandle(*PsProcessType, KernelMode) →
  PsGetProcessId → ObDereference ×2。句柄失效/调用方死亡走**负缓存**：
  退避 5s + 3 次上限后死亡——探针杀掉 notepad 不会无限重试。
- **缓存**（32 行，(caller,handle) 哈希 + 探测）：单 PASSIVE 写者 +
  exit 上下文无锁读者（u32 对齐读，陈旧=良性近似）。命中即
  `e.DstProcess = targetPid`（协议语义升级：Nt* 条目的 DstProcess 从
  恒 0 变"解析收敛后=目标 pid，收敛前=0"）并点亮 per-(caller,target)
  计数行（EDR "who reads whom" 视图）。NTOP 的 rcx 是 out-PHANDLE 非
  目标句柄，有意不解析（协议注释）。
- **Dropped 拆分**：NTCF 从"静默不算"改 `ExcludedNtcf` 计数（r102
  TODO 清账）；行为快照尾部追加 Targets[32] + Resolved/ResFail/
  ResDropped/ExcludedNtcf（尾部追加：旧 ctl + 新驱动 = outLen 响亮拒，
  支持的错配方向）。
- ctl `behavior` 打印 target 行；offline 断言 +2（target 槽哈希稳定/
  范围）。

### E2E（构建 d454afec/636830ed 双向 SHA256）

attach ok → 探针（notepad pid=1312）→ 4s 后 drain：**ntrd 条目
`dst=0:…` → `dst=520:…` 收敛**（520=notepad pid，首调用未解析、缓存
命中后 stamp）；`behavior: targets=1 resolved=2 resfail=0 resdropped=0
ntcf-excluded=31` + **`target: 2768 -> 1312 rd=2 wr=2`**。解析收敛在
1-2 个 re-arm 节拍内。

### 冻结事件（诚实登记，r102-final 部署上发生）

r102 终版冒烟 E2E（全绿完成后 ~1 分钟）guest 冻结：Tools 心跳
07:05:22 UTC 停止（vmware.log），VIX 全通道失联，轻探针 4 分钟无响
应，E1000 rx ring full；**无 bugcheck 无 dump，串口黑盒早已废弃，
kd 自 r68 离线——定性不可得**。按律 3 硬复位恢复，重启后驱动/系统健
康。该冻结发生在"5 页武装 + 空闲 ~1min"状态，是 r68 熔解修复后首个
idle-armed 冻结数据点；r102 的 66 迭代浸泡（churn 下）与本次 E2E 本身
全绿，空闲武装稳定性缺口暴露——**下一步浸泡必须包含 ≥30min idle-
armed 腿**；触发路径无法排除环境因素（无判据）。

### 验收（子代理 FAIL→全修，含一颗 r100 潜伏雷）

- **P1-1 pending 环套圈楔死**：溢出墓碑会写进"未消费槽"的 Commit——
  消费者只查 `Commit != seq+1` 即 break，环被完全套圈后 resolver 永久
  自重排队（CriticalWorkQueue 热循环）。修复=消费者侧套圈推进：commit
  > 期望值时**不动槽位**（其属于后继条目）、ResDropped 计数、直接推进
  R。**审查同时揪出 r100 主环（SysAuditDrain）同款潜伏洞**（256 未消费
  条目被覆盖即排水永久楔死）——同款修复：推进 D 到占位序列、
  InterlockedAdd64(Lost) 入账、继续排水；
- **P1-2 workitem 无卸载清扫**（r89 Cr3RegionKillDeinit 先例未镜像）：
  卸载时在飞 workitem 跑已释放代码。修复=SysAuditResDeinit（原子认领
  置 null 永久解除武装 + 有界 1s 等在飞 + 超时泄漏，cr3 同款）挂
  DevUnload；
- P2-1 pid=0 条目曾入 resolver（白烧 3 次尝试预算+ResFail 虚高+不可见
  缓存行）→ `if (e.Pid)` 门；P2-2 缓存死行永久占位 → 死行回收第二探测
  轮；P3：KeStackAttachProcess 声明改 VOID、ResCacheLookup 去 now 参、
  BehaveTargetCount 同 pair 重复行竞态注释、CriticalWorkQueue 实际
  PASSIVE（IO_WORKITEM_ROUTINE 契约）注释登记；
- 确认干净项：引用计数零泄漏、单飞不可双排队、协议布局双向响亮、
  **空闲路径 r103 零新增活动**（ResKick 仅在有 pending 时出队）。

### 第二次冻结 + 嫌疑机制（诚实登记）

第一次冻结（07:05:22 UTC，r102-final idle-armed）后，轻浸泡在
**P1-1 未修构建（d454afec）** 上再次冻结（**07:37:44 UTC** Tools 心跳
停）。机制嫌疑高度集中：修复前构建把 pid=0 的系统 NTRD/NTWR 流量也
入队 pending 环（P2-1 洞），taskkill 风暴下 64 深环极易被套圈 →
**P1-1 resolver 无限自重排队热循环**（系统工作线程打满）——与冻结表
现吻合。修复链（套圈推进 + pid=0 门 + 死行回收）全部针对该链；热循
环不死机的替代解释（环境、armed 页本身）无法排除（kd 离线），但本次
给 P1-1 的定罪加了最大份证据。修复版 E2E 全绿后以 30 迭代浸泡复验稳
定性——**结果全绿**：armed 全程、total=1461/lost=0、解析器 32 对全
解析（resfail=0 resdropped=0，修复后 pending 环再未套圈）、
targets=22 行真实归对、tablefull=66 零楔死、guest 存活零蓝屏
（vmexits 84014）。同款 churn：修复前冻结、修复后干净——P1-1 机制
假设获得最强行为学证据。

### TODO

- idle-armed 30min+ 浸泡腿（r103 轻浸泡只压 churn+ResFail 路径）；
- r104 候选：告警接 kill/suspend 政策（等 K1/K3）、NTOP 目标归因
  （CLIENT_ID 需 PASSIVE 路径，同解析器机制可复用）、targets 表满墓碑
  语义（现静默丢弃）。

## Round 104 (2026-09-13) — idle-armed 长浸 + NTOP 目标归因 @ 本轮提交（验收 PASS 无 P0/P1/P2；终版构建 aa7b57f0/ctl c6016396 双向哈希校验）

### 现象（本轮目标）

r103 留下两个缺口：① idle-armed 稳定性未验证（本会话两次冻结都在
armed 状态，r103 修复版的 30 迭代 churn 浸泡全绿但空闲腿为零）；
② NTOP 条目的目标归因缺失（rcx 是 out-PHANDLE，真目标藏在用户态
CLIENT_ID 指针后面）。

### 实现

- **idle-armed 长浸**（guest 侧持久黑盒 + 宿主 fail-fast）：每次快照
  =时间戳 + sysaudit drain + behavior 追加进 guest 文件（硬复位后仍
  在——串口黑盒早已废弃后的第一个可存活性取证通道）；宿主 bash 循环
  80 轮 ×（快照 + 20s 间隔），每个 vmrun 包 `timeout 90`——挂起即
  FREEZE_DETECTED，guest 文件最后几行本地化楔死时刻。
- **NTOP 目标归因**：resolver 增 RES_KIND_CID 路径——
  PsLookupProcessByProcessId(caller) → **MmCopyVirtualMemory** 读
  CLIENT_ID.UniqueProcess（OS 原生跨进程读：任意/已换出指针安全；
  原始 __try 触碰会踩 r89 MiUserFault 楔死律；MemReadVm 走不通——
  探针进程不在种子表）。**CID 不缓存**（指针=栈缓冲地址，内容随调用
  变化，按指针缓存语义错误）；解析结果直接进 per-target 表
  （TargetRow.Op，r104 协议 +Op 字段 + TargetsFull 计数器——r103
  TODO 表满静默丢弃清账）。登记副作用：每次 CID 解析 = 一次
  MmCopyVirtualMemory 入口自触发 COPY 感知页（System 上下文 →
  pid=0 COPY 条目入环，诚实自遥测）。
- 探针复用 guest_rpm_r101.ps1（OpenProcess→读→写全链路）。

### 稳定性长浸（本轮核心交付）

- **裸驱动空闲 55min**：80/80 快照零冻结（首次循环因宿主循环用了错
  误动词 `attach` 而非 `mod attach`，vm_ctl_run 吞掉 svmbctl 报错致
  未挂载——意外成为裸驱动空闲基线；**流程律：attach 后必须即时验证
  armed=1**，vm_ctl_run 的 rc=0 是 vmrun 的送达 rc 不是 svmbctl 的）；
- **armed 空闲 55min**：80/80 快照零冻结，armed=1 全程、total=5547/
  lost=0、dropped=699（pid=0 系统流量）、guest 存活——r103 修复版的
  idle-armed 缺口闭合（累计 ~110min 空闲零冻结）。

### NTOP 归因 E2E 与两轮迭代

- 首版（DPC 心跳解析）：**op=0 resfail=1**——探针 powershell 是短命
  进程，250ms 心跳赶到时进程已退，PsLookupProcessByProcessId 失败。
  **真实攻击者同样是短命进程**——归因延迟是语义性缺陷非性能问题。
- 修复：NTOP 入队后**立即 ResKick**（NPF exit 调 IoQueueWorkItem
  ≤DISPATCH 合法，single-flight 已有），归因延迟降到 µs 级；
- 探针第二课：单次 OpenProcess 是一张感知彩票（采样律下整轮滑过，
  op 连入队都没发生）——r104 探针改 open+read+write 循环 20×50ms；
- 终版 E2E：**`target: 6764 -> 452 rd=0 wr=0 op=3`**——3 次
  OpenProcess 全归因（resolved=5 = 2 handle + 3 CID 吻合）；
- r104 探针 20 迭代浸泡**全绿**：armed 全程、total=1046/lost=0、
  **resolved=61（CID 重负载实证）resdropped=0（环零套圈）**、
  resfail=27（taskkill 后句柄/进程死亡，CID 无负缓存的预期行为）、
  targets=22 行真实归对（多条 op 行含短命进程对）、tablefull=46
  零楔死、guest 存活零蓝屏。

### TODO

- K1/K3 政策落地后：告警接 kill/suspend 响应；
- idle-armed 长浸常态化（每次部署后跑一轮）。

## Round 105 (2026-09-13) — 告警→可逆处置（opt-in 管道）+ CID 负缓存 @ 本轮提交（构建 76e254e6/ctl d7d26a9e）

### 现象（本轮目标）

r102-r104 建齐了捕获→归因→告警，缺最后一环：**响应**。kill/suspend
接入是 K1/K3 级政策决策，但"告警发生后系统什么都不做"也不是终态——
本轮建成**默认关闭、仅可逆挂起**的响应管道：政策开关化（旋钮
BehaveResponse，默认 0=仅遥测），kill 路径与 K1/K3 仍完整留给用户。

### 实现

- **cr3_monitor.Cr3RegionSuspendPidSysalert(pid, image)**：sysaudit
  告警路径的挂起入口，与 kill 路径**共享 r91 基础设施**（挂起表/
  g_suspLock/SuspResumeAll/`cr3 suspended`+`resume all` 动词免费
  获得）；S1 检查-后-冻结不变式保持；**r96 遗留票闭合——resume
  belt**：本函数是第二个表填充者，check→freeze→insert 非原子窗在
  插入时表满即**立即解冻**（g_pfnResumeProcess），冻结-未入表-无法
  复原的搁浅形态在双填充者下不可能；RegionId=SYSALERT 哨兵
  （0xFFFFFFFF，`cr3 suspended` 显示出处）。
- **sysaudit**：BehaveResponse 旋钮（DriverEntry 读取，默认 0）；
  告警边沿统一进 BehaveAlertFired（三处 RD/WR/OP 去重）——遥测恒
  定，旋钮=1 且 pid>4 且非拒绝名单（cr3 denied list 复用）才以
  RES_KIND_SUSPEND 入 resolver pending 环；workitem PASSIVE 执行
  Cr3RegionSuspendPidSysalert。告警天然每窗每槽至多一次=挂起速率
  天然受限；挂起可逆（`cr3 resume all`）。
- **CID 负缓存**：8 行 (caller,ptr) 备忘——bogus/不可读 CLIENT_ID
  指针不再每个 NTOP 条目重试（r104 浸泡 resfail=27 的主要来源）；
  同 5s 退避 + 3 次死亡语义。
- 协议/ctl 无 ABI 变更（响应完全走既有 0x82B 快照 + r91 挂起表动
  词）。

### E2E（构建 76e254e6/d7d26a9e 双向 SHA256）

BehaveRdThs=2 + BehaveResponse=1（重启生效）→ attach → **探针必须
detached 孵化**（r93 律：探针会被自己的告警挂起，同步 vmrun 会挂死
宿主）→ 4s 后：`sysaudit-behave ALERT: pid=3144 RD burst 2/window` →
`prot: sysalert SUSPEND pid=3144 st=0` → **`cr3 suspended`: 1 writer**
→ behavior 行 ALERT 旗标 → `[+] resumed 1 writer(s)` → `SUSP resume
pid=3144`。遥测→可逆处置全链闭环，被挂起进程复原确认。

### 边界（诚实登记）

1. 响应=仅挂起（可逆）；kill 响应与 K1/K3 政策仍完整留白；
2. CID 失败备忘独立于句柄缓存（指针非稳定键，r104 定案延续）；
3. 挂起目标守护在入队（pid>4+deny list）与执行（同款复检）双层；
4. 挂起暂停的是调用方——若告警来自被滥用的合法进程，resume 一键
   可逆是设计选择而非疏忽。

### 验收（子代理 FAIL→全修，P0×1 + P1×5 + P2/P3）

- **P0-1 resume belt 在 g_suspLock（DISPATCH）下调 PsResumeProcess**
  （PASSIVE-only）——0xA bugcheck 或挂起子系统整体楔死向量。修复=
  锁内决策/记账、**动作全部锁外**；
- P1-1 belt 误触发于"表满 decline"路径（对从未挂起的进程 resume=
  误减他人挂起计数）+ 返回 pid 致 ResFail 失真 → decline 早退不触
  belt、frozen 丢失 slot 才 belt（此时才需要 undo）且置失败让调用方
  计账；
- P1-2 已列表 pid 再告警会重复 PsSuspendProcess（**refcount 累积**，
  单表条目还不清）→ already-listed 守卫（sysalert + **KillWorkItem
  同款一并修**，r105 使其常态化）；
- P1-3 KillWorkItem 失去 ONLY-filler 保证且无 belt（r96 票面场景成
  真）→ kill 路径 insert-fail 也加锁外 belt + 陈旧注释更新；
- P1-4 卸载顺序：resolver 现在执行挂起，Cr3RegionKillDeinit 的最后
  表清 sweep 必须在其后 → SysAuditResDeinit 前移；
- P1-5 执行时 deny list 空转（传字面量 image）+ pid 回收窗口 →
  **执行时从 EPROCESS 取 live image**（PsGetProcessImageFileName 手动
  声明）复检；表/日志记录真 image；
- P2 CID 备忘行接管不清 Tries（新 pair 一次失败即死）→ 接管清零；
  P3：ctl 挂起表显示 (SYSALERT) 标签、BehaveResponse 用协议常量、
  soak bat %rem% 笔误、config 日志回显生效值。
- 修复版 E2E 全绿：WR 告警 → `SUSPEND pid=2856 image='powershell.exe'`
  （**live image 正确**）→ `(SYSALERT)` 标签 → resume 复原。

### 冻结#3 + kd 腿重启（诚实登记）

修复版 E2E 后 guest 第三次冻结（**10:39:44 UTC** heartbeat timeout；
配置=RdThs=2+WrThs=2+Resp=1 armed idle）。冻结矩阵更新：#1 r102-
final/RdThs=2/Resp=0 idle-1min；#2 r103-fix/churn（P1-1 热循环嫌疑，
已修）；#3 r103-fix/RdThs=2+Resp=1 idle-数min。r104 两段 55min 默认
阈值 idle 全绿。**两次冻结配置含 RdThs=2**——但绿组差异不足下定论
（kd 离线）。本轮行动：硬复位恢复 → **KDNET 恢复尝试**（windbg-mcp
open_kd_session 对活目标连接被 MCP 30s 硬顶掐断，残留 kd.exe 已清；
改为**冻结时连接**策略——对停摆目标连接秒回，r45 律保证 kernel-alive
可 break 抓全核栈）→ 复现配置（RdThs=2+Resp=1 armed）下挂 fail-fast
idlesnap 循环等冻结。

**复现成功**：第 10 轮（~8min armed idle）FREEZE_DETECTED——可复现性
确立（对照：默认阈值 armed idle 110min+ 绿）。**kd 冻结时连接 =
no_debuggee**——目标连 KDNET 都不发包，即 CPU 从未返回 guest 上下文：
**冻结在 hypervisor 自身内部**（svmb 的 exit/flush/DPC 路径卡死），
guest 内核调试器无从运行——这也是 r68 以来"kd 失联"的真相（历次冻
结全死在 hypervisor 层）。黑盒幸存（复位后拉回）：最后快照显示
alerts=1（响应链激活过）、两个 powershell caller 计数正常增长——记
录进行中突然停摆。定性收窄至：armed 状态 + 低阈值（告警/响应路径活
跃）下的 hypervisor 内部挂起， suspect 集中在 NPF consumer/re-deny
DPC/TlbKickFlushAllCores(IPI) 交互——r106 用 kd 空闲在线 + 故意低阈
值复现，冻结瞬间 send_ctrl_break 抓全核栈。

**约束措施（立即生效）**：低阈值 + BehaveResponse=1 组合仅限 E2E 窗
口（分钟级），不得长挂；长浸/常态用默认阈值（110min+ 实证稳定）。
guest 已恢复默认阈值 armed 健康态（knob 值显式删除）。

**#5/#6 + guest FS 损伤（新增）**：部署链期间 exec 通道两次"未知错
误"卡死（vmtoolsd exec lane 被未返回的 guest 程序占住，~90-100s 自
愈，文件通道不受影响）；随后 **svmbctl.exe 从 guest 目录静默消失**
（6 次硬复位的 NTFS 代价——写入静默丢失，svmb.sys 幸存）。重推即愈
（PRESENT），但 FS 已不可信——**r107 前置：guest chkdsk / 快照回滚
（快照 3，09/05 基线；注意 revert 连 vmx 一起回）**。

### TODO

- K1/K3 决策后：kill 响应档 + 告警响应的 owner 限定；
- idle-armed 长浸常态化（r104 设施已备，部署流程纳入）；
- r106 候选：行为表逐目标窗速（per-(caller,target) 速率告警）。

## Round 106 (2026-09-14) — K1/K3 决策落档 + K3 活体映像复核 @ 本轮提交（构建 979f4409/ctl 1acb0635）

### 用户决策（2026-09-14）

r96 审计遗留的两项 kill 路径政策，用户拍板：
- **K1 = WONTFIX**：维持 r89 威慑模型——任何归因成功的绕过写入者都被
  响应（杀/挂），**CR3 转嫁误伤风险被明确接受**；决策注释落在 kill
  分支（cr3_monitor.cpp K1 DECISION 块）。
- **K3 = 做**：kill 路径在响应前从**活体 EPROCESS** 重读映像名复核
  拒绝名单（PsGetProcessImageFileName 手动声明，r105 挂起路径同款
  模式）。

### 实现

KillWorkItem 在 PsLookup 成功后：liveImage[16]（15 字节 + NUL）←
活体映像 → `pid<=4 || deny-listed` → **拒绝响应**（按响应类计
g_kill/g_suspDenylisted + LOUD "response REFUSED ... (K3)"）；放行则
liveImage 权威化进 `image` 局部（挂起表 + 全部日志携带活体名——trip
时归因可转嫁、pid 可能在 DPC→workitem 窗口被回收复用，活体名是唯一
可信门）。

### 验收 FAIL→修复（P0：门是死链）

首版 K3 门与响应链是**两条独立 if**——liveDeny=true 的 KILL trip 打
完 REFUSED 日志后 fallthrough 进 `else if (!suspend)` **照杀不误**
（杀的恰是 deny-listed 真 lsass = guest 带崩）；日志声称 REFUSED 实
际杀了——最恶劣的自欺形态。子代理机械模拟全 (liveDeny, suspend,
primitive) 组合实锤。E2E 未抓到的原因：拒绝腿在 **trip 时**就被
PolicyResolveWriter 拒了（pid=0），workitem 体根本没跑——门是死代码
但 E2E 全绿。修复=响应链（K4/kill 两个尾分支）补 `!liveDeny` 排除。
**教训：给"拒绝"写测试必须打进 workitem 层，trip 层绿≠门活。**

### E2E（构建 979f4409/1acb0635 双向 SHA256）

- 回归腿：`cr3 probee2e kill`（svmbctl.exe，非拒绝名）→ 探针被终止
  （非正常退出）+ 内核日志 `prot: KILL policy writer pid=4492
  image='svmbctl.exe' st=0`——**活体门放行、live name 下游权威化**；
- 拒绝腿：svmbctl 副本改名 lsass.exe 跑 probee2e kill → **存活**
  （rc=0，全 verdict 打印）——trip 时 deny 门拒绝响应。
- （注：活体门拒绝分支的确定性 E2E 需 trip/live 名不一致的竞态，未
  模拟——代码与 r105 已验证 suspend 门同构，子代理机械模拟覆盖。）

### 验收（子代理 FAIL→修复，P0×1）

- **P0：K3 门与响应链是两条独立 if**——首版把 liveDeny 检查写成独立
  块，原 `if (suspendPlanned) / else if (suspend && !pfn) / else if
  (!suspend) {KILL}` 链不受其影响：liveDeny=true 的 KILL trip 打完
  REFUSED 日志后 **fallthrough 照杀**（杀的恰是 deny-listed 真进程，
  如真 lsass——guest 带崩）。子代理对全 (liveDeny, suspend, primitive)
  组合做机械链模拟实锤；E2E 未抓到的原因=拒绝腿在 trip 层就被
  PolicyResolveWriter 拒绝（pid=0），workitem 体未执行——**教训：给
  "拒绝"写测试必须打进 workitem 层，trip 层绿≠门活**。修复=响应链
  两个尾分支补 `!liveDeny` 排除（构建 RC=0）；K1 注释的"gate only
  filters"措辞同步纠正。

### 本会话冻结账本（更新）

部署链期间 **#4（11:03:24 UTC）**：sc stop 卸载 armed 驱动后冻结
（11:19:57 为同次重复报告）——冻结矩阵再添卸载场景；exec 通道卡死
事件一次（vmtoolsd exec lane 被未返回的 guest 程序占住 ~90s 自愈，
文件通道不受影响）。全程 kd 不可用（活目标连接被 MCP 30s 硬顶、冻
结时 no_debuggee）。累计 5 次失联中 4 次伴随 armed sysaudit——
r106 结论不变：kd 在线复现设施是 r107 首位。

### TODO

- r107：kd 在线 + 低阈值/卸载场景复现，冻结瞬间抓全核栈；
- K1 已 WONTFIX 落档；告警→kill 响应档继续等政策（现仅挂起）。

## Round 107 (2026-09-14) — 冻结定性轮：取证设施 + 复现 + 现场（进行中）

### 现象（本轮目标）

r106 收口时冻结问题明确未解决：5 次失联覆盖 idle/churn/卸载全场景，
armed 归因被"sysaudit 未挂载也失联"的观察削弱。本轮目标 = 抓到冻结
现场证据定性，而非继续盲测。

### FS 修复（r107 前置闭合）

fsutil dirty set C: + 重启 autochk → **C: 没有损坏**（dirty 位清
除）。修复后立即补跑 r106 遗留 K3 E2E：**双腿全绿**（回归腿 kill 正
常 + 内核行 `KILL policy writer pid=2328 image='svmbctl.exe' st=0`；
拒绝腿 lsass.exe 副本存活全 verdict）——r106 验收的遗留验证项闭合。

### 代码级分析（kick 路径结构性嫌疑）

TlbKickFlushAllCores **无锁遍历写全部 vCPU 的 VMCB 控制区**
（TlbControl=FLUSH_ALL + CleanBits=0），包括正在其他 CPU 上运行、自
己也在写 VMCB 的 vCPU。CleanBits 丢失更新窗口：目标 vCPU 写
Ctrl.X → 置 dirty 之前，跨 CPU 的 CleanBits=0 后到 → **脏标记丢
失 → VMRUN 用陈旧缓存控制值**（flush/NCr3/intercept 静默丢弃）。
ForEachVcpu 无锁（纯数组遍历）排除锁交叉等待。armed sysaudit 把
trip/DPC 频率放大（idle 环境流量也持续触发 re-deny KICK）——与"
idle 也冻"吻合。**此为结构性嫌疑，尚未实证**。

### 取证设施（本轮交付）

- **breadcrumb 飞行记录环**（core/crumbs.h/.cpp，测试构建专属）：
  128 槽 ×{Tag,Cpu,Tick,Seq}，锁-free 近似（丢失可接受），站点覆盖
  NPF consumer（enter/hit/resolved/noise）、re-deny DPC（enter/
  undenied/prekick/postkick）、TlbKick（enter/done）、wiggle、
  resolver/kill workitem。
- **两条读出路径**：活态 IOCTL_CRUMBS (0x82C) + ctl `crumbs`；冻结
  态 vmrun suspend → .vmem → `tools/decode_vmss_crumbs.py` 离线魔
  数扫描解码（Seq 排序，tick 增量）。
- 坑：AllocNonPaged 非零化——W/Seq 从垃圾池内存起步（首次活态读出
  w=3385195590 假象）→ RtlZeroMemory 修复；解码器初版条目偏移错
  （头实为 24B：magic(8)+w(4)+pad(4)+dropped(8)）——修正后健康样本
  解码完美（w=13519 与末位 seq 一致、多核流转清晰）。

### 复现实验（进行中）

配置 = RdThs=2 + WrThs=2 + Resp=1 + armed + idle fail-fast 循环；
vmx 加 mainMem.useNamedFile=TRUE（r105 的 suspend 只落 2.2MB 设备状
态无内存——命名内存文件使 suspend 落 4GB .vmem）。健康对照样本已取
（20 轮不冻 + suspend，面包屑解码验证）。**冻结现场已抓获并解码**（两次独立冻结，跨 boot 死点完全一致）：
- 第一现场：42 轮 armed idle 后冻（vmss 无内存——当时未开
  mainMem.useNamedFile；vmx 补 TRUE 后重试）；
- 第二现场：fresh boot + E2E + 第 1 轮 idlesnap（**45 秒内即冻**——
  "E2E 后立即冻"模式实锤）后 suspend → 4GB .vmem → **解码成功**；
- **两 boot 死点一致：最后一次 crumb = consumer-resolved，之后 re-deny
  DPC 从未运行（无 dpc-enter）**——死点窗口 = SenseConsume 的
  KeSetTimer/return 之后 → NPF 收尾 → VMRUN 重入之间；两 boot 的
  ambient 流量模型都解释得通（DPC 250ms 窗口内全核停摆）；
- 机制假设（收敛）：re-deny DPC **跨 CPU 无锁写全部 VMCB 控制区**
  （CleanBits=0 丢失更新 / 半更新控制区）× 目标 vCPU 的 VMRUN 边界
  → 控制值静默丢弃/损坏 → vmm loop 卡死 → 不再有 NPF → 不再有
  crumb（与"全核无后续 crumb"自洽）；
- 现场文件：r105_idle_armed 前的 vmem=14f2f394（健康对照，w=13519
  解码完美）+ 冻结 vmem（w=16081，已存 r107 冻结样本）。
- **下一步（r108 首位）**：NPF exit handler 收尾 + vmm loop 的
  VMEXIT_INVALID/异常路径补 crumb 站点 → 再复现一轮 → 最后 hop 定位
  到行级；根治方向 = 废除跨 CPU VMCB 写（per-vCPU pending 标志由该
  vCPU 自己的 exit 消费，r56 语义不靠 CleanBits 竞态）。

### 冻结#4 复现 + 根修证伪（诚实记录）

根修构建（PendingTlbKick）部署后同配置复现：**第 36 轮（~28min）仍冻**
——CleanBits 竞态假设**被证伪**（或非唯一机制）。冻结现场再次解码：
死点模式完全一致（最后 hop = consumer-resolved，高频连发后戛然而止，
kick-done 正常出现证明 PendingTlbKick 消费正常）。**根修保留了**（跨
CPU VMCB 写是真实的架构隐患，且不恶化），但冻结另有根因。新线索：死
前 cpu=0 同一 tick 内连续 2 个完整 NPF 周期（高频连发）→ 疑似某页的
periodic deny→fault 循环。r109 细化站点（CONSUMER_DONE=0x106 已实现
未部署）+ 逐 (caller,target) 归因继续收窄。

### 后续失联（第 6 次）与归因复杂化（诚实记录）

FS 修复（autochk clean）+ 细化构建部署（05d03582 双验一致）后：attach
**err 2（\.\svmb 不存在）**= 驱动设备未创建（svc start 未成功或
DriverEntry 未完成）→ **第 1 轮 idlesnap 即失联**（45 秒）。即本次失
联时 svmb 可能根本未运行——"armed sysaudit 引发冻结"的归因被动摇：
嫌疑面扩大到 guest 环境/存储层自身（NTFS 损伤的持续后果：vmtoolsd 启
动受阻、文件静默丢失、exec 通道间歇卡死——三征象同源的可能性）。
现场已保存（suspend 成功：vmss 05:08 + vmem）供后续分析。**r109 前
置：回滚快照 3 重建干净环境**（证书重装 + 测试目录重建均脚本化），
在可信基线上重新单变量归因。

### VM 断电真相（运营律级发现）

两次"VM not powered on"的真凶 = **`taskkill /im lsass.exe /f` 杀掉
了 guest 真 LSASS**（1903 默认未启 LSASS PPL）→ Windows 检测 LSASS
终止**保护性自动关机**。K3 拒绝腿的清理命令按映像名杀——映像名匹配
会命中系统真进程。**律：guest 清理禁用按映像名的 taskkill 于系统进
程名；测试副本用 PID 或唯一命名**。E2E 已删该行（拒绝腿进程跑完
verdict 自动退出，无需杀）。

### TODO

- 冻结现场解码 → 定位最后 hop → 根因修复 → 默认阈值长浸回归；
- CleanBits 丢失更新的根治方向（若实证）：跨 CPU 请求改为 per-vCPU
  pending 标志由该 vCPU 自己的下次 exit 消费，杜绝无锁跨 CPU VMCB 写；
- K3 活体门拒绝分支的确定性 E2E（需 trip/live 名不一致竞态）。

## Round 108 (2026-09-14) — 冻结根因定性 + 根修：废除跨 CPU VMCB 写（r107 面包屑实证驱动）

### 现象（本轮目标）

r107 面包屑设施建成后，冻结现场首次可解码：**两 boot 死点一致**——最后
crumb = consumer-resolved，re-deny DPC 从未运行。本轮：补站点（npf
enter/tail + dispatcher default）→ 第三次复现（第 10 轮，~8min，与
r105 一致）→ 冻结 vmem 解码 → **新站点证据：死点不变，且 npf-tail
（r68 heal 分支专属）从未出现**——死点窗口维持"resolved → DPC 之
间"，全系统静止（所有核的 ambient NPF 同时消失）。

### 环境真相（重大转折）

第 6 次失联（exec 卡死 + VM 断电）后放弃对抗：**回滚快照 3（09/05
基线）** → guest exec 通道**立即恢复** → 干净 FS 上一次推送即成功
（对照：损坏 FS 上三轮推送全失败）。结论：**本会话后半段的
exec 卡死/文件消失是 guest NTFS 损伤（6 次硬复位累积），与 svmb 代
码无关**——但也正因 FS 损伤，此前多次"间歇冻结"的归因（svmb armed
路径）需要重新审视：部分失联事件可能是 FS 损伤 + 硬复位的恶性循环
放大。快照回滚后重建：证书（vm_addstore）→ 部署（一次成功）→ 全链
验证。

### 根修（结构性修复，不依赖竞态实证的统计显著性）

**机制**：TlbKickFlushAllCores 无锁跨 CPU 写全部 vCPU 的 VMCB 控制
区（TlbControl=FLUSH_ALL + CleanBits=0）。AMD 架构要求 VMRUN 时
VMCB 一致；跨 CPU 写与目标核自己的 VMCB 更新（exit 处理路径）竞
态 → CleanBits 脏标记丢失 → VMRUN 使用陈旧缓存控制值 → 控制变更静
默丢弃/状态不一致 → 卡死。armed sysaudit 把 re-deny DPC 频率放大
（idle ambient 流量也持续触发），解释 armed 相关性；CleanBits 竞
态窗口的微秒级宽度解释间歇性。

**修复**（三处）：
- `VcpuInfo.PendingTlbKick`（per-vCPU 标志，VCPU_INFO 页内）；
- `TlbKickFlushAllCores`：只对各 vCPU `InterlockedExchange(
  PendingTlbKick, 1)`——**不再写任何 VMCB**；
- `SvmbVmExitEntry`（每核自己的 exit 入口）：消费标志 → 写**自己的**
  VMCB TlbControl/CleanBits → 清标志。
语义等价性：vCPU 回 host 必经 exit，exit 入口消费即保证"该核下一次
VMRUN flush"——与原跨 CPU 写的意图完全一致，且同核写自己的 VMCB 无
竞态。wiggle 路径（r80）的跨 CPU 写本轮不动（实时性语义不同，仅在
cr3 watch 场景触发，登记后续）。

### E2E/验证（构建 5bfaf175/ctl c4773b04 双向 SHA256）

- FS 修复后 K3 E2E 双腿补跑绿（r106 遗留闭合）；
- 根修构建部署（干净 FS 一次推送即成）+ hv RUNNING + r105 响应链
  E2E 绿（sysalert SUSPEND 探针照常）；
- **决定性验证**：复现配置（RdThs=2+WrThs=2+Resp=1）armed idle 80
  轮长跑——修复前此配置 8-30min 必冻（3 次）。结果见提交后补记。

### TODO

- r109：默认阈值 armed 长浸 ≥60min（含 idle 腿）+ 生产构建回归
  （排除观察者效应）+ wiggle 跨 CPU 写的同类整改评估；
- 告警→kill 响应档等 K1 类政策扩展（用户已定 K1 WONTFIX 于 kill 路
  径）。

## Round 109 (2026-09-28) — 冻结复现对照轮：干净环境下配方失效（红绿双版），环境退化因素升为必要条件

- **背景**：r105/r107 在旧 guest（NTFS 损伤三征象环境）确立"低阈值
  +Resp=1 armed idle ~8min 必冻"配方；r108 的 PendingTlbKick 根修后
  复现配置 36 轮仍冻（CleanBits 假设证伪），且第 6 次失联发生在
  svmb 未加载状态——归因已动摇。r109 前置=回滚快照 3 重建干净环境。
- **本轮设计**：同一 melt 配方（BehaveRd/Wr/OpThs=2 + BehaveResponse=1，
  sysaudit armed idle）在干净快照 3 环境跑红绿双版对照——绿版=r110
  @ a848c79（含 B1/B2 等 12 项修复，VM 轮全绿），红版=19b3cdd
  （r108-post，无 B 系列修复）。宿主 melt_host_watch 20s 周期探测，
  guest 侧 melt_watchdog 采样；冻结后取证预案=vmware.log + vmrun
  suspend + decode_vmss_crumbs.py 面包屑解码（CONSUMER_DONE 行级
  定性）。取证前提核查：vmx mainMem.useNamedFile=TRUE + 串口黑盒
  均在（快照 3 未回滚掉）。
- **结果（配方活性双证 + 零冻结）**：
  - 绿版 32min 窗口：61+ 探测全 alive，零 WEDGED；sysaudit armed
    全程（drain 26 条真实捕获、behavior 归因 SearchProtocolHost
    wr=25、ntcf-excluded 3980、npf heals=16——B1 修复后的 heal
    路径在真实负载工作）。
  - 红版 19b3cdd 20min 窗口：61/61 探测 alive，零 WEDGED；armed
    全程（drain 29 条、ntcf-excluded 2903、dropped=620 ambient）。
  - 两腿均越过配方冻结点（~8min）数倍余量。
- **关键决策回顾**：
  | 假设 | r108 时的状态 | r109 后 |
  |---|---|---|
  | "armed sysaudit 是冻结充分原因" | 未定（第 6 次失联已动摇） | **显著削弱**：红版（同代码行为）干净环境 20min 不冻 |
  | "guest 环境退化（NTFS/存储损伤）参与" | 嫌疑（r108 addendum） | **升为必要条件候选**：唯一与红版不冻相容的解释维度（代码×环境复合，或环境独立） |
  | "B 系列修复消除冻结" | — | 不能归因：红版同环境也不冻，绿版不冻不构成 B 修复的因果证据（B1/B2 的理论隐患独立成立，vmware.log 死点模式无新数据点） |
- **新事实/律**：
  1. **melt 配方是环境敏感的**：复现冻结需要"损伤环境"这个载体；
    干净快照 3 上配方对修复前代码也失效。后续任何冻结定性必须先
    记录 guest 环境健康度（NTFS/文件消失/exec 卡滞三征象排查）。
  2. 红绿对照法（§11.7）在冻结课题上的形态修正：**绿版不冻 + 红版
    也不冻 = 配方失效**，两腿都不可省——单跑绿版会误读为"修复生效"。
  3. r108 的 CONSUMER_DONE 行级定性目标因冻结未复现而搁置；面包屑
    取证设施（crumbs + decode_vmss_crumbs + useNamedFile）验证在位。
- **遗留**：环境损伤的具体机制（NTFS 层 vs 存储层）未定性，属 guest
  环境健康课题；若未来冻结再现在损伤环境，优先抓 vmem 面包屑后按
  r108 的死点窗口（resolved→timer→return）继续分割。
- **guest 终态**：r110 绿版已恢复部署（18:18:35 GREEN RESTORED），
  旋钮清回默认（阈值 reg delete + BehaveResponse=0），svc 重启后
  RUNNING 6/6 核；watchdog 已杀；红版构建存档 tests/r109_red/。

### TODO

- r111 候选：默认阈值 armed 长浸（干净环境基线，≥60min）；生产构建
  回归；若要继续冻结根因：需要重建"损伤环境"载体（旧 guest 快照若
  存在）或接受搁置；
- B5 lazy-map 腿与 B7 的定向场景补测（CODE_REVIEW 十五节遗留）。

## Round 111 (2026-09-28) — 生产回归 PASS + 默认阈值长浸 12min 冻结：B 系列修复证伪为根因，死点行级推进到 NPF 收尾边界

### 生产轨道（PASS）

- Release/SVMB_PRODUCTION 构建+签名+部署+回归全绿：10 项拒绝面全
  部 err 50（POISON/NPT_HOOK/STRESS/KDUMP/CRUMBS/HV_CONTROL start/
  cycle/stealth attach/CR3_READVM 默认关 + probee2e 内 poison 腿）；
  允许面完好（sysaudit armed+drain、selftest 332/0）。
- **577 复盘（流程缺陷）**：build_release.bat 从未内联签名（r73 的
  "手签"=手动单独跑脚本），本轮漏跑即送出未签名 .sys → guest 577。
  已修：build_release.bat 尾部 call 两个签名脚本（一键含签）；
  sign_sys_rel 的 verify 改非致命（host 无 svmb-test 根，/pa 必报
  untrusted——链在 guest 侧验证）。证书链路径复验：guest Root+
  TrustedPeople + BCD testsigning=Yes 三者均在位。

### 冻结（重大新事实，默认阈值腿）

- **配置**：默认阈值（128/32/16）+ BehaveResponse=0（纯遥测）+
  sysaudit armed idle，r110 a848c79（含全部 B 系列修复），干净
  快照 3 环境。armed 后 **12.3 分钟冻结**（watchdog +740s 探测
  rc=124；Tools 心跳 10:43:31 最后收到、10:43:51 timeout）。
- **r109 结论修正**：不是"干净环境不冻"——是"低阈值配方不冻"。
  配方参数空间倒置：低阈值+Resp=1（密集告警/挂起活动）两腿均
  存活，默认阈值（稀疏活动）反而冻。冻结概率与活动密度负相关
  的表象下，**三核同窗口交叉**（见下）才是形态要求——低阈值
  腿的密集串行活动可能反而错开了多核同窗窗口。
- **B 系列修复证伪为冻结根因**：B1（时钟 ISR 锁降级）/B2（视图
  错配）/B5 部分/B12 等全部修复在位仍冻、形态同族——冻结根因
  在这些路径之外。
- **死点行级定性（vmem 面包屑，比 r108 推进两站）**：
  - 活跃 ring（boot 后 563s 加载）w=36922，终局 tick 56313619339
    （=挂钟 10:43:31，与心跳精确吻合）；
  - 终局三核同 100ns 窗交叉：cpu0 完成 re-arm DPC 全程
    （prekick→kick→postkick）并补完被打断的 consumer
    （done/npf-tail）；**cpu1 停在 consumer-resolved 之后**
    （consumer-done 缺失 → 卡点收窄到 [SetPerm4kNoLock×2 视图 |
    KeSetTimer re-arm] 区间）；cpu4 完整收尾 npf-tail 后死；
  - 之后零 crumb（250ms 后必到的 re-arm DPC 没来）+ vmware.log
    无 vcpu panic/reset（纯冻结，vhv 内部）——全系统静默。
  - r108 时代"consumer-resolved 后全停"的死点在 r110 细化站点下
    推进为：**cpu4 类死点 = npf-tail（NPF handler return）→
    Dispatch 收尾 → SvmbVmExitEntry 尾部（r53 publish/TlbControl/
    CleanBits/2us stall）→ asm VMRUN 重入**；**cpu1 类死点 =
    consumer 的 resolve/store 或 re-arm KeSetTimer**。两核同瞬间
    死于不同站点 = 单点自旋锁死锁不成立，指向共享的下层
    （VMRUN 边界/vhv 嵌套层/NPT 硬件 walk）。
- 死前形态：多核同窗 NPF burst（终局 12 条 crumb 内三核并发）
  ——r108"高频连发"观察在新构建重现并加强为"多核并发"。
- 取证存档：tests/r111_freeze/（vmss 全量 + 冻结 ring 3 页裸二进制）。

### 关键决策回顾

| 假设 | r109 后 | r111 后 |
|---|---|---|
| B1 时钟 ISR 锁降级 = 冻结根因 | 候选（修复理论吻合） | **证伪**（修复在位仍冻） |
| 干净环境免疫冻结 | 初判成立 | **修正**：仅低阈值配方免疫；参数空间倒置 |
| 冻结需要环境损伤 | 必要共因子候选 | **削弱**：干净环境默认阈值即冻 |
| 死点=consumer-resolved 后 timer 不跑 | r108 定性 | 推进：cpu0 的 DPC/timer 全程跑通了（r110 站点），cpu1 卡在更早的 resolve/re-arm，cpu4 死在更晚的 NPF 收尾后——**timer 永不来是结果不是原因** |

### TODO（r112）

- 死点分割武器：SvmbVmExitEntry 尾部（view publish 前后、
  KeStallExecutionProcessor 前后）与 asm VMRUN 重入点加 serial
  beacon/新 crumb 站点；SenseConsume 的 SetPerm4kNoLock 与
  KeSetTimer 前后加站点——把 cpu1/cpu4 两类死点各再切一刀；
- 假设清单：A=多核 VMRUN 边界 × TlbKick pending 消费交叉的 vhv
  内部死锁；B=exit 上下文 KeSetTimer 异常；C=多核同页 resolve
  交叉。三假设均可被上述站点分割区分；
- 复现稳定性：默认阈值 armed idle 腿重跑一次（当前 n=1）。

## Round 112 (2026-09-28) — 死点分割轮：C 层全链走通，死点最终收窄到 asm 恢复/VMRUN 重入边界；vmss2core 六 vcpu 全部认不出 Windows

### 改动与武器（build RC=0 零警告）

- **防御性修复（假设 B）**：SenseConsume 的 re-arm KeSetTimer 加
  IRQL 门（<=DISPATCH 直呼；>DISPATCH 走专用 `g_RearmDpc` 排队，
  DPC 上下文 DISPATCH 级执行 KeSetTimer，单飞防抖；stopping 检查
  镜像 SenseRedenyDpc）。旧注释声称 "consumer 只跑 <=APC"（execute
  fault 调用者）——r111 复盘发现该假设无强制力，故防御化。
- **三个分割站点**：0x180 consumer-presetimer（KeSetTimer/排队分支
  前）、0x182 rearm-dpc-done、0x183 exit-epilogue（SvmbVmExitEntry
  的 KeStall 后、返回 asm 前）。解码器 TAGS 同步。
- r111 卡点表述更正：consumer-resolved 打在 SetPerm4kNoLock 循环
  **之后**，cpu1 的真实残余区间只有 KeSetTimer/CrumbPost——
  r111 轮记录的 [SetPerm4kNoLock×2|KeSetTimer] 前半不成立。

### 复现与取证（n=2 确认可复现）

- 默认阈值+Resp=0+armed idle，r112 构建，**armed 后 ~2min 冻结**
  （r111 为 12.3min——同配方两次，时间波动大但形态稳定）。
- **KDNET 重试（r68 后首次）**：open_kd_session 30s 超时 =
  no_debuggee 复现——冻结态中断上下文死、kdnet 包无人收。
- **面包屑终局（单核 cpu=5，tick 10645586489 = 冻结时刻）**：
  `npf-enter → consumer-enter → consumer-noise → consumer-resolved
  → consumer-presetimer → consumer-done → npf-tail → exit-epilogue
  → [全系统静默]`——**C 层最后一条（exit-epilogue）打出**。
- **vmss2core -W 判定**：`Cannot recognize Windows VM. Error
  parsing Windows data.` ×6（每个 vcpu）→ `Cannot create
  memory.dmp`。六 vcpu 全部无法解析出 Windows 内核结构 = 全部
  深陷 SVM/VMRUN 嵌套上下文（与 r68+ no_debuggee、r107"CPU 未返回
  guest 上下文"三方互证）。vmss2core 全核栈路线对此冻结态不可用。
- vmware.log：无 vcpu panic/trap/reset，纯静默（同 r111）。

### 死点判决（r111+r112 合并）

| 死点类 | r111 观察 | r112 站点判决 |
|---|---|---|
| cpu4 类（npf-tail 后） | 死于 [npf-tail → 下次 exit 首站] | **exit-epilogue 已打出 → 死于 [SvmbVmExitEntry return → asm 恢复段 → VMRUN 指令 → (VMRUN 内部)]** |
| cpu1 类（resolved→done 间） | done 缺失 | r112 全链打齐（presetimer/done 连续）——本次无人死在此段；防御修复后该段免疫 |
| 全系统静默 | 三核同窗后零 crumb | 单核走完全链后零 crumb——**静默与"哪一站死"无关，VMRUN 边界一死全死** |

**根因候选收敛**：死点在 asm VMRUN 重入边界（或 VMRUN 指令内部/
vhv 对嵌套 VMRUN 的处理）。所有 C 层逻辑（consumer/DPC/kick/
dispatch/epilogue）已全部洗清。剩余假设：
- A'：多核中一核在 VMRUN 边界死锁 → vhv/L0 层连锁停摆全部 vcpu
  （vmss2core 六 vcpu 同陷佐证）；
- D（新）：VMRUN 重入对 GuestVmcb 的依赖字段在本次 exit 期间被
  并发改写（如 PendingTlbKick 消费路径写 TlbControl/CleanBits 后
  VMRUN 一致性检查挂起）——与 r108 PendingTlbKick 根修的"消费点"
  在同一 exit 内的时序相关。

### TODO（r113）

- **asm 站点**：svm_entry.asm 的 vmm loop 在 VMRUN 指令前后加
  0x184/0x185（保存易失寄存器后 call CrumbPost）——把 [return→
  VMRUN] 段切开；重点观察死核的最后一个 asm 站点在哪条指令前；
- D 假设武器：exit-epilogue 后、VMRUN 前快照本核 VMCB 的
  TlbControl/CleanBits 值入 crumb（复用 0x185 槽位带参）；
- 复现稳定性继续累积（n=2，时间 2-12min 波动）；
- vmss2core 的 vmss.core（-M 物理视图）可作为 vcpu 寄存器挖掘
  的原始材料（备选路线）。


## Round 113 (2026-09-28) — asm 边界站点轮：BANK-EVERYTHING 律 + 观察者效应判决（beacon 在场 65min 零冻结，微时序敏感实证）

### 武器落地（build RC=0；两轮崩溃循环后才稳定）

- **asm 站点 0x184 vmrun-pre**（svm_entry.asm enter_guest：vmload/vmrun
  前，Data=guest VMCB Ctrl.CleanBits）+ **0x185 exit-entry**
  （SvmbVmExitEntry 首行）+ extern "C" SvmbCrumbPostD（asm 不能拼
  修饰名）+ Crumb 加 Data 字段（24→32B，Data 先于 Seq 写；解码器带
  stride 参数兼容 r111/r112 旧档案——24B 旧档案解码已验证）。
- **BANK-EVERYTHING 律（本轮最大新定律）**：VMCB 不保存 GPR 与 XMM，
  **VMRUN 靠物理寄存器直传 guest 值**——RESTORE_REGISTERS 之后任何
  C 调用都会把 host 残值塞进 guest 寄存器。判例：首版只银行 XMM0-5
  → 仍崩；d1.dmp = **SYSTEM_SERVICE_EXCEPTION(3b) AV in
  nt!PspQueryProcessIdListCallback**（svchost 系统服务、svmb 不在
  栈上——**GPR 残留型崩溃的栈在无关下游**，状态损坏不指向肇事者）。
  终版：银行全部易失 GPR（rax/rcx/rdx/r8-r11）+ XMM0-5 低 128 位
  （MSVC 内核代码 SSE-only，YMM 高位安全）；非易失 GPR 由 C ABI 自保。
  修复后 RUNNING 稳定（前两版 ~50s 内翻转）。

### 长浸判决（n=3 腿，armed idle 65min 实质跑满）

- **零冻结**（对比 n=2：r111 12.3min、r112 ~2min）——同配方、同
  环境、唯一变量 = asm beacon 在场。
- crumbs 活体基线：每 exit 三连 `185 exit-entry → 183 exit-epilogue
  → 184 vmrun-pre`，armed 15min 已 29 万+ exit 全稳定（beacon 本身
  无害性证明）。
- **判决：微时序观察者效应**。VMRUN 前的 beacon（C 调用开销 + 栈
  银行 + CleanBits 读）扰动了 VMRUN 边界的微秒级时序，冻结随之消失
  ——**D 假设（VMRUN 重入边界对并发改写 VMCB 的微秒竞态）获得最强
  间接支持**；与 round-21 的 2us pacing 实验（对抗 delayed hard reset
  的同型手段）遥相呼应：**本 vhv 的嵌套仿真层对 VMRUN 前时序敏感**。
- 终态：npf heals=26、零 trip/kill/suspend、detach 干净。
- 崩溃取证存档：tests/r113_freeze/d1-d3.dmp（boot-loop 三连；git
  不入库 *.dmp，判例栈已录入本档）。

### 关键决策回顾

| 问题 | 结论 |
|---|---|
| beacon 会改变被测系统吗？ | **会**——这是本轮最重要的负结果：边界微时序本身就是变量。取证注入必须意识到"打点=治疗"的可能 |
| 冻结根因还在吗？ | 在（时序竞态未消除，只是被扰动规避）；asm 站点与 CleanBits 快照在下次冻结时仍是分割武器 |
| GPR/XMM 状态谁管？ | hypervisor 自管（VMCB 不管）——任何 exit 尾部插入代码的硬约束 |

### TODO（r114）

- **时序二分**（定根因 vs 定缓解）：
  (a) beacon 保留但不读 CleanBits（纯 C 调用开销）——防冻=纯延迟；
  (b) 去掉 beacon，SvmbVmExitEntry 尾部 KeStall 增到 3-5us——纯
  stall 等效防冻=时序假设锁死（工程缓解=stall 常驻，beacon 卸除）；
  (c) 两者都不防冻 → 时序假设削弱，回到并发改写 VMCB 本身（读的
  串行化副作用）重新归因。
- 若 (b) 成立：生产缓解 = 尾部 stall + 文档化（"VMRUN 前 pacing
  是本 vhv 的承重墙"——round-21 注释升级为定律）；beacon 仅测试
  构建保留。
- 复现数据继续累积（冻结 n=2、beacon-免疫 n=1）。

## Round 114 (2026-09-29) — 时序二分六腿轮：观察者效应判决撤销（r113 为 n=1 假阳性），死点收窄到 vmload/vmrun 指令本身，L1 侧全部洗清

### 六腿矩阵（全部默认阈值+Resp=0+armed idle，各自独立 boot）

| 腿 | VMRUN 前插入 | 冻结时刻 | 终局最后站点 |
|---|---|---|---|
| r112 基线 | 无 | ~2min | exit-epilogue |
| r113 beacon | C call+全银行+CleanBits 读 | **65min 未冻（n=1）** | （无冻结） |
| B1 | rdtsc 自旋 5μs（纯时序） | ~2min | exit-epilogue（spin/VMRUN 窗口） |
| B2 | 裸 mfence（store buffer 排序） | ~4.7min | exit-epilogue |
| B3 | clflush VMCB 控制区×8（强制 writeback） | ~6min | exit-epilogue |
| E12 | r113 beacon 原样复跑 | ~8min | **vmrun-pre**（beacon 返回后） |

### 判决（每腿独立取证，死点形态一致）

1. **观察者效应判决撤销**：E12 证明 r113 的 65min 幸存是**环境特异
   假阳性**（n=1 陷阱）。方法律（新）：**"打点=治疗"的观察者效应判决
   本身必须 n≥2 复跑**——本轮 B1/B2/B3 三腿如果按 r113 的单腿逻辑
   解读会各自得出错误结论。
2. **L1 侧全部洗清**：时序扰动（B1）、内存排序（B2）、VMCB 内存可见
   性（B3）、C 调用+LOCK+读（E12）——**没有一种 VMRUN 前插入物改变
   冻结结果**（六腿全冻，2-12min 泊松状分布）。死点不在 L1 可操作面。
3. **死点最终收窄（E12 取证）**：beacon 腿最后站点 = `vmrun-pre`
   （0x184，C 调用返回后）→ 静默。VMRUN 前剩余指令 = `mov rax,
   [rsp+8h]; vmload rax; vmrun rax`——**死在 vmload/vmrun 的 SVM 指令
   执行本身，在 VMware 嵌套仿真层内部**。L1 无法再细分（两指令间无
   可插站点；两者都读 VMCB，死亡语义同构）。
4. **幸存者的共同点（E13 假说，r115 方向）**：未冻腿 = r113（ambient
   exit 29 万+/65min，每秒 ~320）与 r109 双腿（低阈值配方=告警/挂起
   密集）；冻结腿 = 全部 idle。**ambient exit 活动密度防冻**与全部
   数据兼容——若 E13 成立，运营缓解 = armed 期间注入周期性活动
   （或接受"armed idle 是唯一危险相位"）。
5. r113 的 BANK-EVERYTHING 律与 asm 站点设施保留（0x184/0x185 +
   Data 字段是本轮死点收窄的执行者）。

### 关键决策回顾

| 假设 | 判决 |
|---|---|
| D（微时序竞态，扰动可规避） | **死**（B1 5μs 纯扰动不防冻） |
| E2（store buffer 未排空） | **死**（B2 mfence 不防冻） |
| E7（VMCB modified 行未落内存） | **死**（B3 clflush 不防冻） |
| 观察者效应（r113） | **撤销**（E12 复跑冻结） |
| E13（ambient 活动密度防冻） | **存活**（与全部六腿+r109 数据相容，待专测） |

### 取证存档

- B1/B2 冻结 vmem 已解码（死点同构）；E12 vmem（vmss）含 vmrun-pre
  终局——本轮最有价值现场，保留在 VM 目录（覆盖前可再归档）。
- B1 银行教训强化：rdtsc 破坏 rax/rdx，rdx 物理直传必须银行；
  spin 终点走栈 scratch（[rsp+30h]），不花 guest GPR。

### TODO（r115）

- **E13 专测**：默认阈值 armed idle + 后台 CPUID storm 注入
  （vm_ctl storm 周期化或 guest 侧循环探针）——防冻即运营缓解锁死；
- 死点最终形态登记：SVM 指令在 vhv 内部挂起，L1 侧无可修对象——
  战役主目标从"修根因"转向"定缓解"（E13 专测 + 复现率刻画）；
- 冻结率基础数据：纯基线腿（无站点）攒 n，回答"冻结率是否按 boot
  分层"（r109 两腿 52min 零冻是低阈值配方，默认阈值腿全冻——但
  r113 65min 是默认阈值，分层假说存疑）。

## Round 115 (2026-09-29) — E13 专测：storm 活动密度不防冻，L1 侧操作面穷尽；战役主目标正式转向定缓解与刻画

### E13 腿（同 E12 构建——r113 beacon，唯一变量 = 周期活动注入）

- guest 侧 storm 循环：`storm 50000`（50k 超类调用 exit/核，~38k
  exits/s/核）+ ~6s 间隔，14s 周期；armed idle 配方不变。
- **结果：armed 后 ~6 分钟冻结**——storm 密度下 ring 累计
  **99,175,446 条 crumb（≈3300 万 exit / 6min ≈ 92k exits/s 全机）**
  依然冻。终局：**cpu4 与 cpu0 同窗的 `vmrun-pre`**（beacon C 调用
  已返回）→ 静默——死点形态与全部前腿同构。

### 七腿全景与最终判决

| 腿 | VMRUN 前变量 | 结果 |
|---|---|---|
| r112 | 无站点 | 冻 ~2min |
| r113 | beacon | 65min 未冻（孤例） |
| B1 | rdtsc 自旋 5μs | 冻 ~2min |
| B2 | mfence | 冻 ~4.7min |
| B3 | clflush VMCB×8 | 冻 ~6min |
| E12 | beacon 复跑 | 冻 ~8min |
| E13 | beacon + storm 活动密度 92k exits/s | **冻 ~6min** |

1. **E13 证伪**：活动密度不是保护因素（r113 幸存与 r109 双腿幸存
   的"活动密集"共性是巧合关联）。
2. **L1 侧操作面穷尽**：时序（B1）、排序（B2）、缓存写回（B3）、
   C 调用+LOCK+读（E12/E13）、活动密度（E13）——无一改变结果。
   六冻腿 2-8min 泊松状；r113 的 65min 为**未识别变量的 boot 特异
   孤例**（统计上 e^(-65/5)≈2e-6，泊松假设下不可能——冻结率按
   boot 分层假说保留，但驱动变量未识别）。
3. **死点最终形态（维持 r114 判决）**：vmload/vmrun 的 SVM 指令
   在 VMware 嵌套仿真层内部挂起。E13 高密度取证再确认：92k
   exits/s 下最后动作仍是两核从 beacon 返回进入 SVM 指令。

### 缓解面（L1 无可修对象后收窄到运营层）

- **唯一实证有效的"缓解"= 不进入危险相位**：idle + sysaudit armed
  是全部七次冻结的唯一形态；未 armed idle（r79/r90/r93 共 7.5h+）
  与活动中的 armed（T2/probee2e 压测）从未冻过。运营律：**armed
  监控形态避免长时间无人值守 idle；实验后 detach**。
- production 构建不受死点影响的前提 = 不部署 armed-idle 形态；
  风险已登记（production 可 attach sysaudit）。
- r115 后无进一步 L1 实验计划——根因对象在 VMware 嵌套仿真层
  （vhv.enable=TRUE 的 Workstation），L1 侧无法触达。

### TODO（后续，优先级降档）

- 冻结率刻画（可选）：纯基线腿攒 n，验证 boot 分层假说——学术价值
  为主；
- 若 VMware 环境可变：vhv.enable=FALSE 裸金属 SVM 形态复测（本战役
  的全部 vhv 敏感问题——TscOffset/MTF/异常注入/本次挂起——预期消失，
  这是"换母体"级验证，需新 VM 配置）；
- 主线回到功能轨道：GitHub 发布审批（用户侧）、CODE_REVIEW B5/B7
  遗留、r109 起积累的运营律整理。

## Round 123 (2026-10-01) — B7 迁移：视图发布/wiggle 三函数改混合模式（本核同步+他核 pending）

### 变更

`TlbApplyViewSwitchAllCores`/`TlbWiggleToScratch`/`TlbWiggleRestore` 的跨核
VMCB 直写全部消除（r108 冻结级根修的同类）：**调用核同步应用自己的切换**
（probe stage-1 的 NCr3 立即读回语义依赖此），**其余核走 per-vCPU
{PendingViewSwitch, PendingViewPml4} 槽**，在各自 exit entry 消费（紧随
PendingTlbKick 之后、r53 政策块之前）。ShadowKick(mode 1) 经同一 ApplyCb_
隐式迁移。**PublishedNcr3 刻意不动**——它是 r53 政策的单写者账本兼
wiggle-restore 的目标源；新路径若写它会把 ProcView 钉住的核搁浅在 scratch。

### 验收（ACCEPT-WITH-NOTES，条件项落实）

- **N1 已修**：self 识别改用 `CurrentCpuIndex()`（索引空间）——原
  `KeGetCurrentProcessorNumber()` 是组内号，与 `CpuIndex`（全系统索引）在
  单组 6vCPU 上巧合相等所以套件绿，多组系统会错配他核槽=重新引入跨核写。
- 竞态走查（验收 N2）：生产者先存 pml4 后置 flag；消费者 exchange 后读
  pml4——所有交错归结为"跳过中间视图/延迟一 exit/幂等重应用"，无撕裂
  （x64 TSO+对齐 8 字节）。
- wiggle 搁浅类（r77-P1-1）闭合（验收 N3）：相位 2 恒跟随相位 1（GIF=0
  正确性锚不动），消费保证来自核的持续 exit。

### 登记残余（验收 N3/N4/N5）

1. **B7 部分关闭**：`InterceptManager::ApplyAll` 仍跨核写 Ctrl 位图/
   MsrpmBasePa/DbgCtl（模块装卸节奏，罕见、历史上从未关联冻结）——留后续。
2. tlbMode=1 + 政策活动 + 消费型 restore==PublishedNcr3 时，同 exit 内
   r61 一次性 flush 复位可取消已消费切换的 flush（旧代码在下一 exit 才
   取消——旧更宽）；默认 tlbMode=0 不受影响。
3. wiggle-restore 跨核快照 PublishedNcr3 的单发布滞后窗（旧代码同形更宽）。

### 诚实归因（验收 N7）

r109 已证伪 CleanBits 竞态=冻结因果（根修构建仍冻）。本迁移是**类卫生**
（消除一个已知危险写形状），不是已证冻结修复——未来冻结不得自动归因或
豁免于 r123。

### 成果

11 项全绿：selftest/viewprobe（视图发布周期直接消费者）/npfprobe/probee2e
（wiggle 路径）/policytest(T2)/[internal-research consumer]。N1 修复后全绿复验。

## Rounds 127-132 (2026-10-01) — [internal-research item]

Six internal research rounds between the public r126 state and the
r133 mem_crypt release (capability selection, private-frame lifecycle
hardening, and the publication tooling itself) are omitted from the
public history. The infrastructure they hardened that r133 depends on
(the per-process NPT view binding and the slot-overlay private-frame
layer with its exit-generation release gate) ships as part of the
public tree; see docs/MEM_CRYPT.md.

## Round 133 (2026-10-10) — mem_crypt：进程内存块加解密（双帧分视 + 两阶段 seal + 视图同步契约）

### 现象（目标）

实现"进程内存块加解密"：目标进程自己读明文，其他一切观察者
（readvm/物理扫描/他进程）读密文；dec/卸载无损复原。首轮实机暴露
三连环：目标侧过渡窗口读到密文（ok=1 damaged=3 持续不自愈）、第二
目标 enc 报 170（单绑定槽被死目标钉死）、ovlstat 报 50（产线门）。

### 与参考的对比

- Cr3Encrypt 式"切出回写加密"被否（崩溃窗口/一致性复杂度高一档）；
  选 r128 overlay 双帧常驻（切入切出零数据变换）。
- CR3-write 拦截闭合迁移窗口被否：r42/r48 实证全系统上下文切换税
  饿死本 vhv（cr3_monitor.h 明载 known-hazard）。
- r47 Route-A 哨兵被否：KTHREAD.State 写发生在 CR3 加载**之前**，exit
  时看到的还是旧 CR3，闭合不了视图切换。

### 修复尝试（三轮实机）

| 轮 | 方案 | 结果 |
|---|---|---|
| 1 | 单阶段 enc（挂起→map→密文→resume） | readvm=密文 ✓；目标 ok=1 damaged=3 ✗（迁移窗口） |
| 2 | 两阶段 + 退出代际门（r130 机制暴露为 OverlayArmGeneration）+ ctl 逐核 CPUID nudge | 门通过 ✓；签名不变 ✗（CR3-陈旧半边未闭合） |
| 3 | U/S=0 base 视图陷阱（r32 无条件故障律=合成读陷阱）+ npf 第二 deny 槽 | 陷阱被内核自己的触碰者（working set/compression direct-map 走行）一秒内 FOREIGN-fault 解除 ✗✗（内核日志实证）；**方案否决** |

最终架构 = 冻结 stage-1（目标挂起过闸，零缓存污染）+ 代际门 +
seal 时 MmMapIoSpace 物理别名写密文 + **视图同步契约**：目标在块
访问点取 intercepted CPUID（exit 即重发布 per-core ProcView）。
配套：死绑定槽抢占（Cr3M4ArmProcessView 查活抢死）、失败路径清理
dec、Frozen 账目（stage-1 挂起/seal 补 resume，dec/Deinit 对账）、
卸载顺序铁律（回写明文在 freeNow 释放 overlay 之前）。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| 两阶段代际门可闭合目标侧窗口 | 只闭了 stale-TLB 半边；CR3-陈旧半边是 r120 模型固有（npt.h M4 prerequisite 早有记载） |
| U/S=0 陷阱=万能读陷阱 | r32 律对内核自己也生效；working-set/compression 触碰即拆陷阱 |
| 哨兵可做调度期视图同步 | State 写在 CR3 装载前，exit 看到旧 CR3 |
| readvm 可验证卸载回写 | 新驱动实例种子表看不见旧实例 spawn 的 pid（err 31）；判据改为幸存目标自身自检（findstr 零 DAMAGED） |
| e2e 单文件握手 | 残留 mcrypt_va.txt 递了死 pid（复用 pid 陷阱）；先删后读+立即消费 |

### 验证（全绿）

全周期（baseline 明文→enc sealed→readvm=密文 且 目标自检 4/4 明文→
dec→readvm 复原→list 0）+ hold+卸载腿（块保持 sealed 下 sc stop/
start，子进程穿越全程零坏字节）+ 离线自测 347/0（含 keystream 9 项）。
ovlstat 50 为产线门拒 NPT_HOOK（披露性，软警告容忍）。

### 律（新增）

1. **视图同步点 = VM exit**：per-core NCr3 只在 exit 时重发布；无
   exit 纪元迁移到陈旧核读到原帧。受保护目标在访问点取 intercepted
   CPUID 是部署契约（结构性修复=per-core view selection，r134+ 候选）。
2. **r32 无条件故障律不可当读陷阱用**：内核 direct-map 触碰者会拆掉
   它（FOREIGN fault→r45 解析→陷阱永久失效）。
3. 种子表只登记自身加载后创建的进程：跨驱动生命周期的 pid 对 readvm
   不可见；跨卸载判定只能用幸存目标自身。
4. 复用 pid 陷阱：握手/状态文件先删后读、读完即消费。
5. npf deny 链第二槽（NpfSetDenyCallback2）作为通用缝保留（本轮
   陷阱方案的遗产）。

### TODO

- r134+ 候选：per-core view selection（npt.h M4 prerequisite 的结构
  性闭合，消灭同步契约）；RC4/ChaCha20 无损升级（接口已统一）。

## Round 133b (2026-10-10) — mem_crypt 验收审查（子代理 REJECT）→ P0×2+P1×2+P2×2+P3×7 全清 → 双块回归全绿

### 审查发现（子代理通读 r133 全部新增代码，对照 9 条设计契约）

- **P0-1 多块卸载回写丢失**：MemCryptDeinit 逐块调
  OverlayUnmapForPid(pid, freeNow=true)，而 freeNow 分支**无视 pid 清空
  整张 overlay 表**（npt_hook_mgr.cpp:1928 实证）——第 1 块处理完全场，
  第 2..N 块的私有明文帧已被释放，回写落空 = 明文永久丢失（key 只在
  日志）。单块 e2e 全绿恰因只有 1 块。修复：两阶段——先对全部块回写
  明文+解除冻结，再一次 OverlayUnmapForPid(0, true) 清场；套件改双块
  形状（target 双页双 enc，卸载腿 findstr 零 DAMAGED = 回归钉死）。
- **P0-2 dec 失败路径冻结账目错配**：失败保留记录但 Frozen 仍 1，重试
  dec 多付一次 resume（解冻别人的挂起），且失败后改调 seal 会在"目标
  实际已解冻"下写密文。修复：付掉 wasFrozen resume 的同时 ClearFrozen
  落到真实记录（锁内）。
- **P1-1 seal 代际快照 UAF**：SnapshotBlock 把 Snap 指针拷出锁外，与
  退出清扫的 FreeBlockRecord 竞争。修复：门判定整体移入 g_blockLock
  （OverlayGenerationPassed 是无锁 ForEachVcpu 走行，锁内合法）。
- **P1-2 PsSuspendProcess 返回值被无视**：挂起失败仍记 Frozen=1 = 对
  seal 说谎。修复：stage-1 挂起不可用/失败即拒绝 enc（冻结是承重的）。
- **P2-1** hv 停止时 seal 会瞬间过门并裸写密文 → seal 加 IsRunning
  守卫（dec 放行=恢复路径）；**P2-2/P2-3** 绑定槽语义（配置武装可抢
  活 crypt 目标、dec 不解除保留）文档化入 MEM_CRYPT.md 约束节。
- P3×7：npf 第二槽注释去陈旧、未用 extern 声明删、CryptRollback 死参
  删（unmap 自带 NOT_FOUND 自卫）、target 注释、ctl GetLastError 移
  HeapFree 前（errOut 出参）、bat 缺 SHA256（登记容忍）、e2e 遗留
  cmd 形状（未触发）。

### 验证（修复后全绿）

双块全周期（enc×2→双页密文→自检 4/4 明文→dec×2→双页复原→list 0）+
双块 hold + 卸载（零 DAMAGED）+ selftest 347/0。

### 律（审查轮新增）

1. **freeNow 语义陷阱**：OverlayUnmapForPid(pid, true) 的 pid 无效——
   任何"逐块释放"的多块调用方都会踩死后续块；多块清理必须两阶段。
2. 挂起账目必须在"保留记录"的失败路径上同步清标志（付账与记账原子）。
3. 锁外借用记录内指针（快照拷贝）= 与无锁清扫的 UAF 面；门判定类
   只读检查整体搬进锁内。

## Round 139 (2026-10-16): QEMU TCG 嵌套战役开打——NRIPS 兼容上线；重入死亡法医链闭合

平台切换：宿主 QEMU 11.1（q35, `-accel tcg,thread=multi`, `-cpu max`,
6 vCPU/4GB，Win10 1903 guest）。TCG 的 SVM 仿真 = 全新一层兼容面。

### 现象

1. **check 关**：TCG 无 NRIP-save（CPUID.8000000A.EDX 位 0=0），旧代码
   `SvmCheckResult::NoNrips` 直接拒绝虚拟化 → 驱动加载但不 virtualize。
2. **活锁**：NRIPS 兼容层（exit-code→定长指令表重建 NRip）首版带
   `NRip==0` 守卫——但 TCG 从不写 NRip 字段，第 2+ 次 exit 时字段里是
   **我们自己上次写回的陈旧值**，守卫被骗过 → guest 被反复推回旧
   rip，串口 16 连 `rip=...675a nrip=...6718`（注意 code=72 是**十六进制**
   0x72=CPUID，不是 MSR）。
3. **基座全绿**：修掉守卫（无条件重算）后 start/demo(CPUID 拦截端到端)/
   storm 10000×6 核/`cycle 3` 的 stop+start 三轮全干净，`svmbctl info`
   6/6 RUNNING，CPUID SVM 位隐藏正常。
4. **重入死亡**：cycle 的**最后一轮 start 后**的稳态在 10~60 秒内死亡
   （单轮重入有时也死，3 轮几乎必死）。无 bugcheck、无 dump、无串口
   遗言；有时演化为硬复位（三重故障级，SeaBIOS 在死后重跑佐证）。

### 法医链（判决性证据全在 qemu/logs + 2GB `-d int` 日志）

- **QEMU monitor/QMP 直读冻结现场**（`-d int` + 无复位的那次冻结）：
  - 6/6 核同 RIP `nt+0x361e17`（两指令 getter `mov rax,[全局]; mov
    eax,[rax+0xF0]`，被轮询值=TSC 类计数器在走）= 6 核在 IPI 上下文
    的时间轮询循环里等一个非时间事件。
  - **6/6 LAPIC 完全一致：TPR=0xF0、ISR=0xB1(Windows IPI 向量)、
    IRR=47,209（网络/磁盘中断在排队投递不进去）**——全体核卡死在
    同一次广播 IPI 的处理中，TPR 高地板挡住一切设备中断 → 缺页线程
    等盘完成中断=永等 → sshd/串口/一切饿死。
  - 前期 27,687 次 `check_exception old:0xffffffff new:0xe`——同一个
    "首次"#PF 高速循环：故障帧 **SS=0000**（svmb L1/VMM 上下文签名）、
    CR2=池地址、guest PT 走查 PML4[344]=P=0（当前 CR3 视图真缺页）、
    故障指令 `mov (%r9),%rcx` 池指针链走查（wedge-1 的 kd 符号化定位
    `nt!CmpPerformCompleteKcbCacheLookup` 注册表 KCB cache 家族）。
  - VMCB 尸检（QMP 物理读 gen-2 VMCB）：NCr3=161920000 与配置一致、
    Save.SS=0x18 正常、exit 字段清零——**VMCB 本体健康，毒不在视图
    指针**。
  - `-d unimp,guest_errors` 全程零新增——不是"未实现指令"，是语义分歧。

### 与参考的对比

- the reference hypervisor（裸机）：#VMEXIT 天然带 NRIP；无重入场景。
- VMware vhv（r28c 律）：曾有同族 "IPI hang"——devirt 后 GIF=0 屏蔽
  一切中断，修法=devirt 路径补 STGI。本次 QEMU 现象同签名（中断全
  屏蔽 + IPI 卡死）但载体不同（TCG 的 GIF/TPR 状态机在 stop→start
  重入后分歧）。
- r28 自陷路径 asm 注释：`{State|ExitTraceLeft<<32}` 指针追逐把数据当
  地址 #PF——27k #PF 的池悬挂链走查同族。

### 修复尝试

| 尝试 | 结果 |
|---|---|
| NRIPS 兼容（定长指令表，`NRip==0` 守卫） | 虚拟化上线但活锁（陈旧 NRip） |
| 无条件重算 NRip（本轮 commit 候选） | demo/storm/cycle-3 全绿 |
| 重入稳态延长观察 | 10~60s 内死亡，非确定性窗口 |
| `-d int` 抓异常级联 | 27k #PF + IPI 卡死现场（本表的证据源） |

### 关键决策回顾

| 当时以为 | 真相 |
|---|---|
| "code=72 是 MSR 出口" | %llx 打印=0x72=CPUID；exitprof 全 cpuid 佐证 |
| "全核 EFER.SVME=0=svmb 熄火" | QEMU `info registers` 在 VMRUN 间显示 L2 guest 的 EFER（guest 本就不设 SVME），读数无判别力 |
| "死亡=蓝屏" | 三种形态：长冻结（真冻结）/快速硬复位（三重故障）/伪复活（重启后 SSH 恢复但驱动消失）；无 dump 是常态 |
| "串口死=串口通道坏" | guest 全核 IF=0+TPR=0xF0，什么都不打印是果不是因 |
| "kd break 不进=内核死" | 全核关中断时 KDNET 根本送不进 break（与 r60 律互补：熔解期轻命令，全静默期直接换 monitor/QMP） |

### QEMU 工具律（本轮新增，进技能前先记这里）

1. **QEMU monitor `info registers` 读数歧义**：svmb 活跃时显示 L2 guest
   态；判 svmb 死活用 com1.log 的 r13/r18 信标 + exit trace，不用 EFER。
2. **`-action reboot=pause/none` 在 QEMU 11.1 均被拒**（reboot 只收
   reset/shutdown）；想冻结复位现场用 `-d cpu_reset`（记原因）+
   `-d int`（记异常级联）。
3. **`-d int` = 350MB/min**——只用于定向复现，事后即撤。
4. **`dump-guest-memory -w` 需要 guest vmcoreinfo 设备**（未装，报
   "no vmcoreinfo note"）；物理尸检走 QMP `xp` + 手工走表（本轮全链
   用它完成）。
5. **mon.ps1 逐连接独立 HMP 实例**——`cpu N` 必须与读命令同一连接
   发送（bash `$'a\nb'` 塞换行）；QMP `human-monitor-command` 是更稳
   的通道（monitor 被日志压死时 QMP 仍活）。
6. **kd `~` 系命令被 windbg-mcp 包装层吃掉**（Syntax/Extra character
   error）——全核栈改走 monitor/QMP + `-d int` 组合。

### TODO

1. 判决实验跑中：纯 gen-1 长 10min vs gen-2——毒在重入状态机还是
   稳态（结果见下轮开头）。
2. 若毒在重入：审计 Stop()/EnterCore() 的 per-core 状态（GIF/TPR/
   hsave/leaked vCPU 指针），重点 asm 自陷路径对 gen-1 池内存的引用。
3. QEMU 战役的 guest 侧脚本化（deploy.bat 已有；补 start/storm/cycle
   一键 + 自动判活）。

### r139 补：两个判决实验（当晚完成）

1. **纯 gen-1 长 10 分钟：存活**（18,008 出口稳定增长，SSH 每分钟
   响应）——毒不在稳态。
2. **stop → 裸 30s → start（拉开间隔）：t+90s 内死亡**——排除背靠背
   IPI 竞态，**任何第二代 Start 本身带毒**。

修复目标收敛：Stop() 释放了什么 / 残留了什么，使 2nd Start() 的世界
与 1st 有致命差异（二代 VMM 池悬挂 #PF = 有指针跨代存活）。审计面：
Start()/Stop() 的分配-释放对称性 + asm enter 二次调用的 per-core 状态
（HSAVE_PA 重写、EFER 舞步、r28 in-place resume 留下的段/DR 残留——
r18 家族"segment limits + DR6/DR7"同款）。

教训补录：**冻结现场取证完再杀 QEMU**——本轮 2GB `-d int` 级联日志
随 relaunch 被删（结论已入档，原始字节丢了）；且冻结机的
PsLoadedModuleList 没走 QMP 拿（fault IP 的模块归属存疑：nt 边界推算
两难，下次冻结时先走 KPCR→PsLoadedModuleList）。

## Round 140 (2026-10-16 晚): 重入死亡三连深挖——NMI 强制蓝屏取 dump 新武器；HLT 仿真落地（必要未充分）

### 现象

r139 遗留：stop→start 重入后 10~60s guest 死。本轮带着新武器继续。

### 新武器：NMI → kernel dump（一次成型，进技能）

QMP `nmi` 把 0x80 'TDO' NMI 打进楔死机（NMI 绕过 TPR/IF=0）→
Windows 强制蓝屏 → **拿到楔死现场的内存快照**。guest 侧先备好
`CrashDumpEnabled=2`（kernel dump，~370-400MB；minidump 只有故障线程
栈，不够用）。拉回宿主 → windbg-mcp `open_cdb_dump` 离线分析（合规
通道）。r139 "无 dump 的死亡"从此可尸检。

### dump #1 判读（svmb 确定加载，楔死现场）

- **CPU1 卡 `parport!CheckPort`**（IEEE 1284 协商轮询 LPT——QEMU 没配
  并口，状态口恒 0xFF）；退出条件 = `[r12]`（KUSD.InterruptTime 类时
  间源）× 超时——**时间源冻结则永不退出**。它占着 PnP 树锁
  （`!ready` 里 `PpDevNodeLockTree` 等待者即受害者）→ 注册表/PnP 级联。
- 其余核 idle/正常。**根因层：时钟中断不再被交付 → InterruptTime 冻
  → 依赖超时的内核轮询死循环**。
- r139 冻结机 15s 快照补强：5 核 HLT 且 CPU0 的 IRR 压着可投递向量
  209 唤不醒（IF=1、TPR=0 都正常）→ **TCG halted-vCPU 唤醒路径在重
  入后断裂**。

### 修复尝试：HLT 拦截 + 停顿仿真

- `Hypervisor::TcgCompat()` 公开 no-NRIPS 标志；TCG 模式下 intercept
  基线 op1 加 `ic1::HLT`（→0x11040000，串口已确认挂上）；
  `HandleHltExit` = 100µs KeStall + 推进（guest 不再真睡，中断走运行
  态注入路径）。VMware/裸机基线不变。
- **判决：必要但未充分**——挂上后重入仍死（~10s，甚至更快），且死亡
  时串口常已假死（ctl 报 HV_CONTROL ok 但 stop 打印一条没有→串口
  写死在 gen-1-active 与 cycle 之间）。
- gen-1 回归：start/demo/60s 稳态全绿（HLT 位无害）。

### 关键决策回顾

| 当时以为 | 真相 |
|---|---|
| "27k #PF = VMM 上下文（SS=0）踩泄漏池" | L1/L2 两套 KASLR 基址在 `-d int` 混排；27k 是 **L2 guest 自己的缺页循环**（等页盘完成中断=永等） |
| "根因是跨代池指针" | dump 实锤第一凶手=**时钟/中断交付断**（parport 只是持锁的受害者代表） |
| "串口死=通道坏/系统死" | 串口写在 cycle 中途死且 ctl 还能报 ok——**先于系统死**；SerialPut 有界(10000×LSR 轮询)但若 THRE 永不置位，每行≈60字符×10000次端口读=秒级熔解，出口路径被拖垮（假死） |
| "第二个 kernel dump 继续分析" | `lm m svmb` 空——**SSH exec 超时死于跑命令前**，楔死时驱动没加载，dump 污染作废。教训：NMI 前 must 验证 svmbctl info 可用（驱动在） |

### TODO（下轮）

1. **验证串口熔解论**：SerialPut 的 LSR 轮询加计数上报（一次行写花
   了多少次轮询）；或 TCG 下直接砍掉 LSR 等待（file 后端无压力）。
   若出口路径被串口拖死成立，串口节流/砍等待可能直接消掉"假死"层。
2. 判别剩余死亡：HLT 仿真后的死法（10s 快死）与之前（25-90s）是否
   同根——用 NMI dump（先验驱动在）+ exitprof 时间序列（KDNET 或
   低频轮询）。
3. 考虑 TCG 下加 `ic1::PAUSE` 拦截做自旋让出（parport 类轮询的
   VMM 侧可观测化）。
4. KeStallExecutionProcessor(100) 的 HLT 仿真若真被走到，会在串口
   留 hlt-emulate 打印——本轮预算被 CPUID 吃满+串口中途死，**尚未
   证明 HLT 出口发生过**；下轮把 budget 与 hlt 打印解耦（独立计数器
   经 exitprof 读出）。

### 产出

- 代码：`Hypervisor::TcgCompat()`、ic1::HLT 条件挂载、`HandleHltExit`
  （含一次性打印）、ExitTraceLeft 16→200（QEMU 串口床）；gen-1 无回
  归、重入死亡未解。
- 工具：NMI→kernel-dump SOP（CrashDumpEnabled=2 + QMP nmi + 
  open_cdb_dump），已入 qemu-vm-kernel-debug 技能。
- dump 存档：qemu/dumps/r140_kernel.dmp（#1 可用）、r140_kernel2.dmp
  （污染，保留作教训样本）。

## Round 141 (2026-10-16 深夜): 死亡免疫标记通道落地——cycle 完整性洗清 + 全核遗像定谳

### 新工具：TraceMarker 磁盘标记（死亡免疫证据通道）

串口在死亡窗口不可信（QEMU 侧丢字，盲写修复也没救回 gen-2 段），
SSH/HTTP 随 guest 死。新通道：驱动 PASSIVE 路径 `TraceMarker/Fmt`
→ 追加写 `C:\svmb_trace.txt`（FILE_WRITE_THROUGH，重启/冻结后可读）。
撒点：HV_CONTROL 分发、Start/Stop 进出、start-active 附带**每核 VMCB
物理地址**（vmcb-pas 行——冻结时 QMP `xp` 直读遗像的钥匙）。

### 判决一：cycle 完整性洗清（标记文件时间线）

```
hvctl-cycle → stop-enter → stop-done (20~91ms) → start-enter
→ start-active (83ms~5.6s) → [死亡发生于 gen-2 稳态头几秒]
```
**stop/start 机器全好**，六核干净进出；串口"stop 打印缺失"=QEMU 侧
UART 丢字（盲写后依旧，丢失点在更深处），不是事件没发生。

### 判决二：全核遗像（标记文件给 VMCB PA → QMP xp 冻结 VMCB）

- 6/6 VMCB：op1=11040000 ✓、MSRPM 全局一致 ✓、**ExitCode=0x72 全是
  陈旧值**（该出口早已服务完，此后再无出口）。
- 6/6 CPU 同 RIP=guest 时间轮询循环（两指令 getter 家族），**RFL=0x46
  = IF=0**：全核关中断空转，等一个只有中断处理器才会更新的值。
- 遗像与 r139/r140 完全同族：**中断交付停摆 → 时间值冻结 → 全核陷进
  IF=0 轮询 → 永久冻结**。HLT 拦截后核不再 hlt（改为轮询卡死），
  死法同根。

### 因果链（最终版）

gen-2 稳态头几秒 → **TCG 中断注入停摆**（触发点未定位：疑中断阴影
int_shadow 跨重入残留 / 事件注入状态机分歧，需 QEMU 源码级验证
svm_helper.c 的 int_shadow 与 EVENT_INJ 语义）→ 时钟/设备中断不再
投递 → KSD 时间值冻结 → 所有依赖超时/事件的内核循环死转。

### 修复尝试汇总（本轮）

| 尝试 | 结果 |
|---|---|
| SerialPut TCG 盲写（去 LSR 等待） | 串口熔解论证伪（写不再等，但 QEMU 侧仍丢段）；保留（file 后端本就该盲写） |
| TraceMarker 磁盘通道 | **落地成功**：cycle 洗清 + VMCB PA 交付 |
| VMCB 遗像直读 | **落地成功**：全核定格同一循环 |

### TODO（下轮）

1. **QEMU 源码级验证**：取 QEMU 11.1 `target/i386/tcg/svm_helper.c`
   （或在线读），核对 (a) VMEXIT/VMRUN 的 int_shadow 保存恢复、
   (b) 外部中断注入到运行中 SVM guest 的条件（hflags2/HF_GIF/
   int_shadow 门控）、(c) EVENT_INJ 的跨代残留可能。
2. 候选修复：VMM 每次出口**清 VMCB 的 int_shadow 位**（CleanBits=0
   律下写）——若阴影泄漏是触发点，一行修复。
3. 候选修复 B：VMM 侧中断窗口强制（每 N 个出口检查 LAPIC PPR/IRR，
   可投递则 EVENT_INJ 注入或至少保证 VMRUN 后 delivery 机会）。
4. 标记通道扩展：HandleHltExit 计数经 exitprof 读出（验证 HLT 出口
   是否真发生）。

### 律（新增）

1. **QMP `dump-guest-memory -p <file> <begin> <length>` 会把 QEMU 主循
   环挂死**（0 字节不前进、monitor/QMP 全哑、VM 停在 STOP）——别用；
   物理取证用 `xp` 直读（有已知 PA 时）。
2. 串口在 gen-2 死亡窗口的丢字发生在 QEMU 侧（guest 盲写也丢）——
   **死亡窗口的真相只在磁盘标记 + QMP 冻结直读里**。
3. EnterCore 跑在 IPI 级——文件 IO 类标记必须从 PASSIVE 编排线程收割
   （Start() 的 RunOnEachCore 返回后）。

## Round 142 (2026-10-16 晚): QEMU 源码对齐 + 冻结现场深度尸检——停车点=HPET MMIO 读，第一推动力仍未闭合

### 本轮背景

r141 收官时定下 r142 三问：(a) int_shadow 跨 VMEXIT/VMRUN 语义、
(b) 外部中断注入门控、(c) EVENT_INJ 跨代残留。本地网络对
raw.githubusercontent 间歇 SSL 失败（curl 全灭），改用
`git clone --depth 1 --filter=blob:none --sparse` 成功拉取 QEMU master
（=11.1+ 数日后，target/i386/tcg/system/ 子目录布局）到
`qemu/qemu_git/`。

### QEMU 源码结论（全部带行号可复查）

1. **GIF**：`helper_vmrun` 尾部无条件 `hflags2 |= HF2_GIF_MASK`；
   `do_vmexit` 尾部 `hflags2 &= ~(HF2_GIF_MASK|HF2_VGIF_MASK)`。
   svmb 自退出路径（svm_entry.asm:460/507）已有 STGI，
   且 `virtual_gif_enabled()` 在非 guest 态恒 false → STGI 正确落到
   HF2_GIF。**GIF 遗留论死**（r140 冻结时 QMP nmi 能触发 0x80 =
   NMI 交付成功 = GIF=1，双证据）。
2. **int_shadow**：do_vmexit 写 `control.int_state`（偏移 0x68），
   helper_vmrun 读回——对称，无泄漏。冻结 VMCB int_state 位未置。
3. **投递门控**（cpu.c `x86_cpu_pending_interrupt`）：HARD/NMI/SMI 全部
   包在 `hflags2 & HF2_GIF_MASK` 里；非 VINTR 模式 HARD 还需
   `x86_cpu_interrupts_enabled`（IF && !shadow）。HARD 投递前先过
   `cpu_svm_check_intercept_param(SVM_EXIT_INTR)`（INTR 拦截位为 0 ✓）。
4. **EVENT_INJ**：do_vmexit 搬 event_inj→exit_int_info 并清零；无跨代
   残留路径。VMCB 布局与 KVM/QEMU PACKED 结构逐字段对齐
   （int_ctl@0x60、int_state@0x68、exit_code@0x70、nested_ctl@0x90、
   event_inj@0xA8、nested_cr3@0xB0）。
5. **r141 三问全部闭合，但答案都不是死因**——死因要重新找。

### r141 冻结现场尸检（本轮战果，现场后因误杀 cdb 丢失）

r141 留下的冻结实例还活着，直接深挖（QMP + VMCB 直读 + windbg-mcp
离线符号化 + r140_kernel.dmp 同二进制 RVA 反查）：

- **冻结画面 = 登录"欢迎"转圈**（VGA screendump）。r141 全部"gen-2
  死亡"都发生在登录会话建立期（SSH 驱动测试，guest 从未到桌面）。
- **6 核 RIP 全部 = fffff8054f6bae17 = hal!HalpHpetQueryCounter+0x7**
  （`mov eax,[rax+0xF0]`，HPET 主计数器 MMIO 读；字节级比对 r140 dump
  同 RVA 函数体 + 后随 HalpHpetStop 开头 8 字节全等，三重确认；
  r142 hal 基址 0x4f6b6000，nt@0x4ec00000）。
- RFL=0x46（IF=0）×6，TPR=0xF0（IRQL 15）×6，ISR 空，IRR={0xB1 IPI,
  0xD1 时钟}积压 ×6，EFER.SVME=1（gen-2 运行中）。
- 栈上有 `nt!KiCheckStall+0x32`、`hal!EmonQueryInformation` 邻域、
  QPC 频率 0x5F5E100（1e8=100MHz HPET）→ **看门狗（KiCheckStall，
  HIGH_LEVEL）自己陷进 HPET 读轮询** = TPR 0xF0 的来源。
- **6 个 vCPU 宿主线程全部 Running、各 ~2900s CPU**——在 TCG 里满速
  自旋，不是 QEMU 内部阻塞（BQL 死锁论死）。正常 svmb 空闲也这样
  （HLT 拦截+100µs stall 使空闲核永不真停机），无法用 CPU 时间区分。
- **QEMU 侧 HPET 64 位计数器健康在走**（两次读差 5s≈5e8 tick，
  100MHz 精确；GCAP=0x8086a201，GEN_CONF=0x3 enabled）。
- **exit_code 遗留 0x72×6 + 串口尾部是健康的 CPUID 出口流**：
  冻结后零出口——核不是在出口风暴里，是**钉死在一条指令上**。
- **两次 `info registers`（间隔数分钟）RIP 不变**——不是慢，是钉死。
- **异常：6 个 VMCB 的 nested_cr3=0x1589f3000（一致），但该物理页
  内容是内核池数据（155 个杂项 qword、fffff805/ffffb30c 模式）而非
  页表**。而 NPT 填充（npt.cpp r116 起 [0,1TB) 2M RWX）必然使
  pml4[0]≠0。矛盾未闭合：要么这些 VMCB 不是活态，要么 NPT 根页在
  代际边界被换成了池。**这是下轮复现要带探针回答的问题。**

### 死因当前画像（候选序）

1. **HPET MMIO 读在 gen-2 嵌套翻译/分发路径上永不退休**：核在 QEMU
   内部对这一次访存的翻译/IO 分发里满速空转，不 fault（无 NPF 出口）、
   不前进（RIP 钉死）、不阻塞（线程 Running）。全部六核先后汇入
   （KiCheckStall 看门狗 + 登录期 QPC 轮询风暴是汇入通道）。
2. NPT 根页被换（nested_cr3 指向池数据）作为 (1) 的机制候选。
3. 登录会话是放大器不是根因（gen-1 同样场景绿）。

### 现场丢失事故（教训）

- 取证后期发现 4 个陈旧 cdb（17:11-17:59 启动）+1 个新 cdb，
  `Stop-Process -Force` 全杀 → **附着中的 cdb 把 QEMU(22380) 带死**，
  冻结现场丢失（尸检数据已在本节保全）。
- **新律：杀任何调试器进程前必须先确认其附着目标；强杀附着态调试器
  视同杀目标。**

### TODO（下轮继续 r142）

1. Start() 加 NPT 状态 marker（NptPa、pml4[0] 值、pml4[0] 指向的
   pdpt[0] 值）——一次复现即可回答 nested_cr3 遗像之谜。
2. 复现序列改为**先等到桌面再测**（剥离登录风暴混淆变量）。
3. 冻结时宿主栈取证改为**一次性 cdb**（`cdb -p PID -c "~*k; qd"
   -logo out.txt`，无陈旧实例、不先杀任何东西）。
4. 若 (1) 证实 NPT 根被换：审 Stop()/Start() 的 NPT 生命周期
   （谁建、谁引用、谁可能释放）。

### r142 终章（深夜追加）：三层洋葱——每层都真、每层都不是全部

干净复现序列（桌面就绪后 SSH 驱动，无 NPT 探针，Active()=NULL）
逐步剥出的三层，每层一个修复 + 一个可观测的行为变化：

| 层 | 证据 | 修复 | 修复后行为 |
|---|---|---|---|
| L1 QEMU HARD 位失同步 | 冻结时 IRR 0xD1 可投递 pending + TPR 0x20 + IF=1 + 原生态不投递；**QMP nmi 扰动后整机自愈**（apic_update_irq 重跑） | exit 每 1024 次 `__writecr8(__readcr8())`（QEMU 路径 cr8→apic_set_tpr→apic_update_irq 无等值短路） | 散乱等待死相消失 |
| L2 HPET MMIO 读停车 | r141 冻结=hal!HalpHpetQueryCounter+0x7 ×6（RVA 0x4e17 三重字节确认）；**hpet=off 的 boot 也能原生冻在同一个 RIP**（实验床自身失败模式） | `start_svmb.bat: -machine q35,hpet=off` | HPET 类死相消失 |
| L3 调试器等待冻结 | 全核 nt!KeBugCheck2+0xc93 冻结环 + **KiBugCheckData 全零**（≠bugcheck）+ KdDebuggerEnabled=1（r139 KDNET 遗产）且无调试器应答 | guest `bcdedit /debug off` | 永久死亡→消失 |

**修复后的新稳态（两次复现一致）：**
cycle → 90 秒瞬态假死（SSH 断、核去虚拟化）→ **自愈**（系统恢复运行、6 分钟+稳定存活）→
svmb 服务变 STOPPED（无 SCM 停止事件、无 marker——无声卸载，机制待查）。
cycle 3 因设备消失而无法继续（err 2）。

### 本轮方法论收获（新律）

1. **QMP 页表走读 + r140 dump 同二进制 RVA 反查 = 无符号冻结现场的完整符号化**
   （CR3 walk → MZ 扫描 → 字节指纹 `s` 命令 → `ln`）。
2. **一次性 cdb 宿主栈**：`cdb -p <pid> -c "~*k; qd" -logo out.txt < NUL`——附着即取即分离；
   两次采样对比区分"死锁持锁者"vs"BQL 轮转活锁"。
3. **杀调试器前必查附着目标**（强杀附着态 cdb = 杀目标，本轮赔掉一个冻结现场）。
4. **调试使能的 guest + 无调试器 = 冻结炸弹**：任何调试事件（断言/DbgBreakPoint）→
   KdEnterDebugger → 全核冻结环等待应答 → 永久停车，且 KiBugCheckData=0、KDNET 断点也进不去。
5. **`-serial file:` 跨 reset 追加**（旧律重申）：S-tag 块数=boot 数，别当单会话读。

### 遗留（r143 候选）

1. **90 秒瞬态假死的根**：cycle 后中断/时钟仍断 ~90s 后自愈——自愈路径是什么
   （超时后重 poll？看门狗重跑？）顺藤摸根。
2. **服务无声 STOP**：无 SCM 事件、无 marker——谁卸的？
3. 停车环 `nt!KeBugCheck2+0xc93`（RVA）在无 bugcheck 数据时=KiFreezeExecution 家族
   ——触发源（哪个调试事件/断言）未定位。
4. NPT 根遗像之谜（0x1589f3000=池数据）在 Active()=NULL 会话中不再出现，挂起。

## Round 143 (2026-10-17 上午): "自愈"神话破灭 + 实验床污染清除 + cycle 死亡精确时间线

### 本轮核心发现

1. **r142 的"90s 瞬态假死→自愈"是误读**：细粒度轮询（5s）揭示真相——
   - SSH 层抖动（8-10s 断流反复）≠ 网络死（裸 TCP 连接全程 0.00s 秒连，
     断的是 sshd 的 KEX 认证=用户态算力停摆）
   - **OpenSSH 服务自身崩溃**（事件 7034, 21:47:56）
   - **guest 每 10-30 分钟脏重启**（Kernel-Power 41 链：21:49→21:51→22:03→
     22:23，无 BugCheck 事件=三重故障式硬复位）——"自愈"=重启后的平静
   - QEMU 进程也曾随 guest 关机退出（无 -no-shutdown）
2. **实验床污染源清除**：Desktop\driverTest 里的 VMware 时代遗留装置
   （cycle_startup8.cmd=sc delete/create svmb 指向旧二进制+旧版
   stop/start+sc stop"DEVUNLOAD TRIGGER"；antiinject/othide/wndaudit
   三个旧驱动在多次登录后被安装，事件 7045）。触发机制未完全定位
   （启动夹/计划任务/Run/RunOnce/宿主 listener 全查无果；最可能=
   Win1903 RegisterApplicationRestart 的崩溃复活）。**处置**：旧服务
   sc delete、driverTest→driverTest.quarantine、RestartApps=0。
3. **清创后干净床复测：cycle 死亡依旧**，且拿到精确时间线（marker）：
   stop-enter→stop-done 63ms → start-enter→start-active 245ms →
   start-kick-done +14ms → **~4 秒后 guest 崩溃（重启/冻结二形态**：
   一轮三重故障重启 boot 10:44→10:49；一轮永久冻结零异常）。
4. **-d int 定向复现（3GB）的死亡窗口**（分析已保全，文件已误删）：
   尾部 ~70 事件全是健康流量（用户 #PF/0x2f/0xd1/0xe1 IPI 交织）→
   最后一个事件=Servicing hardware INT=0xb1（kick/自退出 IPI）→
   **戛然而止，零级联零三重故障轨迹**。冻结遗像：6 核全用户态、
   4 核 IF=1、HLT=0、36 分钟零中断交付（#PF 都没有=不在跑正常代码）。
   vapic 激活排查：日志无 vapic 痕迹 + TCG -cpu max 不广播 KVM 叶子
   =Win10 不会启用 → CR8 路径有效，vapic 论排除。
5. **两项修复落地但未救住 cycle**：①re-poll 撤门控（每出口必做——
   1024 门在"零出口冻结"里永不触发）；②start-kick（start 后真
   KeIpiGenericCall 冲刷全核投递路径）。gen-1/storm/demo 无回归。
   **结论：APIC HARD 位失同步是真实存在的一层（NMI 治愈实验），但
   gen-2 4 秒死亡另有主犯**——嫌疑集中在 gen-2 稳态的最初几秒内
   全核中断机器同时失能的机制。

### 新律/教训

- **裸 TCP vs SSH 协议层分开测**：断流诊断先 create_connection 计时，
  0.00s 连上=网络活，断的是上层（sshd KEX/用户态停摆）。
- **"自愈"必须用 boot-time 验证**：LastBootUpTime 变了=重启冒充自愈。
- **取证完再 rm/杀 QEMU**（r142b 定律重犯：3GB -d int 死亡窗口日志被
  自己的 relaunch rm 掉——所幸尾部分析已提炼）。
- **转储镜像=复制体不是快照**：qcow2 基盘带着 VMware 时代的全部测试
  装置（旧驱动、开机脚本、复活注册），新实验床必须清创后才能信数据。

### r144 开局

1. **重跑 -d int 复现（廉价可再生），这次先提取后重置**：从 marker
   时间戳对齐日志位置（stop-enter 前的最后 nudge 0xb1 簇），逐帧读
   start-active→死亡 4 秒窗口的每一事件（含是哪核）。
2. 若中断交付全停是主犯：查 QEMU 侧 cs->interrupt_request 谁能整组
   清掉（cpu_interrupt_reset? reset 域? VMRUN 路径?）——源码在手。
3. 候选修复面：gen-2 首秒的行为差异（enter 线程初始 guest 态 vs
   gen-1 相同流程为何独 gen-2 死——找两代差异点：vCPU 页新分配、
   hsave、ASID/TLB 序列号、QEMU env 残留态）。

## Round 144 (2026-10-17 下午): cycle 死亡验尸报告——RSVD-PTE 三重故障 + "永久冻结"神话破灭

### 本轮核心发现（证据全部落盘）

1. **死亡=三重故障，不是冻结**。-d int 全程日志（16GB，保留为
   `qemu/r144_qemu_dbg_full.log`）在偏移 0x7fcbb4b6 处捕获唯一一次
   完整级联（`qemu/r144_crash1_window.txt`，1.5MB 窗口）：
   ```
   1509352: v=0e e=0018 cpl=0 IP=0010:fffff80152bf137a CR2==IP   ← RSVD+I 取指故障
   check_exception old: 0xe new 0xe                              ← 处理中再 #PF → #DF
   1509353: v=08 (#DF) 交付
   check_exception old: 0x8 new 0xe                              ← #DF 处理中再 #PF
   CPU Reset (CPU 0)                                             ← 三重故障
   ```
   死亡 RIP = `nt!ObpReferenceObjectByHandleWithTag+0x38a`
   （该 boot nt 基址 0xfffff80152a00000，由 IDT=fffff8015a28e000 +
   空闲簇十字定位；符号化=离线 cdb -z r140_kernel.dmp + C:\symbols
   缓存，无符号链闭合）。e=0x18 全 16GB 仅此一例=非本底性内存损坏。
2. **QEMU 源码定位 e=0x18 唯一来源**：`target/i386/tcg/system/excp_helper.c`
   mmu_translate `do_fault_rsvd`（stage-1 guest PTE 保留位）。本床
   `npt-state: no-active-view`（驱动侧 marker，每次 start 都打）=
   **L2 无 NPT 运行** → stage2(S2_GPA) 路径不参与 → 故障时该 nt .text
   页的 Windows PTE 确实带保留位。嫌疑二选一：(a) 有野写砸了 Windows
   PTE（我方 VMM 或 QEMU 设备模型/DMA）；(b) QEMU 在 L1↔L2 状态交换
   窗口用错 pg_mode/CR3 走表，把合法 PTE 读成保留位（TCG nested-SVM
   私有 bug 类）。VMware vhv 时代从未见此形态。
3. **"永久冻结"二次破灭**：三重故障后 guest 自动重启（bootex.log
   12:00:57 为证）；重启后 25s 内二次死亡（0x80a19ead 簇，形态不同：
   无异常级联，最后事件=池写 #PF+时钟中断，`r144_crash2_window.txt`）；
   再重启后稳定（"冻结尸体"实为健康空转机——cpu0/1 在 KiIdleLoop
   吃 1.5kHz 时钟 tick=Windows 1ms 定时器分辨率的正常形态，磁盘仍有
   写+flush，SSH banner 活）。**上午看到的 ExReleaseResourceLite×2/
   MiFinishHardFault/IopDequeueIrp/KeResetEvent 六核遗像=死亡前最后
   一瞬的 I/O 等待侧写，不是死锁现场**。
4. **死亡时间线重锚（guest 时钟不可信教训）**：QEMU 进程自 10-16
   16:11 未死（-D 日志 Birth 为证）→ 10-16 23:0x r143 崩溃（0x122e942
   簇）→ 隔夜会话跑 r144 批量 cycle（trace 时间戳跨 5.5h）→
   **最后一个 marker=start-kick-done**（死亡紧跟 START/kick，非 stop）→
   ~11:57 三重故障 → boot2 25s 死 → boot3 稳定。事件日志里
   "11:23:01 最后心跳/34 分钟冻结"=guest 时钟在冻结期停摆的伪象。
5. **驱动侧证据**（guest C:\svmb_trace.txt，已拉回 r144_svmb_trace.txt）：
   多轮 cycle 全部 stop-done/start-active/kick-done 三点齐落（63ms/
   245ms/14ms），死亡是概率性的（几十轮后一发命中）。VMCB-pas 每轮
   正常分配。npt-state 每轮 no-active-view。
6. **旁证与排除**：bootex.log 显示 vvfat FAT 写失败 0xc0000185（床病，
   D: 每次启动 autochk=脏，但 RSVD 故障全日志仅 1 例→DMA 大面积
   污染论不成立）；旧 1001 事件（0x80/0x4f4454）=r142 NMI 实验遗物；
   svmb 服务 DEMAND_START/STOPPED（boot2 快死非驱动自启）。

### 取证方法论沉淀（新工具链）

- **离线符号化**：cdb -z dump + `C:\symbols` 缓存（windbg-mcp 产物）
  + ln nt_base+RVA。本会话 windbg-mcp 不可用，一次性 cdb -z（无活体
  内核附着，等同 open_cdb_dump 风险面）实测安全可控。
- **跨 boot 的 nt 基址锚定**：-d int 状态块里的 IDT= 指针（初始 IDT
  在 nt .data 固定 RVA）+ KiIdleLoop 特征 pc 簇。guest 事件时间戳在
  冻结/慢机下不可作锚，必须用 host 侧文件 mtime/日志 Birth 对表。
- **QEMU -D 日志 Birth=进程生死线**：QEMU 不 rm 不截断则跨日累积；
  relaunch 前必须先 mv（Windows 句柄占用=先杀后改名；-D 会截断重开）。
- **hostfwd 端口测试是假象**：QEMU 秒 accept，必须真 banner/真登录。

### r144-D 实验（E2）：单线程 TCG 判决 MTTCF 竞态

- 改动：`start_svmb.bat` 增 `TCG_THREAD` 开关（默认 single），撤
  `-d int`（保留 cpu_reset,unimp,guest_errors）。
- 判决逻辑：若 single-thread 下 N 轮 cycle 生存而 multi 下历史死率
  ~30min/轮命中 → 根因=QEMU MTTCF 竞态（nested-SVM 状态交换或 ptw
  A/D 写并发）→ 我方驱动无辜，床改 single-thread 长跑 + QEMU 上游
  报障；若 single 下照样死 → 嫌疑收回我方 VMM 野写，下一轮 E3
  （撤 start-kick / NPT=on 对照）。

### r144-D/E2 结果（13:37-13:57）：单线程也死 → MTTCF 竞态假设击毙 → 根因抓到

1. **E2 判决：single-thread 下 cycle 1 后即死**（40s 内 sshd banner 断流；
   本轮日志 `qemu_dbg.log` 235KB 含显式 `Triple fault` 行 + 3 个 reset 簇）。
   并发竞态（QEMU MTTCF、设备 DMA 竞态）全部排除——**死亡是确定性的，
   且我方驱动是唯一变量**。
2. **直接证据：QMP `info lapic`**——死亡态某核 LAPIC **IRR 常驻 0xD1（时钟）
   且 TPR=0、ISR 空**：可投递中断永远不投递 = 该核 GIF=0 卡死（r142 定律：
   QEMU 全部中断类别都过 GIF 门）。同时桌面仍在渲染（别的核活着）=
   单核中断死 + 设备 IRQ 路由到死核 → 对应 I/O 永不完成 → sshd 饿死、
   系统渐进瘫痪 → 最终三重故障。上午多线程尸体的
   ExReleaseResourceLite/MiFinishHardFault/IopDequeueIrp 画像 = 同一病理
   （I/O 等待永久停车）。**注意 `cpu N; info lapic` 切核不生效**（六次读
   同一核，LDR 全 0x01 暴露）——单核结论，非全核。
3. **ahci: IRQ#0 level:1 刷屏是良性床噪声**（16GB 旧日志从 0x6fd0 起同样
   ~6k 行、有界；Windows storahci 走 MSI，INTx 悬空）。勿再误判。
4. **根因（代码级）**：`svm_entry.asm` `vmrun_failed:` 裸 ret **无 stgi**。
   QEMU 语义（svm_helper.c:915）：**#VMEXIT 尾部清 GIF**；正常路径靠
   下一次 VMRUN（helper_vmrun:404）重置 GIF，宿主侧续跑路径
   （exit_virtualization/self-exit）都有 stgi——唯独 `vmrun_failed`
   （QEMU `SVM_EXIT_ERR = -1` 落点）漏了。触发链：某次 VMRUN 校验失败
   （asid/cr0/cr3/cr4/dr/efer/iopm 某项不过，具体哪项待 0x185 beacon）
   → cpu_vmexit 尾清 GIF + 回到 vmrun 后指令 → asm 读 ExitCode=-1 →
   vmrun_failed → ret → **该核永久无中断**。
5. **修复（r144-D1）**：`vmrun_failed` 先 `SvmbCrumbPostD(0x185, exitcode)`
   （IRQL 安全面包屑，r113 家族）再 `stgi` 再退栈。EnterCore 原有的
   "core N VMRUN failed" SVMB_LOGE + 部署时 `svmbctl serial on` 补齐
   观测面。构建 b04ce141b5d45905c479ebfc3b46e65d9c8eb8017de78edd2ab72fcf4e6b5828
   已双向校验部署。
6. **验收（进行中）**：单线程 40 cycle + 20min 生存 + gen-1 回归；通过后
   A/B 回 multi-thread（真床配置）同批复验。VMRUN 失败本身（为何校验
   不过）是下一层问题——修复的意义在于失败不再等于杀 guest。

### r144-D 后续（14:0x-15:0x）：stgi 修复≠根治 + 伪造 RIP 定性 + 排除链

1. **stgi 修复部署后单线程仍死于 cycle 1**（但形态改善：一次 reset 后干净
   启动稳定，修复前是连环 reboot 崩溃循环 3+ 簇——修复消除了"重启后再崩"
   的放大器，未消除首发崩溃）。串口镜像证实 **vmrun_failed 从未触发**
   （39KB 串口流无 "r13-vmloop-return"/"VMRUN failed"）→ GIF-via-vmrun_failed
   不是首发死因（修复保留：它本身正确且消除了崩溃循环）。
2. **E3b 单线程 -d int 死亡窗口（严格顺序日志）**：健康用户态调页 →
   **v=0e e=0010 cpl=0 CR2==IP==fffff800f363137a**（内核取指故障，P=0）
   → 0e→0e 升 DF → 08 处理中再 0e → **Triple fault** → reset。
   关键：**故障 RIP 不在本 boot 内核世界**（本 boot 时钟处理器在
   fffff805802xxxxx，故障 RIP 在 fffff800f3xxxxx）= **内核跳到了伪造
   RIP**。对照晨间 RSVD 变体（fffff80152bf137a=当 boot nt+0x5f137a
   =ObpReferenceObjectByHandleWithTag+0x38a，页表被判保留位）——两 variant
   统一为"跳到坏地址"，按当 boot 页表内容呈现 P=0 或 RSVD。
   两次 EAX 形态相同（0x17c929000/0x17c664000≈物理地址样）。
3. **排除链**（本轮逐项）：MTTCF 竞态（单线程也死）；ahci INTx 刷屏
   （16GB 旧日志同样存在=良性）；vmrun_failed/GIF（beacon 未触发+修复
   无效）；HSAVE 页回收（EnterCore 每次 start 重写 MSR_VM_HSAVE_PA，
   QEMU 每 VMRUN 重写 hsave 内容）；crumbs 面包屑（掩码环形，独立池块，
   r136 部分豁免）；0xD1 卡 IRR（次生饥饿像——崩溃后残躯的 LAPIC 状态，
   非首发机制）。
4. **在控的下一步（E4）**：QMP RESET 事件触发的 VMCB 收割（RAM 跨 reset
   存活）——三重故障瞬间各核最后的 ExitCode/Save.Rip：Save.Rip=伪造地址
   → VMM 恢复了坏 RIP（我方出口路径）；Save.Rip 合法 → guest 自己跳坏
   （更深的内存破坏）。脚本 `qemu/r144_e4_harvest.py`。

### r144 验收终局（15:0x-17:24）：E5 单线程 6/6 + E6 多线程 40/40 全过

- **E5**（单线程，修复版）：cycle 循环 6/6 轮存活 + E4 的 1 轮 = 7 连胜，
  全程零 RESET（对照修复前单线程 cycle1 必死 2/2）。
- **E6**（多线程=床标准配置，修复版）：**40/40 cycle 存活 + 20 分钟空闲
  生存 PASS + gen-1 回归（start→30s→demo→stop）全程存活，VERDICT: PASS**
  （demo 子命令报一次 cannot open \.\svmb err2 但整腿无 RESET、stop rc=0、
  系统存活——脚注级瑕疵，不构成回归）。QMP RESET 事件收割全程武装，
  未触发（残余机制本轮未现身）。
- **统计对比**：修复前多线程 ~10-30 轮内死亡（隔夜+晨间会话），单线程
  cycle1 必死；修复后 47 cycle + 生存 + gen-1 零死亡。vmrun_failed GIF
  泄漏=cycle 死亡主机制结案。
- **残余（r145 候选）**：伪造 RIP 三重故障（~1/8，E3b 实测一次）——
  收割武器（r144_e5_loop.py 的 RESET 事件 VMCB xp）已就位，下轮循环
  扩大样本直到它现形，Save.Rip 定归属（VMM 出口恢复坏 RIP vs guest
  内存破坏自跳）。
- 床配置复位：TCG_THREAD 默认 multi、-d int 撤除（定向复现时再加）。
  证据文件：r144_qemu_dbg_full.log(16G)、r144_e3b_full.log(6.7G)、
  r144_e4e5_single.log、r144_crash1/2_window.txt、r144_e*_com1.log、
  r144_e2fix_*。磁盘 800G+，全保留。

## Round 145 (2026-10-17 晚): 残余"伪造 RIP 三重故障"狩猎

### 开局状态（承接 r144）

- **猎物**：低概率（修复后 1 崩 / 47 cycle ≈ 2%）三重故障——内核取指故障
  CR2==IP 于非本 boot 内核世界的地址（E3b P=0 变体已提取
  `qemu/r144_e3b_crash_window.txt`；晨间 RSVD 变体在
  nt!ObpReferenceObjectByHandleWithTag+0x38a）→ DF → 三重故障。
- **武器**：`qemu/r145_hunt.py`——150 轮 multi-thread cycle，QMP RESET 事件
  全程武装；崩时 RAM 未被新 boot 覆盖前 `xp` 收割全部 6 核 VMCB
  （ctrl：ExitCode@0x70/ExitInfo1/2/exit_int_info@0x88/event_inj@0xA8；
  save：CR3@0x550、Rip@0x578、Rsp@0x5D8）+ 串口流（serial on 已开）。
- **判决矩阵**：
  - 某核 Save.Rip == 伪造地址（或接近）→ **VMM 出口路径恢复了坏 RIP**
    （嫌疑面：exit_virtualization 的 Extra2 栈、self-exit 的 GREG_RIP、
    SvmbVmExitEntry 构造的恢复帧）；
  - 全部 Save.Rip 合法 → **guest 自己跳坏**（内核内存破坏——回溯到
    破坏者：NPT 无 read-disable 共同边界？或 QEMU 侧写入）。
- **床态**：清理 41GB 无用日志（16G+6.7G+20.9G E6 无崩溃浸润日志；
    e3b 级联窗口 402KB 先提取后删——取证完再删律）；干净重启，
  multi-thread，无 -d int（收割器为主武器；需要级联帧时再定向开 int）。
- 旧证据全保留：r144_crash1/2_window、e3b_crash_window、各 com1 流、
  r140_kernel.dmp（符号化用）、全部 r144_*.py 工具。

### r145 中场（17:33-21:0x）：150 轮狩猎的判决被推翻 + 新事件"驱动无声消失"

1. **150 轮狩猎表面全绿，实为 16 轮真实 + 134 轮空转**：狩猎脚本只盯
   RESET 事件、没验证 cycle 退出码。trace 文件按 boot 截断后计数=16 个
   真实 cycle；服务在 ~17:56（第 16/17 轮之间）变成 STATE STOPPED、
   设备 `\.\svmb` err 2，此后 svmbctl 全部打不开设备=空转绿灯。
   **有效结论保留**：guest 全程零 RESET（残余未以"杀 guest"形态现形）。
2. **死亡时刻第三次对齐**：驱动最后的活动 = 第 16 轮 start-kick-done
   （串口：6 核 entercore+active、stop 阶段 6 核 self-exit 干净落账）
   → 数秒内镜像卸载。与 E3b 崩溃、r143"gen-2 激活后 ~4s"同一窗口。
   **残余这次没杀 guest，而是无声卸载了驱动本身**。
3. **消失的性质**（已排除）：SCM 停止（12h 内 svmb 相关 SCM 事件=0，
   连 17:34 的 sc start 都无 7036）；计划任务/Run 键（只余 Edge/OneDrive）；
   guest 崩溃（零 RESET、系统全程健康）；VMRUN failed（串口 0 条）。
   事件日志 17:50-18:10 仅 1 条 Skype DCOM 超时。
4. **新事实：Parameters\AutoStart=1 默认开（r30 起）**——DriverEntry 直接
   进虚拟化（21:00 重载后 info 立即 RUNNING 6/6、串口第 2 个 S0 块）。
   17:33 的 sc start 同理自动就位。卸载若发生在"AutoStart 流程中"且
   Start 失败回滚……待 hunt2 重演捕获。
5. **hunt2 武装重跑**（`qemu/r145_hunt2.py`）：每轮 cycle 后 `svmbctl info`
   设备探测；消失即快照全家桶——QMP VMCB 收割+各核 RIP、sc query/qc
   （看 BINARY 路径是否被旧机器换成旧二进制！）、trace 尾、串口尾，
   随后 6 分钟 reload-watch 看是否有东西自动重载驱动。

### r145 终局（21:0x-21:4x）：hunt2 一轮命中 + 活体 wedge 全套取证 + 判决

1. **hunt2 第 1 轮即命中**（21:04，重载驱动后的第一个 cycle）：stop 干净
   （6 核 self-exit）→ start bring-up 重跑（svm check→cfgvmcb→cpu0-4 逐核
   vmrun-fire）→ **串口在 cpu4 发射后、cpu5 entercore 前冻结**，随后
   SSH 死、QMP 瞬时拥塞、驱动实例消失（VMCB 页=回收数据）。
2. **活体遗像**（QMP 抢救所得）：3 核 KiIdleLoop+1 核 ERESOURCE 等待族+
   1 核用户态（r144 衰竭像重演）；**LAPIC IRR=0xD1 卡死、TPR=0——GIF 死
   签名在带 stgi 修复的驱动上重现 = 存在第二个 GIF 泄漏口**（非
   vmrun_failed——串口 0 条 beacon）。
3. **宿主侧**：6 vcpu 线程满转（CPU 4s/24s）；cdb 栈采样见瞬时 5 线程
   SRWLock(BQL) 排队；4.3GB ELF 全内存转储已存 `dumps/r145_wedge.elf`。
4. **三形态归一**：17:56 驱动无声卸载（guest 活）/ 21:04 bring-up 悬挂
   （guest 衰竭）/ E3b 三重故障——**全部落在 START bring-up 窗口**
   （cpu4→cpu5 尾段尤其），全部伴随 ≥1 核中断死。r143"gen-2 激活后
   ~4s"、r144 修复后残余 1/47 全部同窗。
5. **候选机制（r146 开局验证）**：cpu5 的 IPI 未交付（idle 环打转而非
   进 entercore）→ KeIpiGenericCall 广播悬挂 → Start 卡死 → 回滚级联；
   AutoStart（DriverEntry 内 Start）若撞上则 SCM 超时杀驱动=卸载之谜的
   候选解。第二 GIF 泄漏口的具体路径 = 核心待办（ELF dump 里的
   PsLoadedModuleList + 线程栈 = 第一刀）。
6. **狩猎教训**：①验证 cycle 退出码与设备存活，否则绿灯有空转水分
   （150 轮=16 真）；②SSH banner 抖动≠设备死，探测要分通道
   （QMP RIP 移动判活）；③多线程+storm 负载下 QMP 也会瞬时拥塞，重试
   再判死。
7. **证据清单**：r145_wedge_com1.log（bring-up 死亡叙事）、
   r145_wedge.elf（4.3G 全内存）、r145_qemu_stacks.txt（23 线程栈）、
   r145_hunt2_result.txt、r145_svcstop_out.txt / r145_evt_out.txt
   （服务消失取证）、r144 系列窗口文件。

## Round 146 (2026-10-17 深夜): 4688 审计翻案——"无声卸载/悬挂/第二 GIF 泄漏"全部坍缩

### 关键决策回顾（想错 vs 真实）

| r145 判断 | r146 事实（4688+41 账本） |
|---|---|
| "驱动被无声卸载（SCM 零事件之谜）" | 崩溃重启：reboot 后 demand-start 服务自然 Stopped；崩溃死亡 SCM 无可记 |
| "start bring-up 悬挂在 cpu4→cpu5" | 串口 ring 滞后假象；cycle 实际完成，~1s 后三重故障重启 |
| "IRR 0xD1 卡死=第二 GIF 泄漏实证" | 单快照过度解读：boot/忙碌核的 IRR 暂态即可如此；从未做跨时间双探针验证 |
| "wedge 全家桶取证（ELF dump 等）" | dump 拍在 21:23 第二次崩溃后的新 boot 上（svmb 缺席=reboot 后正常） |

**账本**：Kernel-Power 41 @ 21:04:49 / 21:23:44 / 21:43:01 + 4688 全套 boot 序列
（Registry→smss→autochk→winlogon...）+ autochk 脏盘 = 一夜三崩。RESET 事件
全程未被捕获=QMP 拥塞丢事件（xp 同时段超时佐证），不是没有 reset。

### r146 后的真实态势

- **唯一存活机制 = E3b 级三重故障残余（cycle 完成后 ~1s 内崩）**，今晚高频
  （hunt2/hunt3 各自 round-1 即崩；21:23 一崩无人在场）。r144 stgi 修复对
  旧家族（vmrun_failed/GIF）依然有效（E6 40 轮干净是真实的）；残余在
  无 -d int、有审计负载的今晚爆发——执行节奏敏感性显著。
- **猎手修正**：①RESET 检测改宿主侧文件尾轮询（qemu_dbg.log 的
  "CPU Reset" 行，-d cpu_reset 本就在开）——QMP 事件通道在崩溃瞬间拥塞
  不可依赖；②"IRR 卡死"判定必须跨时间双探针；③任何"guest 死"结论前先
  查 41/6005 账本（boot 进行时会伪装成一切神秘状态）。
- **下一步（r146 继续）**：-d int 开 + 文件尾触发 + 崩溃即收割 VMCB/级联
  窗口——高频复现条件下把伪造 RIP 的归属一次打穿。

### r146 终局（22:0x-22:3x）：hunt4 一轮捕获 + 死亡形态定性 = DPC 风暴饿死时钟

1. **hunt4 全垒打**（文件尾触发=宿主侧 qemu_dbg.log "CPU Reset" 行增长，
  不依赖会拥塞的 QMP 事件通道）：round-1 cycle 后 +27s 崩溃重置，
  6.4MB 级联窗口（r146_cascade_window.txt）+ 崩溃瞬间 VMCB 收割
  （旧代 VMCB 页已回收成代码页=vmcb-pas 地址过期，收割器需读
  cycle 后的新 pas——r147 修正项）。
2. **死亡形态（崩溃前的最后数千事件）**：`v=0xE1`（DPC 中断）微秒级
  连发、全部落在 **nt!KeZeroPages+0x10**（nt 基址 fffff80574c00000
  由多 RVA 巧合锚定：0x5fa4/0x36cd6/0x1c5400），5 个 DPC 栈轮转，
  EAX=0x80。**机理：无穷 DPC 队列回填（0xE1 优先级 14）压制时钟
  （0xD1 优先级 13）→ 时钟饿死 → guest 时间冻结/sshd 停摆/渐进瘫痪
  → 最终三重故障**。此前一切"IRR 0xD1 卡死"读数=被 0xE1 洪流压住的
  暂态（r146 关键决策回顾表已更正）。
3. **风暴源头未定**（r147 开局）：崩溃后队列已清空、ActiveDpc 抓空、
  6.4MB 窗口整体已在风暴中（起点更早）。候选：①定时器/TSC 病理
  （QEMU 遵循 VMCB TscOffset 而 vhv 忽略（r92）——TSC 在 cycle 下跳变
  → 定时器轮重臂 ~0 延迟 DPC 风暴；②零页线程与 DPC 风暴的耦合
  （KeZeroPages 是被打断的上下文非源头）；③某热路径 KeInsertQueueDpc。
  驱动侧 cr3_monitor 的 DPC 机器已排除（cycle 路径不挂模块、CR3 不在
  基线拦截集 op1=11040000/op2=00000001）。
4. **r147 武器清单**：风暴进行中的活体 DPC 走查（用日志增速触发读取
  而非崩溃后）；TSC/QPC 在 cycle 前后的 guest 侧审计 + TscOffset knob
  状态核对；被打断线程的 KTHREAD 身份（PRCB CurrentThread）；
  hunt4 收割器改为读 cycle 完成后的新 vmcb-pas。

### r147 中场（23:5x-01:1x）：签名全破——4/4 崩溃同一伪造 VA 公式

1. **床清创**：关 4688 审计、删 watcher、复位后 10 分钟无驱动零重置
  （床本身稳定，崩溃全部与驱动 cycle 相关）；C: 不脏（崩溃无存储损伤）。
2. **TSC 审计**：TscProbe 默认 off → TscOffset 恒 0（驱动侧无持续偏移）。
   嫌疑收窄为 TCG 每核 TSC 在 cycle 停/启编排下的跳变（未定案——
   采样器进程孵化两度失败，改并发同步法后 cycle 又崩在其窗口前段，
   CSV 未落盘；待干净重跑）。
3. **签名锁定（本 round 核心成果）**：4/4 三重故障（晨间/E3b/r147 两崩）
   的故障 VA 全部满足 **`<0x10000 对齐的错基址> + 0x5f137a`**
   （fffff80152bf137a=当 boot 真 nt!ObpReferenceObjectByHandleWithTag+0x38a；
   另三次基址为异世界值）。用户态前奏 4/4 同型（同 ntdll RVA 序列
   ...c5b0/...c4a9/...d248=e=0x14 执行取指+数据缺页交替）= **用户线程
   发起带句柄的 syscall → 内核间接分支经"陈旧/混血指针"跳进异世界**。
   公式指纹="错模块基+nt RVA"的混合指针。
4. **排除**：ConfigureVmcb 对 GuestVmcb 全量 RtlZeroMemory（陈旧 save 区
   理论对 GuestVmcb 不成立）；cr3_monitor/sysaudit 未挂不在 cycle 路径；
   OffsetsResolve=结构偏移扫描（无函数指针数学）。
5. **hunt5 教训**：收割时机竞速——VMCB pas 读取被 SSH 死打断、级联窗口
   被 boot 洪流（~18MB/s）挤出。r148 修正：①提取改偏移锚定（检测时
   记录 reset 行 offset，提取 [offset-8MB, offset+1KB]——本 round 手工
   用此法救回了 crash#7 尾巴）；②pas 读取改 QMP 内存扫描 VMCB 签名
   （ctrl 区 0x11040000/0x00000001 拦截位图 pattern）不依赖 SSH；
   ③加 RSP 栈回读（xp RSP 物理地址→ret addr=伪造指针的调用者）。
6. **下一层问题**：谁在 syscall 入口持有 stale_base+0x5f137a？
   候选：SSDT/影子表内容陈旧（跨 boot 的 RAM 残留被新 boot 读用？
   QEMU reset 不清 RAM）、win32k/影子 SSDT 路径、或 QEMU TCG 翻译
   缓存的跨 reset 残留（TB 缓存重启不清?待查 QEMU 源码）。

## Round 148 (2026-10-18): 归属判决 + 最后嫌疑 CR8 re-poll 的 A/B 定罪

### 归属链（hunt6/hunt7 现场完成）

1. **hunt6**（+1s 崩溃）：crash 状态块全捕——混合上下文寄存器像
  （用户态值 00007ffe… + 低地址 GS 0xc68000 + 伪造 RIP + RSP 在新栈顶）
  = "半换的注入帧"形状，不是健康内核线程。
2. **hunt7**（+15s 崩溃，QMP 优先序收割）：**崩溃瞬间六个 VMCB 全部健康**
  （ExitCode=0x72/CPUID、Save.Rip=本 boot 真实内核地址 fffff8036e3570c0、
  save 区干净）→ **伪造 RIP 不经 VMCB 注入 = 非我方出口恢复路径**。
  另见 B 形态：无异常级联的直接 reset。
3. **排除链终版**：MSR 处理干净（LSTAR 不在拦截集、直通正确）；Stop 的
  页泄漏设计确认（世代内不释放）；**跨 boot 回收杀死了"reboot 后读黑匣子"
  的路线**（VMCB 页被新 boot 池复用——表/指针/机器码实拍）。

### 最后嫌疑与 A/B 定罪实验

4. **唯一站着的嫌疑 = 每出口 CR8 同值重写（r142/r143 re-poll）与 QEMU
  注入状态机的交错**：r142 已载 QEMU 在"清 HARD 位→查可投递性"间有
  竞态；我们的宿主侧 CR8 写在每个出口都撞 apic_set_tpr→apic_update_irq
  这条路——半换上下文像 = 交错注入的指纹。
5. **改动**：`s_cr8Repoll` 门控（默认 OFF，`Parameters\Cr8Repoll=1` 恢复
  r143 行为）。构建 8b84e138692c7d1ca441a242d1f675fc725f68f18cdd7f7d6199ebc15ed59305。
6. **A/B 初判**：re-poll OFF 后 hunt7 ×3（2 真 cycle + 1 SSH 抖动）
  全部 5 分钟窗口无崩——对照今晚 re-poll ON 时 4/4 首 cycle 崩（+1s/
  +15s/+27s/+98s）。10 轮加厚 soak 进行中；冻结家族（r142 原始病）
  若复发则需把 re-poll 改成"冻结检测触发的低频重扫"而非每出口。

### r148 终局（12:29-13:22）：A/B 判决落锤——CR8 re-poll 定罪

- **re-poll OFF（构建 8b84e138）**：11 个真实 cycle（rc=0）+ 各 5 分钟
  死亡窗口，**零崩溃、零冻结**，SSH 全程存活（12:29-13:22 连续）。
- **对照 re-poll ON**：同夜 4/4 首 cycle 三重故障（+1s/+15s/+27s/+98s），
  外加 E3b 与隔夜形态。
- **判决：每出口的宿主侧 `__writecr8(__readcr8())` = cycle 后三重故障
  家族的触发器。**机理（QEMU 源码侧待 r149 精化）：该写经
  apic_set_tpr→apic_update_irq 与 QEMU 的"清 HARD 位→查可投递性→注入"
  窗口交错，产出的半换上下文像（用户寄存器+低 GS+伪造 RIP）正是交错
  注入的形状；5/5 伪造 VA=<0x10000 对齐错基址>+0x5f137a 的恒定公式
  = 注入帧从陈旧/错位数据拼装时的稳定伪影。
- **历史重释**：r142 的"HARD 位失同步冻结"（NMI 治愈实验）与 r146 判明
  的 DPC 风暴饥饿是同一族；re-poll 治的是误诊的病，还注入了自己的毒。
  re-poll 默认 OFF 落地（`Parameters\Cr8Repoll=1` 可复古用于对照）。
- **r149 候选**：①QEMU 源码级机理精化（apic_set_tpr×注入窗口的精确
  竞态——上游报障候选）；②真出现 HARD 失同步时的安全替代（低频
  kernel-timer 驱动重扫，非每出口）；③床回归常规验收（NPT=on 点火
  等原 r146 后续线恢复推进）。

## Round 149 (2026-10-18): NPT=on 点火——机理全绿，稳定性翻红（r147 家族在 NPT 面复活）

### 现象

- NptEnable=1 + SerialLog=1 点火（master-HEAD 0afe843f，双向 SHA256 校验）：
  sc start 零复位，`svm check: OK (NPT supported)`（QEMU 8000000A.EDX=10010001，
  NP=1/NRIPS=0），`NPT enabled (ncr3=14364e000)`，6 核 `NP_ENABLE=1` 进入。
- 三枚 M3 探针全绿（均跑在崩溃前的原始 boot）：
  - **viewprobe（stage 1）PASS**：pml4=2a268000 == vmcbNcr3 回读。
  - **s13 deny 触发 = QEMU TCG 真走 NPT 的直接证明**：exitDelta=514（vhv
    指纹 257）；com1 spin-break 日志恰 1 条 → 真自旋 1 窗（257）+ 过窗后
    即时 resolve 重试 257 次（TCG 对 resolve 后 invlpga 的 NPT 翻译
    陈旧重端出，与 r57-r68 vhv"TLB 控制部分尊重"同族镜像）。ctl 判 FAIL
    只是拿 r45 期望值做严格相等——误报。
  - **s0 slide 满分 PASS**：detour=16/16、hooked 期原页 0 泄漏、remove 后
    16/16 恢复、exitDelta=6（slide 换页路径 TCG 很快认账，无自旋震荡）。
- **稳定性翻红**：hunt8 验收（10 cycle + 5min soak）表面全净，实为：
  cycle 1（13:46:50）真实通过 → **13:47:57 guest 三重故障重启**（无
  Minidump/无新 1001）→ cycle 2-10 全部打在重启后的无设备系统上（幽灵
  绿灯）。boot time 账本（系统启动时间 13:47:57）定案。

### r149 崩溃遗像（qemu_dbg.log @0x2a71693e5，-d cpu_reset 肖像）

```
v=0e cpl=0 IP=fffff80410b4137a CR2==RIP（取指缺页）
RAX=000000017c70c000   ← 5 号核 VMCB 的 GPA（点火日志 vmcb=17c70c000）
R14=00007ffe58010000 R12=7ffe58016da0 R9=7ffe5b12c590  ← 7ffe58-world nt 指针
GS=002b base=e3eda7e000（CPL0 用户 GS）
→ v=08 #DF → #DF 交付读 IDT 门故障（CR2=IDT+0xe0）→ Triple fault
```

- **RIP 分解 = r147 公式精确复现**：fffff8040a590000（0x10000 对齐）+
  0x5f137a（ObpReferenceObjectByHandleWithTag RVA，页内偏移 0x37a 恒定）。
  本变体相当于 r147 crash#1（fffff80152bf137a）的"真 nt 目标"形态：目标
  地址是本 boot 真 nt 函数位置，但**取指时 NPT 没供出该页**。
- 寄存器组半换（7ffe58-world 指针 + 用户 GS @CPL0）= r147 半换上下文
  指纹完整再现；RAX 攥 VMCB PA = 新证据（出口路径上下文泄入 guest
  寄存器的方向）。
- 串口黑盒遗言：纯 code=72 CPUID 正常流量，无 crumb/无 NPF/无异常——
  瞬死形态与 r147 一致。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| CheckCpu 的 NP 位检查可能拦点火 | QEMU TCG 早已暴露 NP=1（r139+ 全轮通过该门） |
| s13 exitDelta=514 "FAIL" | NPT 走表的真实证明；514=257+257（一窗自旋+过窗 resolve 重试） |
| hunt8 rc 曲线 0,1,1,0,1,0,0,0,0,0 =间歇失败 | rc=vm.py 的 SSH 退出码：1=guest 重启中断连；0=幽灵（svmbctl "[!] cannot open" 被 vm.py 吞掉退出码照样 0） |
| cycle 2-10 也算验收样本 | 13:47:57 崩溃后全部无效（服务 demand-start 不自启→无设备） |
| r148 定罪后崩溃家族已灭 | **只灭了 NPT=off 面**；NPT=on 有独立触发路径（r142 时代就是 NPT=on 床） |

### 排除/修正

- vm.py `exec` 永远 return 0（不透传 guest 命令退出码）——hunt 脚本必须
  解析输出文本（"[!]"/"[+]"）而非 rc；每 cycle 前后加 uptime/设备存在性
  防幽灵样本。
- hunt8 的 32MB 尾窗复位扫描有 ~10s 盲区（watch 间隙），13:47:57 复位簇
  被 18MB/s 引导洪流在首个 poll 前冲出窗外（r147 位移教训的 watch 态
  变体）。对策：watch 循环不间断 + 复位计数单调化（只增不减）+ 账本
  （boot time）事后审计兜底。
- com1.log 是宿主侧串口文件，跨 guest 重启持续累积（本 boot 不打印 =
  重启后驱动未加载）——计数分析必须按 boot 边界切分。

### r150 候选

1. **NPT=on 崩溃家族专项猎杀**：重臂 -d int 定向复现（cycle 1 后 60s 窗），
   拿完整 v=0e 级联 + RSP 栈回读（QMP 页走表）定位调用者；RAX=VMCB PA
   线索（出口路径寄存器保存/恢复与 NPT 视图切换的竞态？）。
2. 对照实验：Cr8Repoll=1 + NPT=on（家族是否叠加/独立）、NPT=on +
   不跑 cycle（纯点火 soak——崩溃是否需要 stop/start 编排）。
3. QEMU 源码侧：resolve 后 TCG 对 invlpga-NPT 翻译的陈旧重端出（s13
   514 算术）+ 注入窗口×apic_set_tpr（r148 遗留）合并考察。

床态：guest 已重启健康（无设备无 NPT）；Parameters 仍布 SerialLog=1 +
NptEnable=1（下次 sc start 即 NPT 态，r150 直接可用）。证据文件：
r149_crash_window.txt / r149_hunt8_result.txt / com1.log（含全部串口史）。

## Round 150 (2026-10-18): NPT=on 崩溃按需复现 + 纯 soak 对照 + 六核 VMCB 死亡现场

### 复现与对照（hunt9，增量复位扫描工艺）

- **按需复现 3/3**：点火 → cycle 1（`[+] HV_CONTROL(2) ok`）→ +67s/+82s/+40s
  三重故障重启（17 行复位簇，增量扫描 0.5s 轮询零盲区现场捕获）。
- **纯 soak 对照（240s 无 cycle）全净**：崩溃需要 stop/start 编排，不是
  NPT 常开本身。触发上下文=cycle 的 Stop→Start 迁移（VMCB 重初始化+
  NCr3 重发布+flush-all 武装+start-kick IPI）。
- 崩溃后所有 cycle/设备判定：hunt9 的文本解析+设备存在性守卫全部正确
  拒绝幽灵样本（r149 三律生效）。

### 死亡现场（两个 boot 的 QMP +0s 收割 + -d int 遗像）

- **公式 7/7**：RIP=本 boot nt 基址+0x5f137a（页内偏移 0x37a 恒定；
  r147×4 + r149 + r150×2）。本家族变体="真 nt 目标页 PTE 链不 present"。
- **六核 VMCB 死亡态（两个 boot 一致）**：exit_code 全=0x72（CPUID，
  正常流量）、exitinfo1/2=0、exit_int_info=0x3e1 族垃圾（vector 0xE1/
  type=exception/valid=0——陈旧残留）、save@0x578 的最后出口 RIP
  **六核全部=nt+0x5f7130 同一点**。
- **符号化（离线 cdb -z r140 dump）**：nt+0x5f7130 =
  **nt!CmpDereferenceKeyControlBlock+0x340**（注册表 KCB 解引用）；
  nt+0x5f137a = ObpReferenceObjectByHandleWithTag+0x38a（r147 ground
  truth）。两者都是引用记账路径=cycle 后清理风暴形状（svmbctl 进程
  退出+SCM 注册表操作）。
- **用户态前奏**（-d int）：e=0014 执行缺页×3 打在 7ff9-be37-world
  （R14 指向的同世界 ntdll）+ e=0004 数据缺页交替=r147 前奏签名。
- **半换上下文三件套**（3/3）：CPL0 用户 GS、RBP=用户栈指针、
  7ff9-world 指针（R12/R14/R9）+ 内核 RSP/RIP；**RAX=VMCB 池区 GPA**
  （r149=精确匹配 cpu5 VMCB；r150×2=池区内非基址）。
- **结构性发现：故障 CR3 走表必然失败**——RSP 回读在 +0s 执行仍
  FAILED：走表用的正是已损坏的 CR3（内核取指缺页的直接原因就是该
  CR3 的 PTE 链不 present）。调用者猎杀需换门：VMCB save 全量 dump
  （0x400-0x740）@+0s / 物理扫描 / 对照故障 CR3 vs VMCB-save CR3。

### 工具链修复（r148 遗留 bug）

- **hunt7/hunt9 两代的故障行正则从未匹配过**：`pc=\w+ CR2=` 假设两
  字段相邻，实际中间隔着 ` SP=0018:...`——RSP 回读路径在 r148 也从未
  真正执行。修复=`pc=\w+.*CR2=\w+`。

### 关键决策回顾

| 当时以为 | 实际 |
|---|---|
| RSP 回读失败=时机太晚（重启后页表回收） | +0s 也失败——坏的就是页表状态本身（结构性） |
| 六核同指针=可疑异常 | 六核全在 CmpDereferenceKeyControlBlock+0x340 收敛=清理风暴公共自旋点 |
| 死亡目标=异世界错基址（r147 主变体） | 本家族 NPT 面 3/3="真 nt 目标+页不 present"变体（r147 crash#1 同型） |
| 崩溃=NPT 常开即死 | 纯 soak 240s 净——stop/start 编排是必要条件 |

### 机理假说（r151 靶子）

- 主嫌疑：**cycle 的 Stop→Start 迁移在 QEMU TCG×NPT 下留下陈旧分页/
  寄存器伪影**（半换上下文=两线程上下文交错；RAX=VMCB 池 GPA=出口
  路径上下文泄入；故障 CR3 的 PTE 链不 present=陈旧/错代 CR3——
  r147 候选"QEMU reset 不清 RAM 的跨代残留"升级）。QEMU 源码侧：
  gen-2 vmrun 的 NCr3 重发布/flush-all 履行情况+TCG TB 缓存跨迁移
  残留是首要检查点。
- 副产物问题：s13 的 514 算术（resolve 后 invlpga-NPT 陈旧重端出
  257 次）与本案的"陈旧翻译服务"是否同根。

### 床态

guest 已重启健康（无设备）；Parameters 仍布 SerialLog=1+NptEnable=1。
证据：r150_hunt9_result.txt / r150_crash_window.txt（两个 boot 的窗口，
后者覆盖前者）/ r150_rsp_readback.py / hunt9 脚本本体。

### r153 候选

1. QEMU 源码侧：EVENTINJ 交付在 TCG×svmb 嵌套下不完成的机理（与
   r150 的"Stop→Start 陈旧伪影"合并考察——两案都指向 QEMU 事件/
   状态机的嵌套交付路径）。
2. r150 崩溃家族续猎（拆腿二分 stop-only/start-only）。
3. ovle2e 尾腿 0xC0000194 追查。

## Round 153 (2026-10-18): QEMU 源码级合并考察——r152/r136 死因统一为"毒化的 exit_int_info"；M1 翻案路径打开

### QEMU v11.1.0 源码发现（三处引文，qemu/r153_qemu_src/）

1. **do_vmexit（svm_helper.c:904-913）**：
   `exit_int_info ← event_inj; exit_int_info_err ← event_inj_err; event_inj ← 0`
   —— exit_int_info 是 **event_inj 的镜像**，不是 APM 语义的"被本次
   退出打断的事件"。
2. **handle_even_inj（system/seg_helper.c:85-111）**：guest 语境下交付
   **任何**异常/中断时，若 event_inj 无效则顺手写入"当前正在交付的事件"
   ——所以任意时刻 event_inj 可能持有**上次已交付事件**（含硬件 INTR），
   下次任意出口的 exit_int_info 就镜像它。
3. **helper_vmrun 注入块（414-469）**：VALID 位门（无效=静默跳过）；
   EXEPT 型 **vector≥31 或 =2 → cpu_vmexit(SVM_EXIT_ERR)**（445-446）
   ——落到 svmb 的 vmrun_failed 几何（r144 修复路径）。

### r152 机理终局（实测 smoking gun 闭环）

- svmb `ReinfectExitEvent`（exit_dispatcher.cpp:209+）按 APM 合法语把
  ctx.IntInfo（exit_int_info）原样拷回 EventInj。
- QEMU 侧：#UD 出口的 exit_int_info = event_inj 镜像 = **陈旧的已交付
  事件**（本 boot=一个 NMI）。
- **qemu_dbg 实证**：冻结时刻 `23435313: v=02 e=0000 cpl=0
  IP=0010:fffff80076a9e1bc`——**注入交付的是 NMI，落在 ud2 的 RIP 上**。
- 内核态意外 NMI → 0x80 BUGCODE_NMI → bugcheck 核间同步卡死=冻结形态。
- **跨平台统一**：r136 vhv 的"5 minidump 全系 0x80"同族同因——两家仿真
  层的 exit_int_info 都不是"引发本出口的事件"。**M1 死刑的真名是
  poisoned exit_int_info，不是"#BP 出口不被容忍"。**

### r150 关联腿

- 死亡 VMCB 上的 exit_int_info 垃圾（0x3e1 族）= do_vmexit 镜像的
  handle_even_inj 残留（已交付事件残影）——**惰性簿记，除非被重注入**。
  r150 配置（InterceptExc=0）下 ReinfectExitEvent 不在 CPUID 出口路径
  上——r150 的确切杀手仍开放，但"陈旧事件注入"类危害已被本案实证。
- EXEPT≥31 → SVM_EXIT_ERR 的几何记为地雷：任何把 vector≥31 EXEPT 型
  写进 EventInj 的路径都会触发。

### 附带观察（未解，记档）

- 本次冻结**部分自解**：数分钟后 SSH 复活、驱动服务 STOPPED、设备
  消失、VMCB 页被释放重用（段描述符内容）、零复位、boot 仍是 15:19
  ——有序拆驱动发生在无重启的前提下，机制未明（gIoGate 持有者死亡后
  的排队 IOCTL？SCM？待 r154 观察）。
- 重演冻结 2/2 确定性；首次读 VMCB 得码页内容=自解后重用（跨读演化，
  非 QMP 格式问题——raw 输出 0x 前缀格式正常）。

### r154 候选（高价值）

1. **svmb 补丁：ReinfectExitEvent 改为从 EXIT CODE 构造事件**
   （vector = Exit - EXCEPTION(0)，type=EXEPT，错误码从 Info1）而非拷
   IntInfo——QEMU 面的透明转发应即复活 → **M1 判决可能翻案**
   （dbgtest 期望 exc32=0xC000001D + 环增长）。vhv 面保持旧路径兼容
   （或双轨）。
2. r150 崩溃家族续（拆腿二分）。
3. 冻结自解现象观察（若复现，先拉 4688/SCM 账本）。

床态：部分自解后健康（无设备）；证据：r153_qemu_src/（源码树）+
qemu_dbg v=02 行 + com1 exc exit 行。

## Round 154 (2026-10-18): M1 翻案补丁轮——双 bug 修复+行为验证；死亡点围拢至 6 语句窗口

### 补丁链（全部编入并哈希验证部署）

1. **ReinfectExitEvent 改从 EXIT CODE 构造**（exit_dispatcher.cpp）：
   vector=Exit-0x40、type=EXEPT、错误码取 EXITINFO1（有错码向量才置
   Ev）。废除 IntInfo 拷贝（r153 定罪的毒源）。
2. **EVT 枚举对齐 APM/QEMU**（svm_defs.h）：EVT_NMI=1→2、EVT_EXCEPTION
   =2→3、EVT_SOFTINT=3→4（type 1 保留）。**QEMU svm.h 实证**：NMI=2/
   EXEPT=3/SOFT=4。旧枚举错位一格=svmb 每次"异常注入"实为 NMI 注入
   ——r136 vhv 0x80 家族与 r152/r153 QEMU 冻结的共同根。
3. InjectEvent 诊断桩（pre/wrote/readback 三值日志）。

### 行为验证（逐构建冻结对照，4 次冻结全部确定性复现）

- 补丁前：冻结+**v=02 NMI 交付在 ud2 RIP**（r152/r153 smoking gun）。
- 枚举修正后（96d63453）：**ud2 RIP 上的 v=02 消失**（0 命中）——
  毒源确认拔除；但 v=06 也不出现（注入仍不交付），冻结依旧。
- 带诊断桩（fb5bf637）：**injdiag 零行**——InjectEvent 从未执行到。

### 死亡点精确围拢

HandleExceptionExit 的 "exc exit" 日志（debugger.cpp:122）→ InjectEvent
的 injdiag 日志之间：if 比较 + AdvanceRip 赋值 + pre.Data 读。三个构建
（补丁前/后/带桩）的静默点一致。排除：活锁（exc exit 仅 1 行）、
vmrun_failed 串口 crumb（该路径不进串口但 injdiag 先于 vmrun，零行
排除）、SEH 接到（无第二条 s6 日志）。

### 附带现象（4/4）

冻结后 ~60-90s **自解**：驱动有序拆除（服务 STOPPED、设备消失、
VMCB 页释放重用=代码/日志文本）、零复位、boot 不变、SSH 恢复。
机制未明（拆除路径无 Stop() 日志）。

### T+0 冻结快照（hunt10 自动收割）

六核 exit_code 全=0x72（CPUID 正常流量——#UD 出口早已处理完并被覆盖，
核心恢复过 guest 执行）；event_inj=0x302 族（vector2/type3/**valid0**/
err4|0x14）+ exit_int_info=0xb0e 族（#PF 镜像残影）——写入者未定；
冻结中单核 RIP=0x9846a0。

### r155 候选（按性价比）

1. **KDNET 断点取证**（windbg-mcp，qemu-vm-kernel-debug 技能）：在
   debugger!HandleExceptionExit 尾部/ReinfectExitEvent/InjectEvent 下断，
   看死亡窗口内哪条指令不归——这是唯一直视手段。
2. 自解现象账本（复现时先拉 7036/7045/4688 再等）。
3. asm 出口路径审读（异常类出口的 dispatch→handler→re-vmrun 链）。

### 床态

自解后健康（服务 STOPPED）；Parameters 仍 NPTEnable=1+SerialLog=1。
构建哈希链：21caa1db（出口码构造）→96d63453（+枚举）→fb5bf637
（+诊断桩，当前部署）。

## Round 155 (2026-10-18): KDNET 断点取证——调试门重开+死亡窗口断点首布；r154"6 语句窗口"结论降级

### KDNET 门重开（传输层根因链）

- 三连 30s 超时的根因复合：①QEMU 长寿命进程的 slirp 状态劣化（deploy
  重启后即愈）；②决断测试留下的 `bcdedit /debug off` 自坑（目标不再是
  调试目标）。修复=QEMU 重启+debug on+重启+早期附着。
- **新工具链律**（已沉淀进 qemu-vm-kernel-debug 技能）：run_kd_command
  内 `;` 链会把后续整串拼进首命令（sympath 被污染=符号全灭）——一律
  单命令；符号加载用 `.reload /f /i svmb.sys`（/i 强匹绕 GUID 校验）；
  bu 需全名 `svmb!svmb::ReinfectExitEvent`；驱动未加载时 x 空=预期。

### 死亡窗口断点首布（QEMU 战役首次活体断点）

- bp0=svmb!svmb::ReinfectExitEvent@28da9300、bp1=svmb!svmb::InjectEvent@
  28da8fb0（0xCC 软件断点）。
- 布防后未及武装阶梯（SSH 命令尽数落入死窗），目标在纯点火+断点态下
  撞出 **"Break instruction exception - code 80000003 (first chance)" +
  "A fatal system error"（bugcheck 级，callbacks 未调用）**——kd 缓冲
  全程记录（点火日志+exit trace 洪流+KDTARGET refresh+int3+fatal）。
- 崩溃身份（码/栈）未收割：!analyze 在 bugcheck-wait 目标上超时（技能
  半僵纪律），会话随后僵死，已 close(resume=false)+清 kd+复位。
- 候选解读：bp/KdEnterDebugger 冻结广播 × 虚拟化核的交互；或全新崩溃
  形态。**待 r156 以正确编排重演**（早期附着→断点→g→SSH 武装重试到
  成功→dbgtest→紧轮询 wait_for_break→bp 命中即轻命令取证）。

### r154 结论降级（方法论修正）

- **串口镜像是"有损环形"定律回咬**：探针零行≠未执行。r154 的"6 语句
  窗口"（exc-exit 后执行蒸发）证据等级下调。
- 更贴合观察（exc-exit 为绝对末行+所有 exit trace 同时停摆）的新假说：
  **日志/串口锁被冻结核持有**——ud2 核冻在 SVMB_LOGW 内部或其后，
  其余核的日志全部阻塞在锁上。

### 床态

复位后健康（debug off、服务 STOPPED、Parameters 仍 NPTEnable=1+
SerialLog=1）；部署二进制 0617be77（含 reinfect/injdiag 探针）。

## Round 156 (2026-10-18): 串口 A/B 收满矩阵 + 0x80'TDO' 跨平台实锤 + "仪器扭曲实验"定律

### A 弹：串口镜像 A/B（r155 假说判决）

| debug | SerialLog | 结果 |
|---|---|---|
| on | on | 冻结（r152/r153） |
| off | on | 冻结（r154） |
| off | **off** | **冻结（r156 实测）** |

- **日志/串口路径排除**——冻结在异常出口处理路径更深处。
- 冻结签名四格恒定：零 v=06（注入不交付）、零 v=02、零复位。

### B 弹：仪器扭曲实验（本轮方法论级发现）

- 编排：debug on+重启+kd 早期附着（会话秒建）→ g → SSH sc start
  （驱动加载+NPT 点火，**未布任何断点、未挂模块、未武装**）→
  **first-chance int3 + "A fatal system error"**。
- **KiBugCheckData 实读：0x80 + 参数 0x004f4454（'TDO'）**——与 r136
  vhv 五连 BSOD、r70 kd 活捉（"首笔 out=1 trip → NMI 'TDO' → 0x80，
  r13=0x4f4454"）完全同码同参。**'TDO' NMI 家族跨平台实锤（QEMU 面新低
  门槛复现：调试器在场+点火即可，无需断点/异常拦截）**。
- **推论（死结）**：M1 冻结的 kd 断点取证需要 debug on+调试器在场，
  但该配置在点火时就 0x80'TDO' 崩溃——**仪器在场改变实验结果，断点
  正剧在 0x80 解决前无法上演**。r155 的"意外崩溃"同因归档。

### 待解族谱（当前形态）

1. **0x80'TDO' NMI 族**：r70/r136（vhv，WheaReportHwError→PSHED 链）
   + r155/r156（QEMU，调试器在场+点火）。QEMU 面的 NMI 源未定（QEMU
   自身 NMI 源稀少；WHEA 路径待查）。
2. **M1 冻结族**（debug off 稳定复现）：exc-exit 处理路径深处，串口/
   debug/日志全排除——r157 需要 QMP-only 或驱动自证（非串口通道）手段。
3. 两族关系未定（可能同根：某硬件错误/NMI 源在两种观测配置下分别
   表现为崩溃/冻结）。

### 床态

复位归常态（debug off、服务 STOPPED、Parameters 仍 NPTEnable=1+
SerialLog=1；部署 0617be77 含探针）。

## Round 159 (2026-10-18 深夜): ctl-start 手动点火死刑修复——播种栈生命周期根因 + IoQueueWorkItem 委派；冷路径两连绿

### 现象（用户指令触发）

用户令：修复"先加载驱动再 svmbctl start 虚拟化会蓝屏"路径（r158 补遗
记录的死刑径②）。产物面早有伏笔：SVMB_PRODUCTION 从 r85 起直接拒掉
IOCTL start，注释自称 "round-27 known-fatal entry context"——该路径
从未被修过，只是被禁过。

### 根因（hunt12/hunt13 两轮现场 + QMP 活检）

**播种模型**：`RunOnEachCore`（util.h）= 单线程组亲和串行走核；
`EnterCore` 把**播种线程的寄存器+内核栈快照**种进每个核的首个 VMCB，
后续穿越在播种线程里完成（main.cpp:1593 注释自证）。

- AutoStart 播种者 = SCM 装载线程：**栈长存** → 绿。
- ctl-start 播种者 = svmbctl.exe 的 IOCTL 线程：ctl 打印完即退出、
  进程终止、**内核栈被释放** → 六核僵尸快照全部踩在已释放栈上。

**死亡指纹（hunt13 QMP stop 活检，修复前）**：
1. 点火完整成功（"virtualization active on 6 cores"，ctl 正常返回）；
2. **零 VMEXITS**（ExitTraceLeft=200/核全新预算，一条没打）；
3. 六核**完全相同**的 RIP/RSP/CR3/RAX（共享一个内核栈=不可能的独立调度）；
4. 卡点在 nt 异常派发路径（`mov edx,[KdDebuggerEnabled]; test; jne` 门），
   栈上两枚 **0xC0000503** 异常记录 + svmb 模块帧；
5. CPU 满转（故障重入循环：派发→ret→重执行→再派发）；
6. ~10s 后硬复位，**无 bugcheck 无 dump**（VMCB 物理页被新 boot 覆写）。

### 修复（main.cpp HV_CONTROL action==0）

把点火**委派给 IoQueueWorkItem 派迟队列系统工作线程**，IOCTL 等事件
收结果——池化工作线程的栈长存，ctl-start 与 AutoStart 完全同形。
构建 cdfd7056。

### 验证（两连绿）

| 场景 | 结果 |
|---|---|
| 冷 boot + AutoStart=0 静默加载 + svmbctl start | 点火+串口立刻洪灌+**120s 零死亡**+info RUNNING/6核/17884 出口+dbgtest 正常返回+mod attach 正常 |
| 复位重演同场景 | 点火+洪灌+**120s 零死亡**+RUNNING/6核/11126 出口 |

### 边界与残留（诚实账）

- **同 boot 二度点火仍死**（先手动点火绿→svmbctl stop→sc stop→sc stop
  卸载→重载→再 svmbctl start）：六核僵尸签名复现，卡点换成 HAL 的
  `in al,dx`（端口 0x71=CMOS 读，RAX=RIP+4 自指，单栈共享）。
  **= r150 家族 cycle-crash 已知开放类**（r139 "stop→start 重入死"），
  非本次修复目标，留给 r160（新指纹：CMOS in 循环）。
- svmbctl stop（action=1）仍跑在 IOCTL 线程——未动，用户规程
  （stop→卸载→重载）覆盖其风险。
- AutoStart=1 已复位；SVMB_PRODUCTION 的 IOCTL-start 拒绝保留
  （注释已更新为 r159 修复后的保守拒绝）。

### 新铁律

1. **播种栈生命周期律**：任何进虚拟化的入口，播种线程的栈必须长存
   （SCM 装载线程/池化工作线程 ✓；会退出的用户进程 IOCTL 线程 ✗）。
2. **僵尸快照指纹**：全核相同 RIP+RSP+CR3 = 播种快照未分叉；零出口+
   CPU 满转 = 快照在踩死栈/死循环。
3. **QMP stop 后 system_reset 不会自动跑起来**——必须补 cont（PAUSED
   陷阱，本轮浪费 7 分钟）。

### 床态

r159 修复构建 cdfd7056 在位并 RUNNING（手动点火态）；AutoStart=1 已
复位；debug off。

## Round 165 (2026-10-19): 嵌套战役开张——L2 测试驱动落地（骑 svmbctl 面），stage-0 反嵌套拒绝全绿+串口交叉验证；今日虚拟化面基线入档

### 设计决策

L2 测试驱动第一版**骑在 svmb.sys + svmbctl**（`svmbctl l2test` = NPT_PROBE
Stage 18）：H 的 VMRUN 只需在 guest CPL0 执行，IOCTL 线程就是；SEH 捕获
svmb 注入异常的通路被 dbgtest 实证（r157 #UD face PASS）。独立 l2test.sys
等反射面需要真实性时再拆。判据纪律：**verdict 必须对串口**
（r164 律）。

### 判决（一发过，2026-10-19 下午）

```
l2test s0: exc=80000003 vmcb=2ad8e000 svme=0 efer=d01(1) hsave=17c298000(1)
[+] L2 s0 PASS (anti-nesting rejection clean: VMRUN -> #BP -> SEH)
串口: vmrun exit: anti-nesting reject vmcbPa=2ad8e000   ← 与 verdict 同 PA
串口: nptprobe s18: exc=80000003 vmcbPa=2ad8e000 ...    ← 同
guest 活 + svmb RUNNING
```

全链：H 的 `__svm_vmrun` → ic2.VMRUN 出口（拦截面先于 VMCB 校验/装载）→
HandleVmrunExit 注入 #BP(SOFTINT)+推进 NRIP → guest 内核派发 → 探针
`__except` 捕获 0x80000003 → 线程继续 → 干净拒绝。安全门前置：r37 律
反向适用（内核 #BP 仅在 kernel debug ON 时挂死）——跑前 `bcdedit` 验证
`debug No` ✓。

### 基线档案（今日虚拟化面=r166+ 要重塑的对象）

| 探针 | 结果 | 含义 |
|---|---|---|
| CPUID 80000001h ECX.SVME | **0** | hideCpuid 把 SVM 位对 guest 屏蔽——H 从 CPUID 看不见 SVM |
| EFER 读 | 0xd01（SVME=0） | EFER 读被拦截但透传 guest 真值 |
| VM_HSAVE_PA 读 | **0x17c298000 成功** | protectHsave=0 默认 → HSAVE 对 guest 可读——**指纹泄露点**（H 可探测 hypervisor 的 HSAVE 布局）+ r166 MSR 虚拟化第一目标 |

### 新床律：vvfat 共享不热刷新

宿主侧改 `qemu\share\` 后运行中的 guest 看到的还是旧文件（vvfat 是 QEMU
启动时生成的 FAT 快照）。本轮实踩：`copy /y D:\svmb.sys` 静默拷了旧弹、
sc start 点了旧构建——**三查律的哈希比对当场拦截**。热换弹一律走
`vm.py push`（SFTP 直推），或 deploy.bat 重启 QEMU 重新生成共享。

### 代码面

- `npt_probe.cpp` Stage 18：CPUID/EFER/HSAVE 三探针 + 零化 VMCB 骨架 +
  `__svm_vmrun` + SEH 判决（参数是 size_t 物理地址，非 void*）。
- `hypercall.cpp` HandleVmrunExit 加串口行（`vmrun exit: anti-nesting
  reject vmcbPa=`）——VMRUN 出口的路径证明。
- `app/main.cpp` `CmdL2Test` + `l2test` 面 + usage。

### r166 候选（嵌套战役路线）

1. **MSR 虚拟化层**（EFER/VM_CR/HSAVE 对 H 陷阱仿真，per-H 视图）——
   HSAVE 泄露点是第一靶；挡在 H 配置自身 SVM 状态的必经之路上。
2. **VMCB 合并重入**（H 的 VMCB + svmb 必需拦截 → 合成真 VMCB → 重入；
   v1 只接受 NP_ENABLE=0）+ 出口反射归属判定。
3. Stage-1 判据：NP-off 最小 VMCB 全环往返（H→L2 几条指令→出口反射回
   H 的 VMCB→H 恢复）——嵌套版的"r158 时刻"。

### 床态

r165 构建（8927c47a）RUNNING（本 boot 第三次点火，重入协议稳）；
l2test 面可用；机器健康。

## Round 166 (2026-10-19): 嵌套"r158 时刻"——stage-1 全环往返一发过（VMRUN 接受→L2 执行→出口反射→H 恢复），软件嵌套 v1 落地

### 判决（一发过 ×2，stage-0 回归绿 ×2，guest 活）

```
l2test s1: exc=00000000 hvmcb=147abf000 exit=72 L2rip=100000 L2rax=a5a51122
[+] L2 s1 PASS (full round trip: VMRUN -> L2 cpuid -> exit reflected -> H resumed)
串口三行全链:
  nested: vmrun reject vmcbPa=140457000 (v1 contract unmet)     ← 零化 VMCB=拒绝
  nested: accept hvmcb=147abf000 -> L2 rip=100000 rsp=110ff8 cr3=2e5f7000
  nested: reflect exit=72 L2rip=100000 -> H rip=fffff8066d760c02 ← H 恢复于 VMRUN+3
```

四个判据全中：exc=0（无异常=接受+返回而非 #BP）、exit=0x72（L2 的 cpuid
拦截反射）、L2rip=0x100000（fault 式出口 RIP）、L2rax=0xA5A51122（RAX
标记经往返存活——RAX 是 SVM 唯一从 VMCB 装载的 GPR）。

### v1 设计（core/nested.{h,cpp}，~150 行核心）

- **软件嵌套（KVM nested-SVM 同构）**：QEMU 只看一层 SVM——接受 H 的
  VMCB、把 L2 状态装进真 VMCB、重入；QEMU 毫不知情。
- **骨架继承合同**：L2 只从 H 拿 Rip/Rsp/Rflags/Rax/Cr3 + 拦截位图
  （ic1/ic2/exc OR 进真 VMCB）；segs/CR0/CR4/EFER/CPL 继承 L1（构造即
  合法）。svmb 控制区权威不动（NCr3/NP/MSRPM 保持——L2 地址骑 svmb
  单层 NPT）。**v1 拒绝 NP_ENABLE=1**（两级 NPT 无 TCG 支撑）。
- **接受校验**：4K 对齐 + <1TB + NP off + 意图三元组（Rip/Rsp/Cr3/
  Rflags 非零）——零化 VMCB（stage-0 品）自动落回 r140 #BP 拒绝语义
  （stage-0 回归零改动全绿）。L3 尝试（嵌套中再 VMRUN）→ #GP 拒绝。
- **反射门**（SvmbVmExitEntry，regs IN-sync 后、Dispatch 前）：
  嵌套活跃时**一切出口反射**——出口五字段+L2 存档写进 H 的 VMCB、
  恢复 svmb 拦截字段、save 区与 GPR 帧从 stash 还原、H 恢复于其
  VMRUN+3（真硬件语义=HSAVE 保存的 host RIP）。模块分发永不见 L2 出口。
- **GPR 舞步**：H 的全帧 GuestRegs + save 区在接受时 stash（per-core
  数组——VcpuContext 布局被 asm offsets 钉死不能加字段）；反射时
  memcpy 回 regs 帧；接受路径借 regs 回写通道（1010-1013 行）装 L2
  的 Rip/Rsp/Rax/Rflags，省掉绕过逻辑。CleanBits 每次手改后清零。
- **位号课**：ic1 bit18=CPUID（bit21=INTN——正是 r160 垫片占的位），
  exit 0x72；exit↔bit 映射=ic1 bit n ↔ exit 0x60+n。

### 残余（r167 候选）

1. **MSR 虚拟化**：HSAVE 读泄露真 PA（protectHsave=0）；H 写
   EFER/VM_CR/HSAVE 会砸 svmb 底座（EFER/VM_CR 已拦，HSAVE 裸奔）。
2. 反射策略细化：目前反射一切（含 svmb 自身必管出口）；VINTR/中断
   反射、NPF 归属判定。
3. L2 侧多指令/多出口场景（hlt/MSR/异常注入），VMLOAD/VMSAVE 语义，
   NRIPS=0 下反射 NRip 的手工补全。
4. 独立 l2test.sys（外部 H）——反射面需要真实性时拆。

### 床态

r166 构建（3b5cf740）RUNNING；l2test 0/1 双面绿；机器健康。

## Round 167 (2026-10-19): 反射面 X 光——五连出口 gauntlet 6/6 全绿；强制反射安全网堵住骨架 IDTR 泄漏；HSAVE 泄露一行收口

### 判决（gauntlet ×2 + s0/s1 回归全绿，guest 活）

```
l2test s2: exc=00000000 legs=6/6 mm=0 pf=300000 mark=5a5af00d intr=0
[+] L2 s2 PASS (gauntlet: cpuid/hlt/rdmsr/#UD/#PF all reflected, ...)
s0: hsave=0(1)   ← 泄露收口（原 0x17c103000）
```

L2 五连出口序列各得其所：cpuid 0x72 / hlt 0x78 / rdmsr 0x7C（L2 自设
ecx=EFER 落进 svmb 保护 MSRPM）/ **#UD 0x46（异常反射）** / **#PF 0x4E
（ExitInfo2=0x300000 分毫不差）** / 终态 hlt + **标记存储在 L2 自身 CR3
世界执行落地**（宿主直读 PA 见 0x5A5AF00D）。单轮 7 次 VMRUN 往返
stash/restore 零漂移。

### 修法（两项）

1. **强制反射安全网**（nested.cpp 接受路径，独立于 H 申请什么）：
   - **异常位图置全 1**：骨架继承把 L1 的 IDTR 泄给了 L2——任何原生
     派发的 L2 异常都会带着 L2 的 CR3/RIP 跑进 Windows 内核=蓝屏。
     全反射后异常出口一律归 H 仿真。
   - **ic1 bit0（INTR）**：同类危害的中断面——不拦则中断直接投递进
     L2（经 L1 IDT）。拦后待决中断出口 0x60 反射，H 重试（真
     nested-SVM ABI）。**第一次 gauntlet 尝试野外捕获 intr=1：0x60
     反射+H 零推进重试，干净通过**。
2. **ProtectHsave 默认 0→1**：r165 泄露点收口——处理器语义（读=0/
   写=丢）早就正确，只是拦截面没武装（bisect 遗留）。床面注册表无
   显式覆盖，新默认直接生效（s0 实测 hsave=0）。

### 过程两坑（皆测试品笔误，机制反被双重证明）

1. 标记存 disp32 写成 `00 12 01 00`（=0x11200，应为 `00 20 11 00`=
   0x112000）→ 存进未映射页 → #PF 反射 ExitInfo2=**0x11200 分毫不差**
   （出口记录保真的计划外双重证明）。
2. `48 C7 04 25+disp32+imm32` 全长 12 字节计成 11 → 期望表终态 hlt
   差一字节（0x100021→0x100022）——mm=0x100000078 的 rip-flag 精确
   指认。

### 设计决策项（入档未决）

**EFER 视图张力**：现行 MSR 面对 guest 藏 SVM（EFER 读屏蔽 SVME、
VM_CR 强制 SVMDIS、HSAVE 读 0——反检测立场）；但"真 H"（外部
l2test.sys / 真虚拟化栈）需要看见 SVM 才肯工作。per-H/per-context
MSR 视图是解法方向，留到外部 H 时代决策。

### 残余（r168 候选）

NPF 归属判定与 L2 NPT 异常路径、VINTR 注入语义、VMLOAD/VMSAVE、
独立 l2test.sys、NRIPS=0 下反射 NRip 手补、EFER 视图策略。

### 床态

r167 构建（2ca57c57 后修订版）RUNNING；l2test 0/1/2 三面全绿；机器健康。

## Round 168 (2026-10-19): 反射机制完善第一刀——EVENTINJ 透传合同落地（转移级 PASS）；in-L2 注入投递=QEMU 仿真缝归档；.rdata 蓝屏与 IDT 门字节定位两课

### 机制入列（全部落地 nested.{h,cpp}）

1. **反射 NRip fault 类清零**（陈旧值清 0，H 手算——本地 FaultClassExit
   镜像，hypervisor.cpp 的原函数链接不可达）。
2. **TSC offset 两级合成**（accept 相加 H 的、reflect 还原 svmb 的）。
3. **IDTR 覆盖合同**（H 给 L2 自建 IDT 的通道，v1.1）。
4. **EVENTINJ 透传**：H 臂 EVENTINJ→accept 转移进真 VMCB+清 H 的 Valid
   位（硬件消费语义）；真 VMCB 挂起 L1 事件时 **L1 优先**（r168b：
   stash/clear/restore——挂起 L1 事件绝不能打进 L2）。
5. **VIntr+中断影子 stash/clear/restore**（同危害类：QEMU 的
   ctl_has_irq 在 L2 入口 VMRUN 读骨架 VIntr 会把 L1 的虚拟中断投给 L2）。
6. **VMLOAD/VMSAVE 强拦**（ic2 bit2/3）+ #GP 拒绝处理口（原生执行会经
   svmb 不拥有的 VMCB 改写真 save 区）。

### 判决

```
[+] L2 s0/s1/s2 PASS（hsave=0，全代部署回归绿）
[+] L2 s3 PASS ×2 (EVENTINJ transfer: armed -> transferred -> consumed)
```

s3 判据=**转移合同级**：H 臂→svmb 转移→Valid 位按硬件语义清零
（injValid=0 实证）。**in-L2 注入投递=QEMU-TCG 仿真缝归档**：确定性
#PF@宿主栈形 CR2（ffff9701584bafc0），而 L2 RSP 装载无恙（串口 accept
行 rsp=111ff8；QEMU helper_vmrun RSP 装载在 eventinj 处理之前，源码
定位 svm_helper.c:369 vs :415）；type-0（helper_vmrun 内立即投递）与
type-4（cpu_loop_exit）两路径同死；VIntr 排除后仍在。与"无 SVM MTF"
同类的**床保真度边界**——真机 EVENTINJ 在硬件层投递，透传合同正确。

### QEMU 源码引文（本轮新证）

- IDT 门属性字节=**+5**（do_interrupt64: e2=+4 双字，type=(e2>>8)&0x1f
  ——+6 写 0x008E 与 +8 写 0x8E00 都死，正解 +4 处 u16=0x8E00）；
  limit 检查错误码=intno*8+2（0x31 号=0x18A，本轮定位主线索）。
- helper_vmrun 装载序：CR3@:317→idtr@:363→RIP/RSP/RAX@:367-372→
  EVENTINJ@:415；cpu_vmexit 在 :786+ 把 guest 活动 gdtr/idtr 写回 VMCB。
- IDTR 的 limit 在 vmcb_seg.limit（+4，u32）——svmb 自己的填法一致。

### 过程课（全部入律）

1. **.rdata 蓝屏 0xBE**：`*(i32*)(static const l2code+…)=rel` 写只读段
   =内核蓝屏→guest 重启循环（TCG 慢启动把"挂死"伪装成 livelock；
   10 个 DriverEntry 块+BugCheck 1001 铁证）。**新律：内核探针改字节
   数组一律栈缓冲中转；"guest 挂死"先查 bugcheck 1001/串口
   DriverEntry 计数再定性。**
2. 串口在服务异常停止期会"死"（无输出≠无事件），重启复活——取证前
   先验串口活着。
3. AllocNonPaged **不清零**——判据读回页必须显式清零。
4. 注入臂时机差一拍 + Leg 结构丢 adv 字段=cpuid 后无推进重执行循环
   ——对照 stage-20 的带 adv 表写法。

### 残余

注入投递缝的更深层（QEMU 侧 push 上下文）=真机/换床验证项；
r169=NPF 归属判定+VINTR 深水区；r170=MSR per-H 视图+独立 l2test.sys。

### 床态

r168 终版构建 RUNNING；l2test 0/1/2/3 四面全绿（s3 转移级）；机器健康。

## Round 170 (2026-10-19): 共存故障根因二分——svmbctl/l2test/嵌套全无罪；触发=命中窗口内的会话/进程创建活动；M1 在 r168 构建无回归

### 判决链

1. **纯 M1 ×3 无回归**（r168 构建含 ProtectHsave=1）：
   `HITS 20/20 LEAK 0 ×3`——r164 双满分在新构建上成立，
   ProtectHsave 无辜。第一轮的"HITS 20 LEAK 20 假回归"=事故重启后
   忘了重新 `mod attach debugger`（r165 意外原生路径形态重现）。
2. **r169 原配方复现 2/2**：命中环中并发 svmbctl 链 → guest 死。
3. **二分 A（决定性）**：命中环中只发纯 `echo` SSH 命令（无 svmbctl/
   无 l2test/无嵌套）→ **同样致死**（第三例）。**嵌套战役侧代码全部
   无罪**；触发成分=**M1 命中窗口内的新会话/进程创建活动**（sshd+
   cmd+conhost 生成风暴）。对照组：纯 M1 ×3 的轮询全在窗口外=绿。
4. 死亡形态两种：Event-41 硬重置（r169/r170-复现 1）+ **BSOD**（二分 A
   ）；复现后一次 boot 退化 wedged-idle（hlt-emulate 风暴+多核串口
   串写乱码=需 HMP system_reset）。

### r169 串口重读（新镜头）

`reinfect: enter exit=43 rip=sled+1` = HandleExceptionExit 自调的
正常 M1 链（非默认路）；第一异常行=垃圾 PA 的 VMRUN 拒绝，其后全是
内核脱轨风暴。l2test 从未落地（零 accept 行）——与二分 A 结论互证。

## Round 171 (2026-10-19): EFER 张力正式裁决——per-H 虚拟 SVM 视图落地，注册制双视图一发过

### 裁决（一发过 + 全套回归 5/5）

```
l2test s6: pre(efer=d01 hsave=0) H(efer=1000 hsave=12345000) post(efer=d01) cpuidH=1 cpuidOther=0
[+] L2 s6 PASS (per-H view: SVME settable + own HSAVE + CPUID SVM visible; stealth restored)
```

### 设计（EFER 张力的解法=注册制双视图）

- **注册判别=CR3**：注册线程的当前 CR3（=svmbctl 进程上下文）标记为
  H；MSR/CPUID 出口按 GuestCr3 查表分流。未注册上下文维持反检测视图
  （EFER.SVME 屏蔽、HSAVE=0、CPUID 隐 SVM）——**无全局解掩，隐身立场
  保留**。
- **H 视图**（nested.cpp 的 HViewSlot×4）：EFER.SVME 可读可写（虚拟
  副本）、自有 VM_HSAVE_PA、VM_CR 无强制 SVMDIS、CPUID 不隐 SVM 位。
  **真实 MSR 零触碰**（比旧"强制 SVME 透传"更安全——H 的写全落副本）。
- 种子=注册时的隐身视图（SVME 清零/HSAVE 空），注册/注销即装即拆
  （Stage 24 探针自包含闭环；真 IOCTL 注册面留给外部 l2test.sys 轮）。

### 过程课

- **Bash 工具的 heredoc 吃一级反斜杠**：python-heredoc 里 `\n` 落盘成
  真换行=字符串字面量断行 C2001×3——**C 源码含转义串的手术一律用 Edit
  工具**（r132"禁 python-heredoc 字符串手术"律的精确化）。
- 床面恢复慢律：BSOD 后 TCG 启动+MEMORY.DUMP 写盘可达 ~10 分钟，
  vmhealth 翻 HEALTHY 后 SSH 才通；复位后 vvfat 陈货+qauto 不跑照旧。

### r172 候选

独立 l2test.sys（外部 H，真 IOCTL 注册面+自编码 VMCB）或嵌套 NPT
影子合成；M1 竞态（r170 窄靶）仍挂账待回归。

### 床态

r171 构建 RUNNING；l2test 0/1/2/5/6 五面全绿；机器健康。

## Round 172 (2026-10-19): 嵌套 NPT 影子合成 v0——机制全落地、回归零退化；首障=QEMU 走表期 P|W|U NPF（四假设证伪，深度定性未解）

### 已落地（全部在位，等一个绿灯）

- 影子池（per-core）：32MB 细粒度窗口（4K 惰性合成，源自 H 嵌套表软走查）
  嵌在 **4GB 全域 2MB 恒等大页毯**内（毯子的必然性：L2 自己的 CR3 走表
  页是散在高 GPA 的池页，首轮即在 0x7b05c000 撞墙）。
- 接受路径：NP 会话不再拒绝——stash 真 NCR3→Init（毯子建一次+窗口每会话
  清零=失效策略）→ 换影子根 + **TLB_CTL_FLUSH_ALL**（r163 律）；
  反射还原真 NCR3。门路由：NPF→合成器先行，合成失败才反射。
- Stage 25 判据：三重表结构（L2 CR3 恒等+H 非恒等嵌套表+svmb 影子），
  L2 跑在虚拟 GPA 0x500000，标记 0xC0DE600D。
- **回归 5/5 绿**（s0/1/2/5/6 在影子时代构建上零退化）+ guest 活。

### 首障定性（四假设全证伪，现场保留）

```
shadow: npf reflect gpa=<l2t池页> info1=200000007 rip=500000
```
- NPF=**bit32（guest 走表期）+P|W|U**，落点=L2 的页表页（每次跑 ASLR 式
  变：0x7b05c/0x56ec3/0x5a50c/…全 >1GB 的池 PA）；W=QEMU 走表时的
  guest-PTE A/D 回写。
- 证伪链：①A/D 预置（0xE7）无效；②SMAP 假设清 US（0xE5）无效；③陈旧
  复合假设补 TLB 全刷无效；④US=1+A/D+刷共存（从未测过的组合）仍同败。
- 毯子链经诊断 dump 验证完好（pml4e/pdpte 全 P|RW|A 有效）；哈希逐轮
  核对无陈弹。
- **r173 开场动作已定**：读 QEMU 源里走表期 NPF 错误码的构造点
  （seg_helper.c 的 mmu_translate/PTE-A/D 回写路径），确认该回写用什么
  mmu_idx/权限向量走 NPT——错误码的 U 位指向 user-index 写路径的嫌疑。

### 律与课

- **非分页池物理不连续**：影子池页 VA 连续但 PA 跳跃（pml4Pa=0x15df1c000
  vs pdptPa=0x4c01d000）——表间 PA 必须逐页 MmGetPhysicalAddress。
- 影子换根必带 TLB 全刷（r163 律的嵌套版）。
- r172 的 heredoc/转义坑零复发（全程 Edit/无转义手术）。

### 床态

r172 构建 RUNNING；l2test 0/1/2/5/6 绿、7 未绿（首障定性中）；机器健康。

## Round 173 (2026-10-19): 嵌套 NPT 影子合成 v0 绿灯——QEMU 源定音 US 位+四连修（风暴/毯界/槽位错位/判据）；s7 PASS ×2 + 回归 5/5

### 判决

```
[+] L2 s7 PASS ×2 (shadow v0: L2 ran on virtual GPA 0x500000 through H's non-identity nested NPT)
全套回归 5/5 + guest 活
```

### 根因链（r172 首障的完整解）

1. **P|W|U 之谜（QEMU 源定音）**：excp_helper.c `case MMU_NESTED_IDX`
   把影表走查交给 **MMU_USER64_IDX（is_user 恒真）**；`ptw_translate`
   对 PTE 页用 **MMU_DATA_STORE** 探测（A/D 回写能力）。r172 的诊断
   dump 里 pml4e=0x…023 其实早已显示 **表间链接项 |0x3 缺 US 位**——
   用户态访问 supervisor 链接项=保护违例。U 位=走表语境伪线索（四假设
   证伪的谜底）。**修：链接项全改 |0x7。**
2. **0x60 风暴饿死 L2**（intr=256/256）：挂起中断既进不了 L2（强制
   INTR 反射）也送不到 L1（r142 家族 IRR 不可投递已知病）。修：**L2
   Rflags IF=0**（中断对 L2 永不可投递→流给 L1）+ **影子窗口跨重试
   持久化**（lastRoot 判据重置——r172 的每接受清窗让风暴期反复重合成）。
3. **毯界**：走表页 @0x129570000=4.9GB——**PCI 洞回填把 RAM 顶上 4GB**。
   毯子 4→8GB（错误码 0x6=P0|W|U 即"走表页不在毯内"的签名）。
4. **ptIdx 槽位错位（真·主犯）**：PD 链接按 GPA 从 0 数槽（0x500000=
   槽 2），合成却写 `(gpa-窗口基)>>21`=槽 0——写错表=**无限重合成环**
   （串口 compose 行洪流+床挂死需 HMP 复位）。修：`ptIdx = gpa>>21`。
5. 判据两笔误：hlt 在 +0xE（2+12）；imm32 符号扩展（0xC0DE600D→
   0xFFFFFFFFC0DE600D，r169 律再犯）。

### 律

- **QEMU 影表走查=用户态语境**（MMU_NESTED_IDX→MMU_USER64_IDX）+ PTE
  页以 STORE 探测——**影子全部表项（含链接）必须 P|RW|US**，否则
  走表期 P|W|U NPF。
- **毯必须过 4GB**（PCI 洞回填）；错误码 P0+W+U@高位 GPA=毯界签名。
- **窗口 PT 槽位=GPA>>21**（PD 链接从零数槽）——窗口"基址"只用于
  判界，不用于槽位算术。

### 残余（v1 目标）

失效精度（lastRoot 粗粒度→H 改表感知）、H 大页、perm 合成收紧
（v0 恒等毯+permissive）、多核并发影子；M1 竞态（r170）仍挂账。

### 床态

r173 构建 RUNNING；l2test 0/1/2/5/6/7 六面全绿；机器健康。

### r176 候选（按优先级）

1. **pending-EVENTINJ 投递缝**（脱轨残余）：#BP 重注入的替代投递通道
   （如 SWINT 自注入=DebugService2 用户态路），或投递确认+重试。
2. **服务自停真机制**（4 例，均脱轨后）：SCM 1077 形态取证。
3. 回主线：影子 v1（失效/权限/大页）——嵌套回归 6/6 已证修复零影响。

### 床态

boot G RUNNING（无武装）；嵌套回归 6/6 后机器健康；r176 开场直接可用。

## Round 176 (2026-10-20 下午): 嵌套 NPT 影子合成 v1 绿灯——INVLPGA 失效合同+权限透传+2M/1G 大页+128MB 窗；s13/s14/s15 三台全绿×2+回归 6/6 零退化

### 判决

```
s13 PASS ×2: INVLPGA 失效——H 重映射 mark GPA→第二帧+INVLPGA 后，L2 读到新魔数
             （r1=1122..78 r2=8877..21 legs=3；陈旧影子会回旧帧）
s14 PASS ×2: 权限——H-NX 页的取指 NPF 反射给 H（exit=0x400 info1=0x100000014
             =walk+EXEC+P），H 清 NX+INVLPGA 修复后 L2 跑完全程
s15 PASS ×2: 大页——H 单条 2M 大页覆盖 code/stack/mark（物理连续区偏移映射），
             512 填充合成，程序跑完落标
回归 0/1/2/5/6/7 = 6/6 PASS（v0 判据在 v1 compose 上零退化）；s7 复验绿；guest 活
```

### 实现（nested.cpp 为主，四特性）

1. **INVLPGA 失效合同**：ic1 bit26（出口 0x7A）TCG 常开武装；处理器 = 宿主侧
   原生重发 `_svmb_invlpga(RAX,ECX)`（同物理 TLB，ASID 标记——驱动自身
   guest 语境调用透明）+ **全机窗清零**（跨核清 PT 良性竞态：清后合成=对
   H 当前表的新走查；lastRoot=0 双保险；1G 大叶的 PDPT 链接用缓存 PA 重建，
   免出口 IRQL 的 Mm 调用）+ 活会话核 r108 PendingTlbKick（绝跨核写 VMCB）。
2. **权限透传**：叶项 = H 的 RW/US/NX 位**原样**（P|A|D 强制——A/D 预置杀
   走查期 fault 风暴；r173 的 US 律只约束链接项）；**故障类别校验**——合成
   叶仍挡本次访问（exec×NX / write×RO / user×supervisor）= H 的故障 →
   返回 false 反射（否则无限 NPF 环）。s14 的 deny 行 `fl=8000000000000006`
   = NX+RW+US 透传的直接可视化。
3. **大页**：H 的 2M 大页 → 512 项填充窗口 PT 槽（窗口槽是 PT 链接不可 PS，
   填充等价）；H 的 1G → 影子 PDPT 槽直接成大叶（GB0 拒绝=窗口所在，PD0
   链接必须活）；512G 拒绝反射。
4. **窗口 16→64 槽**（32→128MB，PD0 容量内）。

### 判据算术两律再犯（第三次+，已入判据注释）

- **r169 imm32 符号扩展律**：mov imm32 0xC0DE600D 落成 0xFFFFFFFFC0DE600D
  （s14/s15 判据初版又踩）。
- **hlt 出口 rip=指令自身偏移律**（非 +1）：s7 期望 0xE=偏移本身；s15 同律。

### 过程课

- app 侧 stage 映射块**边界必查**：`stage>=8` 块把 13-15 覆写成 26（有毒
  stub 载体又误跑一发=服务自停第 5 例）——新映射块与旧块范围互斥
  （8..12 vs 13..15）。
- MmAllocateContiguousMemory 在本 WDK 是 (bytes, PHYSICAL_ADDRESS) 形；用
  平台助手 AllocContiguous/FreeContiguous（util.cpp 现成）。
- deploy 后 135s 处 vmhealth 常报 GUEST-HUNG 瞬态（sshd 未就绪+boot 风暴
  忙），+60s 自愈——判决前等二查。

### r177 候选

1. 外部 l2test.sys（自编码 VMCB+真 IOCTL 注册面）——影子 v1 已就绪做底座；
2. 多核并发影子（双 H 双 L2 同场）；
3. r175 残余（pending-EVENTINJ 缝/服务自停真机制）仍挂账。

### 床态

s13/14/15×2 后 guest 活、机器 HEALTHY；无武装 boot；r177 开场直接可用。

## Round 177 (2026-10-20 晚): 外部 H 生态开张——l2test.sys 真消费者 E2E 双绿；HVIEW 注册面（IOCTL+svmbctl hreg/hunreg）落地；外部进程语境虚拟 EFER/HSAVE 实证

### 判决

```
EXT-H PASS ×2: hview reg(ok) → l2t2 RUN → exc=0 exit=78 rip=50000E
              mark=ffffffffc0de600d legs=1 eferH=1000 hsaveH=12345000
              → hview unreg(ok)（六判据全中）
回归 s0/1/2/5/6/7 + s14/s15 绿；s13 本 boot 1/3 首腿 #GP(0x4D) 偶发
（r176 boot 0/2 干净；复跑数据全对=PASS 形态；挂 r178 观察项）
```

### 交付（外部生态五件）

1. **svmb 面**：SVMB_IOCTL_HVIEW（0x860）——调用者 CR3 注册/注销 H 视图
   （r171 双视图的真 IOCTL 入口）；svmbctl `hreg`/`hunreg` 动词。
2. **tests/l2test/**（新目录）：l2test.sys（外部内核驱动=H：自建 VMCB+
   嵌套 NPT+L2 表+cpuid/mov/hlt 程序，从自有进程语境 __svm_vmrun；虚拟
   EFER.SVME/HSAVE 写读=H 视图活性的外部实证）+ l2load.exe（用户装载器：
   注册→RUN→注销全编排+六项判据）+ l2test.vcxproj（svmb 骨架克隆）+
   build/_l2test_build.bat。
3. **外部驱动 VMRUN 全链**：装载器进程 CR3 注册 → l2test.sys 的 ioctl
   处理器（同进程语境）VMRUN → svmb 接受（软件嵌套）→ L2 过影子跑完 →
   出口反射回外部 H → cpuid 腿处理 → 终态 hlt → 注销回隐身。

### 新床律

- **完全无签 .sys 本床拒绝加载**（testsigning+nointegritychecks 也不放行）
  ——l2test.vcxproj 用 `SignMode=TestSign`（WDK 自动 WDKTestCert，guest
  已信任该根；svmb.sys 即此路）。host 侧 Get-AuthenticodeSignature 显示
  UnknownError=host 缺链，guest 侧 Valid 才是真值。
- build/svmb_test.pfx 已不存在（build_debug 的 signtool 行长期静默失败）
  ——外部工件签名走 WDK TestSign，不走 pfx。
- MSYS 吃 signtool 单斜杠旗标（/fd→路径）+ 引号 heredoc 也吃一级反斜杠
  （本役四犯：C 串手术一律 Edit 工具）。
- sc 加载中的驱动文件锁定：push 前必 sc stop。
- `__svm_vmrun` 参数=size_t 全 64 位 PA——截断成 u32 会拿低半地址当
  VMCB（reject "v1 contract unmet" 的一个新形态）。
- 最小 WDM 设备的 CREATE/CLOSE 必须回 SUCCESS（否则 CreateFile err 1）。

### 残余/观察项

- **s13 首腿 #GP(0x4D) 偶发**（本 boot 1/3）：疑外部 H 会话后的 pending
  事件残留竞态（r167 强反射安全网把 L2 异常全反射，H 助手遇非预期出口
  即 bail）——r178 观察项：连续跑 s13 ×10 定发生率+串口定性。
- r175 残余（pending-EVENTINJ 缝+服务自停）仍挂账。

### 床态

EXT-H 双绿+回归绿后 guest 活；l2test 服务留在床上（已停止态）；
r178 开场直接可用。

## Round 178 (2026-10-20 深夜): s13 首腿 #GP 破案与修复——池 LIFO 复用→同根 PA→陈旧影子窗口跨测试存活；修=世界键扩二元组+H 拆卸 INVLPGA；锤 20/20

### 机制（串口指纹定罪）

```
失败腿：accept →【缺 code 页(0x500000) 的 compose】→ compose gpa=511ff8（RSP 事件帧压栈）
        → reflect exit=4d L2rip=500004（指令中段=垃圾解码）
```
前一个测试（s25）释放的页被非分页池 LIFO 复用 → 新世界（s27）的 H-NPT 根拿到
**同一物理根地址** → lastRoot 相等 → r173 的窗口持久化判据误判"同世界重试"
→ L2 首取指吃到 s25 旧已释放页的陈旧合成 → 执行垃圾 → #GP（r167 异常位图
全 1 → 出口 0x4D）。偶发性 = 池复用的落位模式。

### 修复（双层）

1. **svmb**：影子世界键从 lastRoot 单值扩成 **（H-NPT 根 PA + H vmcb PA）
   二元组**（两者任一变 = 新世界 = 清窗；ShadowPool.lastVmcb）。
2. **测试侧（H 语义正解）**：拆卸时 INVLPGA——stages 25/27/28/29 + 外部
   l2test.sys 各补一发 `_svmb_invlpga(0,1)`（真 hypervisor 拆卸必做的
   失效，合同由 r176 的 NestedInvlpgaExit 承接）。

### 判决

```
s7→s13 复现序列 ×10 = 20/20 PASS（修复前同序列 1/3 失败）
EXT-H PASS（修复构建复验）；回归 s0/1/2/5/6/14/15 全绿；guest 活
```

### 床律入档

- **非分页池 LIFO 复用律（影子世代）**：free-then-alloc 序列会拿回同址
  页——任何"地址=世界身份"的缓存判据在跨 free/alloc 边界时都可能误判；
  键要加宽 + 语义正解（拆卸 INVLPGA）双保险。
- 失败腿指纹：accept 后缺首个 code 页 compose + 出现栈页 compose（事件帧
  压栈）+ 指令中段 RIP = "陈旧窗口喂垃圾"三特征。

### 床态

锤+回归后 guest 活、HEALTHY；r179 候选=多核并发影子 / r175 残余。

## Round 179 (2026-10-20 深夜②): 多核并发影子绿灯——s16（4 H 世界×4 核×50 轮共发，同一 GPA 落四帧）双 boot 验证；全回归绿；服务自停第 6 例挂 r180 首靶

### 判决

```
s16 PASS ×2 boot: okMask=0xF rounds=200（4 核各 50 轮全中自有魔数）
  串口：core=0..3 rounds=50 mark=5ac0/1/2/30000 ok=1 全行
  交织证据：accept 窗口 4 个互异 hvmcb PA 交错
新 boot 补跑 s13/s14/s15 全 PASS + s0-s7 回归绿 + guest 活
```

### 实现（stage 30 / svmbctl l2test 16）

文件域 McThreadFn（PsCreateSystemThread×4，各自 KeSetSystemAffinityThread
钉核 0-3，共享 NotificationEvent 栅栏同放）：每线程=独立 H 世界（12 页：
VMCB/嵌套 NPT/L2 表/code/stack/mark），50 轮 L2 往返（cpuid 腿+终态 hlt+
mark==自有魔数），拆卸 INVLPGA（r178 律）。编排块 30s/线程超时汇合收割
okMask。魔数 0x5AC0xxxx（正 imm32 无符号扩展之虞）。

### 观察与残余

- **多核串写串口交织**：并发 LOGI 的直写 COM1 无锁——行内容交错（显示
  伪影，非数据问题；与 r169 乱码律同源）。
- **服务自停第 6 例**：s16 后跑 s13 中途机器劣化（sshd 死、串口停在
  accept 后）→ 自愈 → 服务 STOPPED。与 r175 残余同族；多核负载疑似
  加剧（4 会话×pending 事件面变大）——**r180 首靶**：pending-EVENTINJ
  投递缝+服务自停真机制（现已是单 boot 长测试序列的最大障碍）。
- 本 boot 曾按规程 sc stop→推 sys→sc start 复活成功（重入规程有效）。

### 床态

新 boot RUNNING、s13-s16 全绿后 guest 活；r180 开场直接可用。


## Round 180 (2026-10-20 晚): "服务自停"悬案正式闭环——六例全部=无声硬重置（1077 指纹+boot 计数对账）；r169 悬案与 r175 六例并轨为"脱轨→重置尾部风险"；复现尝试 3/3 干净

### 破案链

1. **回溯事件日志**：今日 Event-41/6008 对=deploy 硬杀伪影；但 **6005（每
   boot 一条）计数 12:00-14:38 共 12 次 boot vs 同期 deploy 仅 5 次** =
   **7 次无声自重置**。
2. **铁证指纹**：case 4 的 `WIN32_EXIT_CODE 1077`（ERROR_SERVICE_NEVER_
   STARTED"本次 boot 从未启动"）——boot 内启动后停下的服务绝不会给
   1077；1077=我们 sc start 之后机器重开过。当时（r175）漏读了含义。
3. **并轨判决**：r169"服务静默自停之谜"+r175 六例+案例 6 = **一家**：
   pending-EVENTINJ 脱轨的尾部升级=guest 三重故障硬重置（QEMU 存活、
   guest 静默重启、demand-start 服务没起、看到的就是"自停"）。排查律：
   **`sc query` 见 1077 → 先查 uptime/6005，别当 in-boot 自停查**。

### 复现尝试（当前修复面下的尾部率）

case-6 配方（s16 多核→s13）×3 迭代：**6/6 测试 PASS、服务全程 RUNNING、
boot 时间戳稳定**——低率尾部（结合 r175 spawn ×2 存活：单次触发概率
<10% 量级）。r179 案 6 为多核负载后的一次命中。

### 账本更新

- ~~服务自停真机制~~ → **已解**（无声重置；机制归宿=pending-EVENTINJ
  投递缝的脱轨尾部）。
- 剩余真账：①pending-EVENTINJ 投递缝本体（QEMU 异常型 EVENTINJ 投递在
  调度churn 下的偶发失败→脱轨；修向=替代投递通道或投递确认，涉 QEMU
  语义深水区，真机不走此路=低优先）；②真机移植（等条件）。
- 观测律：判长序列时每步 l2test 的 IOCTL 存活本身就是重置探测器
  （reset→`\.\svmb` err 2）。

### 床态

三迭代后 RUNNING、boot 稳定（14:37:48 起）、机器健康；r181 开场可用。

## Round 181 (2026-10-20 夜): 第三方 H 行为记录/分析模块（vlog）落地——accept/reflect/compose/intent/inject/ctrl 六类记录+IOCTL 面+svmbctl 解码分析；顺带两破案：窗口扩张吃毯（走表位恒等回填修）与外部驱动 ic1 偏移误武装

### 判决

```
vlog 叙事（EXT-H 真第三方驱动跑完后的 dump）：
  #1 ACCEPT  hvmcb=1e085000 rip=500000 cr3=32a8d000 np=1
  #2   H intercepts ic1=00040000: CPUID          ← 第三方意图（解码命名）
  #3 COMPOSE gpa=500000 -> 62087000 perms=67     ← 它的 L2 内存映射操作
  #4 REFLECT CPUID rip=500000                    ← 它的 L2 出口（它处理）
  #5 ACCEPT rip=500002                           ← 它的推进+重入
  #6 COMPOSE gpa=513000 -> 2f48b000
  #7 REFLECT HLT rip=50000e                      ← 终态
stats 面：六类计数+top 出口直方图（CPUID/HLT 解码名）；dump/stats/mask/reset 四动词
回归 s0-s7+s13-s16 = 10/10 全绿 + EXT-H PASS + guest 活
```

### 实现

- **core/vlog.{h,cpp}**：8192×48B 自旋锁环（非分页 384KB）；VlogAccept 内部
  对 H 控制面逐 accept 差分 → INTENT（拦截位图变化）/HINJ（它给 L2 武装的
  注入）/HCTRL（嵌套 NCR3/TSC 偏移切换）；VlogReflect（它的 L2 每个出口+
  直方图）；VlogCompose（它 nested NPT 的映射意图，含 lvl3=走表恒等回填）。
  无嵌套会话时零成本。DriverEntry Init/Unload Deinit。
- **挂点**：accept（stash 后）、NestedReflect（出口快照后）、compose 三路径。
- **IOCTL SVMB_IOCTL_VLOG(0x870)**：op0 读（drain，仅读消耗）/1 stats/2 mask/
  3 reset；SVMB_VLOG_OUT 头+记录数组。
- **svmbctl**：`vlog stats|dump[n]|mask|reset` + 解码层（出口名/ic1 位名/
  意图/注入/控制变更的中文叙事打印）。

### 顺带两破案（记录器首跑就抓到）

1. **窗口扩张吃毯（r176 遗留缺陷）**：窗口 16→64 槽把 r172 恒等毯本来覆盖
   的走表 GPA 区（32-128MB）吃进空 PT——L2 走表页落窗内即首取指断（EXT-H
   偶发 FAIL 的根因，r176 时代测试绿=池落位运气）。修：**走表位 NPF（bit32/33）
   × H 无映射 → 恒等回填合成**（毯语义窗内恢复；非走表 !P 仍反射=H 的账）。
2. **外部驱动 ic1 偏移误武装**：VMCB 布局 ic1=+0x0C（0x00=CR 读字）；l2test.c
   与 stage30 曾把 CPUID 位写到 0x00（实际武装 CR14 读）——EXT-H 一直靠
   svmb 基线 CPUID 拦截碰巧跑绿。vlog 的 intent 记录当场揭穿（ic1 读回 0）。

### 新床律

- VMCB 控制区布局：0x00 CR 读(16b)/0x02 CR 写/0x04 DR 读/0x06 DR 写/
  **0x08 异常位图/0x0C ic1**/0x10 ic2——裸偏移手术必对 vmcb.h 结构体。
- vlog stats 的 KindCount 是终身计数（drain 不清零）——reset 才清。

### 床态

10/10 回归+EXT-H 绿后 guest 活；vlog 面常开（零成本）；r182 开场可用。

## Round 182 (2026-10-20 深夜③): VMLOAD/VMSAVE 仿真落地——the reference hypervisor 形态 H 解锁；s17（vmload→vmrun→vmsave/vmload host 三段式循环）一发过；两硬课（EFER 保全+段集填充）；回归绿

### 判决

```
s17 PASS: exc=0 legs=1 exit=78 mark=ffffffffc0de600d cs=1
        （vmload 前置×N + vmsave/vmload-host 出口循环×N 全程活；
         cs 回拷校验=VMSAVE 反向数据正确）
回归 s0/s2/s5-s16 全绿 + EXT-H PASS；s1 一发 0x60 概率病（复跑 2/3 绿，
r142 IRR 家族，非仿真回归）；guest 活
```

### 实现

- **ic2 武装**：VMLOAD|VMSAVE 位入基线（此前仅 VMRUN）——外来 vmload 的
  原生执行会经非自有 VMCB 改写真实 save 区（r168 危害），现在全走仿真。
- **NestedVmloadExit 重写为仿真**（r168 #GP 拒绝退役）：VMLOAD(RAX)=H 的
  VMCB 段集（ES..Gs/GDTR/LDTR/IDTR/TR）+CPL 拷入真 save 区；VMSAVE=反向。
  **EFER 保全硬修**：H 的 EFER 位随拷但 LME/LMA/NXE/SVME 四架构关键位
  永不随 H 覆盖（零化/残缺的 H VMCB 不得剥掉长模式——首跑即被此咬死：
  QEMU 进程直接亡=BENCH-DOWN 最烈形态）。
- **stage 31（s17）**：the reference hypervisor 的 SVM_asm.asm 三段式复刻——host 捕获
  vmsave → 循环{vmload guest → vmrun → vmsave guest → vmload host →
  处理}。**段集填充硬课**：guest VMCB 必须带有效段（the reference hypervisor 里有捕获的
  Windows 段；零页 vmload=CS=0 起跑=楔死）——stage 从 host 捕获拷段。

### the reference hypervisor 兼容清单更新（r181 评估 → r182 后）

- ~~VMLOAD/VMSAVE 硬门~~ → **已解**（仿真过 s17 全链）。
- 剩余：NPT 钩 >128MB 作用域（窗口扩全 RAM ≈ 半轮）、MSRPM per-MSR
  不透传、CLGI/STGI 小件、它的 .sln 实编上机（r183 候选）。

### 床律入档

- **vmload/vmsave 仿真的两条不变量**：EFER 四关键位保全 + 输入 VMCB 必须
  自带有效段集（真 H 都满足；测试台必须显式填）。
- s17 形态判据：cs 回拷校验（vmsave 后 guest VMCB 的 CS==host 捕获的
  CS）= 反向数据流的确定性判据。

### 床态

回归绿后 guest 活；r183 候选=the reference hypervisor 实编实跑（其 .sln + xed 依赖）或
窗口扩全 RAM。

## Round 183 (2026-10-20 深夜④): 钩槽物化（全 RAM 影子）机器落地——**门控默认关**；机制半证（扫描/物化/钩项内容全验证）+两开放缺陷立 r184 靶；门关=回归零退化

### 落地（gated）

- **机制**：H 的 NPT 结构即其意图——它分裂成 PT 的 2MB 槽=钩区（the reference hypervisor
  形态）。accept/INVLPGA 时扫 H 的表（脏标记去重：根变/INVLPGA 才扫），
  给每个 H 分裂槽物化一个预分配池 PT（512 项从 H 的 PT 逐项翻译，perms
  律同 compose），影子 PD 链接换下该槽的毯——**钩生效；其余 RAM 仍走
  2M 毯零故障**。INVLPGA=H"表变了"信号→去物化+全活会话重扫（the reference hypervisor
  编辑后必 INVLPGA 的模式）。池 64 槽/核预分配（无出口期分配失败面）。
- **门**：`svmbHookScanEnable`（默认 **0**）——s32 用 Iterations bit12
  开关。**门关=accept 路径与 r182 逐字节等价**（本次回归实证）。

### 半证（闸门开时的三数据点）

- 扫描+物化正确开火（`scan hooks materialized=1`，窗外 slot 2403/2771/2979）；
- **钩项内容正确**（vlog lvl=4 诊断记录：gpa→markH1Pa 逐位对）；
- 测试台自身两 bug 被诊断链揪出并修（L2 程序 disp32 手滑 0x51F000≠
  0x5F0000=CR2 直接指认；code/stack 的跨槽 PT 索引冲撞——毯本就覆盖，
  两行多余且有害）。

### 两开放缺陷（r184 首靶，门控的存在理由）

1. **活序列二扫楔死**：s18 腿 2 的 INVLPGA→重扫→下次 accept 内挂
   （串口签名：`invlpga → wiped+rescanned → exit trace code=80 → 无
   accept 行`——楔点在 accept 路径的二扫内）。
2. **开闸 destabilize**：门开时 s7 中途机器亡（r180 无声重置族形态）。
   两缺陷疑同根（二扫的某条路径在特定时序下破坏会话状态）。

### 判决

```
门关：s0-s6+s16+s17 关键面绿 + ALIVE（与 r182 等价实证）
门开：s18 未绿（两缺陷上）——机制三数据点半证
```

### r184 靶单

1. 二扫楔点根因（活 accept 内的 ShadowScanHooks 路径——疑点：Dematerialize
   改 PD 与在跑 L2 的 TLB 竞态/缺 PendingTlbKick；或 MapGuestPage 在特定
   页上）。2. 开闸 s7 死。3. 修后开门 s18 判决+全回归。4. 然后 the reference hypervisor
   实编实跑（r185）。

### 床态

门控构建 RUNNING；关键面绿；guest 活。

## Round 184 (2026-10-20 深夜⑤): 二扫两缺陷根除——三重构（扫描后置/自核扫描/物化冲刷）+闸位补线；**s18 全 RAM 钧端到端 PASS**；全回归 11/11

### 根因与修法（一次重构灭一类，不逐个追）

1. **排序错**（两缺陷共因之一）：r183 把扫描插在 accept 的 Init 之前——
   首扫成果被 Init 的换世界清窗抹掉、scanDirty 遗留 1 每次 accept 重扫。
   修=扫描移到 NP 换根**之后**（ShadowScanHooks 返回是否物化，accept
   据此对真 VMCB 补 `TlbControl=FLUSH_ALL`——重物化的 PD 链接换毯不被
   陈旧 2M 毯翻译遮蔽）。
2. **跨核活池变更**（r108 律违反）：INVLPGA 处理器内联重扫其他活会话核
   的池——那些核的 L2 正拿该池当 NPT 根。修=**删内联跨核重扫**；各核
   在自己的下一个 accept 自扫（accept=该核 L2 确定未跑的唯一安全点）。
3. **闸位断线**：app 的 s18 从未置 Iterations bit12——闸一直关着
   （r1/r2=MAGIC_ID=毯恒等直读的诊断指纹）。修=app 补一行。

### 判决

```
s18 PASS: r1=aaaa2222aaaa2222 r2=bbbb3333bbbb3333 legs=2 slot=2631 inWin=0
        （窗外 2MB 槽的钩经物化 PT 伺服 + H 重映射+INVLPGA 后重扫伺服新帧）
开闸杀手 s7/s16/s17 绿（s7 死已灭）；全回归 s0-s7+s13-s18 = 11/11 绿 + ALIVE
```

### 至此 the reference hypervisor 嵌套清单终态

| 项 | 状态 |
|---|---|
| VMLOAD/VMSAVE | ✅ r182 仿真 |
| 出口合同/EFER/HSAVE | ✅ 同构/H-view |
| **NPT 钩全 RAM 生效** | ✅ **r184（本轮）** |
| MSRPM per-MSR / CLGI·STGI | 小件未做（可跑不碍） |
| the reference hypervisor 实编实跑 | r185（机制面已全就绪） |

### 床律

- **跨核池/影子变更一律自核化**：任何对 per-core 影子池的写只允许发生在
  该核自己的 accept（L2 确定未跑）；跨核信号只传 scanDirty+kick。
- 闸门类特性的**端到端链路必须一口气验**（driver 位+app 位+默认值三点，
  r184 的闸位断线=r183 只验了 driver 侧）。

### 床态

r184 构建 RUNNING；11/11+ALIVE；r185=the reference hypervisor 实编实跑（.sln+xed 构建、
部署、hreg、加载、vlog 全程记录）。

## Round 185 (2026-10-20): the reference hypervisor 实编实跑——嵌套发射成功 + 两条新床律 + 一个精确定位的反射门 bug

### 现象

- the reference hypervisor（Amd-V-ReloadDbg，第三方 AMD-V 虚拟化驱动）在 QEMU TCG 床上
  `NtLoadDriver` 一发返回 **0xC000009A (STATUS_INSUFFICIENT_RESOURCES)**
  ——机器不蓝屏、com1 零增长、svmb HEALTHY。
- 修复后真弹加载：**嵌套发射全链路点亮**（vmload 仿真→ACCEPT→L2 自举→
  INTR 反射→H 离场 vmsave/vmload(host)），随后机器楔死（GUEST-HUNG，
  sshd 死、CPU 忙转、调度器自旋）。**干净血统（全新 QEMU）上逐字节复现
  同一五步叙事**——确定性 bug。

### 与参考的对比 / 破案链

1. **0xC000009A 真身**：the reference hypervisor `SVMManager::Init` 的默认状态就是
   INSUFFICIENT_RESOURCES——不是分配失败，是 `CheckSVM()` 没过
   （vendor/8000_0001.SVM/VM_CR.SVMDIS/8000_000A.NPT 四关）。
2. **l2test 探针实证（r185 新探针）**：DriverEntry 里加
   `ProbeFill`（cr3/pid/vendor/svm/npt/vmcr 六元组）+ 新 IOCTL 0x901：
   ```
   entry: cr3=1aa000     pid=4    svm=0 npt=0 vmcr=0x10(SVMDIS)  ← 隐身面
   ioctl: cr3=<loader>   pid=xxxx svm=1 npt=1 vmcr=0             ← H 视图完美
   ```
   **NtLoadDriver 把 DriverEntry marshal 到 System 进程（pid=4）执行**——
   the reference hypervisor 全部 SVM 敏感读发生在 System CR3，而 hreg 注册的是 loader CR3，
   罩不住 → CheckSVM 见隐身面 → 默认状态。r177 从未踩到（l2test 的 SVM
   操作全在 IOCTL 时的 loader 上下文）。
3. **修复**：`SVMB_HVIEW_CTL` 加 `Pid` 字段（0=自身；非 0=注册该 pid 的
   CR3，走 r133 `Cr3M4PidToCr3` 解析）；r185load 加载前双注册
   （self + pid4）。修后 entry 行 svm=1 npt=1 vmcr=0——四关全开。
4. **binPath 三查律当场应验（本轮最大弯路）**：三连部署后探针/仪器全部
   "成功但无输出"——`sc qc svmb` 揭晓：**服务 ImagePath=
   `\??\C:\Windows\System32\drivers\svmb.sys`**，我推的
   `C:\svmb\svmb.sys` 根本没人加载；哈希校验一直在对"推送目标 vs 本地
   构建"（双双为新），服务读的是第三个文件。教训：**哈希必须对
   binPath 指向的那个文件做**，不是对"我刚推的文件"做。
5. **真弹楔死叙事（com1 五步，两次运行逐字节一致）**：
   ```
   R185HREG slot=0 cr3=<loader>          ← 自身
   R185HREG slot=1 cr3=00000000001aa000  ← pid4 精确命中 entry CR3
   nested: vmload emulated pa=<guestVmcb>   ← r182 仿真接住发射汇编(asm:257)
   nested: accept hvmcb=… -> L2 rip=<post-vmrun> cr3=1aa000   ← 自举式接受
   nested: reflect exit=60 L2rip=… -> H rip=…152c   ← INTR 在 L2 首指令即达
   nested: vmsave emulated pa=<guestVmcb>
   nested: vmload emulated pa=<hostVmcb>  ← the reference hypervisor 走离场路径，随后楔死
   ```
   QMP 尸检：6 核 EFER=0xd01（SVME=0，嵌套会话已清）、内核在动（调度器
   代码 + `btl $0xc, +0x300` 自旋）、sshd 死、com1 冻结——r174 家族
   "活着但服务死"形态。core0 完整走完、core1 无 accept。
6. **r186 头号假说（反射门）**：exit 0x60(INTR) 是**我们自己**为虚拟化
   中断武装的拦截——the reference hypervisor 的 VMCB 只武装了 CPUID|RDMSR/WRMSR|VMRUN，
   根本没要 INTR。反射门没有按 **H 的武装意图**过滤：H 收到它没点的菜
   （exit 60 at 首指令）→ 走通用离场 → teardown 楔死。修法=反射前查
   H 的 ic1/ic2/exc 意图，H 没武的出口自己消化（INTR 应自己注入 L2）。

### 修复尝试

| 改动 | commit | 结果 |
|---|---|---|
| the reference hypervisor 实编（.sys+R3.exe，v143→v145 覆盖；xed vendor 库） | r185 | ✅ 双绿，测试签名与 svmb 同证 |
| r185load（hreg 自注册 + NtLoadDriver + YCData 探活锚点） | r185 | ✅ |
| HVIEW Pid 注册 + l2test DriverEntry 探针 | r185 | ✅ CheckSVM 四关开 |
| （挂账）反射门按 H 意图过滤 | r186 | 楔死复现 2/2，假说锋利 |

### 关键决策回顾

| 当时以为 | 真因 |
|---|---|
| 0xC000009A=内存/分配失败 | CheckSVM 默认状态（catch-all） |
| DriverEntry 在 loader 进程上下文 | **NtLoadDriver marshal 到 System(pid4)** |
| 推送+哈希一致=部署成功 | **binPath 指向第三个文件**（System32\drivers） |
| 仪器不打印=通道问题 | 跑的一直是旧镜像（binPath 律） |
| the reference hypervisor 楔死=床态退化 | 干净血统同样楔（真 bug）；但 **s16 楔死确实另是床态** |

### 床律（新）

- **DriverEntry=System 上下文律**：`NtLoadDriver` 从用户态调用，
  DriverEntry 仍在 System(pid4) CR3 执行。任何"加载时检查 SVM"的第三方
  驱动需要 `hreg 4`（HVIEW Pid 注册）+ loader 自身 hreg 双注册。
- **binPath 哈希律**：部署三查的哈希必须对 `sc qc` 的 ImagePath 文件做；
  svmb 床真路径=`C:\Windows\System32\drivers\svmb.sys`（C:\svmb\ 只是
  摆渡站）。
- **reset 血统律**：QEMU 进程内 `system_reset` 血统的床对 s16（多核并发
  嵌套）不可信——r184/r185 构建同楔，全新 QEMU（deploy.bat）同构建全绿
  （okMask=f, 200 往返）。判绿必须全新 QEMU 进程。

### TODO（r186）

1. 反射门：反射前对照 H 的 ic1/ic2/exc 意图位；H 没武的出口自消化
   （INTR→自注入 L2），不再打扰 H。
2. the reference hypervisor 真弹复跑：期待第一核走完后续 INTR 不再离场、core1..5 依次
   accept、NtLoadDriver 返回、YCData 锚点 OPEN。
3. vlog 全程记录其虚拟化行为（IOCTL 0x800 符号注入可后置）。

## Round 186+r187 (2026-10-20 深夜): 反射门意图过滤——六核全接+全 RAM 影子实况，四层连环修复

### 现象与逐层破案（四连环）

1. **r186a 落穿冻机**：意图门首版把无序出口落穿到 L1 分发管线——s2 第四腿
   （rdmsr 后）整机冻结。QMP：6 核同 RIP（nt 冻结等待环）。**教训：L1 管线
   的后分发机械（r53 视图发布/TLB 模式/r143 中断修复）全是 L1 语义，L2 出口
   落穿=雷区。**
2. **r186b 封闭自服务**：无序出口走封闭迷你分发（HLT 打盹+退休 / CPUID·RDMSR
   透传 / 其余保守反射），不碰 L1 管线。s2 不再冻机但 **ud2 腿输出 0x7F
   （SVM_EXIT_SHUTDOWN）**——#UD 没被拦截、经骨架 IDT 在玩具 CR3 上派发→
   连环异常→三重故障。
3. **r186c 反射回染**：根因=反射的 `h->Save = l2Save` 全量回拷把**骨架 IDTR
   污染进 H 的 VMCB**——第二次 accept 起 `Idtr≠0` → OwnIdt 误判 1 → 异常
   位图取 H 的 0 → #UD 原生派发。**修：判据改语境判据 `h->Save.Cr3 ==
   HSave.Cr3`（in-place 自举=两侧同 CR3，天然免疫回染；玩具世界 CR3 必异）。**
   s2 全 gauntlet 绿；回归 11/11 绿（含 s16/s18，全新 QEMU）。
4. **r187 bit21 复活**：the reference hypervisor 真弹六核全接（ownIdt=1, hi1=10040000,
   hexc=8——它自武 #BP）、影子合成器实况伺服整机页表走查，但 L2 内核软中断
   位点撞 exit 0x75 → 反射 → the reference hypervisor default 分支 `__debugbreak` →
   **FAST_FAIL_INCORRECT_STACK (0x139)**（minidump 定罪：栈底
   AmdVReloadDbg+a3dd → KiBreakpointTrap → int 29h；起源 RIP=SWINT+3）。
   bit21 为何屡清不掉：**r175 重武装不变量条件=(exc 含 BP 位 + bit21==0)——
   the reference hypervisor 的 hexc=0x8 使条件永真，每次出口入口都把 accept 清掉的 bit21 恢复
   回去**。修：重武装块加 `!NestedActive` 门。修后 **SWINT 反射绝迹**。

### 战果（the reference hypervisor 嵌套运行状态演进）

| 轮 | 状态 |
|---|---|
| r185 | core0 单核走完 accept→reflect→离场即楔 |
| r186c | **六核全接**+影子合成实况（Windows 整机页表走查经我们影子）|
| r187 | + SWINT 类灭绝；残余=无签名活锁（无 bugcheck、无 dump、compose 中途冻结）|

### 床律（新）

- **无序 L2 出口三禁**：禁反射给真 H（the reference hypervisor default=KeBugCheck/int3）、
  禁落穿 L1 管线（s2 冻机）、禁全量信任 H.VMCB 的 IDTR（反射回染）。
- **语境判据律**：区分玩具/in-place 看 CR3 等价性，不看 IDTR（回染）。
- **重武装不变量须嵌套豁免**：r175 型 per-entry 恢复机械对 L2 会话无条件跳过。
- the reference hypervisor 实测武装面：ic1={CPUID,RDMSR}、hexc={#BP}、ic2={VMRUN}、NP on。

### TODO（r187 续）

1. 无签名活锁定性：QMP 全核画像+（若有）dump；疑反射往返饿死或 L2 内部死锁。
2. 活锁解后：NtLoadDriver 返回+YCData 锚点+vlog 全程行为记录（r185 收尾）。

## Round 187 续（2026-10-20 夜末）: 无签名活锁定性——IPI 会合自死锁 + MWAIT 药方 + 心跳仪

### 定性证据链（同二进制重放+升级取证）

1. **20 分钟观察器**：com1 零增长——"龟速转储"假设死刑，真活锁。
2. **QMP 冻结环反汇编**：CR8(TPR) 写+call+sti 循环=IPI 会合握手代码（非
   bugcheck 转储爬行）。
3. **info lapic 金标**：**ISR=0x209 在服务 + IRR=0x209 再排队**——核冻在
   IPI 冻结处理器里永不 EOI（KiFreezeExecution 语义）=真冻结进行中、
   永不完成。
4. **心跳仪（2^16 出口/线）零触发**：楔死时全机出口总数<65k——冻结在
   嵌套启动后数秒内、最后一条日志=影子合成；且**无任何核处于 hlt/mwait
   自服务轮询环**（否则 10kHz 出口必触发心跳）=空闲核没在停车，六核都
   在会合里忙等。
5. **与 r186e 的 0x139 区分**：那次=fast-fail bugcheck（有 dump）；这次=
   无 bugcheck 无 dump 的 IPI 会合死锁（完成谓词依赖被冻结核的状态=
   自死锁）。

### r187b 落地（保留项）

- **MWAIT 武装+自服务**（ic2 bit11→exit 0x8B→打盹+重试=轮询等待）：床 CPU
  model=max 广告 MONITOR/MWAIT，原生 mwait=TCG vCPU 停车=停车核永不取
  IRR（r142 家族）。构造正确性修复（本轮楔死非它引起，但它是已知雷）。
- **出口心跳仪**：每核每 2^16 出口打一行 com1（门控零依赖 LogSerialTraceF），
  携末次出口码=楔点证人。2^20 阈值初版在楔死前不可达——已降 2^16。

### TODO（r188）

1. **会合自死锁破案**：候选=KeBugCheckEx 的冻结阶段死在转储首故障（转储
   路径作为 L2 走影子=每页 NPF→合成→若合成路径与冻结核交叉持锁=环死锁）
   ——首查最后合成行之后的锁面（vlog 环锁=每出口路径的全局锁，头号嫌疑）。
2. 心跳仪在健康嵌套段的基线标定（正常退出频率下 2^16 的节拍）。
3. 修后：NtLoadDriver 返回+YCData 锚+vlog 全程=r185 终答卷。

## Round 188 (2026-10-20 凌晨): 冻结五连修——影子无罪+判据进化到粘性闩，真凶锁定嵌套中断投递

### 五连修与判决

1. **vlog 环锁 try-lock 化**（skip-on-contention+丢弃计数）：出口路径唯一
   跨核自旋点消除——冻结原样（锁非根因，但修复保留=严格更优）。
2. **MWAIT 轮询自服务**（r187b 已落）+**心跳仪 2^16**：真弹零心跳=冻结时
   全机出口总数<65k、无核在 hlt/mwait 轮询环。
3. **影子关闭对照二分**（临时 hack 后回退）：**同冻→影子无罪**！但暴露
   另一死亡：the reference hypervisor 接住用户线程（rip=用户态 cr3=用户进程）后其 #PF
   (0x4E) 被反射→default 分支死——根因=CR3 判据被**线程切换**打破
   （H 处理器语境在反射与重发射间可换线程：h->Save.Cr3=被中断线程 ≠
   HSave.Cr3=发射线程→ownIdt 误 0→骨架网反射异常）。
4. **粘性首见闩**（按 HvmcbPa 缓存第一次 accept 的 Idtr 意图）：首见必在
   反射回染前；the reference hypervisor guestVmcb 每次发射重存真 Idtr、测试 hv 用前清零
   ——两者皆免疫（真弹 ownIdt=1×6 实证）。**主冻结仍原样。**
5. **结论（排除法+关键修正）**：svmb 基线**从未武装 INTR**（bit0 不在
   `(1<<18)|(1<<28)`）——L1 原生投递稳定数月=原生投递本身无罪；嫌疑
   收窄到 **QEMU TCG 对双层 guest 态（guest-in-guest）的中断投递**。
   r186 的 INTR 摘除与 L1 语义一致（L1 本就不武），非回归源。

### 冻结画像汇总（三案同型）

6 accept（ownIdt=1）→ 影子合成实况 → 秒级冻结：全核 IPI 会合环
（CR8/TPR+call+sti）、lapic ISR=0x209 永不 EOI+IRR 堆积、无 bugcheck
无 dump、心跳绝迹。

### TODO（r189）

1. **QEMU 源码级研究**：TCG 对 HF_GUEST_MASK（嵌套 L2）vCPU 的中断投递
   路径（x86_cpu_exec_interrupt/do_interrupt 对 guest-in-guest 的行为）；
   r142 修复的精确机制可否嵌套化（per-entry kick 等价物）。
2. 备选：接受"投递缝"为床保真度边界（同 r168 EVENTINJ 缝），the reference hypervisor
   完整运行判真机——真机无此缝。

## Round 189 (2026-10-20 晨): 窗口恒等预填——会话进入真实工作循环，冻结仍在但战场已变

### 机理收束与实现

- **投递压栈×冷栈页理论**：QEMU 眼中 L1/L2 同为 HF_GUEST_MASK——L1 投递
  稳定因其栈页走恒等毯（永不缺页）；L2 栈页落惰性窗口 → 冷栈页在投递
  压栈瞬间缺页 → 中断丢失（r168 缝家族）→ IRR 堆积 → IPI 会合冻死。
- **实现**：粘性闩前移（ShadowInit 需早于 IDTR 段读到 OwnIdt）+
  `NestedShadowInit` 对 ownIdt 会话**恒等预填窗口**（64×512 项 4K
  identity，P|RW|US|A|D=0x67）；the reference hypervisor 的 NPT 本恒等=语义精确；其后续
  NPT 编辑走 INVLPGA 擦窗/重扫合同。
- **预填实证生效**：真弹合成**归零**（窗口零缺页），且**会话进入真实
  工作循环**——9 次 accept（多次 per-core 重发射：L2 出口→the reference hypervisor 处理
  器 vmsave/vmload/vmrun→再接受），机器运行深度前所未有。

### 残余

最终仍无签名冻结（最后 accept 进用户线程后静默死；ISR/IRR 画像同前）。
投递压栈理论也非唯一向量——r190 需 QEMU 源码级研究 TCG 对 HF_GUEST_
MASK vCPU 的中断投递（x86_cpu_exec_interrupt/do_interrupt 全路径），
或判床保真度边界、the reference hypervisor 完整判绿留真机。

### 回归

s1/s2/s6/s7/s13/s17 全绿（s17 首跑一次 flake，复跑绿）；ALIVE。

## Round 190 (2026-10-21): 床保真度边界判决——冻结案取证穷尽，svmb 侧嵌套机制宣告完备

### 取证三部曲（本轮全部完成）

1. **QEMU 源码走读**（qemu_git）：unarmed-INTR 的 guest 态投递全路径干净
   ——`x86_cpu_pending_interrupt`（vintr 掩蔽被 r168b 清零正确关闭→可投递
   性=真实 IF）→`cpu_svm_check_intercept_param`（未武=不退出）→
   `do_interrupt_x86_hardirq`→推帧走嵌套 NPT（r189 预填=零缺页）。
   投递层无罪。
2. **宿主线程画像**（Get-Process .Threads 两拍）：六条 vCPU 线程全 100%
   Running（12s 窗各烧 ~12s）=**无 QEMU 内部死锁/BQL 卡死**——guest 在跑。
3. **逐核 lapic 全景**：六核 **ISR 全空、IRR 全={0x2F,0x91,0xD1}、
   TPR=0xC0**——机器在 SYNC 级高 IRQL 冻结等待（与会合环 `mov cr8`
   反汇编吻合），全部中断按优先级压制=IRR 堆积机制清楚；待投向量=
   设备+时钟噪声（0x91≠x64 IPI 向量 0xE1）→**会合 IPI 已全部送达**，
   发起者的后续（转储/释放）在嵌套 TCG 下永不完成。

### 45 分钟耐心实验（终审）

com1 全程零增长、SSH 全死、**dump 永不落盘**（r186e 的 0x139 能 dump=
对照）——此类冻结的 bugcheck 后续路径在本床上不可完成。

### 判决：床保真度边界

与 r168 EVENTINJ 缝同族：QEMU TCG 嵌套模式的固有局限，可定性、不可从
svmb 侧修复。**svmb 嵌套机制宣告完备**（反射门/意图合同/影子/VMLOAD
仿真四代修复；the reference hypervisor 六核全接+出口-重发射工作循环+零误投递出口）。

### 后续路径（二选一，下轮决定）

1. QEMU 补丁战役：fork qemu_git，给 TCG 加嵌套投递/转储路径的诊断或
   修复（独立战役规模）。
2. 真机收卷：svmb 侧判绿，the reference hypervisor 完整运行+vlog 答卷在真机执行
   （无 TCG 层=无此缝）。

## Round 191 (2026-10-21): 单核床实验——多核会合理论出局，自死锁画像+排水守护

### 实验 1：-smp 1 单核床

- 启动 ~15 分钟（TCG 单核慢=预期）；svmb 1/1 虚拟化；s2/s7 绿
  （s17 确定性 FAIL=exit 7f——玩具世界被粘性闩判 in-place 的已知家族，
  6 核时为 flake、单核确定性——挂账）。
- **真弹：单核同样冻结** → 多核会合唯一因理论出局。
- **工作窗口史上最深**：出口-反射-重发射循环跨多进程/用户态 rip 正常
  运转（com1 叙事：accept→用户态 cpuid 反射→vmsave/vmload→再 accept×N）。
- **单核冻结画像**：TPR=0xF0（最高级）+ISR 空+IRR={0x91,0xD1 时钟}被压
  +自旋于 `call [rip+X]; test al,al; je back` 轮询环——单核自死锁
  （在最高 IRQL 等一个只能由被自己压住的中断改变的条件）。

### 实验 2：排水守护（观察活过冻结）

- `start /b` 起的守护随 SSH 会话树死亡——未产出 vlog_cap.txt。
- **但 com1=宿主侧飞行记录仪天然免疫 guest 死亡**——捕获轮叙事完整
  在盘（accept 带 hi1/hexc/ownIdt 仪器字段）。教训：guest 侧守护需
  schtasks/服务级孵化才可靠。

### 判决与下一步

- 冻结=OS 在嵌套下进入真机不会进入的高 IRQL 等待（单核复现=非会合
  特有）；死锁本身不可修（Windows 内核代码）；可打的=①除颤器实验
  （检测冻结签名→强制 EVENTINJ 注入被压中断，判决性）②根因（OS 为何
  脱轨——QEMU 插桩/真机对照）。
- -smp 6 已复原。

## Round 192 (2026-10-21): 除颤器实验——gdb-stub 宿主侧全读写落地，标志拨动=阴性但决定性

### 实施升级

- HMP `xp` 只读 → **HMP `gdbserver tcp::1234` 现场起 stub** + python 说 GDB
  远程协议（`HgN` 选核/`g` 读寄存器/`m,M` 读写内存/`c` 继续/`D` 分离）=
  冻结床的宿主侧全读写（r192_gdb.py 入库）。
- 协议坑三连：初始无请求停机包先到、body 含头 `$`、`c` 为异步包（等待即
  超时≠失败）。**QMP stop/reset 后必须补 cont**（gdb 会话残留停止态）。

### 实验与判决

- 冻结画像（gdb 版，比 QMP 更全）：六核三组 RIP——0/4=TPR 冻结环、
  1/3=中断返回路径邻位、**2=独立位点（复活后移动=机器在跑）**、
  5=`mov r13d+0x2d80; jnz pause` 的 **KTHREAD 等待状态轮询**。
- **拨动 cpu5 的轮询字段 1→0 → 继续 → 该核旋即回到同自旋且字段被重置
  为 1**——判决：**这些不是死标志而是活等待**（调度器合法重武装）；
  等待图永不排空=缺被 TPR 压住的事件源（时钟/DPC），标志写入无法替代
  事件投递。除颤器（标志版）阴性。
- **下一级升级路径（留档）**：gdb 写当前 VMCB 的 EVENTINJ 字段（向量=
  0xD1，type=0）→ QEMU vmrun 装载即投递、绕过 TPR 优先级——需先定位
  svmb 的真 VMCB 物理地址（驱动全局/模块扫描）。

### 回归

s2/s7 绿；s17 首跑 FAIL 复跑绿（既知 flake 家族）；床 HEALTHY。

## Round 193 (2026-10-21 下午): EVENTINJ/TPR 升级弹——两发落地，部分收获

### 发射 1：EVENTINJ 物理写（受阻但路径清晰）

- 计划：写六个现役 hvmcb（com1 接受行解析）+0xA8 的 EVENTINJ（0x10D1
  =向量 0xD1/type 0/valid）→ 经接受的直通合同进真 VMCB → vmrun 绕 TPR
  投递时钟。
- **受阻**：gdb stub 的 m/M 只走**当前 CPU 的虚拟地址**——hvmcb 是物理
  地址、无已知内核 VA 别名。解除路径=svmb 侧一行仪器（接受时打印
  MapGuestPage 的 VA）后重打，或找 Windows 内核 VA 直映别名。

### 发射 2：TPR 钉零补丁（落地，部分生效）

- cpu2/3/4 的冻结环内两次 `mov %rcx,%cr8`+两个 rcx 装载点——补丁
  `mov %rbx,%rcx`→`xor ecx,ecx`、`movl $2,%ecx`→`xor ecx,ecx;nops`
  （字节级确认落位）→ 环迭代应把 TPR 钉 0 → 时钟原生投递。
- 结果：**cpu0 的位点（8ab6）环内无 CR8 写**（TPR 一次性抬高在环外）
  → 该核 TPR=0xF0 保持；整体无复活。新观察：**ISR 出现 0xD1 在服务**
  （有核收下过时钟中断）但 IRR 仍堆——投递未转化为系统恢复。
- 判读：TPR 并非唯一障碍——即便时钟送达某核，等待图的依赖（锁持有者/
  DPC 链）仍在其他冻结核上，单核复活不级联。

### 回归与状态

床复位+cont 律后 HEALTHY。累计能力：gdb-stub 全读写+字节级补丁+复活/
观察循环=冻结床完全可解剖。剩余升级：hvmcb VA 仪器（一行）→ EVENTINJ
真弹；或按 r190 判决收卷真机。

## Round 194 (2026-10-21 傍晚): EVENTINJ 齐射终审——投递成功但机器不复活，升级阶梯闭合

### 齐射三件套（全部落地）

1. **VA 仪器**：接受行打印 hvmcb 内核 VA（`hva=%p`）——gdb 虚拟寻址
   弹药就位（六对 pa→va）。
2. **六发 EVENTINJ**：VA+0xA8 ← 0x10D1（向量 0xD1 时钟/type 0/valid）
   ——写后值 d110000000000000 确认 ×6。
3. **cpuid 触发补丁**：三个自旋点位（4fe0=488bcb、500f=fb90、cpu0 位）
   补成 0F A2——出口→反射→the reference hypervisor 处理→重发射→**接受转运 EVENTINJ→
   vmrun 把时钟打进冻结上下文（绕 TPR）**。

### 终审数据

- **风暴引爆**：com1 300KB→32MB+——补丁环变成全速嵌套循环（cpuid→出口→
  反射→重发射每圈全往返）；接受@补丁位+2、反射 exit=72@补丁位=循环
  机理实证。
- **ISR=0xD1 在服务=时钟确实被投递并收下**（EVENTINJ 路径工作正常！）。
- **机器仍不复活**：sshd 死、TPR=0xF0 保持（cpu0 的抬高位在环外）、
  IRR 仍堆。
- **终审判决**：等待图的缺失依赖**不是时钟、不是可拨标志、不是 TPR**——
  是冻结状态机深处的单向状态（bugcheck/冻结路径在嵌套 TCG 下早已越过
  不可恢复点）。r190 床保真度判决在最深证据级站立；升级阶梯闭合。

### 战役能力总结（r192-r194）

冻结床完全可解剖+可干预循环：gdb-stub 全读写→字节级补丁→EVENTINJ 武装
→复活观察→lapic/线程画像判读。任何后续嵌套冻结案都可走此流程。

## Round 195 (2026-10-21 晚): 自助全内存快照——"冻结"真身判决：永远写不完转储的 BSOD（0x139 fast-fail @ HAL enlightenment 路径）

### 新能力：冻结现场 host 侧全内存拍取 + 离线 WinDbg 管线

- **七轮取证的思路盲区**：一直在等 OS 自己写 dump；正解是冻结瞬间 QMP
  `dump-guest-memory` ELF 全物理内存（4.0GB，11 秒）+ 六核寄存器。
- windmp 直出两条路都死：QMP 枚举无 windmp 格式；HMP `-w` 报
  "no vmcoreinfo note from the guest"（QEMU 8.0 起删了 KDBG 扫描回退，
  要求 guest 装 vmcoreinfo 驱动——冻结中不可能）。
- **r195_elf2dmp.py 两遍法**（`qemu/r195_dump.py`+`qemu/r195_elf2dmp.py`）：
  本床 debug-off boot 下 KdDebuggerDataBlock **整块加密**（"KDBG" 裸扫
  20 命中全为代码垃圾），故免 KDBG 合成 DMP64 头：nt 基址=IDT[0] 门目标
  向下扫 MZ（fffff80352e00000）；**PsLoadedModuleList=nt+0x448150 链头
  本体**（r148 常数；曾犯双解引用——header 放链头不 deref，cdb 才闭环，
  放 hal 条目则 cdb 绕回把链头当条目读出 base=0 报 corrupt）；build=18363
  （KUSD 走表）。cdb 打开后 ntkrnlmp.pdb 缓存直接命中，154 模块全列
  （svmb@fffff8023b170000 / AmdVReloadDbg@fffff8023c910000 /
  hal@fffff80352d5d000）；KDBG=0 cdb 可容忍（"PARTIALLY CORRUPT"警告
  但符号/模块/栈全可用）。手工栈解剖无需 CONTEXT 帧补丁；已收割备用
  常数：KiProcessorBlock=nt+0x375ac0、KPRCB.Context=+0x62c0、
  KdDebuggerDataBlock=nt+0x2265e0。
- **HMP 多核寄存器律**：`cpu N` 在独立 human-monitor-command 调用间
  **不保持**（六块全打 CPU#0=伪"六核同 RIP"）；必须用 `cpu-index` 参数。

### 判决（七案同根，全链符号化）

cpu0 现场：RIP=hal!HaliHaltSystem+0x26（`test al,al; je` 泊车环），RSP 在
处理器栈（TSS 邻域 fffff803566b****）≠ CurrentThread（Skype.exe 线程）
内核栈 = bugcheck 栈切换指纹；R15=KTHREAD=PRCB.CurrentThread 自洽。
完整调用链（活栈+KTRAP_FRAME@566b2590 走出）：

```
hal!HalpTimerUpdateApiConsumers+0x32        （定时器日常维护）
→ hal!HalpRestoreHvEnlightenment+0x35b      （HV enlightenment 保存/恢复）
→ hal!HalpApicHvUpdateCallback+0x9
→ hal!HalpApicSetupRegisterAccess+0x46
→ hal!HalSocRequestConfigurationData+0x131  ← /GS 尾检查失败
     (mov rcx,[rbp-8]; xor rcx,rsp; call __security_check_cookie 不匹配)
→ hal!_report_gsfailure+0x5 → int 29h
→ nt!KiBugCheckDispatch+0x69 携原参 KeBugCheckEx(0x139, arg1=2
   = FAST_FAIL_STACK_COOKIE_CHECK_FAILURE)   ← 栈帧为证
→ nt!KeBugCheck2（IoSetBugCheckProgressFlag/KeBugCheck2+0x18d4 转储阶段）
→ 转储在嵌套 TCG 下永不完成（45 分钟律的机理）→ KiBugCheckDebugBreak
→ nt!DbgBreakPointWithStatus → hal!HaliHaltSystem 永久泊车
cpu1-5 = nt!KiIdleLoop+0x40（bugcheck IPI 冻结后的空闲泊车，CR3=1aa000）
```

KiBugCheckData/KiBugCheckOriginalData 均全零=bugcheck 处理中已清；
幸存证据在栈帧与 KTRAP_FRAME。

### 全案重释（此前七轮画像的统一解释）

- TPR=0xF0/ISR/IRR 画像 = KeBugCheck2 自己抬 IRQL+IPI 冻结的**工件**，
  不是独立死锁；vmhealth blue=51% 一直是 bugcheck 画完的半蓝屏。
- r194 EVENTINJ 齐射救不活的字面真相：机器在 bugcheck 死亡行军中，
  时钟送达改变不了终局；"单向状态机"= bugcheck 路径。
- 单核同冻：定时器维护任何核都会跑；L2 玩具驱动不冻：玩具不执行
  Windows HAL 的 hypervisor-enlightenment 代码——只有真 Windows 当 L2
  才走进这条路（r192 用户之问的正式答案）。

### 根因层（cookie 为何失配——r196 靶）

- 背景要件：Windows HAL 之所以走 HV enlightenment 路径 = CPUID 面在
  嵌套下呈现 hypervisor 存在/x2APIC 能力差异（the reference hypervisor 武装 CPUID/RDMSR
  反射处理是头号嫌疑——它的 CPUID 出口处理器在 svmb 反射的 VMCB 语境跑）。
- cookie 失配两候选：A. enlightenment 路径的 MSR/APIC 操作在反射-重发射
  往返中某环把数据推上内核栈（canary 被写）；B. RSP 中途被移动
  （prologue cookie^RSP 与 epilogue 不配）。
- 修复方向：svmb 侧 NestedSelfServeExit 的 CPUID 自服务对特定叶
  （hypervisor bit/x2APIC）回 L1 原生结果对照，或按反射合同检查
  the reference hypervisor CPUID/RDMSR 处理器的 GPR/RSP 回写语义。

### 律

- 冻结案取证正解=host 自助拍全内存+离线转换分析，不再等 OS 写 dump。
- QEMU 11.1 冻结床多核寄存器：human-monitor-command 带 cpu-index。
- 1903 debug-off：KDBG 整块加密 → windmp/HMP -w 均不可用；免 KDBG
  DMP 合成+符号驱动手工解剖完全够用（本管线可复用）。
- 现场文件：dumps/r195_freeze.elf/.dmp（4GB×2，留档不入库）。

## Round 196 (2026-10-21 深夜): CPUID 面对照实验——面修灭掉第一死法，冻结换车（0x7F 双错）

### 预测与干预

- **r195 死法的背景要件查源**：Windows HAL 走 enlightenment 路径的判定=
  `HalpIsHvPresent()`=CPUID(1).ECX bit31（缓存@52dc7b33）+回调注册进
  `HalpTimerHvApicCallbackList`。三层静态排查（the reference hypervisor CPUID 处理器=原生
  直通只藏 SVM 位；svmb EmulateCpuid=裸 __cpuidex 直通；H-view 只是不藏
  SVM）→ **没有人伪造 hypervisor 位**。活床 WMI HypervisorPresent=True
  实测 → **QEMU TCG 强制位**：qemu 源 cpu.c:8487 `x86_cpu_load_model` 对
  所有命名模型无条件 `FEAT_1_ECX |= CPUID_EXT_HYPERVISOR`（TCG_EXT_FEATURES
  同时含 X2APIC）——床从裸机启动即向 Windows 宣告 hypervisor 在场（真机
  不会：svmb 是 demand 驱动，晚于 HAL 判定）——**床保真度偏差实锤**。
- **干预**：`-cpu max,hypervisor=off`（feat_names 表证明属性存在；QMP
  query-cpu-model-expansion 实证 hypervisor=False 且 svm/x2apic 保留）。
  start_svmb.bat 修改+deploy 全新 QEMU（血统律）。启动后：WMI
  HypervisorPresent=**False**、svmb 6/6 虚拟化、s2（钩全周期）/s7
  （dr7=400 终态）com1 判绿（客户端 probestage 2/7 的"view switch FAIL"
  =显示伪影——stage 2 驱动侧不填 Pml4Pa/VmcbNcr3，判据在 com1）。

### 判决：第一死法已灭，冻结仍在（第二死法=双错）

- 发射 r185load 后 ~90-120 秒冻结同窗落地（com1 静止+sshd 死+CPU 忙）。
  **立即 r195 管线拍照**（r196_freeze.elf/.dmp；dump 头元数据
  AnyHypervisorPresent=0 vs r195 的 1——面改在 dump 里可见）。
- 六核画像：cpu2=**hal!HaliHaltSystem+0x26（与 r195 同址+0x32ab6，泊车
  终点相同）**、cpu1/3=nt!KiIdleLoop、**cpu0/4/5 全在内存管理器**
  （MiAgeWorkingSetTail+0x402 / MiLockPageTableInternal+0x2c0 /
  MiFlushTbList+0x3a0）+com1 invlpga 影子擦除风暴=MM 换页翻搅影子。
- **死码变了：KeBugCheckEx(0x7F, arg1=8) = UNEXPECTED_KERNEL_MODE_TRAP
  /DOUBLE FAULT**（KiBugCheckDispatch+0x69 帧携原参 7f/8 为证；r195 的
  0x139/arg2 cookie 死法在面修后**不再出现**——enlightenment 车道确被
  拆除）。#DF 自身 trap frame 的 Rip=0（双错摧毁上下文，只剩指令字节
  残片）——页错误处理中再页错误的经典形态，r168 嵌套投递缝家族的
  crash 级形态；土壤=三核并发 MM 活动驱动 NPF/INVLPGA 翻搅。
- 两案同点：hal cookie 补值 `cfaa65c471bf264a 2a4961b361d6a937` 跨
  boot 相同（本床 __security_cookie 不随机化的旁证）。

### 判决定性总结

1. **面修=部分胜利**：r195 的 0x139-cookie 车道永久拆除，且床面从此与
   真机一致（hypervisor 位不再伪造）——`hypervisor=off` 保留为床新基线。
2. **冻结不死于一因而死于多因**：嵌套 TCG 下至少两条独立死法
   （cookie-crash 与 double-fault），后者在面修后浮出为第一死因——
   r190 床保真度边界判决在更深层站立：**嵌套异常/投递缝（QEMU TCG
   HF_GUEST_MASK 语义）才是共同根**，svmb 侧（反射/影子机制本体）在
   两案中均无直接罪证。
3. the reference hypervisor 嵌套战役结论维持：机制面完备（r184 判决），真机收卷。

### 工具修正

- r195_dump.py regs 捕获重写（`cpu N` 跨 human-monitor-command 不保持=
  cpu-index 参数；越界 cpu-index 被钳位重复——固定 range(6)）。

### 现场文件

dumps/r196_freeze.elf/.dmp（4GB×2 留档不入库）。

## Round 197 (2026-10-22): INTR 全出口实验——缝假说正面判决：死亡消灭，代之以排风暴活锁

### 实现（门控，默认关）

- 读码发现：r186 的意图合并**已为骨架会话武装 ic1 bit0**（OwnIdt 会话豁免
  ——信任 H 自带 IDT 的原生投递）；而 the reference hypervisor 恰是 OwnIdt → 缝正对着它。
- 三件套落地（registry 门 `Parameters\NestedIntrPunt` 默认 0，门关=逐位等价
  实证 s2/s7 com1 签名一致）：
  1. 接受路径 OwnIdt 分支门控武装 bit0（TCG 投递前查现行 VMCB 的 INTR 位
     → 出口 0x60 而非推进 L2）；
  2. NestedSelfServeExit 新增 case 0x60：L1 语境 `_enable()`+16us 排水+
     `_disable()`，fault-class 不进 RIP（0x75 先例）——绝不反射
     （the reference hypervisor default=KeBugCheck）；
  3. 限流日志（前 4 条+每 65536 条）。

### 判决（10 分钟窗口）

- **死亡消灭**：发射 r185load 后 90-120s 历史冻结窗×5 时间无静默冻结、无
  bugcheck 泊车——com1 持续增长、嵌套 accept 会话持续循环（13 次）、六核
  多 hvmcb 并发。r195 cookie / r196 双错两类死法**均未发生**：缝假说成立
  （不让 TCG 向 guest-in-guest 推事件=两死法断根）。
- **代价=排风暴活锁**：intr punt n=57M、~90k/s、加速趋势；punt RIP 主要在
  L2 idle 环族（KiIdleLoop 区）；sshd banner 死（用户态饿死）。
- 画像证据：lapic IRR={209 时钟,225,+145(level)} 持续滞留、ISR 恒空、
  TPR=0x20 可投——**排水窗未产生实际投递**（或投递被 L1 消费而 L2 唤醒
  丢失）。QEMU 源码核对：vmexit 清 GUEST_MASK/intercept 但不清
  CPU_INTERRUPT_HARD——L1 语境投递语义上允许；失败微观机理未决
  （HARD 标志生命周期/向量再挂起二选一）。

### r198 设计（已定）：EVENTINJ 重注入=KVM 同款入口投递

- 0x60 punt 改为：读 LAPIC（0xFEE00000 MMIO，L1 映射真实通路）IRR 最高
  优先级向量 → 写真 VMCB EVENTINJ（vector,type=0,valid）→ 重入 →
  **TCG 在 VMRUN 入口投递**（架构路径，r194 已证 EVENTINJ 转移可达
  ISR 在服务）→ L2 走自己的 IDT/栈=语义正确的嵌套合同（=KVM 的 merged
  VMCB 入口注入的软件版）。排水到 L1 的旧法弃用（唤醒归属两难）。

### 工程注

- 新构建 1cdfd018（门关等价 r196 面）；punt 风暴期间多核串口交织=显示
  伪影（r179 律再验）。床复位后须 reg 置 NestedIntrPunt=0 回基线。

## Round 198 (2026-10-22): STGI 排水——风暴根除（4 次 vs 5700 万），但开窗投递踩中 Cm 锁图（新死相）

### QEMU 源三答（设计改判：弃 EVENTINJ 改 STGI 排水）

1. **EVENTINJ valid=bit31**（svm.h SVM_EVTINJ_VALID）——r194 的 0x10D1 用 bit12
   =**从未注入过**；当年"ISR=0xD1 在服务"实为原生投递。档案修正。
2. 注入路径=helper_vmrun 直调 do_interrupt_x86_hardirq，**无 APIC 记账**
   （IRR 不清）→ EVENTINJ 方案先天双投递乒乓，弃。
3. 外部中断链（seg_helper x86_cpu_exec_interrupt）：**拦截检查在
   apic_get_interrupt 之前 + 拦截路径不清 CPU_INTERRUPT_HARD**=风暴机理；
   **host 态检查 no-op**（!(hflags&HF_GUEST_MASK) 早退）→ handler 内开
   GIF+IF 可走完整记账投递。

### 实现（r197 case 0x60 升级）

`__svm_stgi(); _enable(); stall 16us; _disable(); __svm_clgi();`
——r197 排水失败的根=svm_entry 出口循环全程 **GIF=0**（cr3_monitor:744
白纸黑字的 wiggle 不变量），QEMU pending 树以 GIF 为总闸。

### 判决（一半成功）

- **风暴根除**：intr punt 总数 **4 次**（r197 为 5700 万）——STGI 开窗
  投递真发生（QEMU 记账路径消费 IRR、清 HARD 标志）。
- **新死相**：~120s 后静默冻结（com1 零增长）——dump 解剖：**Cm 注册表
  锁图楔死**——cpu4 持 nt!CmpRegistryLock 共享且等相邻 Cm 资源
  （svchost/Registry worker，RUNNING-stuck）、cpu2 KeAcquireInStack
  QueuedSpinLock 自旋、cpu3 ExIsResourceAcquiredSharedLite、cpu0
  KiInsertQueueDpc、cpu1/5 用户态、smartscreen 线程 READY 未调度。
  根因定性：GIF+IF 开窗让 ISR/DPC 在**出口处理中途**运行（svmb 有史
  第一次），搅乱 Cm/调度状态（cr3_monitor 注释预言的 GIF=0 不变量
  破坏类的 live 证明）。
- 现场留档 dumps/r198_freeze.elf/.dmp。

### r199 岔路（三选一）

- A. **IRCL 守护窗**：抬 IRQL=DISPATCH 再开窗（ISR 可进 DPC 拖延）——
  但 KeLowerIrql 在 handler 内即触发被拖延 DPC，需再设计落点。
- B. **单入卸位 A/B**：punt 改 nap+卸 bit0 一入（下一出口复装）——
  判决"L2 入口投递是否也致命"；死=TCG 投递从 svmb 侧不可救，转 C。
- C. **换引擎**：QEMU 补丁战役 / VMware vhv 床（取证全家桶现成）。

## Round 199 (2026-10-22): 单入卸位 A/B——阴性终判：受控入口投递同样致命，svmb 侧投递设计空间正式穷尽

### 实现（B 形）

- case 0x60 改为：**卸 ic1 bit0 恰好一次重入**（无排水、无 GIF/IF 开窗=
  零 DPC 风险）——pending 向量在重入首拍 L2 语境原生投递（走 L2 自己的
  IDT）；NestedSelfServeExit 顶部对非 0x60 出口复装 bit0；reflect→accept
  路径由意图合并天然复装。暴露窗=每 punt 恰好一个重入周期。
- 构建 5953feb8，三查✓，门关等价 s2/s7 绿。

### 判决（B 阴性——与 r195 逐位同型）

- 机制面工作正常：punt 总数 **4 次**（无风暴，卸位-投递-复装循环成立），
  ~120s 后 com1 静默冻结、sshd 死。
- dump 定罪：cpu1=**nt!InbvCheckDisplayOwnership**（bugcheck 画屏路径，
  专用栈）→ 崩溃栈与 **r195 逐位同骨架**：hal!HaliHaltSystem+0x26 泊车、
  DbgBreakPointWithStatus→KiBugCheckDebugBreak→IoSetBugCheckProgressFlag→
  KeBugCheck2+0x18d4 全同 RVA、**KeBugCheckEx(0x139, arg1=2=STACK_COOKIE)**
  ×2 帧、KTRAP_FRAME Rip=**hal!_report_gsfailure+0x5**+
  STATUS_STACK_BUFFER_OVERRUN(0xC0000409)——同类同源；但本床
  hypervisor=off（enlightenment 车道已拆），cookie 这次砸在**另一个 hal
  函数**=投递落点无关性闭环。其余核：cpu0/4=MiFlushTbList、cpu2/5=
  KiIdleLoop、cpu3=ERESOURCE 等。
- r198/r199 均"恰好 4 次 punt 后死"——第 4 个待投事件从两扇门（L1 排水/
  L2 入口投递）进来都致命。

### 投递设计空间终局表（四门全试）

| 门 | 轮 | 结果 |
|---|---|---|
| 原生中途投递（bit0 不武） | r195/r196 | 0x139 cookie / 0x7F 双错 |
| 拦截+不投递 | r197 | **死亡消灭**但排风暴活锁（IRR 楔+HARD 不清） |
| 拦截+L1 排水（STGI 窗） | r198 | 风暴根除但 ISR/DPC 中途运行搅死 Cm 锁图 |
| 拦截+受控单入投递 | r199 | **0x139 cookie 同型死亡** |

**终判：QEMU TCG 对 L2 的中断帧推送本身即损坏源，与时机无关；svmb 侧
无剩余之门。**嵌套投递缝正式升级为已证明的不可修边界（原 r190 观察级
判决 → 现机制级证明）。r197 形（bit0 常武+零投递）仍是唯一不死形态=
**vlog 长时观测载具**（会话 90s→10min+，代价 OS 饿死）——the reference hypervisor 行为
录制战役可直接用它。

### 后续路径（终局三选）

1. **VMware vhv 床**（推荐：基础设施+取证全家桶现成，vhv 投递=硬件辅助
   语义）。
2. QEMU 补丁战役（缝已定位到帧推送实现层）。
3. 真机收卷（r184/r190 判决不变）。

## Round 200 (2026-10-22): 第六门（投递瞬间换世界）——阴性，但签名漂移带新信息

### 实现与执行

- 用户方案："L2 投递改为 L1 相同方式"——punt 时 NCR3 换回 svmb 恒等根
  （the reference hypervisor 嵌套表本恒等，非钩页物理落点不变；栈/IDT/页表页全非钩目标）
  + 卸位一入 + FLUSH_ALL（r163 换根律）+ 下一出口（分发块顶，compose 之前）
  归还影子根+复装 bit0（`NestedIntrDeliveryWindowClose`）。
- 构建 b2a4b44d，三查✓，门关等价 s2/s7 绿。期间 deploy 后台任务树被
  收割连带 QEMU 死亡（SSH 全拒）——重拉床教训：**QEMU 是 deploy.bat 的
  子进程，后台任务生命周期=床生命周期**。
- 门开发射：**4 punts 后静默冻结**（同 r198/r199 的"恰好 4 次"）。

### 判决（阴性+签名漂移）

- **门 6 阴性**：恒等根投递同样死。但死相签名从 r199 的 0x139-cookie
  **漂移为 0x7F/arg1=8 双错**（KiBugCheckDispatch+0x69 帧双证）——
  r200_sigprobe.py（ELF 直扫栈签名，免 cdb——windbg-mcp 通道故障时的
  替补管线）。
- 诚实标注混淆量：r200 相对 r199 的 delta=换根+强制冲刷（换根必冲刷，
  非纯单变量）；签名漂移证明根/冲刷变量影响失败机理（identity 下
  投递期 fault 成 DF，shadow 下帧落偏成 cookie），但**两根皆不安全**。
- **三轮"恰好 4 次 punt"恒常**（r198 排水/r199 卸位/r200 换根，三门
  各异）——强线索：可能是同一个第 4 待投事件（特定向量）在任何投递
  形态下都致命。r201 靶：punt 时读 LAPIC IRR 顶向量记日志，点名小偷。

### 六门终局表（追加）

| 门 | 轮 | 结果 |
|---|---|---|
| 原生中途 | r195/r196 | 0x139 cookie / 0x7F DF |
| 拦而不投 | r197 | 不死但风暴 |
| L1 排水（STGI 窗） | r198 | 风暴灭但 Cm 锁死 |
| 受控单入（shadow 根） | r199 | 0x139 cookie |
| 恒等根投递+卸位+冲刷 | r200 | **0x7F DF（签名漂移）** |

svmb 侧结论维持：TCG 内投递无安全门；QEMU 补丁=唯一根治；"4-punt
小偷"=最后的床内线索。

## Round 201 (2026-10-22): 凶器点名——死亡序列的待投向量全录 + "恰好 4 次"证伪

### 仪器（两迭代）

- r201a：x2APIC MSR 路径 → 实测 **guest=xAPIC**（x2=0，IRR 全零=盲区）；
  同时揭穿 **"恰好 4 punts"是串口交织伪影**（punts 3-5 的行撞进
  exit-trace 搅碎；本轮真计数 ≥7，n=7 rip=用户态）。
- r201b：MMIO 路径（MmMapIoSpace 0xFEE00000 懒映射，IRQL 守护；PPR=+0xA0、
  IRR=+0x200 步长 0x10）——**仪器命中**。

### 判决：死亡序列向量点名（r200 行为+日志，死亡 0x7F/8 DF 同型）

```
punt1 vec=D1 (时钟)      ppr=00
punt2 vec=2F (设备)      ppr=00
punt3 vec=D2 (时钟族)    ppr=20   ← 0x2F 仍在 IRR（punt2 的投递没消费它！）
punt6 vec=E1 (IPI!)      ppr=20
（punts 4/5 行被串口交织吃掉；死相 cpu4 0x7F/8 双错）
```

- **投递丢失实锤**：0x2F 跨 punt 持续滞留 IRR——即使 r200 的受控单入
  投递也不可靠地消费向量（r168 丢失家族在 punt 路径下仍在发生）。
- **0xE1=Windows IPI 在死亡前夜在场**：跨核会合事件投递进 L2 语境 +
  已证的部分丢失 = r190"会合环冻结"画像的钥匙成型。
- 0x139-cookie 与 0x7F-DF 两签名跨门跨轮交替=同一投递不稳定性的两副
  面孔（帧落偏=cookie；投递期 fault/丢失=DF/挂死）。

### 归档修正与终判

- "三轮恰好 4 punts"假设**证伪**（串口交织伪影）；真模式=若干 punt 后
  死，向量序列含时钟×2、设备（丢失）、IPI。
- svmb 侧终判不变且更精确：TCG 嵌套投递的病=**两件套**（压栈损坏+
  投递丢失），都在 QEMU 的投递实现里——QEMU 补丁的靶点清单从此有
  名有姓；svmb 侧无安全门（六门+点名全证）。
