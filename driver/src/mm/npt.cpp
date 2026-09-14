#include "mm/npt.h"
#include "platform/logger.h"

namespace svmb
{

namespace
{
// per-view mutation lock guard (see the CONCURRENCY note on NptView::MapRange)
struct ViewLock
{
    explicit ViewLock(KSPIN_LOCK& l) : L(l) { KeAcquireSpinLock(&L, &Old); }
    ~ViewLock() { KeReleaseSpinLock(&L, Old); }
    ViewLock(const ViewLock&) = delete;
    ViewLock& operator=(const ViewLock&) = delete;
    KSPIN_LOCK& L;
    KIRQL Old;
};
} // namespace

// ---- NptView ----------------------------------------------------------------

NTSTATUS NptView::Init()
{
    if (Pml4Va_)
        return STATUS_ALREADY_REGISTERED;

    KeInitializeSpinLock(&Lock_);
    NTSTATUS status = Pages_.Init(128, TAG_NPT);
    if (!NT_SUCCESS(status))
        return status;
    status = Regions_.Init(64, TAG_NPT);
    if (!NT_SUCCESS(status))
    {
        Pages_.Deinit();
        return status;
    }

    Pml4Va_ = AllocNptPage(Pml4Pa_);
    if (!Pml4Va_)
    {
        Pages_.Deinit();
        Regions_.Deinit();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    NTSTATUS st = RegisterPage(Pml4Va_, Pml4Pa_);
    if (!NT_SUCCESS(st))
    {
        FreeNptPage(Pml4Va_);
        Pml4Va_ = nullptr;
        Pml4Pa_ = 0;
        Pages_.Deinit();
        Regions_.Deinit();
        return st;
    }
    return STATUS_SUCCESS;
}

void* NptView::VaForPa(u64 pa) const
{
    HashNode* n = Pages_.Find(pa);
    return n ? ((NptTablePage*)n)->Va : nullptr;
}

NTSTATUS NptView::RegisterPage(void* va, u64 pa)
{
    NptTablePage* p = (NptTablePage*)AllocNonPaged(sizeof(*p), TAG_NPT);
    if (!p)
    {
        // without the VA registration the page is unreachable by software
        // walks - callers must roll the page back and fail the operation
        SVMB_LOGE("npt: table page registration OOM (pa %llx)", pa);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    p->Key = pa;
    p->Va = va;
    p->NextAll = PageList_;
    PageList_ = p;
    Pages_.Insert(p);
    return STATUS_SUCCESS;
}

void NptView::Deinit()
{
    if (Pml4Va_)
    {
        // free table pages top-down via Pages_ resolution; a page with no VA
        // registration cannot exist (every alloc goes through RegisterPage)
        for (u32 i = 0; i < 512; ++i)
        {
            u64 pml4e = ((u64*)Pml4Va_)[i];
            if (!(pml4e & NPT_P))
                continue;
            void* pdptVa = VaForPa(NptPaFromPfn(pml4e));
            if (!pdptVa)
                continue;
            for (u32 j = 0; j < 512; ++j)
            {
                u64 pdpte = ((u64*)pdptVa)[j];
                if (!(pdpte & NPT_P))
                    continue;
                void* pdVa = VaForPa(NptPaFromPfn(pdpte));
                if (!pdVa)
                    continue;
                for (u32 k = 0; k < 512; ++k)
                {
                    u64 pde = ((u64*)pdVa)[k];
                    if ((pde & NPT_P) && !(pde & NPT_PS))
                    {
                        void* ptVa = VaForPa(NptPaFromPfn(pde));
                        if (ptVa)
                            FreeNptPage(ptVa);
                    }
                }
                FreeNptPage(pdVa);
            }
            FreeNptPage(pdptVa);
        }
        FreeNptPage(Pml4Va_);
        Pml4Va_ = nullptr;
        Pml4Pa_ = 0;
    }

    // free bookkeeping nodes
    NptTablePage* p = PageList_;
    while (p)
    {
        NptTablePage* n = (NptTablePage*)p->Next;
        FreeNonPaged(p, TAG_NPT);
        p = n;
    }
    PageList_ = nullptr;
    NptRegion* r = RegionList_;
    while (r)
    {
        NptRegion* n = (NptRegion*)r->Next;
        FreeNonPaged(r, TAG_NPT);
        r = n;
    }
    RegionList_ = nullptr;

    Pages_.Deinit();
    Regions_.Deinit();
}

NTSTATUS NptView::EnsurePde(u64 gpa, u64** pdeOut)
{
    u64* pml4e = (u64*)Pml4Va_ + Pml4Idx(gpa);
    if (!(*pml4e & NPT_P))
    {
        u64 pa;
        void* va = AllocNptPage(pa);
        if (!va)
            return STATUS_INSUFFICIENT_RESOURCES;
        NTSTATUS st = RegisterPage(va, pa);
        if (!NT_SUCCESS(st))
        {
            FreeNptPage(va);
            return st;
        }
        // upper-level entries: P|RW|US always (perm enforcement lives in the
        // leaf; NPT intermediate entries carry no permission checks)
        *pml4e = NptPfn(pa) | NPT_P | NPT_RW | NPT_US | NPT_A | NPT_D;
    }
    void* pdptVa = VaForPa(NptPaFromPfn(*pml4e));
    if (!pdptVa)
        return STATUS_INVALID_PARAMETER;

    u64* pdpte = (u64*)pdptVa + PdptIdx(gpa);
    if (!(*pdpte & NPT_P))
    {
        u64 pa;
        void* va = AllocNptPage(pa);
        if (!va)
            return STATUS_INSUFFICIENT_RESOURCES;
        NTSTATUS st = RegisterPage(va, pa);
        if (!NT_SUCCESS(st))
        {
            FreeNptPage(va);
            return st;
        }
        *pdpte = NptPfn(pa) | NPT_P | NPT_RW | NPT_US | NPT_A | NPT_D;
    }
    void* pdVa = VaForPa(NptPaFromPfn(*pdpte));
    if (!pdVa)
        return STATUS_INVALID_PARAMETER;

    *pdeOut = (u64*)pdVa + PdIdx(gpa);
    return STATUS_SUCCESS;
}

NTSTATUS NptView::GetPde(u64 gpa, u64** pdeOut) const
{
    u64* pml4e = (u64*)Pml4Va_ + Pml4Idx(gpa);
    if (!(*pml4e & NPT_P))
        return NPT_STATUS_NOT_MAPPED;
    void* pdptVa = VaForPa(NptPaFromPfn(*pml4e));
    if (!pdptVa)
        return STATUS_INVALID_PARAMETER;
    u64* pdpte = (u64*)pdptVa + PdptIdx(gpa);
    if (!(*pdpte & NPT_P))
        return NPT_STATUS_NOT_MAPPED;
    void* pdVa = VaForPa(NptPaFromPfn(*pdpte));
    if (!pdVa)
        return STATUS_INVALID_PARAMETER;
    *pdeOut = (u64*)pdVa + PdIdx(gpa);
    return STATUS_SUCCESS;
}

NTSTATUS NptView::PtePtr(u64 gpa, u64** pteOut) const
{
    u64* pde = nullptr;
    NTSTATUS st = GetPde(gpa, &pde);
    if (!NT_SUCCESS(st))
        return st;
    if (!(*pde & NPT_P))
        return NPT_STATUS_NOT_MAPPED;
    if (*pde & NPT_PS)
        return NPT_STATUS_NOT_SPLIT; // caller must Split2M first
    void* ptVa = VaForPa(NptPaFromPfn(*pde));
    if (!ptVa)
        return STATUS_INVALID_PARAMETER;
    *pteOut = (u64*)ptVa + PtIdx(gpa);
    return STATUS_SUCCESS;
}

NTSTATUS NptView::MapRange(u64 gpa, u64 len, NptPerms perm)
{
    if (!Pml4Va_ || len == 0)
        return STATUS_INVALID_PARAMETER;
    if (gpa & OFF_4K)
        return STATUS_INVALID_PARAMETER; // 4K alignment required

    u64 start = gpa;
    u64 end = gpa + len;
    if (end <= start)
        return STATUS_INVALID_PARAMETER; // wrap

    ViewLock lk(Lock_);
    for (u64 chunk = start & ~OFF_2M; chunk < end; chunk += (1ull << 21))
    {
        u64* pde = nullptr;
        NTSTATUS st = EnsurePde(chunk, &pde);
        if (!NT_SUCCESS(st))
            return st;

        u64 chunkEnd = chunk + (1ull << 21);
        bool full = (chunk >= start) && (chunkEnd <= end);

        if (!(*pde & NPT_P))
        {
            if (full)
            {
                *pde = NptPfn(chunk) | NptPermBits(perm) | NPT_PS | NPT_P | NPT_A | NPT_D;
            }
            else
            {
                // boundary chunk on a fresh PDE: split table, only the
                // covered 4K pages present
                u64 pa;
                void* ptVa = AllocNptPage(pa);
                if (!ptVa)
                    return STATUS_INSUFFICIENT_RESOURCES;
                NTSTATUS stReg = RegisterPage(ptVa, pa);
                if (!NT_SUCCESS(stReg))
                {
                    FreeNptPage(ptVa);
                    return stReg;
                }
                u64 from = start > chunk ? start : chunk;
                u64 to = end < chunkEnd ? end : chunkEnd;
                for (u64 g = from & ~OFF_4K; g < to; g += 0x1000)
                    ((u64*)ptVa)[PtIdx(g)] = NptPfn(g) | NptPermBits(perm) | NPT_P | NPT_A | NPT_D;
                *pde = NptPfn(pa) | NptPermBits(perm) | NPT_P | NPT_A | NPT_D;
            }
            continue;
        }

        if (*pde & NPT_PS)
        {
            if (full)
            {
                // re-permission the large page in place (PFN unchanged)
                *pde = (*pde & ~(NPT_RW | NPT_US | NPT_NX)) | NptPermBits(perm);
            }
            else
            {
                // partial over an existing large page: split (mirrors perm),
                // then fill not-present 4K entries of the covered part.
                // Split2MLocked - the view lock is already held here.
                NTSTATUS stSplit = Split2MLocked(chunk);
                if (!NT_SUCCESS(stSplit))
                    return stSplit;
                void* ptVa = VaForPa(NptPaFromPfn(*pde));
                if (!ptVa)
                    return STATUS_INVALID_PARAMETER;
                u64 from = start > chunk ? start : chunk;
                u64 to = end < chunkEnd ? end : chunkEnd;
                for (u64 g = from & ~OFF_4K; g < to; g += 0x1000)
                {
                    u64* pte = (u64*)ptVa + PtIdx(g);
                    if (!(*pte & NPT_P))
                        *pte = NptPfn(g) | NptPermBits(perm) | NPT_P | NPT_A | NPT_D;
                }
            }
            continue;
        }

        // already split: fill not-present covered entries only
        void* ptVa = VaForPa(NptPaFromPfn(*pde));
        if (!ptVa)
            return STATUS_INVALID_PARAMETER;
        u64 from = start > chunk ? start : chunk;
        u64 to = end < chunkEnd ? end : chunkEnd;
        for (u64 g = from & ~OFF_4K; g < to; g += 0x1000)
        {
            u64* pte = (u64*)ptVa + PtIdx(g);
            if (!(*pte & NPT_P))
                *pte = NptPfn(g) | NptPermBits(perm) | NPT_P | NPT_A | NPT_D;
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS NptView::Split2M(u64 gpa)
{
    if (!Pml4Va_)
        return STATUS_INVALID_PARAMETER;
    ViewLock lk(Lock_);
    return Split2MLocked(gpa);
}

NTSTATUS NptView::Split2MLocked(u64 gpa)
{
    u64* pde = nullptr;
    NTSTATUS st = GetPde(gpa, &pde);
    if (!NT_SUCCESS(st))
        return st;
    if (!(*pde & NPT_P))
        return NPT_STATUS_NOT_MAPPED;
    if (!(*pde & NPT_PS))
        return STATUS_SUCCESS; // already split (idempotent)

    // build the 4K table mirroring the large page (perm bits carried over)
    u64 pa;
    void* ptVa = AllocNptPage(pa);
    if (!ptVa)
        return STATUS_INSUFFICIENT_RESOURCES;

    u64 basePa = NptPaFromPfn(*pde);
    u64 lowBits = *pde & ~(NPT_PFN_MASK | NPT_PS);
    for (u32 i = 0; i < 512; ++i)
        ((u64*)ptVa)[i] = NptPfn(basePa + (u64)i * 0x1000) | lowBits | NPT_P;

    NTSTATUS stReg = RegisterPage(ptVa, pa);
    if (!NT_SUCCESS(stReg))
    {
        FreeNptPage(ptVa);
        return stReg;
    }
    *pde = NptPfn(pa) | lowBits | NPT_P; // PS stays 0

    NptRegion* reg = (NptRegion*)AllocNonPaged(sizeof(*reg), TAG_NPT);
    if (reg)
    {
        reg->Key = gpa & ~OFF_2M;
        reg->SplitRefs = 1; // the view itself holds the first reference
        reg->NextAll = RegionList_;
        RegionList_ = reg;
        Regions_.Insert(reg);
    }
    return STATUS_SUCCESS;
}

bool NptView::IsSplit2M(u64 gpa) const
{
    u64* pde = nullptr;
    return NT_SUCCESS(GetPde(gpa, &pde)) && (*pde & NPT_P) && !(*pde & NPT_PS);
}

NTSTATUS NptView::GetPte4k(u64 gpa, u64& pteOut) const
{
    u64* pte = nullptr;
    NTSTATUS st = PtePtr(gpa, &pte);
    if (!NT_SUCCESS(st))
        return st;
    pteOut = *pte;
    return STATUS_SUCCESS;
}

NTSTATUS NptView::SetPerm4k(u64 gpa, NptPerms perm)
{
    if (!Pml4Va_)
        return STATUS_INVALID_PARAMETER;
    ViewLock lk(Lock_);
    u64* pte = nullptr;
    NTSTATUS st = PtePtr(gpa, &pte);
    if (!NT_SUCCESS(st))
        return st;
    *pte = (*pte & ~(NPT_RW | NPT_US | NPT_NX)) | NptPermBits(perm);
    return STATUS_SUCCESS;
}

NTSTATUS NptView::SetPerm4kNoLock(u64 gpa, NptPerms perm)
{
    // r47: see npt.h. No lock, no IRQL ceiling - the PTE walk reads tables
    // that are stable while views are not created/destroyed; the store is
    // a single aligned qword.
    if (!Pml4Va_)
        return STATUS_INVALID_PARAMETER;
    u64* pte = nullptr;
    NTSTATUS st = PtePtr(gpa, &pte);
    if (!NT_SUCCESS(st))
        return st;
    *pte = (*pte & ~(NPT_RW | NPT_US | NPT_NX)) | NptPermBits(perm);
    return STATUS_SUCCESS;
}

NTSTATUS NptView::SwapPage4k(u64 gpa, u64 newPfn, NptPerms perm)
{
    if (!Pml4Va_)
        return STATUS_INVALID_PARAMETER;
    ViewLock lk(Lock_);
    u64* pte = nullptr;
    NTSTATUS st = PtePtr(gpa, &pte);
    if (!NT_SUCCESS(st))
        return st;
    *pte = ((newPfn << 12) & NPT_PFN_MASK) | NptPermBits(perm) | NPT_P | NPT_A | NPT_D;
    return STATUS_SUCCESS;
}

NTSTATUS NptView::RefSplit(u64 gpa)
{
    HashNode* n = Regions_.Find(gpa & ~OFF_2M);
    if (!n)
        return STATUS_NOT_FOUND;
    InterlockedIncrement(&((NptRegion*)n)->SplitRefs);
    return STATUS_SUCCESS;
}

NTSTATUS NptView::UnrefSplit(u64 gpa)
{
    HashNode* n = Regions_.Find(gpa & ~OFF_2M);
    if (!n)
        return STATUS_NOT_FOUND;
    InterlockedDecrement(&((NptRegion*)n)->SplitRefs);
    return STATUS_SUCCESS;
}

// ---- NptManager --------------------------------------------------------------

NTSTATUS NptManager::Init()
{
    NTSTATUS status = Ranges_.Init();
    if (!NT_SUCCESS(status))
        return status;
    status = DefaultView_.Init();
    if (!NT_SUCCESS(status))
    {
        Ranges_.Deinit();
        return status;
    }
    return STATUS_SUCCESS;
}

void NptManager::Deinit()
{
    for (u32 i = 0; i < MAX_VIEWS - 1; ++i)
    {
        if (Slots_[i].Used)
        {
            Slots_[i].View.Deinit();
            Slots_[i].Used = false;
            Slots_[i].Id = 0;
        }
    }
    Active_ = nullptr;
    ActiveId_ = 0;
    WiggleId_ = 0;
    WigglePa_ = 0;
    Enabled_ = false;
}

NTSTATUS NptManager::FillDefaultView()
{
    if (Filled_)
        return STATUS_ALREADY_REGISTERED;
    NTSTATUS st = FillView(DefaultView_);
    if (NT_SUCCESS(st))
        Filled_ = true;
    return st;
}

NTSTATUS NptManager::FillView(NptView& v)
{
    if (!v.Inited())
        return STATUS_INVALID_PARAMETER;

    NptPerms rwx = {true, true, true};
    for (u32 i = 0; i < Ranges_.Count(); ++i)
    {
        u64 base = 0, len = 0;
        if (!Ranges_.Range(i, base, len))
            break;
        // map only the RAM-covered part of each intersecting 2M chunk;
        // MapRange handles partial chunks by building split tables whose
        // present 4K entries cover exactly the requested sub-range
        u64 end = base + len;
        for (u64 chunk = base & ~NptView::OFF_2M; chunk < end; chunk += (1ull << 21))
        {
            u64 from = base > chunk ? base : chunk;
            u64 to = end < chunk + (1ull << 21) ? end : chunk + (1ull << 21);
            NTSTATUS st = v.MapRange(from, to - from, rwx);
            if (!NT_SUCCESS(st))
                return st;
        }
    }

    // r61 MELT FIX: the RAM-only canvas left the MMIO holes (LAPIC at
    // 0xFEE00000, IO-APIC, HPET, ...) not-present. While a core ran this
    // view under the per-core policy, every interrupt-EOI write NPF'd into
    // the unhandled non-RAM path and retried forever - a single-core exit
    // flood that melts the guest (r60/r61 melts; captured live: a
    // `npf: mapped non-RAM gpa=fee000b0` storm on the target core right
    // after the first out=1 sentinel trip). The base view identity-maps
    // [0, RamEnd_) as 2M RWX (the reference semantics) exactly to keep those
    // accesses silent; every published view must carry the same coverage.
    // MapRange preserves existing finer-grained chunks: already-split 2M
    // chunks only gain identity 4K entries for their not-present holes,
    // so armed sentinel denies in this view stay intact.
    if (Ranges_.Count() != 0 && Ranges_.RamEnd() != 0)
        return v.MapRange(0, Ranges_.RamEnd(), rwx);
    return STATUS_SUCCESS;
}

// Identity-map the entire [0, RamEnd_) PA window as 2M RWX large pages,
// covering BOTH RAM regions and the MMIO holes between/around them (APIC
// at 0xFEE00000, IO-APIC, HPET, SMM, PCI ECAM, ...). Each 2M-aligned
// chunk is filled as a single large-page PDE so the table cost stays at
// 512 PML4 entries * 512 PDPT entries * one PD each - no per-leaf 4K page
// table allocations.
//
// Why this matches the reference hypervisor's working pattern
// BuildNptPageTable -> CallNptLargePageProcessor(0, 0xFFFFFFFFFF)):
//   The reference identity-maps the entire 40-bit PA space at init as 2M RWX.
//   Without that fill, any guest walk landing on a not-present NPT leaf
//   (which is every MMIO access - APIC, IO-APIC, HPET, ...) would fault
//   into VMM and the lazy-map path would either fail or absorb the write
//   into a stand-in RAM frame, breaking interrupt delivery and wedging
//   the OS scheduler. The brute-force 1 TB fill is what the reference relies on
//   to keep NPF silent during normal operation.
//
// Why we cap at RamEnd_ instead of going to 1 TB: the reference pays a 2 GB
// pool price (1 TB / 4K * sizeof(u64)) only because its FillPageFault
// path is forced into 4K leaves; the svmb MapRange large-page path keeps
// the table at ~2 MB total regardless of PA range size. Capping at
// RamEnd_ is therefore a defensive limit, not a correctness limit - any
// PA above RamEnd_ would not appear in the firmware table and the guest
// never touches it.
NTSTATUS NptManager::FillIdentityWholePa()
{
    if (!DefaultView_.Inited())
        return STATUS_INVALID_PARAMETER;
    if (Ranges_.Count() == 0 || Ranges_.RamEnd() == 0)
        return STATUS_UNSUCCESSFUL;
    return FillIdentityView(DefaultView_);
}

NTSTATUS NptManager::FillIdentityView(NptView& v)
{
    if (!v.Inited())
        return STATUS_INVALID_PARAMETER;
    if (Ranges_.Count() == 0 || Ranges_.RamEnd() == 0)
        return STATUS_UNSUCCESSFUL;

    NptPerms rwx = {true, true, true};
    u64 paEnd = Ranges_.RamEnd();
    // The whole [0, paEnd) window is one contiguous run from MapRange's
    // point of view: every 2M-aligned PDE that doesn't yet exist will be
    // created with PS=1 and identity PFN (chunk start). That is exactly
    // the reference NPT-fill behavior, just bounded by RamEnd_.
    return v.MapRange(0, paEnd, rwx);
}


// ---- M3 skeleton: view registry + activation --------------------------------

NTSTATUS NptManager::CreateView(u32& idOut)
{
    for (u32 i = 0; i < MAX_VIEWS - 1; ++i)
    {
        if (Slots_[i].Used)
            continue;
        NTSTATUS st = Slots_[i].View.Init();
        if (!NT_SUCCESS(st))
            return st;
        Slots_[i].Id = NextId_;
        Slots_[i].Used = true;
        idOut = NextId_;
        ++NextId_;
        return STATUS_SUCCESS;
    }
    return STATUS_INSUFFICIENT_RESOURCES;
}

NptView* NptManager::View(u32 id)
{
    if (id == 0)
        return &DefaultView_;
    for (u32 i = 0; i < MAX_VIEWS - 1; ++i)
        if (Slots_[i].Used && Slots_[i].Id == id)
            return &Slots_[i].View;
    return nullptr;
}

NptView* NptManager::ViewByPml4Pa(u64 pml4Pa)
{
    if (!pml4Pa)
        return nullptr;
    if (DefaultView_.Inited() && DefaultView_.Pml4Pa() == pml4Pa)
        return &DefaultView_;
    for (u32 i = 0; i < MAX_VIEWS - 1; ++i)
        if (Slots_[i].Used && Slots_[i].View.Pml4Pa() == pml4Pa)
            return &Slots_[i].View;
    return nullptr;
}

NTSTATUS NptManager::DestroyView(u32 id)
{
    if (id == 0)
        return STATUS_INVALID_PARAMETER;
    for (u32 i = 0; i < MAX_VIEWS - 1; ++i)
    {
        if (Slots_[i].Used && Slots_[i].Id == id)
        {
            if (Active_ == &Slots_[i].View)
                return STATUS_INVALID_PARAMETER; // switch away first
            Slots_[i].View.Deinit();
            Slots_[i].Used = false;
            Slots_[i].Id = 0;
            // r80: a destroyed view must never serve as the wiggle target
            // (a stale PML4 PA could be pool-reused)
            if (WiggleId_ == id)
            {
                WiggleId_ = 0;
                WigglePa_ = 0;
            }
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

NTSTATUS NptManager::SetActiveView(u32 id)
{
    NptView* v = View(id);
    if (!v || !v->Inited())
        return STATUS_NOT_FOUND;
    Active_ = v;
    ActiveId_ = id;
    if (ApplyCb_ && Enabled_)
        ApplyCb_(v->Pml4Pa(), ApplyCbCtx_);
    return STATUS_SUCCESS;
}

NTSTATUS NptManager::SetWiggleView(u32 id)
{
    NptView* v = View(id);
    if (!v || !v->Inited())
        return STATUS_NOT_FOUND;
    WiggleId_ = id;
    WigglePa_ = v->Pml4Pa();
    return STATUS_SUCCESS;
}

// r77 shadow kick: the vhv nested shadow ignores TlbControl and only
// rebuilds on an NCr3 VALUE change (r69 law). After a trip resolve the
// faulting instruction can otherwise retry thousands of times on the
// stale deny entry (the r76 2282-exit storm); wiggling NCr3 scratch ->
// active forces the rebuild so the retry lands on the first try.
//
// Exit-context safety (acceptance r77):
//  - IRQL gate: callers run at ANY IRQL on the NPF path (CLOCK2 clock-tick
//    writes included). ApplyCb pushes a dbg-ring event whose Push takes a
//    spinlock - legal only at <= DISPATCH. Above DISPATCH we skip the kick
//    and let the r76 bounded-storm fallback ride (DPC re-arm still makes
//    progress).
//  - NO lock and NO Active_ mutation: two concurrent kickers interleave
//    only their two broadcasts and both phase-2 writes restore the same
//    active Pml4Pa, so the final NCr3 always converges on the active view.
//    Racing a sanctioned SetActiveView publisher leaves the transient
//    divergence the activation protocol already tolerates (self-heals on
//    the next publish). Not touching Active_ also keeps DestroyView's
//    active-view guard meaningful during the wiggle window.
//  - r53 ProcView interaction: cores the per-core policy pinned to the
//    ProcView NCr3 get published back to the manager-active view; the
//    policy republishes on the target's next CR3 switch. ProcessView x
//    region combos degrade transiently (documented, r80 will restore
//    per-VCPU).
NTSTATUS NptManager::ShadowKick()
{
    if (!Enabled_ || !ApplyCb_ || !WiggleId_)
        return STATUS_DEVICE_NOT_READY;
    if (KeGetCurrentIrql() > DISPATCH_LEVEL)
        return STATUS_DEVICE_NOT_READY; // ring Push + broadcast need <=DISPATCH
    NptView* w = View(WiggleId_);
    if (!w || !w->Inited())
        return STATUS_DEVICE_NOT_READY;
    NptView* back = Active();
    if (!back)
        back = &DefaultView_;
    ApplyCb_(w->Pml4Pa(), ApplyCbCtx_);
    ApplyCb_(back->Pml4Pa(), ApplyCbCtx_);
    return STATUS_SUCCESS;
}

NTSTATUS NptManager::Enable()
{
    if (!Active_)
        return STATUS_DEVICE_NOT_READY; // pick a view first
    Enabled_ = true;
    if (ApplyCb_)
        ApplyCb_(Active_->Pml4Pa(), ApplyCbCtx_);
    return STATUS_SUCCESS;
}

void NptManager::SetApplyCallback(void (*cb)(u64 pml4Pa, void* ctx), void* ctx)
{
    ApplyCb_ = cb;
    ApplyCbCtx_ = ctx;
}
} // namespace svmb
