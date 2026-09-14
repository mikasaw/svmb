// svmb - exit event registry dispatcher.
// Replaces the monolithic switch: handlers register per exit code and form a
// priority-ordered chain per exit. Unhandled exits fall through to a
// configurable default policy (never bugchecks by default).
#ifndef SVMB_EXIT_DISPATCHER_H
#define SVMB_EXIT_DISPATCHER_H

#include "core/vcpu.h"
#include "platform/base.h"

namespace svmb
{

class Hypervisor;

// ---- guest context handed to every handler ----
struct GuestContext
{
    VcpuContext* Vcpu;
    GuestRegs* Regs;
    u64 Exit;        // VMCB.ExitCode
    u64 Info1;       // VMCB.ExitInfo1
    u64 Info2;       // VMCB.ExitInfo2
    u64 IntInfo;     // VMCB.ExitIntInfo
    u64 NRip;        // VMCB.NRip (next instruction for intercept-class exits)
    bool AdvanceRip; // rip := NRip on dispatch completion; honored even when an
                     // event is injected. Handlers wanting fault re-execution
                     // (e.g. reinjected page faults) set this to false
    bool EventInjected;

    VMCB* Vmcb() const { return Vcpu->GuestVmcb; }
    u64 GuestCr3() const { return Vmcb()->Save.Cr3; }
    u32 CpuIndex() const { return Vcpu->Info.CpuIndex; }
};

// return true = handled (stops the chain); false = pass to next handler
using ExitHandlerFn = bool (*)(GuestContext& ctx, void* userData);

// default behavior when no handler handled the exit
enum class DefaultPolicy : u32
{
    AdvanceOrReinject = 0, // best-effort transparent resume (default)
    InjectUd,              // inject #UD at the guest
    Log,                   // log only, resume transparently
    Bugcheck,              // bring the machine down (debug builds only)
};

// ---- exit classes determining the default rip behavior ----
enum class ExitClass : u8
{
    Intercept,  // instruction was emulated/passed: rip = NRip (cpuid, msr, cr, dr, vmmcall...)
    Fault,      // faulting instruction must re-execute: rip unchanged (npf, exceptions)
};

struct ExitHandlerEntry
{
    ExitHandlerEntry* Next;
    ExitHandlerFn Fn;
    void* UserData;
    ModuleToken Owner;   // ModuleToken for batch detach (CORE_TOKEN = core built-ins)
    u32 Priority;        // higher runs first
    u64 ExitCode;
    bool Disabled;
};

class ExitDispatcher
{
public:

    NTSTATUS Init();
    void Deinit(); // frees all entries (called only after all cores left SVM)

    NTSTATUS Register(u64 exitCode, ExitHandlerFn fn, void* userData,
                      ModuleToken owner, u32 priority);
    // removes (disables) every entry registered by `owner`; returns count
    u32 UnregisterOwner(ModuleToken owner);

    void Dispatch(GuestContext& ctx);
    void SetDefaultPolicy(DefaultPolicy p) { Default_ = (LONG)p; }
    u64 TotalUnhandled() const { return Unhandled_; }

private:
    void ApplyDefault(GuestContext& ctx);

    // hash buckets of priority-sorted handler lists, guarded by spinlock.
    // Exit codes are sparse (max 0x403 + specials) so a fixed-multiply hash
    // over 512 buckets keeps each hot-path list short.
    static constexpr u32 BUCKETS = 512;
    ExitHandlerEntry* Buckets_[BUCKETS];
    KSPIN_LOCK Lock_;
    ExitHandlerEntry* Graveyard_; // disabled entries, freed at Deinit
    volatile LONG Default_ = 0;   // DefaultPolicy::AdvanceOrReinject
    volatile LONG64 Unhandled_ = 0;

    static u32 BucketOf(u64 exit) { return (u32)((exit * 0x9E3779B97F4A7C15ull >> 32) & (BUCKETS - 1)); }
};

// ---- shared helpers for handlers (exit path, any IRQL <= DISPATCH) ----

// rip = NRip (for intercept-class exits)
inline void AdvanceGuestRip(GuestContext& ctx)
{
    ctx.Vmcb()->Save.Rip = ctx.NRip;
    ctx.Regs->Rip = ctx.NRip;
}

// inject an event to be delivered on guest re-entry. type: EVT_* from svm_defs.
// errorCodeValid mirrors the EV bit; error code pushed for gates that expect it.
void InjectEvent(GuestContext& ctx, u32 vector, u32 type, bool errorCodeValid, u32 errorCode);

// software-exception flavor used by #BP/#UD/#DB handlers (type 3)
inline void InjectException(GuestContext& ctx, u32 vector)
{
    InjectEvent(ctx, vector, EVT_EXCEPTION, false, 0);
}

// reinject the event that caused an exception-class exit (VMCB.ExitIntInfo)
void ReinfectExitEvent(GuestContext& ctx);

// passthrough emulation helpers
void EmulateCpuid(GuestContext& ctx);        // execute CPUID on host, write regs
u64   ReadGuestMsrPassthrough(u32 msr);      // raw rdmsr (host)
void  WriteGuestMsrPassthrough(u32 msr, u64 value);

} // namespace svmb

#endif // SVMB_EXIT_DISPATCHER_H
