#include "mm/tlb.h"
#include "core/hypervisor.h"
#include "core/crumbs.h"
#include "platform/util.h"

namespace svmb
{

void TlbApplyViewSwitchAllCores(u64 pml4Pa)
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || !hv->IsRunning())
        return; // offline: activation bookkeeping only; VMCBs pick the view
                // up at their next configure (enter)

    hv->ForEachVcpu([pml4Pa](VcpuContext* v) {
        v->GuestVmcb->Ctrl.NCr3 = pml4Pa;
        v->GuestVmcb->Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
        v->GuestVmcb->Ctrl.CleanBits.Bits = 0; // NCr3/TLB Control dirty
    });
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
// really executed on the scratch view in between).
void TlbWiggleToScratch(u64 scratchPml4Pa)
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || !hv->IsRunning())
        return;
    hv->ForEachVcpu([scratchPml4Pa](VcpuContext* v) {
        v->GuestVmcb->Ctrl.NCr3 = scratchPml4Pa;
        v->GuestVmcb->Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
        v->GuestVmcb->Ctrl.CleanBits.Bits = 0; // else the vmx drops the write
    });
}

void TlbWiggleRestore(u64 activePml4Pa)
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || !hv->IsRunning())
        return;
    hv->ForEachVcpu([activePml4Pa](VcpuContext* v) {
        // per-core truth: a ProcView-pinned core keeps ITS OWN view (the
        // r53 policy recomputes want per exit and would otherwise leave
        // the core stranded on the manager-active view - r77-P1-1)
        u64 want = v->Info.PublishedNcr3 ? v->Info.PublishedNcr3
                                         : activePml4Pa;
        v->GuestVmcb->Ctrl.NCr3 = want;
        v->GuestVmcb->Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
        v->GuestVmcb->Ctrl.CleanBits.Bits = 0; // else the vmx drops the write
    });
}

} // namespace svmb
