# svmb 架构

```
L5 modules/   功能模块：demo_cpuid | npt_hook_mgr | cr3_seed | dbg_events
              debugger（DR 阴影/伪造 + MTF 单步）| cr3_monitor（CR3 伪造/布防）
              （仅经公开 API / 独立组件接口编写，零核心改动）
L4 platform/  日志（每核环形缓冲 + DbgPrint/COM1 镜像）| 内核工具
              （分配封装 / RunOnEachCore / HashTable）
L3 mm/ + hw/  NPT 视图与多视图激活（mm/npt）| NPF 决策/handler（mm/npf）|
              TLB 协议（mm/tlb）| 物理内存范围（mm/phys_mem）|
              指令长度解码（mm/insn_len）| MSRPM/IOPM（三页轮转影子）|
              VMCB | asm VMRUN 循环
L2 core/      生命周期（hypervisor）| Exit 事件注册分发 | 拦截位引用计数 |
              hypercall 双通道 | 模块框架
L1 app/       svmbctl 控制台（IOCTL 单一来源：shared/svmb_protocol.h）
```

## 关键机制

### Exit 事件注册表
`Hypervisor::Dispatcher()` 按 exit code 维护优先级处理链；未处理出口走可配置
默认策略（透传恢复 / 注入 #UD / 记日志 / Bugcheck）。`AdvanceRip` 在注入事件时
同样生效——需要 fault 重执行的处理器（如重注入缺页）自行将其置 false。

### 拦截位引用计数
`InterceptManager` 按读/写两个方向独立计数（RefR/RefW），双向归零才回收条目；
`RequireOpcode` 只接受能映射到 VMCB 控制位的 exit code。

### 三页轮转 MSRPM
`Msrpm::BeginBuild()` 清空并指向影子页 → `SetIntercept` 写影子 →
`CommitBuild()` 角色轮转（shadow→active，active→spare，spare→shadow）并返回
新 active PA → `ApplyToVcpu` 同步逐核发布（VMRUN 时才采样）。消除"清零-重建"
窗口期；退役页要经过两个完整提交轮才会被复用清零，背靠背重建不再命中滞后核
（核静默屏障才能彻底关闭，登记于 CODE_REVIEW §十）。

### NPT 视图与激活（mm/npt）
每视图独立 PML4；2M 大页默认映射 RAM，`Split2M` 引用计数拆分为 4K；
`SwapPage4k` 实现隐藏页（原始物理页永不修改）。视图变更入口
（MapRange/Split2M/SetPerm4k/SwapPage4k）持 per-view 自旋锁——IOCTL 侧与
退出路径（NPF 懒映射、hook slide）并发变异安全；读路径无锁（对齐 u64
原子读 + 上层表项 publish-once）。多视图注册表 + `SetActiveView`/`Enable`
激活协议：发布经 `SetApplyCallback` 回调（DriverEntry 接
`TlbApplyViewSwitchAllCores`，写每个 VMCB 的 NCr3 + `TLB_CTL_FLUSH_ALL`，
完成后推 `SvmbDbgEvtViewActivate` ack 事件）；`NptEnable` 注册表开关在驱动
加载时走 `SetActiveView(0)` → `Enable()` → `ArmSlide` 布防已装 hook。

### NPF 决策与 hook 滑动（mm/npf + modules/npt_hook_mgr）
`NpfClassify` 是纯函数决策核（RAM/非RAM 懒映射、DenyWrite/DenyExecute/
DenyUser）。hook 页在每个 present 故障上先经 `NpfSetSlideHook` seam（mm 层不
反向依赖 modules 层）进入滑动状态机：
- DATA 态：原页 P|RW|NX——数据读写畅通，取指被 NX 挡下触发 #NPF；
- EXEC 态：隐藏补丁页 P|X 无 RW——补丁代码执行，写故障翻回 DATA。
退出路径只刷本核 INVLPGA（`TlbInvlpgaLocal`），跨核靠重故障收敛（两个状态都
是有效映射，stale 无害）。EXEC 态下数据读可见补丁字节是 NPT 无读禁用位的
固有权衡。hook 支持同页多实例：footprint 两两不相交的约束下补丁组合进共享
隐藏副本，按 hook 独立撤销；页级资源（发布/引用/隐藏页）恰一次回收。被移除
的 hook 节点进有界 graveyard（MAX_GRAVEYARD=32）而非立即释放——退出路径
stale 读取永远落在存活内存上（UAF 关闭），只余良性 stale 状态。

### CR3 监控与进程种子表（modules/cr3_monitor + cr3_seed）
`Cr3Seed`（PsSetCreateProcessNotifyRoutineEx）存 pid→CR3+映像 basename；
`cr3 watch <exe> [xorkey]`：命中已运行进程立即布防，后续同名进程经种子创建
通知自动布防（推 `SvmbDbgEvtProcessWatch` 事件）。CR3 读伪造策略在纯函数
`Cr3ResolveRead`：全局伪造（ReadSpoof+XorKey）与目标进程伪造（TargetCr3 命中）。
CR3 写路径全程模拟进 VMCB save area（上下文切换热路径），并按
`EnableProcessView` 在进出目标进程时切换 NPT 视图（M3×M4：进程视图是全 RAM
画布，M4 策略可在其中藏页——全局发布 vs 每核保真的前置条件见 CODE_REVIEW §十）。

### 端口拦截（hw/msrpm Iopm）
IOPM 按 (端口, 方向) 引用计数：`RequirePort` 0→1 置位、`ReleasePort` 1→0
清位，过度释放 validate-first 拒绝（先验证后变更，无回滚路径）；双向归零
回收跟踪节点。供后续模块 API 暴露端口拦截使用。

### 调试器原语（modules/debugger）
- 异常面：#BP/#DB/#UD 拦截 → 事件推环 → 透明重注入（#BP 跳过，fault 重执行）；
- DR 面：`HideDr` 把 DR0-3/6/7 读写重定向到每核阴影（guest 既看不到也改不掉
  真实调试寄存器），`SpoofDr7Zero` 让 DR7 读 0（硬件断点隐身）；读取解析在纯
  函数 `DrResolveRead`；
- 单步：`DebuggerArmSingleStep` 经拦截管理器要求 MTF，武装后的 #DB 类退出被
  消费（不下发 guest），作为事件上报。

### 两阶段进出虚拟化
`_svmb_save_or_load_regs` 保存线程上下文并植入 guest 恢复点；VMRUN 后该线程
以 guest 身份继续。devirtualization 由 `HC_EXIT_VMM` 通过 GuestRegs.Extra1/2
通知汇编循环。

## 离线自测
`svmbctl selftest`（IOCTL_SELFTEST）在驱动加载、hypervisor 未启动的状态下
验证：物理内存枚举、NPT 映射/拆分/换页/权限/多视图激活协议、NPF 分类（含
U/S）、hook 滑动状态机、同页多 hook 生命周期、指令解码样本、GuestGpr 索引、
DR/CR3 伪造解析、映像名匹配、MSRPM 三页轮转、调试事件环 FIFO/丢失计数。

## 待办（里程碑）
- ~~前置：master 线 `start` 后 vCPU shutdown 的根因定位~~ ✅ r27/r28（enter
  上下文依赖 + 原地 devirt；AutoStart 默认开启绕开 IOCTL enter）
- ~~M3 联调：`NptEnable=1` 下 NPT 上线、NPF 实路径、TLB 协议验证~~ ✅ r32
  （NPF 滑动实路径全计数断言 PASS；view 切换发布 + VMCB NCr3 读回 PASS；
  5min 空闲浸泡 + storm 后存活。关键修复：滑动页 NPT 权限必须 U/S=1——
  VMware vhv 下 U/S=0 叶子无条件 #NPF，见 CRASH_DEBUG_LOG r31/r32）
- M4 联调：mod attach debugger/cr3_monitor → 事件到 R3、CR3 伪造实测
- Live-NPT 前置清单（CODE_REVIEW §十）：hook 节点寿命竞态、Remove 后跨核
  正翻译、退出路径懒映射的锁契约、核静默屏障
- 钩子页铁律：目标页绝不能含 VMM exit 路径自身执行的代码（日志/分发器）；
  滑动页权限 U/S 必须为 1（r31/r32）
