// svmb - r166 nested virtualization v1 (software nesting, QEMU-shaped).
//
// A guest-kernel hypervisor ("H") executing VMRUN exits to us via the
// mandatory ic2.VMRUN intercept. v1 emulates the nesting in software:
// accept H's VMCB, install L2 state into the REAL VMCB (skeleton-inherit
// contract: only Rip/Rsp/Rflags/Rax/Cr3 come from H, everything else is
// L1's current state; svmb's control area stays with H's intercept
// bitmaps OR-ed in), re-enter - the CPU now runs L2 while QEMU still
// sees exactly one SVM level. Every later exit on that core REFLECTS to
// H: exit info + L2 state are copied into H's VMCB, H's stashed state
// (save area + GPR frame) is restored, and H resumes just past its
// VMRUN - the nested-SVM contract KVM implements in hardware.
//
// v1 contract (documented, enforced at accept):
//   - H_vmcb PA: 4K-aligned, nonzero, inside the identity-mapped RAM
//   - H_vmcb.Ctrl.Np.NpEnable == 0   (nested paging unsupported in v1 -
//     two-level NPT has no TCG backing; L2 addresses ride svmb's single
//     NPT directly)
//   - intent triad nonzero: Save.Rip && Save.Rsp && Save.Cr3 &&
//     Save.Rflags (a zeroed/dummy VMCB - the stage-0 probe - is
//     REJECTED with the legacy #BP anti-nesting semantics)
// Anything else rejects exactly like the r140 anti-nesting path.
//
// The per-core state lives OUTSIDE VcpuContext (its layout is pinned by
// hw/asm/offsets.inc). Only the owning core ever touches its slot.
#ifndef SVMB_NESTED_H
#define SVMB_NESTED_H

#include "core/exit_dispatcher.h" // GuestContext
#include "core/regs.h"
#include "hw/vmcb.h"

namespace svmb
{

struct VcpuContext;

constexpr u32 NESTED_MAX_CORES = 64;

struct NestedState
{
    volatile LONG Active;   // L2 is running on this core
    u64 HvmcbPa;            // H's VMCB (guest physical)
    VmcbSave HSave;         // H's full save-area state at its VMRUN
    GuestRegs HRegs;        // H's GPR frame at its VMRUN
    u32 SaveIc1;            // svmb's InterceptOpcode1  before the OR
    u32 SaveIc2;            // svmb's InterceptOpcode2  before the OR
    u32 SaveExc;            // svmb's InterceptException before the OR
    u64 SaveTscOffset;      // svmb's TscOffset before the r168 compose
    EventInj SaveEventInj;  // r168b: L1's pending EVENTINJ stashed at
                            // accept (must never fire into L2)
    volatile LONG PendingL1Event; // 1 = SaveEventInj is live
    u64 SaveVIntr;          // r168b: L1's VIntr (ctl_has_irq hazard)
    u64 SaveIntStatus;      // r168b: L1's interrupt-shadow word
    // r172 nested-NPT shadow v0: svmb's real NCR3 stashed at accept while
    // the shadow root runs; NestedNp=1 marks an NP session (compose route)
    u64 SaveNcr3;
    u32 NestedNp;
    u64 HNptRoot;           // H's nested NPT PML4 (an L1-GPA)
    void* ShadowRaw;        // per-core shadow page pool (never freed, reused)
    u64 ShadowPml4Pa;
    // r186 intent gate: H's OWN intercept request (what it armed in its
    // VMCB) and whether it supplied L2's IDT. An L2 exit reflects only
    // when H ordered it; H's own IDT means native event delivery into L2
    // is safe (the in-place shape: L2 IS the OS context).
    u32 HIc1;
    u32 HIc2;
    u32 HExc;
    u32 OwnIdt;             // r186c: 1 = in-place session (h->Save.Cr3 ==
                            // the launcher's CR3 - native event delivery
                            // into L2 is sound; toy worlds get the net)
    u32 IntrDeliveryWorld;  // r200: 1 = intr-delivery window open (the
                            // punt swapped NCR3 to the L1-stable root;
                            // the next exit restores the shadow)
};

inline NestedState& NestedFor(u32 cpuIndex)
{
    static NestedState s_states[NESTED_MAX_CORES];
    return s_states[cpuIndex < NESTED_MAX_CORES ? cpuIndex : 0];
}

inline bool NestedActive(u32 cpuIndex)
{
    return cpuIndex < NESTED_MAX_CORES &&
           NestedFor(cpuIndex).Active != 0;
}

// HandleVmrunExit body: accept-and-enter L2, or reject (anti-nesting).
bool NestedVmrunExit(GuestContext& ctx);

// r168: VMLOAD/VMSAVE executed inside the guest are refused with #GP -
// their native execution would rewrite the REAL save area from a VMCB
// svmb does not own. Registered for both exit codes in hypervisor.cpp.
bool NestedVmloadExit(GuestContext& ctx, void* ud);

// SvmbVmExitEntry gate: reflect the current exit into H's VMCB and
// restore H. Runs BEFORE module dispatch - handlers never see L2 exits.
void NestedReflect(VcpuContext* vcpu, GuestRegs* regs);

// r200: close the intr-delivery world window (restore shadow root +
// re-arm the punt bit) at the earliest exit; no-op when no window open
void NestedIntrDeliveryWindowClose(VcpuContext* vcpu);


// r171: per-H virtual SVM view (the EFER-tension adjudication). A
// registered CR3 (the H process's context - captured at registration by
// the registering thread) sees EFER.SVME settable/readable, its own
// VM_HSAVE_PA, and the unmasked CPUID SVM bit; every other context keeps
// the stealth face (SVME masked, HSAVE 0, CPUID hidden). Registration is
// explicit opt-in - no global unmasking, the anti-detection stance holds
// for the rest of the guest. H's writes land in the virtual copies only;
// the real MSRs are never touched through this path.
u64 NestedHViewServe(u64 cr3, bool isWrite, u32 msr, u64 value,
                     bool* served);
void NestedHViewRegister(u64 cr3);
void NestedHViewUnregister(u64 cr3);
bool NestedHViewActive(u64 cr3);

// r172 nested-NPT shadow v0 (see nested.cpp). Called from the reflection
// gate for NPF exits while an NP session runs: software-walk H's nested
// NPT for the faulting L2-GPA and compose the shadow PTE (L2-GPA -> the
// L1-GPA H maps it to, svmb's own NPT then translates). info1 is the NPF
// error word: a composed leaf that would STILL fault this access (exec on
// an H-NX leaf, write on an H-RO leaf, user on an H-supervisor leaf) is
// H's fault to handle - false reflects the NPF. true = composed, re-enter
// L2; false = H's walk faulted or the perms deny -> reflect to H.
bool NestedShadowCompose(u32 cpuIndex, u64 l2Gpa, u64 npfInfo1);
bool NestedShadowInit(NestedState* ns);

// r186 intent gate: did H's own VMCB arm this L2 exit? Only ordered exits
// reflect; the rest fall through to svmb's dispatch (transparent service).
bool NestedExitOrderedByH(u32 cpuIndex, u64 exitCode);

// r186b: serve an exit H never ordered INLINE - a closed mini-dispatch
// that never touches the L1 pipeline (view publish / TLB mode / interrupt
// repair are L1 machinery; falling through there froze the bench on s2).
// Real third-party H's (the reference hypervisor) KeBugCheck on exits they don't handle,
// so unordered exits must never reflect either.
void NestedSelfServeExit(VcpuContext* vcpu, GuestRegs* regs);

// r176: INVLPGA intercept body. H executes INVLPGA (any core - an SMP H
// edits its nested NPT from another core while L2 runs here) meaning
// "flush the nested translations": every shadow window's composed world
// is now stale. The handler re-issues the native invalidation host-side
// (same physical TLB, ASID-tagged - non-nested callers like the driver's
// own guest-context hook paths keep working), wipes every composed
// window, and TLB-kicks cores with live sessions (the r108 pending
// protocol). Registered for exit 0x7A in hypervisor.cpp.
bool NestedInvlpgaExit(GuestContext& ctx, void* ud);

} // namespace svmb

#endif // SVMB_NESTED_H
