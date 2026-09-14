// svmb - hypercall dispatch: VMMCALL primary channel + CPUID-leaf fallback.
// Protocol:
//   VMMCALL: rax=nr, rbx=a1, rcx=a2, rdx=0 -> rax=result
//            (ring3 callers: only HC_PROBE is answered)
//   CPUID leaf 0x400000FF: eax=leaf, ecx=nr, rbx=a1, rdx=a2
//                          -> rax=result (ring3: HC_PROBE/HC_VERSION only)
#ifndef SVMB_HYPERCALL_H
#define SVMB_HYPERCALL_H

#include "core/exit_dispatcher.h"
#include "platform/base.h"

namespace svmb
{

using HypercallFn = u64 (*)(u64 a1, u64 a2, u64 a3, GuestContext& ctx, void* userData);

class Hypercall
{
public:
    static constexpr u32 MAX_HANDLERS = 64;

    NTSTATUS Init();
    void Deinit();

    NTSTATUS Register(u32 nr, HypercallFn fn, void* userData, ModuleToken owner);
    u32 UnregisterOwner(ModuleToken owner);
    // returns hypercall result convention: 0 = success, else NTSTATUS-ish error
    u64 Invoke(u32 nr, u64 a1, u64 a2, u64 a3, GuestContext& ctx);

    // built-in handlers
    static u64 HcProbe(u64 a1, u64 a2, u64 a3, GuestContext& ctx, void* ud);
    static u64 HcVersion(u64 a1, u64 a2, u64 a3, GuestContext& ctx, void* ud);
    static u64 HcExitVmm(u64 a1, u64 a2, u64 a3, GuestContext& ctx, void* ud);
    static u64 HcUpdateBarrier(u64 a1, u64 a2, u64 a3, GuestContext& ctx, void* ud);

private:
    struct Entry
    {
        HypercallFn Fn;
        void* UserData;
        ModuleToken Owner;
        u32 Nr;
        bool Used;
    };
    Entry Entries_[MAX_HANDLERS];
    KSPIN_LOCK Lock_;
};

// exit handlers registered with the dispatcher for both channels
bool HandleVmmcallExit(GuestContext& ctx, void* ud);
bool HandleCpuidExit(GuestContext& ctx, void* ud); // hypercall leaf part; falls through
bool HandleMsrExit(GuestContext& ctx, void* ud);   // EFER/VM_CR/HSAVE protection + passthrough
bool HandleVmrunExit(GuestContext& ctx, void* ud); // anti-nesting

} // namespace svmb

#endif // SVMB_HYPERCALL_H
