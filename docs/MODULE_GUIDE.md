# svmb 模块开发指南

一个模块 = 一个实现文件 + `core/module.cpp` 注册表里的一行。模块只接触
`inc/svmb/module_api.h` 的函数表，核心不感知任何模块代码。

## 最小示例

```cpp
// src/modules/my_module.cpp
#include "svmb/module_api.h"

namespace
{
using namespace svmb;

bool MyHandler(GuestContext& ctx, void*)
{
    if ((u32)ctx.Regs->Rax != 0x40000101)
        return false;              // 不是本模块的 leaf，链继续
    ctx.Regs->Rax = 0x4D4F444E;    // 应答
    ctx.AdvanceRip = true;         // 注入与否都会尊重此标志
    return true;
}

NTSTATUS MyInit(const SvmbApi* api)
{
    // 拦截需求（引用计数，detach 自动释放）
    NTSTATUS st = api->RequireMsr(api, 0xC0010117, true, true);
    if (!NT_SUCCESS(st))
        return st;
    // 出口 handler（priority 高者先执行）
    return api->RegisterExitHandler(api, vmexit::CPUID, MyHandler, nullptr, 10);
}

void MyStop()
{
}
} // namespace

namespace svmb
{
SVMB_DEFINE_MODULE(ModuleMy, "my_module", 0, MyInit, MyStop)
}
```

注册：`src/core/module.cpp` 的 `gModuleRegistry[]` 加一行 `&ModuleMy,`。
构建后在 VM 里 `svmbctl mod attach my_module` 即生效；detach 时框架自动
回收 handler/拦截位/hypercall。

## IRQL 契约

exit handler 运行在 guest 触发 exit 时所处的 IRQL（可能在中断里）。若拦截
内核在高 IRQL 频繁触碰的资源（热 MSR、CR），handler 必须无锁且不可分页。

## 现有内建组件（不经模块框架，直接可用）

| 组件 | 说明 |
|---|---|
| `mm::NptManager`（gNpt） | 默认 NPT 视图 + 多视图注册表（MapRange/Split2M/SwapPage4k/SetActiveView） |
| `NptHookManager`（gHooks） | 隐藏页 hook，同页多实例补丁合成（IOCTL_NPT_HOOK / svmbctl npt）；NPT live 时经 NPF slide 激活 |
| `Cr3Seed`（gCr3Seed） | pid→CR3+映像名 进程表（进程创建时填充，支持按名查找/创建通知） |
| `Cr3Monitor` 模块 | CR3 读伪造/写监控，按映像名布防（IOCTL_CR3_CONFIG / svmbctl cr3 watch） |
| `Debugger` 模块 | #BP/#DB/#UD 事件 + DR 阴影/伪造 + MTF 单步（IOCTL_DBG_CONFIG / dbg events） |
| `DbgEventRing`（gDbgRing） | 调试事件环（IOCTL_DBG_EVENTS），BP/单步/#UD/进程布防事件的生产者 |

## 内建模块说明

### debugger
`mod attach debugger` 后 #BP/#DB/#UD 出口被上报到事件环并透明重注入。
`IOCTL_DBG_CONFIG`（`SVMB_DBG_CONFIG`）：`HideDr=1` 开启 DR 阴影（guest 的
DR 读写全部落在每核阴影里，硬件断点对外隐身）；`SpoofDr7Zero=1` 让 DR7 读 0。
`DebuggerArmSingleStep()` 是 MTF 单步原语（模块内消费武装步骤）。detach 时
框架自动撤销异常/DR 拦截。

### cr3_monitor
`IOCTL_CR3_CONFIG`（`SVMB_CR3_CONFIG`）三种目标方式，优先级 image > pid > raw：
`TargetImage` 按映像名布防（已运行进程立即生效，后续同名进程经种子表创建
通知自动布防并推 `SvmbDbgEvtProcessWatch`）；`TargetPid` 经种子表解析；
`TargetCr3` 直接指定。`XorKey`+`EnableReadSpoof` 全局伪造，目标进程读伪造
始终用同一 key（无 key 时不伪造，驱动会打告警）。控制台入口：
`svmbctl cr3 watch <exe> [xorkeyhex]` / `cr3 unwatch` / `cr3 stats`。

`svmbctl selftest` 覆盖以上全部的离线断言（无需 `start`）。

### stealth (r92)
`mod attach stealth` 开启 CPUID 环境伪装，`mod detach stealth` 精确还原
（生命周期即开关，无旋钮无 IOCTL）。抹除集合：leaf 1 的 ECX[31]
（hypervisor present）+ 0x40000000–0x40000010 签名叶（"VMwareVMware"、
Hv#1、0x40000010 TSC/总线频率提示）。刻意不抹 svmb 自有通道
（0x400000FE presence probe / 0x400000FF hypercall / 0x40000100 demo），
ctl 与 hypercall 在挂载期间照常工作。所有权判定与变换拆成纯函数
（`StealthScrubLeaf`/`StealthApplyScrub`，modules/stealth.h），由
`svmbctl selftest` 的 stealth.* 断言离线钉死；命中计数在 detach 时打进
日志汇总。**不做开机自动挂载**——Windows 引导期按 hypervisor 叶做校准，
冷启动抹 0x40000010 的行为未验证。处理器优先级 10 > core 的
HandleCpuidExit(0)，被接管的叶不会再落到 core；未接管的叶原样放行。
