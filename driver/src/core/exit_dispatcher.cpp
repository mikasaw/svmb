#include "core/exit_dispatcher.h"
#include "core/crumbs.h"
#include "core/hypervisor.h" // r157: g_stepProbe death-window probe
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
        ExitHandlerEntry* n = g->GraveNext;
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
    e->GraveNext = nullptr;

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
                // r110 B10: link the graveyard through GraveNext and leave
                // e->Next untouched - Dispatch may be parked in an unlocked
                // handler call holding e, and its `e = e->Next` resume must
                // stay on the live chain. Overwriting Next (the old code)
                // redirected that walk into the graveyard and silently
                // skipped every remaining handler of the bucket.
                e->GraveNext = Graveyard_;
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
    InterlockedExchange64(&g_stepProbe[1], 6); // r157: InjectEvent enter
    EventInj inj = {};
    EventInj pre = {};
    pre.Data = ctx.Vmcb()->Ctrl.EventInj.Data;
    InterlockedExchange64(&g_stepProbe[1], 7); // r157: post pre-read
    g_stepProbe[3] = (LONG64)pre.Data;
    inj.F.Vector = vector;
    inj.F.Type = type;
    inj.F.Ev = errorCodeValid ? 1 : 0;
    inj.F.ErrorCode = errorCode;
    inj.F.Valid = 1;
    ctx.Vmcb()->Ctrl.EventInj.Data = inj.Data;
    g_stepProbe[4] = (LONG64)inj.Data;
    g_stepProbe[5] = (LONG64)ctx.Vmcb()->Ctrl.EventInj.Data;
    InterlockedExchange64(&g_stepProbe[1], 8); // r157: post write+readback
    // r154 diagnosis: write-readback - the T+0 freeze snapshot showed
    // 0x302 (valid=0, vector=2, type=3) at 0xA8 where this code writes
    // 0x800003xx. Log pre/post values to pin whether the store lands
    // intact and who rewrites it.
    SVMB_LOGW("injdiag: pre=%llx wrote=%llx readback=%llx rip=%llx",
              (unsigned long long)pre.Data, (unsigned long long)inj.Data,
              (unsigned long long)ctx.Vmcb()->Ctrl.EventInj.Data,
              (unsigned long long)ctx.Regs->Rip);
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
    InterlockedExchange64(&g_stepProbe[1], 4); // r157: Reinfect enter
    // r155: triangulation probe - prints before anything else in the
    // r154 death window (exc-exit log printed, injdiag never did)
    SVMB_LOGW("reinfect: enter exit=%llx rip=%llx",
              (unsigned long long)ctx.Exit,
              (unsigned long long)ctx.Regs->Rip);
    // r154: the EXIT CODE is the authoritative event source for
    // exception-class exits (APM: 0x40+N for exception N). EXITINTINFO is
    // NOT portable truth: QEMU TCG's do_vmexit mirrors event_inj into
    // exit_int_info (svm_helper.c:904) - the residue of the LAST DELIVERED
    // event (handle_even_inj repopulates event_inj on every delivery),
    // not the event that caused this exit. Copying it reinjects a stale
    // event: r153's smoking gun was a stale NMI delivered on the faulting
    // RIP -> kernel 0x80 bugcheck family (r136's vhv 0x80 minidumps are
    // the same death, unified).
    if (ctx.Exit >= vmexit::EXCEPTION(0) && ctx.Exit <= vmexit::EXCEPTION(31))
    {
        u32 vector = (u32)(ctx.Exit - vmexit::EXCEPTION(0));
        // EXITINFO1 carries the error code; the Ev bit is only meaningful
        // for the has-error-code vectors (#DF/#TS/#NP/#SS/#PF/#AC)
        bool hasErr = vector == 8 || (vector >= 10 && vector <= 14) || vector == 17;
        u32 err = hasErr ? (u32)ctx.Info1 : 0;
        InterlockedExchange64(&g_stepProbe[1], 5); // r157: range ok
        InjectEvent(ctx, vector, EVT_EXCEPTION, hasErr, err);
        return;
    }
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



