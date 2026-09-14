// svmb - NPF (#NPF exit, reason 0x400) handler interface: the decision core
// is a pure, offline-testable classifier; the dispatcher handler applies the
// decision to the guest. Activation requires NPT to be enabled (tier-3) -
// with NPT off the handler declines and exits fall to the default policy.
#ifndef SVMB_NPF_H
#define SVMB_NPF_H

#include "mm/npt.h"
#include "core/exit_dispatcher.h" // GuestContext (glue: handler signature)

namespace svmb
{

enum class NpfAction : u32
{
    MapRam = 0,   // absent leaf inside a RAM range: lazily map identity, re-execute
    MapNonRam,    // absent leaf outside RAM (MMIO): skeleton maps RWX too to
                  // keep the guest alive; tier-3 must narrow this per-device
    DenyWrite,    // write fault on a read-only leaf: module-policy point (M4)
    DenyExecute,  // exec fault on an NX leaf: hook/#BP scheme landing spot
    DenyUser,     // user-mode access to a supervisor leaf: policy point
    Unhandled,    // spurious or undecided - falls through to the chain
};

struct NpfDecision
{
    NpfAction Action;
    u64 Gpa;
};

// pure classifier (no side effects): info1 is VMCB.EXITINFO1 (P/W/U/X bits),
// info2 the faulting GPA
NpfDecision NpfClassify(NptView& view, PhysRanges& ranges, u64 gpa, u64 info1);

// slide seam for the NPT hook engine (layering: mm must not depend on
// modules, so the hook module registers this instead of npf.cpp calling it).
// Invoked from the NPF handler for every PRESENT fault before
// classification: when the registered engine owns the faulting page it
// applies its exec/data slide and returns true (fault serviced,
// re-execute). cb(ctx, view, gpa4k, info1).
void NpfSetSlideHook(bool (*cb)(void* ctx, NptView& view, u64 gpa4k,
                                u64 info1),
                     void* ctx);

// r47 Route-A sentinel seam: a module-owned DENY consumer. Called on every
// DenyWrite/DenyExecute/DenyUser classification BEFORE the generic
// spin-breaker applies. cb(gpa4k, info1, currentCr3, rip) returns true when
// it consumed the fault (it has recorded/resolved/re-armed itself); false
// falls through to the generic deny policy.
struct GuestContext;
void NpfSetDenyCallback(bool (*cb)(void* ctx, GuestContext& g, u64 gpa4k, u64 info1,
                                   u64 currentCr3, u64 rip),
                        void* ctx);

// dispatcher handler: register with vmexit::NPF; ud = NptManager*.
// Returns true when the fault was serviced (re-executed); false declines to
// the default policy.
bool HandleNpfExit(GuestContext& ctx, void* ud);

// r68 hard-net telemetry: how many Unhandled NPFs were resolved outright
// after a full flush window (see g_npfHeals in npf.cpp)
u32 NpfHealCount();

} // namespace svmb

#endif // SVMB_NPF_H
