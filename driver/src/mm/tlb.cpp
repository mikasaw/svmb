#include "mm/tlb.h"
#include "core/hypervisor.h"
#include "core/crumbs.h"
#include "platform/util.h"

namespace svmb
{

// r123 (B7): the view-publish/wiggle broadcast shape. The calling core
// applies its own switch SYNCHRONOUSLY (the probe stage-1 NCr3 readback
// semantics depend on it); every other core gets a per-vCPU pending slot
// consumed at its own exit entry - a VMCB is never written from another
// CPU (the r108 freeze class: lost CleanBits updates racing the owner's
// VMRUN boundary). PublishedNcr3 is deliberately NOT written here - it
// stays the r53 policy's single-writer bookkeeping (and the
// wiggle-restore target source), exactly as the old direct writes left
// it. Last-writer-wins on the pending slot is benign: a later phase
// overwrites the target before a quiet core consumes the earlier one,
// which is semantically "that core skipped the intermediate view".
template <typename F>
static void TlbViewPublishHybrid(F&& targetFor)
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || !hv->IsRunning())
        return;
    u32 self = CurrentCpuIndex(); // r123 acceptance N1: INDEX space -
                                  // CpuIndex is an index, and the group-
                                  // relative KeGetCurrentProcessorNumber
                                  // can falsely match another core's slot
                                  // on multi-group systems
    hv->ForEachVcpu([&](VcpuContext* v) {
        u64 want = targetFor(v);
        if (v->Info.CpuIndex == self)
        {
            v->GuestVmcb->Ctrl.NCr3 = want;
            v->GuestVmcb->Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
            v->GuestVmcb->Ctrl.CleanBits.Bits = 0; // NCr3/TLB dirty
        }
        else
        {
            v->Info.PendingViewPml4 = want;
            InterlockedExchange(&v->Info.PendingViewSwitch, 1);
        }
    });
}

void TlbApplyViewSwitchAllCores(u64 pml4Pa)
{
    // offline: activation bookkeeping only; VMCBs pick the view up at
    // their next exit entry (r123) / configure (enter)
    TlbViewPublishHybrid(
        [pml4Pa](VcpuContext*) -> u64 { return pml4Pa; });
}

NTSTATUS TlbInvlpgaPageAllCores(u64 gva)
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || !hv->IsRunning())
        return STATUS_SUCCESS; // offline no-op: no live translations exist

    return RunOnEachCore([gva](u32) -> NTSTATUS {
        // hardware form: invlpga gva, asid - ASID 0 would target all ASIDs
        _svmb_invlpga(gva, 0);
        return STATUS_SUCCESS;
    });
}

void TlbInvlpgaLocal(u64 gpa)
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || !hv->IsRunning())
        return; // offline: no live translations exist (and INVLPGA would #UD)
    _svmb_invlpga(gpa, 0); // ASID 0 = all ASIDs
}

void TlbKickFlushAllCores()
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || !hv->IsRunning())
        return;
    if (Hypervisor::TlbModeKnob() != 0)
        return; // tlbMode=1 = never-flush semantics: do not degrade it
    CrumbPost(CRUMB_KICK_ENTER);
    // r108 root fix (freeze class): NEVER write another vCPU's VMCB from
    // this CPU - racing the owning core's own VMCB updates lost CleanBits
    // dirty markers at the VMRUN boundary (stale cached controls). Each
    // core consumes its pending flag at its own SvmbVmExitEntry instead.
    hv->ForEachVcpu([](VcpuContext* v) {
        InterlockedExchange(&v->Info.PendingTlbKick, 1);
    });
    CrumbPost(CRUMB_KICK_DONE);
}

// r80: see tlb.h. Broadcast #1 must complete before the exit handler
// returns so the very next VMRUN runs on the scratch view; the restore
// half runs from the re-arm DPC (>=1 timer tick later, i.e. the guest
// really executed on the scratch view in between). r123: "complete"
// now means the calling core switched synchronously and every other
// core holds a pending slot that lands before its next VMRUN - the
// per-core excursion guarantee is unchanged.
void TlbWiggleToScratch(u64 scratchPml4Pa)
{
    TlbViewPublishHybrid(
        [scratchPml4Pa](VcpuContext*) -> u64 { return scratchPml4Pa; });
}

void TlbWiggleRestore(u64 activePml4Pa)
{
    TlbViewPublishHybrid(
        [activePml4Pa](VcpuContext* v) -> u64 {
            // per-core truth: a ProcView-pinned core keeps ITS OWN view
            // (the r53 policy recomputes want per exit and would
            // otherwise leave the core stranded on the manager-active
            // view - r77-P1-1)
            return v->Info.PublishedNcr3 ? v->Info.PublishedNcr3
                                         : activePml4Pa;
        });
}

} // namespace svmb
