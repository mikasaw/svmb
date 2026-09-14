#include "mm/npf.h"
#include "core/crumbs.h"
#include "mm/tlb.h"
#include "platform/logger.h"

namespace svmb
{

namespace
{
// skeleton-rate limit: unbounded logs inside an NPF storm would livelock the
// exit path; the cap makes the loop visible without drowning the log
constexpr u32 MAX_DENY_LOGS = 64;
volatile LONG g_denyLogs = 0;

// r31: entry diagnostics - the first NPFs after a new trigger tell the whole
// story (fault type, hook ownership, slide decision); capped hard so a
// livelock cannot flood the ring
constexpr u32 MAX_NPF_DIAG = 32;
volatile LONG g_npfDiagLogs = 0;

// r31 spin breaker: a Deny decision re-executes the faulting instruction
// forever (visible policy violation). That is correct for a live policy but
// fatal for a probe: the spinning thread holds the main.cpp IO gate and every
// diagnostic IOCTL deadlocks on it, so the ring cannot even be read. Past
// the bound, advance RIP instead - the guest skips one instruction (seen as
// a probe FAIL), the gate is released, and the log tells what happened.
constexpr u32 NPF_SPIN_BREAK = 256;
volatile LONG g_npfDenySpins = 0;

// r68 hard net: the Unhandled self-heal (one-shot flush-all) trusts the
// vhv's nested shadow to honor the flush. The r57 base disease says it
// sometimes does not - a stale (shadow-)TLB entry then NPFs forever and the
// guest spins at whatever IRQL the fault came from (the r62 death form).
// After a full spin window of consecutive heals, resolve the page outright:
// one over-permissive page beats a wedged machine. The counter resets on
// any healthy lazy-map (fresh translation = the world moved on).
volatile LONG g_npfHeals = 0;

// hook slide seam (registered via NpfSetSlideHook, see npf.h)
bool (*g_slideCb)(void*, NptView&, u64, u64) = nullptr;
void* g_slideCtx = nullptr;

// r47 sentinel seam (registered via NpfSetDenyCallback, see npf.h)
bool (*g_denyCb)(void*, GuestContext&, u64, u64, u64, u64) = nullptr;
void* g_denyCtx = nullptr;
} // namespace

void NpfSetSlideHook(bool (*cb)(void* ctx, NptView& view, u64 gpa4k,
                                u64 info1),
                     void* ctx)
{
    g_slideCb = cb;
    g_slideCtx = ctx;
}

void NpfSetDenyCallback(bool (*cb)(void* ctx, GuestContext& g, u64 gpa4k,
                                   u64 info1, u64 currentCr3, u64 rip),
                        void* ctx)
{
    g_denyCb = cb;
    g_denyCtx = ctx;
}

u32 NpfHealCount()
{
    return (u32)g_npfHeals;
}

NpfDecision NpfClassify(NptView& view, PhysRanges& ranges, u64 gpa, u64 info1)
{
    NpfDecision d = {NpfAction::Unhandled, gpa};
    bool faultPresent = (info1 & NPF_PRESENT) != 0;
    bool faultWrite = (info1 & NPF_WRITE) != 0;
    bool faultExec = (info1 & NPF_EXECUTE) != 0;
    bool faultUser = (info1 & NPF_USER) != 0;

    if (!faultPresent)
    {
        // absent translation: lazily identity-map. NPT has no read-disable,
        // so an absent leaf can only be a not-present fault.
        d.Action = ranges.IsRam(gpa) ? NpfAction::MapRam : NpfAction::MapNonRam;
        return d;
    }

    // present translation: permission violation - resolve the leaf
    u64 pte = 0;
    NTSTATUS st = view.GetPte4k(gpa, pte);
    if (st == NPT_STATUS_NOT_MAPPED)
    {
        // PDE said present but the leaf does not exist (large page => GetPte
        // reports NOT_SPLIT; treat both as present-perm faults below)
        d.Action = NpfAction::Unhandled;
        return d;
    }
    if (st == NPT_STATUS_NOT_SPLIT || NT_SUCCESS(st))
    {
        if (faultWrite && NT_SUCCESS(st) && !(pte & NPT_RW))
        {
            d.Action = NpfAction::DenyWrite;
            return d;
        }
        if (faultExec && NT_SUCCESS(st) && (pte & NPT_NX))
        {
            d.Action = NpfAction::DenyExecute;
            return d;
        }
        if (faultUser && NT_SUCCESS(st) && !(pte & NPT_US))
        {
            d.Action = NpfAction::DenyUser;
            return d;
        }
        // large-page perm fault or spurious - undecided for the skeleton
    }
    return d;
}

bool HandleNpfExit(GuestContext& ctx, void* ud)
{
    NptManager* m = (NptManager*)ud;
    if (!m || !m->NptEnabled())
        return false; // NPT inactive: the exit is not ours
    NptView* view = m->Active();
    if (!view)
        return false;

    bool slideTook = false;
    CrumbPost(CRUMB_NPF_ENTER);
    // hooked pages own every present fault on their GPA: the slide engine
    // decides exec (hidden page) vs data (original page) - before the
    // generic classifier, whose DenyExecute would misread the armed NX as a
    // policy violation
    if (g_slideCb && (ctx.Info1 & NPF_PRESENT))
    {
        slideTook = g_slideCb(g_slideCtx, *view, ctx.Info2 & ~NptView::OFF_4K,
                              ctx.Info1);
        if (slideTook)
        {
            // r31b: fetch faults know their GVA (Save.Rip). INVLPGA takes a
            // GVA, not a GPA - the slide's own local flush passes the GPA
            // and invalidates nothing useful. If VMware's nested shadow
            // honors the per-line invalidation better than the flush-all
            // VMRUN control, this is what makes the flip stick.
            if (ctx.Info1 & NPF_EXECUTE)
                TlbInvlpgaLocal(ctx.Regs->Rip & ~NptView::OFF_4K);
            return true;
        }
    }

    NpfDecision d = NpfClassify(*view, m->Ranges(), ctx.Info2, ctx.Info1);

    // r47 sentinel seam: a module-owned deny consumer takes the fault
    // before the generic spin-breaker tax (records, resolves and re-arms
    // on its own schedule - zero skipped instructions, one NPF per trip)
    if (g_denyCb &&
        (d.Action == NpfAction::DenyWrite || d.Action == NpfAction::DenyExecute ||
         d.Action == NpfAction::DenyUser) &&
        g_denyCb(g_denyCtx, ctx, d.Gpa & ~NptView::OFF_4K, ctx.Info1,
                 ctx.Vmcb()->Save.Cr3, ctx.Regs->Rip))
    {
        CrumbPost(CRUMB_NPF_TAIL);
        return true;
    }

    // r31 entry diagnostics: first faults only (see MAX_NPF_DIAG)
    if (g_npfDiagLogs < MAX_NPF_DIAG)
    {
        InterlockedIncrement(&g_npfDiagLogs);
        SVMB_LOGI("npf: diag gpa=%llx info1=%llx slide=%u action=%u",
                  ctx.Info2, ctx.Info1, slideTook ? 1u : 0u, (u32)d.Action);
    }

    switch (d.Action)
    {
    case NpfAction::MapRam:
    case NpfAction::MapNonRam:
    {
        InterlockedExchange(&g_npfHeals, 0);
        // lazy identity map (RWX skeleton policy). NPF is a fault-class
        // exit: rip is not advanced, the instruction re-executes against
        // the freshly present translation.
        // r89: a speculative rate-limited ShadowKick here (lazy fill vs
        // nested-shadow staleness) was REVERTED - the readvme2e wedge root
        // cause was the MDL unlock-on-failed-probe (bugcheck 0x76), and
        // the kick correlated with mid-session instability. Lazy fill has
        // worked since r12; do not re-add without a repro.
        NptPerms rwx = {true, true, true};
        NTSTATUS st = view->MapRange(d.Gpa & ~NptView::OFF_4K, 0x1000, rwx);
        if (!NT_SUCCESS(st))
        {
            // r31: capped - this used to be an unbounded per-iteration LOGE
            // inside the exit path (log-storm livelock fuel)
            if (g_npfDiagLogs < MAX_NPF_DIAG * 2)
            {
                InterlockedIncrement(&g_npfDiagLogs);
                SVMB_LOGE("npf: lazy map failed gpa=%llx st=%08x", d.Gpa, st);
            }
            return false;
        }
        if (d.Action == NpfAction::MapNonRam && g_denyLogs < MAX_DENY_LOGS)
        {
            InterlockedIncrement(&g_denyLogs);
            SVMB_LOGW("npf: mapped non-RAM (MMIO?) gpa=%llx", d.Gpa);
        }
        return true;
    }
    case NpfAction::DenyWrite:
    case NpfAction::DenyExecute:
    case NpfAction::DenyUser:
    {
        // policy landing spot: M4 modules (cr3 read spoof, hook execute
        // routing) register here. Until then the fault stays visible (capped
        // log) and the two build policies diverge:
        //
        // RELEASE (SVMB_PRODUCTION, r36): fail fast. A VMM that cannot
        // resolve an NPF has a wrong model of the machine; skipping
        // instructions in live kernel threads is corruption (reference
        // parity: __debugbreak + KeBugCheck). MANUALLY_INITIATED_CRASH +
        // 'SVMB'+1 tag keeps postmortems distinguishable from the unload
        // guard ('SVMB').
        //
        // DEBUG: the r31 spin-breaker, r45 semantics - spin NPF_SPIN_BREAK
        // times with capped logs for observability, then RESOLVE the fault
        // (restore RWX on the page, re-execute). Skipping instructions is
        // rejected: AdvanceRip on a page-wide restriction walked execution
        // off the page into arbitrary code (r45 guest-chaos root cause).
        if (g_denyLogs < MAX_DENY_LOGS)
        {
            InterlockedIncrement(&g_denyLogs);
            SVMB_LOGW("npf: deny action=%u gpa=%llx rip=%llx cpl=%u",
                      (u32)d.Action, d.Gpa, ctx.Regs->Rip,
                      (u32)ctx.Vmcb()->Save.Cpl);
        }
        // r85 audit P1-2: the Release fail-fast (0xE2 'SVMC') was reachable
        // from DAILY guest activity, not just attacker paths - hook-page
        // denies (flip-flood / stale shadow) and any consumer-contract gap
        // (r76 proved those exist) turned into an unattended machine kill
        // (two such BSODs on record). A wrong VMM model must not cost the
        // whole machine: resolve the fault like the Debug path does (the
        // r45 semantics - nothing skipped, page re-runs for real) and log
        // loudly. The deny-consumer contract (r76) remains the first line
        // of defense; this is the last-resort net.
        LONG spins = InterlockedIncrement(&g_npfDenySpins);
        if (spins == NPF_SPIN_BREAK + 1)
        {
            SVMB_LOGE("npf: deny spin break gpa=%llx rip=%llx (resolving, "
                      "instruction re-executes)",
                      d.Gpa, ctx.Regs->Rip);
        }
        if (spins > NPF_SPIN_BREAK)
        {
            // r45: RESOLVE, do not advance. AdvanceRip here walks execution
            // through the rest of the still-restricted page one NPF per
            // byte and off the page into arbitrary code (the r45 instant
            // guest chaos behind stage-12/13). Restoring RWX on the faulting
            // page re-runs the instruction for real: nothing is skipped,
            // the probe returns, guest unharmed.
            NTSTATUS stRes =
                view->SetPerm4k(d.Gpa & ~NptView::OFF_4K, {true, true, true});
            if (NT_SUCCESS(stRes))
                TlbInvlpgaLocal(ctx.Regs->Rip & ~NptView::OFF_4K);
            // restore failure: leave the fault in place; the next NPF
            // re-enters this branch (bounded by the spin cap).
        }
        return true;
    }
    case NpfAction::Unhandled:
    default:
        // r63 FIX (r62's version used MapRange here = view spinlock above
        // DISPATCH = instant crash: spurious NPFs arrive from CLOCK2, the
        // clock ISR writing KTHREAD fields on a sentinel page - exactly
        // the r47 law). A spurious NPF is a stale (shadow-)TLB entry: the
        // tables already allow, so re-arm a ONE-SHOT full flush on the
        // re-entry VMRUN instead. Plain VMCB store - no locks, CLOCK2-safe.
        // My r61 Fix C restores TlbControl afterwards on tlbMode=1 cores.
        //
        // r68 HARD NET: if the shadow keeps serving the stale entry through
        // a full spin window of consecutive flushes (the r57 base disease -
        // this vhv's nested shadow honors TLB controls only partially), the
        // faulting thread would spin at its IRQL forever. Resolve the page
        // (no-lock PTE store, CLOCK2-legal) and keep flushing: the next
        // walk gets a real translation and the spin ends.
        if (InterlockedIncrement(&g_npfHeals) % NPF_SPIN_BREAK == 0)
        {
            NTSTATUS stRes =
                view->SetPerm4kNoLock(d.Gpa & ~NptView::OFF_4K,
                                      {true, true, true});
            SVMB_LOGE("npf: unhandled heal x%d gpa=%llx st=%08x "
                      "(resolved after flush window)",
                      (int)g_npfHeals, d.Gpa, stRes);
        }
        ctx.Vmcb()->Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
        ctx.Vmcb()->Ctrl.CleanBits.Bits = 0; // else the vmx drops the write
        return true; // instruction re-executes against a flushed TLB
    }
}

} // namespace svmb
