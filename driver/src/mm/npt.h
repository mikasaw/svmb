// svmb - nested page table (NPT) views: GPA->HPA tables built and maintained
// entirely offline. Activation (writing NCr3 into a VMCB) is a tier-3 step.
//
// Layout model: PML4/PDPT/PD are always 4K tables; the PD entry defaults to a
// 2M large page (PS=1) and can be split to a 4K PT per 2M region. Page tables
// are our own pool allocations, so every table PA is registered in a hash
// table with its VA (hardware reads them by PA, software writes by VA).
#ifndef SVMB_NPT_H
#define SVMB_NPT_H

#include "mm/phys_mem.h"
#include "platform/util.h"

namespace svmb
{

// NPT PTE bits (AMD APM Vol.2, nested paging)
constexpr u64 NPT_P = 1ull << 0;
constexpr u64 NPT_RW = 1ull << 1;
constexpr u64 NPT_US = 1ull << 2;
constexpr u64 NPT_A = 1ull << 5;
constexpr u64 NPT_D = 1ull << 6;
constexpr u64 NPT_PS = 1ull << 7;
constexpr u64 NPT_NX = 1ull << 63;
// 52-bit physical address space (APM max; PFN field is bits 51:12)
constexpr u64 NPT_PFN_MASK = 0x000FFFFFFFFFF000ull;

// driver-local NPT status codes (no WDK equivalents)
constexpr NTSTATUS NPT_STATUS_NOT_MAPPED = (NTSTATUS)0xC0DE0001; // no PDE/leaf present
constexpr NTSTATUS NPT_STATUS_NOT_SPLIT = (NTSTATUS)0xC0DE0002;  // 2M region still large-page

struct NptPerms
{
    bool Write;    // read is implied by P (NPT has no read-disable bit)
    bool Execute;  // false -> NX=1
    bool User;     // U/S
};

constexpr u64 NptPermBits(NptPerms p)
{
    return (p.Write ? NPT_RW : 0) | (p.User ? NPT_US : 0) | (p.Execute ? 0 : NPT_NX);
}

constexpr u64 NptPfn(u64 pa) { return pa & NPT_PFN_MASK; }
constexpr u64 NptPaFromPfn(u64 pte) { return pte & NPT_PFN_MASK; }

// one registered page-table page (hash key = PA); NextAll links all nodes of
// a view for teardown (HashTable itself has no enumeration)
struct NptTablePage : HashNode
{
    NptTablePage* NextAll;
    void* Va;
};

// a split 2M region (hash key = 2M-aligned GPA); SplitRefs supports future
// refcounted collapse - the view itself never re-collides
struct NptRegion : HashNode
{
    NptRegion* NextAll;
    volatile LONG SplitRefs;
};

class NptView
{
public:
    NptView() = default;
    ~NptView() { Deinit(); }
    NptView(const NptView&) = delete;
    NptView& operator=(const NptView&) = delete;

    NTSTATUS Init();  // allocates and zeroes the PML4
    void Deinit();    // frees every table page and bookkeeping node
    bool Inited() const { return Pml4Va_ != nullptr; }
    u64 Pml4Pa() const { return Pml4Pa_; }

    // raw mapping (no RAM check): fills [gpa, gpa+len) 4K-aligned with 2M
    // large pages where possible. Mapping never lowers an existing entry:
    // fully-covered present entries keep their perms; partially-covered
    // chunks map only not-present 4K entries (boundary chunks of a fresh PDE
    // build a split table with only the covered pages present).
    //
    // CONCURRENCY: MapRange/Split2M/SetPerm4k/SwapPage4k serialize on the
    // per-view spinlock - the IOCTL side and VM-exit handlers (NPF lazy map,
    // hook slide) can mutate one view concurrently. Reads (GetPte4k/
    // IsSplit2M) are deliberately lock-free: aligned u64 PTE reads are
    // atomic and a walk racing one store sees pre- or post-state, never a
    // torn value. RefSplit/UnrefSplit stay atomic (hash spinlock +
    // interlocked refcount). Deinit runs under the driver gate with the
    // hypervisor stopped. Exit handlers already assume IRQL <= DISPATCH
    // (spinlocked hash tables, pool allocations).
    NTSTATUS MapRange(u64 gpa, u64 len, NptPerms perm);

    // 2M large page -> 4K PT (mirrors perm bits); idempotent when already
    // split; NPT_STATUS_NOT_MAPPED when the PDE has no mapping yet
    NTSTATUS Split2M(u64 gpa);
    bool IsSplit2M(u64 gpa) const;

    // value-copy of the 4K PTE; requires the 2M region to be split
    NTSTATUS GetPte4k(u64 gpa, u64& pteOut) const;
    NTSTATUS SetPerm4k(u64 gpa, NptPerms perm);
    // r47: exit-path variant - no ViewLock (legal at ANY IRQL incl. clock
    // ISR levels where KeAcquireSpinLock would bugcheck raising DOWN to
    // DISPATCH). Caller guarantees the page tables are stable (no view
    // destroy/create in flight); single PTE store, safe for concurrent use.
    NTSTATUS SetPerm4kNoLock(u64 gpa, NptPerms perm);
    // hidden-page swap: repoint the 4K PTE at newPfn (PFN, not PA)
    NTSTATUS SwapPage4k(u64 gpa, u64 newPfn, NptPerms perm);

    // split-region bookkeeping for external users (hook manager)
    NTSTATUS RefSplit(u64 gpa);
    NTSTATUS UnrefSplit(u64 gpa);

    static constexpr u64 OFF_2M = (1ull << 21) - 1;
    static constexpr u64 OFF_4K = (1ull << 12) - 1;

private:
    static u32 Pml4Idx(u64 gpa) { return (u32)((gpa >> 39) & 0x1FF); }
    static u32 PdptIdx(u64 gpa) { return (u32)((gpa >> 30) & 0x1FF); }
    static u32 PdIdx(u64 gpa) { return (u32)((gpa >> 21) & 0x1FF); }
    static u32 PtIdx(u64 gpa) { return (u32)((gpa >> 12) & 0x1FF); }

    // pointer to the PDE for gpa; upper tables created on demand (Ensure) or
    // required present (Get)
    NTSTATUS EnsurePde(u64 gpa, u64** pdeOut);
    NTSTATUS GetPde(u64 gpa, u64** pdeOut) const;
    // pointer to the 4K PTE; requires a split region
    NTSTATUS PtePtr(u64 gpa, u64** pteOut) const;

    // Split2M body - caller must hold Lock_ (MapRange splits partial chunks
    // while already holding the view lock)
    NTSTATUS Split2MLocked(u64 gpa);

    void* VaForPa(u64 pa) const;
    NTSTATUS RegisterPage(void* va, u64 pa);

    void* Pml4Va_ = nullptr;
    u64 Pml4Pa_ = 0;
    KSPIN_LOCK Lock_;    // serializes table/PTE mutations (see MapRange note)
    HashTable Pages_;    // key = table page PA -> NptTablePage{Va}
    HashTable Regions_;  // key = 2M-aligned GPA -> NptRegion{SplitRefs}
    NptTablePage* PageList_ = nullptr;
    NptRegion* RegionList_ = nullptr;
};

// NPT view registry + activation gate (M3 skeleton).
// Views are ordinary NptViews; the manager tracks the active one and, when
// an apply callback has been registered by the live-NPT integration,
// publishes switches through it. NPT itself stays DISABLED until Enable() -
// offline the manager is pure bookkeeping (fully self-testable).
class NptManager
{
public:
    static constexpr u32 MAX_VIEWS = 8; // slot 0 = the default view

    NTSTATUS Init();  // PASSIVE_LEVEL: enumerate RAM ranges + alloc PML4
    void Deinit();
    // map every RAM range into the default view (2M pages; boundary chunks
    // map only their covered 4K pages)
    NTSTATUS FillDefaultView();
    // full-RAM RWX fill for ANY view (per-process view canvas: the process
    // view starts identical to the default; M4 policy later hides pages in
    // it). Not guarded by the default-view Filled_ flag.
    NTSTATUS FillView(NptView& v);
    // identity-map the entire 40-bit physical address space ([0, 1 TB)) as
    // 2 M RWX large pages into the default view. Matches the reference's
    // NPT init (BuildNptPageTable -> CallNptLargePage
    // Processor with end=0xFFFFFFFFFF). Without this fill, guest accesses
    // to MMIO physical addresses (APIC 0xFEE00000, IO-APIC, HPET, ...) hit
    // the NPF lazy-map path and end up mapped to a stand-in RAM frame, so
    // the OS writes never reach real hardware and the scheduler wedges.
    // The RWX skeleton policy is intentional: this fill mirrors the reference's
    // pre-hook init - any per-page policy lives in M4 hooks (NX, deny-w,
    // etc.) and re-permissions those leaves over this base. Idempotent
    // (MapRange never lowers an existing present entry).
    NTSTATUS FillIdentityWholePa();
    // same [0, RamEnd_) identity RWX fill for an ARBITRARY view (M3 view-
    // switch probe: a second view must cover MMIO too before any live core
    // can run on it)
    NTSTATUS FillIdentityView(NptView& v);
    NptView& Default() { return DefaultView_; }
    PhysRanges& Ranges() { return Ranges_; }

    // ---- M3 skeleton: view registry + activation ----
    // allocates an empty view (PML4 only); id != 0
    NTSTATUS CreateView(u32& idOut);
    NTSTATUS DestroyView(u32 id); // must not be the active view
    NptView* View(u32 id);        // null when the id is unknown/inactive slot
    // r57: resolve a view from its published PML4 PA (the per-core policy
    // stores PAs, probes want NptView*); null when no live view matches
    NptView* ViewByPml4Pa(u64 pml4Pa);
    NptView* Active() { return Active_; }

    // records the active view and, when the live-NPT integration registered
    // an apply callback (hypervisor running + NPT enabled), publishes it
    NTSTATUS SetActiveView(u32 id);

    // tier-3 gate: flips NPT on for future VMCB configures (enter/vmrun).
    // Requires an active view. OFF by default - the crash-line work stays
    // unaffected until this is explicitly called by the activation module.
    //
    // ACTIVATION PROTOCOL (must hold before Enable() is ever called live):
    //  1. publishers of activation changes (Enable/SetActiveView and any
    //     future leaf-permission change) MUST hold the main.cpp IO gate -
    //     the same serialization Start/Stop use. SANCTIONED EXCEPTIONS
    //     (exit context, no gate acquirable, memory-safe, divergence
    //     self-heals on each full republish):
    //     a. the cr3_monitor CR3-write path (per-core view publish);
    //     b. r77 ShadowKick (two broadcast-only republishes, Active_
    //        untouched, self-gated <= DISPATCH).
    //  2. publish = write Active_ (x64 aligned-pointer store: hardware
    //     atomic) -> TlbApplyViewSwitchAllCores writes every VMCB's
    //     NCr3/TlbControl + CleanBits=0 -> each core observes it at its next
    //     VMRUN; the publish completing is acked to R3 via a
    //     SvmbDbgEvtViewActivate event;
    //  3. view mutation paths (MapRange/Split2M/SetPerm4k/SwapPage4k) are
    //     serialized by the per-view spinlock - exit-context lazy maps and
    //     hook slides no longer race the IOCTL side on table structure.
    //
    // M4 PREREQUISITE: the publish model is GLOBAL (one Active_ for all
    // cores) while the CR3-write trigger is PER-CORE - a core entering the
    // watched process republishes NCr3 for every core, including ones
    // running other processes (they self-heal only on their own next
    // switch). Harmless while all views are identical; BEFORE any per-
    // process page hiding ships, switch to per-core view selection
    // (ConfigureVmcb/VMRUN picks NCr3 from that core's CR3) or equivalent.
    NTSTATUS Enable();
    void Disable() { Enabled_ = false; }
    bool NptEnabled() const { return Enabled_; }

    // r77 shadow kick: the nested shadow only rebuilds on an NCr3 VALUE
    // change (r69 law) - after a trip resolve, the faulting instruction can
    // otherwise retry thousands of times on a stale deny entry (the r76
    // 2282-exit storm). Register a scratch view (identity 2M RWX), then
    // ShadowKick() wiggles NCr3 scratch -> active via two plain broadcasts
    // so the next retry walks a freshly built shadow and lands on the
    // first try. Lockless (concurrent kickers' final broadcasts converge on
    // the same active Pml4Pa) and self-gated to <= DISPATCH_LEVEL; above
    // DISPATCH it no-ops and the r76 bounded-storm fallback rides. While
    // the wiggle NCr3 is published every core transiently runs identity
    // RWX, so per-process hidden pages are visible for the window (sensing
    // trade-off, trips are rare); ProcView-pinned cores are republished by
    // their policy on the next CR3 switch (r80 will restore per-VCPU).
    NTSTATUS SetWiggleView(u32 id);
    NTSTATUS ShadowKick();
    u32 ActiveViewId() const { return ActiveId_; }
    // r80: scratch view PML4 for the real-wiggle halves (0 = no wiggle view)
    u64 WigglePml4Pa() const
    {
        return WiggleId_ ? WigglePa_ : 0;
    }

    // live-NPT integration hook: invoked on every activation change with the
    // new active PML4 PA (null = bookkeeping only)
    void SetApplyCallback(void (*cb)(u64 pml4Pa, void* ctx), void* ctx);

private:
    PhysRanges Ranges_;
    NptView DefaultView_;
    bool Filled_ = false;

    struct ViewSlot
    {
        NptView View;
        u32 Id = 0;
        bool Used = false;
    };
    ViewSlot Slots_[MAX_VIEWS - 1]; // slots 1..MAX_VIEWS-1 (0 = default)
    u32 NextId_ = 1;
    NptView* Active_ = nullptr;
    u32 ActiveId_ = 0;
    u32 WiggleId_ = 0;
    u64 WigglePa_ = 0;
    bool Enabled_ = false;
    void (*ApplyCb_)(u64 pml4Pa, void* ctx) = nullptr;
    void* ApplyCbCtx_ = nullptr;
};

// global instance accessor (defined in main.cpp; hypervisor.cpp consults it
// when configuring VMCBs)
NptManager* NptInstance();

} // namespace svmb

#endif // SVMB_NPT_H
