// svmb - TLB protocol for NPT view switches and online permission changes.
// GLUE LAYER: the only mm/ file allowed to reach into core/ - view switches
// must publish NCr3 into live VMCBs and flush translations before the guest
// can observe stale mappings.
//
// APM semantics (Vol.2 ch.15): the CPU caches nested translations per ASID;
// after NCr3 or a leaf permission change, stale entries must be invalidated
// either by TLB_CONTROL on the next VMRUN or by INVLPGA from the core that
// cached them.
#ifndef SVMB_TLB_H
#define SVMB_TLB_H

#include "platform/base.h"

namespace svmb
{

// VMCB.TLB_CONTROL encodings used by the framework
constexpr u8 TLB_CTL_DO_NOTHING = 0;  // no flush (ASID change is the flush)
constexpr u8 TLB_CTL_FLUSH_ALL = 3;   // flush entire nested TLB on next VMRUN

// ASID used by every view (single-address-space guest); changing an ASID
// implies a full flush, so all views share one
constexpr u32 NPT_ASID = 1;

// view switch: publish the new NCr3 into every live VMCB and request a full
// flush on the next VMRUN. No-op offline (hypervisor not running) - the
// bookkeeping side still updates; VMCBs pick the view up at their next
// configure (enter).
void TlbApplyViewSwitchAllCores(u64 pml4Pa);

// online leaf permission change: broadcast INVLPGA for the page (all ASIDs)
// so every core drops the stale translation. No-op offline.
NTSTATUS TlbInvlpgaPageAllCores(u64 gva);

// single-core INVLPGA for a nested-translation page (exit-path use: only the
// faulting core must drop its stale translation right now). Other cores
// converge by refaulting - a stale slide translation is benign, it keeps
// serving either the original page (data) or the hidden page (exec). No-op
// offline (INVLPGA faults as #UD outside SVM host mode).
void TlbInvlpgaLocal(u64 gpa);

// r69 freeze attack: arm a ONE-SHOT full flush on every live VMCB (plain
// control-area store, no locks - CLOCK2/DPC-legal, the r63 npf heal
// pattern) so the next VMRUN on each core drops the entire nested TLB.
// Existence proof for the r57 base disease: sentinel re-denies went
// invisible (trips froze) while INVLPGA-all-ASID broadcasts were in use -
// this is the stronger hammer, gated to tlbMode=0 whose semantics ARE
// flush-all. No-op offline and on tlbMode=1 (never-flush semantics).
void TlbKickFlushAllCores();

// r80 real-wiggle halves: phase-1 sends every core to the scratch view -
// unlike ShadowKick's back-to-back double write, the scratch NCr3 is
// actually observed at the core's next VMRUN, so the shadow genuinely
// rebuilds and a storming write lands on the identity 2M RWX view.
// phase-2 sends each core back to ITS OWN pinned view (PublishedNcr3 when
// set - heals the r77-P1-1 ProcView degradation - else the manager-active
// PML4). Plain VMCB stores, no locks, exit/DPC-context legal. No-op
// offline.
void TlbWiggleToScratch(u64 scratchPml4Pa);
void TlbWiggleRestore(u64 activePml4Pa);

} // namespace svmb

#endif // SVMB_TLB_H
