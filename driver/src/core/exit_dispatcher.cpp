#include "core/exit_dispatcher.h"
#include "core/crumbs.h"
#include "platform/logger.h"
#include "platform/util.h"

namespace svmb
{

NTSTATUS ExitDispatcher::Init()
{
    KeInitializeSpinLock(&Lock_);
    RtlZeroMemory(Buckets_, sizeof(Buckets_));
    Graveyard_ = nullptr;
    return STATUS_SUCCESS;
}

void ExitDispatcher::Deinit()
{
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    for (u32 i = 0; i < BUCKETS; ++i)
    {
        ExitHandlerEntry* e = Buckets_[i];
        while (e)
        {
            ExitHandlerEntry* n = e->Next;
            ExFreePoolWithTag(e, TAG_SVMB);
            e = n;
        }
        Buckets_[i] = nullptr;
    }
    ExitHandlerEntry* g = Graveyard_;
    while (g)
    {
        ExitHandlerEntry* n = g->Next;
        ExFreePoolWithTag(g, TAG_SVMB);
        g = n;
    }
    Graveyard_ = nullptr;
    KeReleaseSpinLock(&Lock_, old);
}

NTSTATUS ExitDispatcher::Register(u64 exitCode, ExitHandlerFn fn, void* userData,
                                  ModuleToken owner, u32 priority)
{
    if (!fn)
        return STATUS_INVALID_PARAMETER;

    ExitHandlerEntry* e = (ExitHandlerEntry*)AllocNonPaged(sizeof(*e), TAG_SVMB);
    if (!e)
        return STATUS_INSUFFICIENT_RESOURCES;
    e->Fn = fn;
    e->UserData = userData;
    e->Owner = owner;
    e->Priority = priority;
    e->ExitCode = exitCode;
    e->Disabled = false;

    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    ExitHandlerEntry** pp = &Buckets_[BucketOf(exitCode)];
    // insert sorted by priority descending; equal priority -> newest runs first
    while (*pp && (*pp)->Priority >= priority)
        pp = &(*pp)->Next;
    e->Next = *pp;
    *pp = e;
    KeReleaseSpinLock(&Lock_, old);
    return STATUS_SUCCESS;
}

u32 ExitDispatcher::UnregisterOwner(ModuleToken owner)
{
    u32 count = 0;
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    for (u32 i = 0; i < BUCKETS; ++i)
    {
        ExitHandlerEntry** pp = &Buckets_[i];
        while (*pp)
        {
            if ((*pp)->Owner == owner)
            {
                ExitHandlerEntry* e = *pp;
                *pp = e->Next;
                e->Disabled = true;
                e->Next = Graveyard_;
                Graveyard_ = e;
                ++count;
            }
            else
            {
                pp = &(*pp)->Next;
            }
        }
    }
    KeReleaseSpinLock(&Lock_, old);
    return count;
}

void ExitDispatcher::Dispatch(GuestContext& ctx)
{
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    ExitHandlerEntry* e = Buckets_[BucketOf(ctx.Exit)];
    bool handled = false;
    while (e)
    {
        if (!e->Disabled)
        {
            ExitHandlerFn fn = e->Fn;       // snapshot: safe against concurrent unregister
            void* ud = e->UserData;
            KeReleaseSpinLock(&Lock_, old); // handlers may register/log etc.
            handled = fn(ctx, ud);
            KeAcquireSpinLock(&Lock_, &old);
            if (handled)
                break;
        }
        e = e->Next;
    }
    KeReleaseSpinLock(&Lock_, old);

    if (!handled)
    {
        InterlockedIncrement64(&Unhandled_);
        ApplyDefault(ctx);
    }

    // rip bookkeeping: AdvanceRip is honored even when an event was injected.
    // Injecting without advancing leaves the guest on the un-executed
    // instruction (infinite re-intercept loop); handlers that need fault
    // re-execution semantics set AdvanceRip = false themselves (the
    // AdvanceOrReinject default does exactly that for exception-class exits)
    if (ctx.AdvanceRip)
    {
        AdvanceGuestRip(ctx);
    }
}

void ExitDispatcher::ApplyDefault(GuestContext& ctx)
{
    CrumbPost(CRUMB_DISPATCH_DEFAULT);
    switch ((DefaultPolicy)Default_)
    {
    case DefaultPolicy::Bugcheck:
        KeBugCheckEx(MANUALLY_INITIATED_CRASH, (ULONG_PTR)ctx.Exit,
                     (ULONG_PTR)ctx.Info1, (ULONG_PTR)ctx.Info2, (ULONG_PTR)ctx.NRip);
        break;

    case DefaultPolicy::InjectUd:
        if (ctx.Exit >= vmexit::EXCEPTION(0) && ctx.Exit <= vmexit::EXCEPTION(31))
        {
            // exception exits are faults: reinjecting UD on top of a fault is
            // only safe for #UD-class; otherwise forward the original event
            ReinfectExitEvent(ctx);
        }
        else
        {
            InjectException(ctx, EXC_UD);
        }
        break;

    case DefaultPolicy::Log:
        SVMB_LOGW("unhandled exit %llx info1=%llx info2=%llx",
                  ctx.Exit, ctx.Info1, ctx.Info2);
        break;

    case DefaultPolicy::AdvanceOrReinject:
    default:
        if (ctx.Exit >= vmexit::EXCEPTION(0) && ctx.Exit <= vmexit::EXCEPTION(31))
        {
            ReinfectExitEvent(ctx);
            ctx.AdvanceRip = false; // faults re-execute
        }
        else
        {
            // unknown intercept-class exit: resume transparently
            ctx.AdvanceRip = true;
        }
        break;
    }
}

void InjectEvent(GuestContext& ctx, u32 vector, u32 type, bool errorCodeValid, u32 errorCode)
{
    EventInj inj = {};
    inj.F.Vector = vector;
    inj.F.Type = type;
    inj.F.Ev = errorCodeValid ? 1 : 0;
    inj.F.ErrorCode = errorCode;
    inj.F.Valid = 1;
    ctx.Vmcb()->Ctrl.EventInj.Data = inj.Data;
    // r38: this VMware vhv silently DROPS control-area writes unless the
    // VMCB clean bits are cleared (same law as the NCr3 publish and the
    // live intercept arming - stage 1 / M4-D). Without this, the injected
    // event never fires and the guest re-executes the faulting instruction
    // in an infinite #VMEXIT loop holding whatever locks its thread owns.
    ctx.Vmcb()->Ctrl.CleanBits.Bits = 0;
    ctx.EventInjected = true;
}

void ReinfectExitEvent(GuestContext& ctx)
{
    // ExitIntInfo describes the event that triggered an exception-class exit;
    // copy it back into EventInj so the guest receives it transparently
    EventInj src = {};
    src.Data = ctx.IntInfo;
    if (!src.F.Valid)
    {
        // no usable event info: fall back to a software exception vector match
        if (ctx.Exit >= vmexit::EXCEPTION(0) && ctx.Exit <= vmexit::EXCEPTION(31))
            InjectEvent(ctx, (u32)(ctx.Exit - vmexit::EXCEPTION(0)), EVT_EXCEPTION, false, 0);
        return;
    }
    EventInj inj = {};
    inj.F.Vector = src.F.Vector;
    inj.F.Type = src.F.Type;
    inj.F.Ev = src.F.Ev;
    inj.F.ErrorCode = src.F.ErrorCode;
    inj.F.Valid = 1;
    ctx.Vmcb()->Ctrl.EventInj.Data = inj.Data;
    // r39: same law as InjectEvent below - this vhv drops control-area
    // writes unless CleanBits is cleared. ReinfectExitEvent is the path the
    // exception exits actually use; the r38 fix covered only InjectEvent,
    // so every reinjected #BP/#UD was silently swallowed (kd break int3 ->
    // swallowed -> KDNET retry spin -> hang -> WER live dump -> soft reset).
    ctx.Vmcb()->Ctrl.CleanBits.Bits = 0;
    ctx.EventInjected = true;
}

void EmulateCpuid(GuestContext& ctx)
{
    int out[4] = {};
    __cpuidex(out, (int)ctx.Regs->Rax, (int)ctx.Regs->Rcx);
    ctx.Regs->Rax = (u32)out[0];
    ctx.Regs->Rbx = (u32)out[1];
    ctx.Regs->Rcx = (u32)out[2];
    ctx.Regs->Rdx = (u32)out[3];
}

u64 ReadGuestMsrPassthrough(u32 msr)
{
    return __readmsr(msr);
}

void WriteGuestMsrPassthrough(u32 msr, u64 value)
{
    __writemsr(msr, value);
}

} // namespace svmb
