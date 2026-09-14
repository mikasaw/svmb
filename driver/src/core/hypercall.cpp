#include "core/hypercall.h"
#include "core/hypervisor.h"
#include "hw/svm_defs.h"
#include "platform/logger.h"

namespace svmb
{

NTSTATUS Hypercall::Init()
{
    KeInitializeSpinLock(&Lock_);
    RtlZeroMemory(Entries_, sizeof(Entries_));
    return STATUS_SUCCESS;
}

void Hypercall::Deinit()
{
    RtlZeroMemory(Entries_, sizeof(Entries_));
}

NTSTATUS Hypercall::Register(u32 nr, HypercallFn fn, void* userData, ModuleToken owner)
{
    if (!fn)
        return STATUS_INVALID_PARAMETER;
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    for (u32 i = 0; i < MAX_HANDLERS; ++i)
    {
        Entry& e = Entries_[i];
        if (e.Used && e.Nr == nr)
        {
            KeReleaseSpinLock(&Lock_, old);
            return STATUS_OBJECT_NAME_COLLISION;
        }
    }
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    for (u32 i = 0; i < MAX_HANDLERS; ++i)
    {
        Entry& e = Entries_[i];
        if (!e.Used)
        {
            e.Used = true;
            e.Nr = nr;
            e.Fn = fn;
            e.UserData = userData;
            e.Owner = owner;
            status = STATUS_SUCCESS;
            break;
        }
    }
    KeReleaseSpinLock(&Lock_, old);
    return status;
}

u32 Hypercall::UnregisterOwner(ModuleToken owner)
{
    u32 count = 0;
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    for (u32 i = 0; i < MAX_HANDLERS; ++i)
    {
        Entry& e = Entries_[i];
        if (e.Used && e.Owner == owner)
        {
            e.Used = false;
            ++count;
        }
    }
    KeReleaseSpinLock(&Lock_, old);
    return count;
}

u64 Hypercall::Invoke(u32 nr, u64 a1, u64 a2, u64 a3, GuestContext& ctx)
{
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    HypercallFn fn = nullptr;
    void* ud = nullptr;
    for (u32 i = 0; i < MAX_HANDLERS; ++i)
    {
        Entry& e = Entries_[i];
        if (e.Used && e.Nr == nr)
        {
            fn = e.Fn;
            ud = e.UserData;
            break;
        }
    }
    KeReleaseSpinLock(&Lock_, old);
    if (!fn)
        return (u64)STATUS_NOT_IMPLEMENTED;
    return fn(a1, a2, a3, ctx, ud);
}

// ---- built-in handlers ----

u64 Hypercall::HcProbe(u64, u64, u64, GuestContext&, void*)
{
    return HYPERCALL_MAGIC;
}

u64 Hypercall::HcVersion(u64, u64, u64, GuestContext&, void*)
{
    return SVMB_DRIVER_VERSION;
}

u64 Hypercall::HcUpdateBarrier(u64, u64, u64, GuestContext& ctx, void*)
{
    // M3 skeleton: cross-core "apply pending state" rendezvous. Modules
    // queue per-core updates (view switch, intercept change) and call
    // HC_UPDATE_BARRIER on the cores that must observe them before
    // continuing; today there is no queued state, so this is a compile
    UNREFERENCED_PARAMETER(ctx);
    // barrier + acknowledgment
    MemoryBarrier();
    return 0;
}
u64 Hypercall::HcExitVmm(u64, u64, u64, GuestContext& ctx, void*)
{
    // kernel-mode callers only; devirtualize the core this exit came from
    VcpuContext* vcpu = ctx.Vcpu;
    if (!IsKernelVa(ctx.NRip) || vcpu->GuestVmcb->Save.Cpl != 0)
    {
        SVMB_LOGW("HC_EXIT_VMM rejected: user-mode caller");
        return (u64)STATUS_ACCESS_DENIED;
    }

    // atomic claim: only one devirtualization per core
    LONG expected = (LONG)VcpuState::Guest;
    if (InterlockedCompareExchange(&vcpu->Info.State, (LONG)VcpuState::Leaving, expected) != expected)
    {
        SVMB_LOGW("HC_EXIT_VMM reject: core %u state %ld != Guest",
                  vcpu->Info.CpuIndex, (LONG)vcpu->Info.State);
        return (u64)STATUS_UNSUCCESSFUL;
    }

    // NO logging inside this handler until the handoff is complete.
    // DbgPrint (the KdDebuggerEnabled branch of LogWrite) takes the debugger
    // transport lock and sends over KDNET; from inside a VMEXIT-return path -
    // with GIF just re-enabled and a not-yet-fully-settled host state - that
    // can re-enter the debugger or nest another #VMEXIT and wedge the core
    // mid-handoff (observed as Stop hangs in RunOnEachCore, every time with
    // a debugger or SerialLog attached). The completion line is logged by
    // Stop() after _svmb_hypercall returns, on a fully restored core.

    // Restore guest state first (consumes the guest VMCB while the core is
    // still ours), then re-enable GIF.
    __svm_vmload((SIZE_T)vcpu->Info.GuestVmcbPa);
    _disable();
    __svm_stgi();

    // leave SVM on this core
    u64 efer = __readmsr(MSR_EFER);
    __writemsr(MSR_EFER, efer & ~(1ull << EFER_SVME));

    // ask the asm loop to abandon the VMM and resume at NRip
    ctx.Regs->Extra1 = (u64)ctx.Regs;      // flag + base pointer (asm contract)
    ctx.Regs->Extra2 = ctx.NRip;           // resume rip (asm contract)
    return 0;
}

// ---- exit handlers ----

bool HandleVmmcallExit(GuestContext& ctx, void* ud)
{
    Hypercall* hc = (Hypercall*)ud;
    u32 nr = (u32)ctx.Regs->Rax;

    // VMMCALL is a kernel-mode channel; ring3 callers only get the probe
    if (ctx.Vmcb()->Save.Cpl != 0 && nr != HC_PROBE)
    {
        ctx.Regs->Rax = (u64)STATUS_ACCESS_DENIED;
        ctx.AdvanceRip = true;
        return true;
    }

    ctx.Regs->Rax = hc->Invoke(nr, ctx.Regs->Rbx, ctx.Regs->Rcx, ctx.Regs->Rdx, ctx);
    ctx.AdvanceRip = true;
    return true;
}

bool HandleCpuidExit(GuestContext& ctx, void* ud)
{
    Hypercall* hc = (Hypercall*)ud;

    // hypercall fallback channel
    if ((u32)ctx.Regs->Rax == HYPERCALL_CPUID_LEAF)
    {
        u32 nr = (u32)ctx.Regs->Rcx;
        // CPUID is the public detection channel: probe/version answer any
        // CPL (R3 tools probe presence through it), everything else is ring0
        if (ctx.Vmcb()->Save.Cpl != 0 && nr != HC_PROBE && nr != HC_VERSION)
        {
            ctx.Regs->Rax = (u64)STATUS_ACCESS_DENIED;
            ctx.AdvanceRip = true;
            return true;
        }
        u64 res = hc->Invoke(nr, ctx.Regs->Rbx, ctx.Regs->Rdx, 0, ctx);
        ctx.Regs->Rax = res;
        ctx.AdvanceRip = true;
        return true;
    }

    // presence probe answer (educational-hypervisor style)
    if ((u32)ctx.Regs->Rax == HYPERCALL_CPUID_LEAF - 1)
    {
        ctx.Regs->Rax = MAKE_TAG_('S', 'V', 'M', 'B');
        ctx.AdvanceRip = true;
        return true;
    }

    // normal CPUID: passthrough, but hide the SVM/NPT capability bits.
    // capture the INPUT leaf first - EmulateCpuid overwrites Rax with the
    // result's EAX, which never equals the leaf constant
    u32 leaf = (u32)ctx.Regs->Rax;
    EmulateCpuid(ctx);
    if (GuestCpuidHideEnabled)
    {
        if (leaf == CPUID_EXT_FEATURES)
            ctx.Regs->Rcx &= ~(1ull << SVM_CPUID_BIT);
        if (leaf == CPUID_SVM_FEATURES)
            ctx.Regs->Rdx &= ~(1ull << NPT_CPUID_BIT);
    }
    ctx.AdvanceRip = true;
    return true;
}

bool HandleMsrExit(GuestContext& ctx, void* ud)
{
    UNREFERENCED_PARAMETER(ud);
    u32 msr = (u32)ctx.Regs->Rcx;
    bool isWrite = (ctx.Info1 & 1) != 0;

    if (msr == MSR_VM_CR)
    {
        if (isWrite)
        {
            u64 value = (ctx.Regs->Rax & 0xFFFFFFFF) | (ctx.Regs->Rdx << 32);
            // keep SVM disabled from the guest's perspective
            value |= (1ull << VM_CR_SVMDIS);
            WriteGuestMsrPassthrough(msr, value);
        }
        else
        {
            u64 value = ReadGuestMsrPassthrough(msr);
            value |= (1ull << VM_CR_SVMDIS);
            ctx.Regs->Rax = (u32)value;
            ctx.Regs->Rdx = (u32)(value >> 32);
        }
        ctx.AdvanceRip = true;
        return true;
    }

    if (msr == MSR_VM_HSAVE_PA)
    {
        if (isWrite)
        {
            // drop guest writes: the host HSAVE area must stay put
        }
        else
        {
            // hide the non-zero HSAVE PA - the classic SVM-presence tell
            ctx.Regs->Rax = 0;
            ctx.Regs->Rdx = 0;
        }
        ctx.AdvanceRip = true;
        return true;
    }

    if (msr == MSR_EFER)
    {
        if (isWrite)
        {
            u64 value = (ctx.Regs->Rax & 0xFFFFFFFF) | (ctx.Regs->Rdx << 32);
            value |= (1ull << EFER_SVME); // SVM stays on while we are alive
            WriteGuestMsrPassthrough(msr, value);
        }
        else
        {
            u64 value = ReadGuestMsrPassthrough(msr);
            value &= ~(1ull << EFER_SVME);
            ctx.Regs->Rax = (u32)value;
            ctx.Regs->Rdx = (u32)(value >> 32);
        }
        ctx.AdvanceRip = true;
        return true;
    }

    // any other MSRPM-intercepted MSR: transparent passthrough
    if (isWrite)
    {
        u64 value = (ctx.Regs->Rax & 0xFFFFFFFF) | (ctx.Regs->Rdx << 32);
        WriteGuestMsrPassthrough(msr, value);
    }
    else
    {
        u64 value = ReadGuestMsrPassthrough(msr);
        ctx.Regs->Rax = (u32)value;
        ctx.Regs->Rdx = (u32)(value >> 32);
    }
    ctx.AdvanceRip = true;
    return true;
}

bool HandleVmrunExit(GuestContext& ctx, void* ud)
{
    UNREFERENCED_PARAMETER(ud);
    // anti-nesting: guest tries to enter SVM. Inject #BP with int3 (software
    // interrupt) semantics and resume at NRip so the VMRUN itself is skipped -
    // injecting without advancing would re-execute VMRUN forever
    InjectEvent(ctx, EXC_BP, EVT_SOFTINT, false, 0);
    ctx.AdvanceRip = true;
    return true;
}

} // namespace svmb
