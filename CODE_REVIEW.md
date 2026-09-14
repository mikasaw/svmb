# svmb 代码审查报告

- **审查日期**：2026-09-04
- **审查对象**：`<repo>` @ master（c83941e）
- **审查范围**：driver 全部源码（core/hw/platform/main/modules，约 4300 行）、shared 协议头、app 控制台、构建脚本、工程文件
- **对照参考**：`<host>\Downloads\参考实现\参考实现`（项目自述的技术来源）、SimpleSVM（NRIP 用法佐证）、AMD APM Vol.2 第 15 章
- **审查方法**：通读全部源码；汇编 ↔ C++ 契约逐行核对（offsets.inc / regs.h / vcpu.h / svm_entry.asm）；与 参考实现 逐函数对比进出虚拟化、GIF、事件注入路径；并发与生命周期建模

---

## 一、总体评价

项目整体质量明显高于其参考实现：分层清晰（hw/core/platform/modules）、偏移用 `static_assert` + `offsets.inc` 双向锁死、VMCB save-area 全部偏移与 APM 核对一致、EFER/VM_CR 伪造不再动辄 `KeBugCheck`、退出分发和拦截管理做成引用计数 + 优先级链、模块经函数表解耦。M0–M2 里程碑（生命周期 / 扩展内核 / demo 模块）按计划落地。

但审查发现 **2 个确定的正确性缺陷（P0）**、**3 个高优先级缺陷（P1）**，以及一组并发/生命周期与健壮性问题。P0 之一（CPUID 隐藏失效）在当前测试路径下不可见（demo 模块提前短路），属于"测试全绿但功能实际是死代码"的典型；之二（注入事件时不推进 RIP）只在反嵌套/默认策略路径触发，触发即挂死或蓝屏循环。另有若干 API 契约陷阱会在 M3/M4（NPT、CR3、调试器）开发时集中爆发，建议在进入 M3 前先修复 P0/P1。

| 级别 | 数量 | 含义 |
|---|---|---|
| P0 | 2 | 确定的功能失效/挂死缺陷，现在就该修 |
| P1 | 3 | 高概率导致状态损坏、崩溃或 API 误用 |
| P2 | 8 | 并发窗口、健壮性与隐蔽性缺口 |
| P3 | 7 | 文档/一致性/边角问题 |

---

## 二、P0 — 确定性正确性缺陷

### P0-1 CPUID 的 SVM/NPT 位隐藏完全失效（死代码）

**位置**：`driver/src/core/hypercall.cpp:189-194`（`HandleCpuidExit`）

```cpp
// normal CPUID: passthrough, but hide the SVM capability bit
EmulateCpuid(ctx);                                   // <-- 先把 Rax 覆盖成结果 EAX
if ((u32)ctx.Regs->Rax == CPUID_EXT_FEATURES)       // <-- 再拿“结果 EAX”和“leaf 常量”比
    ctx.Regs->Rcx &= ~(1ull << SVM_CPUID_BIT);
if ((u32)ctx.Regs->Rax == CPUID_SVM_FEATURES)
    ctx.Regs->Rdx &= ~(1ull << NPT_CPUID_BIT);
```

**问题**：`EmulateCpuid` 内部执行 `ctx.Regs->Rax = (u32)out[0]`，把 Rax 从“请求的 leaf”覆盖成“CPUID 结果的 EAX"。此后再与 `0x80000001` / `0x8000000A` 比较：
- `CPUID(0x80000001).EAX` 返回的是 CPU family/model 特征位（如 `0x00b20ff2`），永远不等于 `0x80000001`；
- `CPUID(0x8000000A).EAX` 返回 SVM revision（1/2），永远不等于 `0x8000000A`。

两个条件恒为假，**隐藏逻辑从未执行过**。参考实现（SVM.cpp:648-651）是先判输入 leaf 再 `__cpuidex`，顺序正确。

**影响**：
1. 框架的核心自隐藏承诺失效——guest（包括任何反虚拟化探测）执行 `CPUID(0x80000001)` 能看到 `ECX[2]=SVM`。`tests/VM_SETUP.md` 第 2 节明确承诺“只用 CPUID 透传 + 隐藏 SVM 位”，实际未兑现。
2. guest 内核/工具会认为自身支持 SVM 并可能尝试 `VMRUN`——随后落入 P0-2 的 #BP 死循环。两个缺陷存在连锁放大关系。

**为什么测试没发现**：`svmbctl demo` 查询的是 `0x40000100`，由 demo 模块处理并提前 `return true`，不经过这段 fallthrough；`svmbctl info`/`storm` 也不检查 0x80000001 的输出。

**修复建议**（在 EmulateCpuid 前捕获输入 leaf）：

```cpp
u32 leaf = (u32)ctx.Regs->Rax;      // 输入 leaf，先存下来
u32 subleaf = (u32)ctx.Regs->Rcx;
EmulateCpuid(ctx);
if (leaf == CPUID_EXT_FEATURES)
    ctx.Regs->Rcx &= ~(1ull << SVM_CPUID_BIT);
if (leaf == CPUID_SVM_FEATURES)
    ctx.Regs->Rdx &= ~(1ull << NPT_CPUID_BIT);
```

同时在 `tests/` 增加一条断言：start 后 `CPUID(0x80000001).ECX[2] == 0`、stop 后恢复为 1，防止回归。

---

### P0-2 注入事件时忽略 AdvanceRip → guest 卡死在原指令（#BP/#UD 无限循环）

**位置**：`driver/src/core/exit_dispatcher.cpp:127-136`（`Dispatch` 收尾）与 `hypercall.cpp:260-268`（`HandleVmrunExit`）

```cpp
// Dispatch:
if (ctx.EventInjected)
{
    // event delivery consumes the "advance"; rip stays where the event
    // semantics require (handler set Save.Rip explicitly if needed)   <-- 契约靠约定
}
else if (ctx.AdvanceRip)
    AdvanceGuestRip(ctx);

// HandleVmrunExit:
InjectException(ctx, EXC_BP);   // EventInjected = true
ctx.AdvanceRip = true;          // <-- 被 Dispatch 静默忽略
return true;
```

**问题**：`Dispatch` 的设计是"注入事件则不自动推进 RIP，由 handler 自己设置"。但 `HandleVmrunExit` 只设了 `AdvanceRip = true`（被忽略），没有设置 `Save.Rip`。结果 guest RIP 停留在**未执行的 VMRUN 指令**上，#BP 注入后 guest 异常处理返回时再次回到 VMRUN 指令 → 再次拦截 → 再注 #BP → **无限循环 / 蓝屏循环**。

参考实现 在同类路径（VMRUN 拦截、SVM.cpp:755-763）是**注入的同时显式 `rip = nRip`**。

**同类受影响路径**：
- `DefaultPolicy::InjectUd`（exit_dispatcher.cpp:148-158）：对非异常类 exit（如模块拦截了 RDPMC/IOIO 却无人处理）注入 #UD 也不推进 → 同样死循环。
- 例外：`AdvanceOrReinject` 对异常类 exit 的 `ReinfectExitEvent` + 不推进是**正确**的（异常是 fault，应重执行），不要一并改动。

**修复建议**（二选一，推荐前者，契约更显式）：

```cpp
// 方案 A：Dispatch 里注入事件也尊重 AdvanceRip
if (ctx.AdvanceRip)          // 注入与推进解耦：fault 类 handler 自会将 AdvanceRip 置 false
    AdvanceGuestRip(ctx);

// 方案 B：保持 Dispatch 现状，修复各注入点
// HandleVmrunExit:
InjectException(ctx, EXC_BP);
ctx.Regs->Rip = ctx.NRip;                 // 显式推进，同时写入 VMCB 由 SvmbVmExitEntry 回写
ctx.Vmcb()->Save.Rip = ctx.NRip;
ctx.AdvanceRip = false;                   // 防御：表明已处理
```

采用方案 A 时需同步核对 `ReinfectExitEvent` 路径（其 `AdvanceRip=false` 已正确）与 `ApplyTfFixup`（在 Dispatch 之后执行，#DB 语义为 trap，RIP 已推进到 NRip，正确）。

**关联问题**：`InjectException` 统一使用 `EVT_EXCEPTION(2)`。对 `int3` 来源的 #BP，参考实现使用 type 3（software interrupt，异常帧 RIP 语义不同）。修 P0-2 时一并确认：VMRUN 反嵌套注入建议 `InjectEvent(ctx, EXC_BP, EVT_SOFTINT, false, 0)`。

---

## 三、P1 — 高优先级缺陷

### P1-1 `_svmb_hypercall` 汇编破坏 Win64 ABI（clobber RBX）

**位置**：`driver/src/hw/asm/svm_entry.asm:226-234`

```asm
_svmb_hypercall Proc
    mov r10, r9
    mov rax, rcx
    mov rbx, rdx      ; <-- RBX 是 Win64 非易失寄存器，被静默改写
    mov rcx, r8
    vmmcall
    mov [r10], rax
    ret
```

**问题**：调用方是普通内核 C++ 代码（`Hypervisor::Stop` 的 lambda，运行于 guest 态）。编译器按 ABI 假设 RBX 跨调用保持。虽然 VM exit 路径会备份/恢复 guest GPR，但恢复的是 **vmmcall 时刻**的寄存器——即 `mov rbx, rdx` 已经执行之后的状态。于是调用返回后 RBX 变成了 a1（HC_EXIT_VMM 时为 0）。当前调用点恰好没在 RBX 里放活值，纯属侥幸；编译器版本/优化等级一变就是不可复现的状态损坏。

**附带协议缺陷**：vmmcall 时 `rdx` 未被设置，handler 收到的 `a3` 实际是调用方残留在 rdx 里的 **a1**（见 `HandleVmmcallExit` → `Invoke(nr, Rbx, Rcx, Rdx)`）。CPUID 通道则恒传 `a3=0`。两个通道对 a3 的语义不一致，`hypercall.h` 头注释（"args rbx/rcx/rdx/rsi/rdi"）也与实现不符——这是给未来模块 hypercall 埋的陷阱。

**修复建议**：

```asm
_svmb_hypercall Proc
    push rbx                ; 保持 ABI
    mov r10, r9
    mov rax, rcx
    mov rbx, rdx            ; a1
    mov rcx, r8             ; a2
    xor rdx, rdx            ; a3 显式清零（或增加独立 a3 参数）
    vmmcall
    mov [r10], rax
    pop rbx
    ret
```

同步修正 `hypercall.h` 注释，明确两通道的参数表；或给 `_svmb_hypercall` 增加第 5 参数映射到 rdx。

### P1-2 InterceptManager 引用计数不对称：拦截静默失效 + 条目泄漏

**位置**：`driver/src/core/intercept_manager.cpp:80-99`（`Release`）与 `36-78`（`AddRef`）

```cpp
// AddRef: 命中同 key 条目时 Access |= access; ++RefCount;
// Release:
if (--e->RefCount == 0)
    e->Used = false;
else
    e->Access &= ~access;    // <-- 第一次 Release 就清位，无视还有别的引用在用
```

**问题场景**（同一 owner 对同一资源 Require 两次是合法用法，如两个子功能都要拦 CR3 读）：

1. `RequireCr(cr, r)` ×2 → Entry{Access=1, RefCount=2}
2. 第一次 `ReleaseCr(cr, r)` → RefCount=1 ≠ 0 → `Access &= ~1` → **Access=0，拦截位被撤下**，但模块仍认为有一个引用在生效；
3. 第二次 `ReleaseCr(cr, r)` → `e->Access & access == 0` → 返回 `STATUS_NOT_FOUND`，条目永远 `Used=true`（**槽位泄漏**，MAX_ENTRIES=128 会被耗尽）。

另有一处过度释放被静默接受：只 Require 过 r 的条目，`Release(r|w)` 会因 `Access & 3 != 0` 通过检查并把整个条目释放。

**影响**：模块运行期动态增减拦截（M3/M4 的核心场景：NPT hook 增删、调试器断点注册）会出现“拦截突然不触发”与“资源账目对不上”。`ReleaseOwner` 兜底可以清理泄漏条目，但掩盖不了第 2 步的静默失效。

**修复建议**：为读/写两个方向分别计数（或禁止合并不同 access 的请求）：

```cpp
struct Entry { ... u32 RefR; u32 RefW; };   // 替代 Access + RefCount
// AddRef:  r ? ++RefR : 0; w ? ++RefW : 0;
// Release: 先校验对应计数 > 0，再 --；两个计数都为 0 时 Used = false
// Fold:    取 (RefR > 0, RefW > 0) 作为置位依据
```

### P1-3 RequireOpcode 接受无法映射的 exit code，且 IOIO 可导致 VMRUN 一致性检查失败

**位置**：`driver/src/core/intercept_manager.cpp:125-132`（入参校验）、`188-205`（`Fold` 的 RK_OPCODE 映射）；`intercept_manager.cpp:221-274`（`ApplyToVcpu` 从不设置 `IopmBasePa`）

两个独立但同源的问题：

**(a) 静默吞掉不可映射的 exit code**。`RequireOpcode` 只校验 `exitReason <= 0x403`，但 `Fold` 的映射表覆盖不到：
- `0x28-0x2F`、`0x38-0x3F`（DR 空档，本就非法）；
- **`0x90-0x9F`（CR0-15 写陷阱类，映射到 ic2[16+n]）**——M4 的 CR3 监控正要用它；
- **`0x400-0x403`（NPF/AVIC/VMGEXIT）**——由 NPT 开关控制而非 opcode 位图，M3 的 NPF 拦截入口。

这些值被接受、返回 `STATUS_SUCCESS`，模块以为拦截已生效，实际 `Fold` 直接跳过。**API 契约性缺陷，M3/M4 一上就会踩**。

**(b) IOIO 位可置 1 但 IOPM 基址恒为 0**。`RequireOpcode(vmexit::IOIO /*0x7B*/)` 会置 ic1 bit 27；`ConfigureVmcb`/`ApplyToVcpu` 均不设置 `IopmBasePa`，`Iopm` 类从未实例化。VMRUN 一致性检查要求 IOIO 拦截启用时 IOPM 物理地址有效 → **下一次 vmrun 以 #VMEXIT_INVALID 失败，该核退出虚拟化，系统层面表现为随机崩溃/启动失败**。

**修复建议**：
1. `RequireOpcode` 改白名单校验：显式拒绝当前不支持的段（返回 `STATUS_NOT_IMPLEMENTED` + 日志），而不是 `<= 0x403` 一律放行；
2. 补全 `0x90+n → ic2 bit 16+n` 映射（`Requirements::Opcode2` 需扩到 32 位）；
3. IOIO：要么在框架里实例化并挂接 IOPM（`ApplyToVcpu` 设置 `IopmBasePa`），要么 `RequireOpcode(0x7B)` 直接返回不支持——绝不能让该位在无 IOPM 的情况下置 1。

---

## 四、P2 — 并发 / 生命周期 / 健壮性

### P2-1 IOCTL 面完全无互斥，gInstance 单例无同步

**位置**：`driver/src/main.cpp:27-216`（`DevCtrl`）、`core/module.cpp`（`ModuleManager`）

- 两个并发 IOCTL（如 `stop` 与 `mod attach`）会在 `Hypervisor::Stop()` → `delete hv` 与 `Instance()->Dispatcher().Register(...)` 之间产生 use-after-free；
- `ModuleManager` 声明并初始化了 `Lock_`，但 `Attach`/`Detach`/`DetachAll` **全程不加锁**——并发 attach 同名模块可双双通过“空闲槽位”检查写入同一槽；
- `DevUnload` 也没有阻止“句柄未关、IOCTL 在途”的卸载。

测试工具单用户场景下概率低，但 `svmbctl` 的 demo/storm 命令都隐含多步 IOCTL 序列，一旦叠加重试脚本就会触发。

**建议**：设备扩展里放一个 `FAST_MUTEX`/`ERESOURCE`，`HV_CONTROL(start/stop)` 与 `MODULE_CTL` 全程持有（它们都在 PASSIVE_LEVEL）；`ModuleManager` 补上已有的自旋锁；`DevUnload` 先 `IoDetachDevice` 式排空（WDM 下可简单用互斥 + 引用计数）。

### P2-2 Stop() 逐核去虚拟化遇错即断，随后无条件释放 VcpuContext

**位置**：`core/hypervisor.cpp:285-332`

```cpp
RunOnEachCore([this](u32 idx) -> NTSTATUS {
    ...
    return NT_SUCCESS((NTSTATUS)out) || out == 0 ? STATUS_SUCCESS : (NTSTATUS)out;
});                      // RunOnEachCore 内部 !NT_SUCCESS 即 break
...
// 之后无条件释放所有 Vcpus_
```

若某核的 `HC_EXIT_VMM` 返回非成功（例如 CAS 竞态返回 `STATUS_UNSUCCESSFUL`），`RunOnEachCore` 中止，**其后所有核仍处于 Guest 态**，`Stop()` 却继续释放 `Vcpus_`/MSRPM/分发器 → 仍在 VMRUN 循环里的核踩已释放内存，通常表现为关机/重启时蓝屏。

**建议**：devirt 阶段改为“收集结果、不提前 break”；释放前逐核断言 `State == Off`，凡有核未退出则拒绝释放并返回错误（保留资源总比 UAF 好）；对 CAS 失败（Leaving 态）做重试。

### P2-3 Start() 部分失败路径不做清理，main.cpp 也不回收对象

**位置**：`core/hypervisor.cpp:114-136`、`driver/src/main.cpp:85-90`

- `Vcpus_` 分配循环中途失败直接 `return STATUS_INSUFFICIENT_RESOURCES`：`gInstance` 已指向 this、部分 vcpu 已分配、子系统已 Init，无任何回滚；
- `main.cpp` 对 `h->Start()` 失败的分支既不 `Stop()` 也不 `delete h`，对象与半初始化状态一直驻留（直到用户手动再发一次 stop 才能回收）；
- `EnterCore` 失败路径虽然内部调了 `Stop()`，对象本身仍由 main.cpp 泄漏。

**建议**：Start 内部统一失败出口（goto/RAII 调 Stop）；main.cpp 失败分支补 `h->Stop(); delete h;`。

### P2-4 ApplyAll 重建 MSRPM 存在“清零窗口”

**位置**：`core/intercept_manager.cpp:276-291`、`hw/msrpm.cpp`

`ApplyAll` 先 `Msrpm_->ClearAll()`（RtlZeroMemory 整页），再 `Fold` 重设位。MSRPM 是**全局共享**的，清零与置位之间，其他核的 guest 对 EFER/VM_CR 的访问不产生 exit，直接穿透到真实 MSR——理论上 guest 可在此窗口清掉 EFER.SVME 导致后续 vmrun 失败。窗口极小、Windows 常规路径不写这些 MSR，但这是框架级承诺（"protect EFER/VM_CR always-on"）的破绽。

**建议**：影子构建 + 切换（预置新 MSRPM 页，原子换 `MsrpmBasePa` + CleanBits 失效），或按差量以 InterlockedOr8/And8 逐位收敛（先设新增位、后清移除位，保证“需要拦截的位始终在”）。

### P2-5 cycle 压力测试会静默卸载所有模块

**位置**：`main.cpp:102-118`（Action=2）→ `Stop()` → `Modules_.Deinit()` → `DetachAll()`

`svmbctl cycle 100` 之后所有已 attach 的模块（如 demo）全部消失，且不会自动重新挂载。作为“纯进出压力”命令与用户预期不符，也让 `cycle + demo` 的组合测试结果失真。

**建议**：要么在 Stop/Start 循环外保存并重挂模块列表，要么在帮助文本与文档里明确标注副作用。

### P2-6 VM_HSAVE_PA（0xC0010117）未拦截：检测与篡改向量

guest 可直接 `RDMSR 0xC0010117` 读到非零物理地址——这是最经典的 SVM 存在性检测手段之一；也能 `WRMSR` 破坏 host 保存区。参考实现 同样没拦，但既然 svmb 的定位是修复参考实现的缺陷（计划文档原话），建议把 HSAVE 加入 CORE_TOKEN 常驻拦截：读返回 0，写丢弃/强制原值。注意 HSAVE 属于 `0xC0010000` 段，MSRPM 映射已支持。

### P2-7 CheckCpu 未校验 NRIPS 能力位

**位置**：`core/hypervisor.cpp:37-61`

整个框架的 RIP 前进机制建立在 `VMCB.Ctrl.NRip(0xC8)` 由硬件自动填充之上（与 参考实现/SimpleSVM 的用法一致——已对照 SimpleSVM 源码确认其同样直接使用 NRip 且不设任何 NRip-save 控制位；`svm_defs.h` 里也已定义 `NRIPS_CPUID_BIT`）。但 `CheckCpu` 从不检查 `CPUID(0x8000000A).EDX[3]`。在不支持 NRIPS 的旧 AMD 型号（或某些嵌套实现）上，NRip 恒为 0，每次 exit 后 guest 跳到 0 地址——症状是“一进虚拟化就崩”，且报错信息毫无指向性。

**建议**：CheckCpu 增加 NRIPS 检查，不支持时明确返回错误码并打日志（比 NoNpt 更合理的失败方式）；或实现 NRip 缺失时的指令长度回退（M3 做 hook 时反正需要解码器）。

### P2-8 VMMCALL 通道对 ring3 完全开放

`HandleVmmcallExit` 不检查 CPL。任何用户态进程执行 `vmmcall rax=nr` 都能触达已注册的 hypercall（HC_PROBE/HC_VERSION 无害，但模块注册的 nr 可能不无害）。参考实现对其 exit-SVM 命令做了内核态校验。建议：默认拒绝 `Save.Cpl != 0` 的调用（HC_PROBE 可放行），由注册 API 提供显式的“允许用户态”标志。

### P2-9 高 IRQL 下的 MSR 拦截风险（设计约束，需文档化）

exit 可发生在任意 guest IRQL（例如时钟 ISR 内的 RDMSR）。`Dispatch`/`Invoke` 拿自旋锁、`LogWrite` 调 `DbgPrintEx`，这两者在 IRQL > DISPATCH_LEVEL 都是非法的。当前默认只拦 EFER/VM_CR（Windows 在高 IRQL 几乎不碰），风险低；但模块一旦拦热 MSR（GS_BASE、TSC 相关等）就可能在中断上下文频繁触发 exit。建议：至少在 `module_api.h` 注明“拦截高 IRQL 路径会触达的 MSR 需要无锁 handler”；Dispatch 可以为高 IRQL 场景提供 bypass 模式（只查位图不拿锁的单读路径）。

---

## 五、P3 — 一致性与边角问题

| # | 位置 | 问题 | 建议 |
|---|---|---|---|
| P3-1 | `hypercall.cpp:137-151` | `HcStress` 注释称“guest-side: this exits”，实际它在 VMM 上下文直接 `__cpuidex`，**不产生 VM exit**，整个循环无意义（真正的风暴在 IOCTL_STRESS 的 guest 侧路径）。注册号 `0x000F` 是魔数未入 `svm_defs.h` | 删除或改为在 guest 上下文发起；魔数入 defs |
| P3-2 | `hypercall.cpp:270-281` | `HandleMtfExit` 从未注册（死代码），且“注入 #DB + 不推进”与注释表述矛盾；MTF 由 `DbgCtl` 触发时 exit code 是 0x40+1（异常类） | M4 实现单步时重写；现在先删或标注 |
| P3-3 | `platform/logger.cpp:130-143` | 环形缓冲**先提交计数后判满**：丢弃消息时 `Write` 已前进，Drain 会把陈旧字节当新消息（首圈为零字节时被 `len>1` 过滤，重写后输出乱码）。另 `e.Level` 恒为 Info（级别丢失）；SerialWrite 对每条消息固定追加 `\r\n` | 判满放在 InterlockedAdd64 之前；保存 level；去掉硬编码换行 |
| P3-4 | `hw/vmcb.h:113` | `Reserved6[0x2C0]` 少 4 字节（0x11C→0x3E0 应为 0x2C4），`HostDefined` 实际落在 0x3DC 而非注释的 0x3E0。因 pack(1)+自然对齐，`sizeof==0x1000` 断言碰巧仍通过 | 改为 `[0x2C4]` 并补 `static_assert(offsetof(Ctrl,HostDefined)==0x3E0)` |
| P3-5 | `main.cpp:243-272` | `DriverEntry` 中 `IoCreateSymbolicLink` 失败路径不调用 `LogDeinit()`（DriverEntry 失败不触发 Unload，日志池泄漏）；设备无 ACL，任何进程可 open 并 start/stop hypervisor | 失败路径补 LogDeinit；测试驱动也建议 SDDL 限制 Administrators |
| P3-6 | `tests/VM_SETUP.md` | 第 1 节串口管道名 `\\.\pipe\com_1` 与第 5/6 节 `\\.\pipe\svmb-serial` 不一致 | 统一命名 |
| P3-7 | `core/hypercall.cpp:74`、`exit_dispatcher.h:84` 等 | `Invoke` 函数体首行格式（`{    KIRQL old;`）；“buckets”注释与实现描述（每 exit code 一个桶，实际是 hash 桶）小偏差 | 顺手清理 |

另有两处**确认不是问题**、但值得留档的核对结论（避免后续误改）：

1. **NRIP 无需 NRipSave 控制位**：与 参考实现、SimpleSVM 逐行对照确认，三者的 VMRUN 循环都直接消费 `Ctrl.NRip` 且不设置任何额外控制位（SimpleSvm.cpp 中 `Rip = ControlArea.NRip` 同款用法）。svmb 该处实现正确，无需改动——但见 P2-7 的能力位检查建议。
2. **GIF 语义**：`#VMEXIT 时 GIF=0`（参考实现 原文注释“进入 Host 模式的时候 GIF 是关闭状态”），因此 exit handler 全程免中断；`HcExitVMM` 的 `_disable() → STGI → vmload → 清 SVME` 序列移植正确，IF=0 保证从 handler 返回到 `exit_virtualization` 的 `popfq` 之间不会接收中断。`_svmb_vmm_loop` 的 vmload(guest)→vmrun→vmsave(guest)→vmload(host)→处理→vmsave(host) 循环、`_save_or_load_regs` 两阶段进出、`exit_virtualization` 的 rsp/rip/rflags 恢复序列均与参考实现一一等价且正确。

---

## 六、已核对无误的关键面（摘要）

- **VMCB 布局**：save area 全部使用到的偏移（EFER 0x4D0 / CR3 0x550 / RIP 0x578 / RSP 0x5D8 / RAX 0x5F8 / RFLAGS 0x570 / NRip 0xC8 / EventInj 0xA8 等）与 APM Vol.2 一致，且被 static_assert 锁定；`VcpuContext` 分页布局与 offsets.inc 一致。
- **进出虚拟化核心路径**：两阶段寄存器保存/恢复、per-core HSAVE 先写后开 SVME 的顺序、host=guest 快照、逐核亲和性进入、失败回滚——正确。
- **TF 补注入**（ApplyTfFixup）：与参考实现一致，EventInjected 时正确跳过，RIP 已由 Dispatch 推进，trap 语义成立。
- **EFER/VM_CR 伪造**：读隐藏 SVME/伪造 SVMDIS、写强制置位，比 参考实现 的 KeBugCheck 处理稳健。
- **MSRPM 位图**：三段映射（0x0000/0x0800/0x1000）、2bit/MSR、8KB 连续内存，与 参考实现 的 RTL_BITMAP 算法等价；Iopm 类本身实现正确（只是未接线，见 P1-3b）。
- **模块框架**：token 化资源追踪 + Detach 时 `UnregisterOwner/ReleaseOwner/UnregisterOwner` 三重兜底，设计良好（缺锁问题见 P2-1）。
- **协议/工具**：shared/svmb_protocol.h 单一来源、kernel 与 R3 两侧类型宽度一致（unsigned long = uint32_t on Win x64）、METHOD_BUFFERED 用法正确；sign.cmd 与解决方案输出目录（根 x64\Release）匹配。

---

## 七、修复优先级路线建议

1. **立即（进入 M3 前）**：P0-1（CPUID 隐藏）、P0-2（注入+RIP 推进）、P1-1（hypercall ABI）、P1-2（引用计数）——这四项直接影响 M3/M4 的 API 可信度。
2. **短期**：P1-3（RequireOpcode 白名单 + IOPM）、P2-1/P2-2/P2-3（并发与生命周期），并补上针对 P0-1 的回归测试（`cpuid 0x80000001` 隐藏位断言）。
3. **M3 设计时一并解决**：P2-4（MSRPM 影子切换，NPT 视图切换同样需要原子发布语义）、P1-3a 的 CR-write-trap 映射、P2-7（NRIPS 检查）。
4. **随手项**：P3 全部 + P2-5/P2-6/P2-8。

---

## 八、修复记录（2026-09-04，审查后同日落实）

以下缺陷已在本仓库工作区修复并通过 Release/Debug 双配置 MSBuild 编译验证（零错误、零警告），改动共 17 个文件：

| 编号 | 修复内容 | 落点 |
|---|---|---|
| P0-1 | CPUID 先捕获输入 leaf 再 Emulate，SVM/NPT 位隐藏真正生效 | `hypercall.cpp` |
| P0-2 | Dispatch 无条件尊重 AdvanceRip；VMRUN 反嵌套改为 int3 软中断语义 + 推进 RIP | `exit_dispatcher.cpp`、`hypercall.cpp` |
| P1-1 | `_svmb_hypercall` 保存/恢复 RBX、a3 显式清零；协议注释同步修正 | `svm_entry.asm`、`hypercall.h` |
| P1-2 | Entry 改为 RefR/RefW 双向计数，按方向校验与递减，双向归零才回收条目 | `intercept_manager.{h,cpp}` |
| P1-3 | RequireOpcode 白名单校验（拒绝 NPF/AVIC/VMGEXIT/IOIO/空洞段，返回 STATUS_NOT_IMPLEMENTED）；补 0x90+n → ic2 bit16+n 映射；Opcode2 折叠循环扩到 32 位 | `intercept_manager.{h,cpp}` |
| P2-1 | IOCTL 层 FAST_MUTEX 串行化所有控制路径 + DriverUnload；ModuleManager 契约注释 | `main.cpp`、`module.h` |
| P2-2 | Stop() 逐核去虚拟化不提前中断；新增硬门禁——有核未退出则拒绝释放并保留现场可重试；devirt 成功后状态置 Off | `hypervisor.cpp` |
| P2-3 | Start() 统一失败出口（内部回滚）；IOCTL start 失败即 delete；stop 失败不再盲目 delete（防 UAF） | `hypervisor.cpp`、`main.cpp` |
| P2-5 | cycle 循环前快照已挂载模块，循环成功结束后自动重挂 | `main.cpp` |
| P2-6 | VM_HSAVE_PA 常驻拦截：读返回 0（隐藏存在性）、写丢弃 | `hypervisor.cpp`、`hypercall.cpp` |
| P2-7 | CheckCpu 增加 NRIPS（0x8000000A.EDX[3]）检查，新增 SvmCheckResult::NoNrips=6 | `hypervisor.{h,cpp}` |
| P2-8 | VMMCALL 通道 ring3 仅放行 HC_PROBE；CPUID 通道 ring3 仅放行 HC_PROBE/HC_VERSION，其余返回 ACCESS_DENIED | `hypercall.cpp` |
| P2-9 | module_api.h 增加"高 IRQL exit 处理器必须无锁"契约说明 | `module_api.h` |
| P3-1/P3-2 | 删除 HcStress（0x000F 魔数注册）与 HandleMtfExit 死代码 | `hypercall.{h,cpp}`、`hypervisor.cpp` |
| P3-3 | 日志环形缓冲：判满改为不推进 Write（杜绝幽灵推进）；消息带级别字节（Level 不再恒为 Info）；Drain 遇未写完消息等待下轮；LogDeinit 先置空后释放 | `logger.cpp` |
| P3-4 | VMCB 控制区补 pad 字节使 BusThresholdCounter 落在 APM 规定的 0x11A、HostDefined 真实落在 0x3E0，并补 static_assert（原结构 pack(1) 下整体偏移差 1 字节、尾差 5 字节） | `vmcb.h` |
| P3-5 | DriverEntry 两条失败路径补 LogDeinit；改用 IoCreateDeviceSecure + SDDL（仅 SYSTEM/Administrators 可打开设备），vcxproj 链接 wdmsec.lib | `main.cpp`、`svmb.vcxproj` |
| P3-6 | 串口管道名统一为 `\\.\pipe\com_1`，故障表新增 NoNrips 行 | `tests/VM_SETUP.md` |
| P3-7 | Invoke 花括号格式、ExitDispatcher 桶注释与实现对齐 | `hypercall.cpp`、`exit_dispatcher.h` |
| 回归辅助 | `svmbctl info` 新增 SVM 隐藏位检查：运行中 `CPUID(0x80000001).ECX[2]` 应为 hidden，否则打印 LEAK 警告 | `app/main.cpp` |

**遗留（按计划延后）**：
- P2-4（MSRPM 全量清零窗口）→ 按报告路线并入 M3，与 NPT 视图切换一同做影子双缓冲 + 原子发布；
- 高 IRQL 日志路径（P2-9 提到的 DbgPrintEx 在 exit 上下文的理论风险）已文档化，实现层缓解待 M4 调试器模块设计时评估；
- 设备安全描述符使非管理员打开设备失败——`sc start`/`svmbctl` 均需管理员运行（与 VM_SETUP.md 现有流程一致）。

**验证状态**：静态修复 + 双配置编译通过；未在测试 VM 上运行（修复涉及 VMRUN/进出虚拟化路径，务必按 `tests/VM_SETUP.md` 先打快照，再回归 `info → start → info（确认 svm cpuid bit: hidden）→ storm → demo → cycle → stop`）。

---

## 九、修复验收（2026-09-04，修复后复核）

对第八节全部改动做了一次逐文件复核（最终状态通读 + 全路径语义推演 + 双实现交叉），结果如下。

### 验收结论：20/21 项通过；复核新发现 2 处由修复引入的缺陷，已当场修复并重新编译验证

**新发现并已修复（编号续 A 系列）**：

| 编号 | 缺陷 | 根因 | 修复 |
|---|---|---|---|
| A1 | `Stop()` 空指针崩溃路径 | 重构去掉了原 `if (Running_)` 守卫后，`RunOnEachCore` lambda 与释放门禁循环无条件解引用 `Vcpus_[idx]`；而 `Start()` 的 `SVMB_START_FAIL` 会在 `Vcpus_` 数组分配失败时以 `Vcpus_ == nullptr` 进入 `Stop()` → 近空指针读取 → 蓝屏 | devirt 阶段与门禁循环均加 `if (Vcpus_)` 守卫（`hypervisor.cpp`） |
| A2 | `Stop()` 中止后状态机漏洞 | 中止分支设置 `Running_ = false`，但此时仍有核在 SVM；后续 start IOCTL 误判"未运行"→ 在旧实例仍存活时创建新 Hypervisor → 对已虚拟化核重跑进入序列 → VMRUN 拦截 #BP 死循环 | 中止路径保持 `Running_ = true`：start 正确返回 `STATUS_ALREADY_REGISTERED`，stop 可重试（`hypervisor.cpp`） |
| A3 | **TR 基址高 32 位错误 → 第一次 R3→R0 转换即三重故障（vCPU 关闭）**。实机联调时由"死在 vmload 附近"线索定位。`GetSegmentBase` 对 16 字节系统描述符（TSS/LDT）用 `p[7]<<32` 取高基址——byte 7 实为基址 bits 24-31（低 32 位的一部分），真高 32 位在 bytes 8-11。Windows TSS `0xFFFFF880_xxxxxxxx` 被填成 `0x00000088_xxxxxxxx`；VMLOAD/VMRUN 不校验 TR base，guest 内核可跑，但首次用户态中断/syscall 从垃圾 TSS 取 RSP0 → 异常投递栈错 → #PF/#DF → 三重故障 → VMware 报 vCPU 进入关闭状态。为预存在缺陷（首次审查时该项误判为"已核对无误"），与 参考实现（正确读第二个 qword 的 dword）分叉点 | 改为逐字节读取 bytes 8-11 组装 bits 32-63（`segments.cpp`） |

### 逐项验收记录

| 原编号 | 验收要点 | 结论 |
|---|---|---|
| P0-1 | 输入 leaf 在 `EmulateCpuid` 覆盖 Rax 前捕获；两条隐藏分支可达；demo 模块短路路径不受影响；`svmbctl info` 新增 LEAK 检测 | ✅ |
| P0-2 | `Dispatch` 无条件尊重 `AdvanceRip` 后逐路径推演：CPUID/MSR/VMMCALL（推进✓）、VMRUN 反嵌套（int3 + 推进✓）、AdvanceOrReinject 异常类（ReinfectExitEvent 自置 `AdvanceRip=false`，不推进✓）、InjectUd 非异常类（推进+注入✓，死循环消除）、ApplyTfFixup（在推进后注入 #DB，trap 语义✓、EventInjected 时跳过✓） | ✅ |
| P1-1 | `push/pop rbx` 恢复 ABI（入口 rsp%16==8，push 后对齐）；`xor edx,edx` 使 a3 确定为 0；`mov [r10],rax` 的 r10 经 BACKUP/RESTORE_REGISTERS 与 exit_virtualization 恢复链验证无恙 | ✅ |
| P1-2 | AddRef/Release/ReleaseOwner/Fold/ApplyToVcpu 全路径核对：双向计数、按方向校验、双向归零回收；Require* 包装层对非读写类传 access=1 → RefR 通道，语义一致；"Require×2 + Release×1"场景下拦截位保持、无泄漏 | ✅ |
| P1-3 | 白名单覆盖 CR 读写/DR 读写/异常 0x40-0x5F/ic1 0x60-0x7F（排 IOIO）/ic2 0x80-0x8F/CR 写陷阱 0x90-0x9F；拒绝段返回 `STATUS_NOT_IMPLEMENTED`；Fold 新增 0x90+n → ic2 bit16+n；`Opcode2[32]` + op2 循环 32 位 | ✅ |
| P2-1 | DevCtrlLocked 写 IoStatus、wrapper 释锁后完成 IRP（恰好一次）；STRESS 持锁全程为可接受设计；DevUnload 持锁排空在途 IOCTL | ✅ |
| P2-2 | devirt 不提前断 + 硬门禁 + A1/A2 修复后：数组未分配/分配失败/部分进入/CAS 竞态四个场景全部推演安全，失败可重试 | ✅（含 A1/A2 修正） |
| P2-3 | `SVMB_START_FAIL` 统一回滚（含 `#undef`）；start 失败即 delete；stop 失败不 delete | ✅ |
| P2-5 | cycle 快照 ≤16 模块名（15 字节 + NUL），Describe 截断语义正确，重挂失败仅告警 | ✅ |
| P2-6 | HSAVE 拦截位于 VM_CR 与 EFER 之间；读 0/写丢弃；VMM 自身进出虚拟化序列的 wrmsr 在宿主态执行不经拦截，无自锁风险 | ✅ |
| P2-7 | NRIPS 位检查 + `NoNrips=6` + VM_SETUP 故障表/文档同步 | ✅ |
| P2-8 | VMMCALL 通道 ring3 仅 HC_PROBE；CPUID 通道 ring3 仅 HC_PROBE/HC_VERSION；CPL 取自 exit 时刻 Save.Cpl | ✅ |
| P3-3 | 写侧 [level][text][NUL] 与读侧 consumed=len+2 严格对称；判满不推进 Write（幽灵推进消除）；未写完消息留待下轮 drain；`w-r ≤ RING` 不变量保证写者不覆盖未读字节；LogDeinit 先置空后释放（DevUnload 在 Stop 失败后释放日志池时 exit 路径 LogWrite 安全早退） | ✅ |
| P3-4 | pack(1) 下补 0x119 pad：BusThresholdCounter 落 0x11A、HostDefined 落 0x3E0，新增 static_assert 锁定（构建通过证实） | ✅ |
| P3-1/2/5/6/7 | 死代码零残留（grep 验证）；SDDL 设备限制 SYSTEM/Administrators（与测试流程管理员运行一致）；串口名统一；格式清理 | ✅ |

### 残余已知限制（有意接受，非缺陷）

1. **P2-4（MSRPM 清零窗口）**：按既定路线并入 M3 与 NPT 视图切换一并做原子发布（报告第七节）。
2. **Stop() 中止后卸载驱动**：若 Stop 失败且随后 SCM 卸载驱动，驱动映像可能在仍有核处于 SVM 时被卸载——该场景下系统已处于不可恢复状态，当前策略（保内存 + 日志池先置空）只求不加剧破坏。
3. **静态验证边界**：全部为代码级推演与双配置编译；VMRUN/进出虚拟化路径的最终确认必须实机回归（见下节清单）。

### 实机回归清单（快照后执行）

已固化为脚本 `tests/run_regression.cmd`（与 `svmb.sys`/`svmbctl.exe` 同目录、管理员运行；用法与断言见 `tests/VM_SETUP.md` §4.1），等效手动流程：

```bat
svmbctl info      & rem OFF, magic ok
svmbctl start     & rem 系统稳定
svmbctl info      & rem RUNNING + svm cpuid bit: hidden（P0-1 防回归断言）
svmbctl storm 100000
svmbctl demo      & rem attach 前后签名变化
svmbctl cycle 100 & rem 结束后 mod list 仍应包含先前挂载的模块（P2-5）
svmbctl mod list
svmbctl stop      & rem OFF
svmbctl log
```



## 十、Live-NPT 前置条件登记（T2 激活路径验收新增，2026-09-05；T9-T12 批次更新处置）

在启用任何 live hook/NPF 路径的 IOCTL 之前必须解决（与 mm/npt.h 激活协议
注释同清单）：

1. ✅ **hook 节点寿命竞态（已关闭，182a4a5）**：Remove 不再释放节点/隐藏页/跳板——
   进有界 graveyard（MAX_GRAVEYARD=32，超限释放最老节点、仅该节点竞态窗口重开）。
   退出路径 stale 读取永远存活内存；exec 翻转落到已撤补丁的隐藏页（原字节，良性），
   write 翻转与末 hook 移除相交可能遗留无 hook 的 NX 页（取指 DenyExecute 自旋、
   日志封顶，见 npt_hook_mgr.h 并发注释）。
2. ⏳ **Remove 后跨核正翻译（范围收窄，保持开放）**：广播 INVLPGA 的既有实现
   （RunOnEachCore 线程亲和）在核被虚拟化期间会永久阻塞——不可用。现行方案
   保持本核 INVLPGA + 重故障收敛；graveyard 使 stale 翻转落在存活页上。彻底关闭
   需 IPI 拦截感知的跨核机制（tier-3）。
3. ✅ **退出路径 lazy MapRange 无锁并发改视图（已关闭，873e1e7）**：NptView 四个变更
   入口（MapRange/Split2M/SetPerm4k/SwapPage4k）持 per-view 自旋锁；读路径无锁（对齐
   u64 原子读 + 上层表项 publish-once 不可撕裂）。验收抓到并修复 MapRange→Split2M
   递归取锁死锁（Split2MLocked）。
4. ⏳ **MSRPM 滞后核残余窗口（保持开放）**：三页轮转下退役页两轮宽限（retire@K→
   spare@K+1→shadow@K+2→clear），≥3 次背靠背 build 才可能命中滞后核；彻底关闭需核
   静默屏障（登记于 hw/msrpm.h 注释，T4）。
5. ❓ **新登记（d19185d）：全局发布 vs 每核视图保真**：CR3 写路径的视图切换从
   exit 上下文全局发布 NCr3，未运行目标进程的核也被切到进程视图（靠其自身下次
   切换自愈）。当前进程视图≡默认视图时不可见；**M4 在进程视图中藏页之前
   必须改为每核视图选择（ConfigureVmcb/VMRUN 按本核 CR3 选 NCr3）**——登记于
   npt.h 激活协议 M4 PREREQUISITE。
