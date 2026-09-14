#include "mm/offline_tests.h"
#include "mm/insn_len.h"
#include "mm/npf.h"
#include "mm/npt.h"
#include "modules/npt_hook_mgr.h"
#include "modules/dbg_events.h"
#include "modules/debugger.h"
#include "modules/cr3_monitor.h"
#include "modules/cr3_seed.h"
#include "modules/mem_read.h"
#include "modules/stealth.h"
#include "modules/sysaudit.h"
#include "core/regs.h"
#include "hw/msrpm.h"
#include "hw/svm_defs.h"
#include "mm/phys_mem.h"
#include "platform/logger.h"

namespace svmb
{

namespace
{

struct StCtx
{
    u32 Total = 0;
    u32 Failed = 0;
    char LastFail[96] = {};
};

void Check(StCtx& c, bool ok, const char* name)
{
    ++c.Total;
    if (!ok)
    {
        ++c.Failed;
        if (c.Failed == 1)
            strncpy_s(c.LastFail, name, _TRUNCATE);
        // r35: log every failure (suites cap low; the ring wraps anyway) -
        // "6 failed, first=..." alone forced a VM round trip to see the rest
        SVMB_LOGE("selftest FAIL(%u): %s", c.Failed, name);
    }
}

void TestPhysRanges(StCtx& c)
{
    PhysRanges r;
    Check(c, NT_SUCCESS(r.Init()), "physmem.init");
    Check(c, r.Count() > 0, "physmem.count>0");
    Check(c, r.RamEnd() > (1ull << 30), "physmem.ramend>1GB");
    Check(c, r.IsRam(0x1000), "physmem.isram(0x1000)");
    Check(c, r.IsRam(r.RamEnd() - 1), "physmem.isram(last)");
    Check(c, !r.IsRam(r.RamEnd()), "physmem.isram(ramend)=false");
    u64 base = 0, len = 0;
    Check(c, r.Range(0, base, len) && len > 0, "physmem.range(0)");
    Check(c, !r.Range(r.Count(), base, len), "physmem.range(oob)");
    r.Deinit();
}

void TestNptView(StCtx& c)
{
    NptView v;
    Check(c, NT_SUCCESS(v.Init()), "npt.init");

    // 1. map 4 consecutive 2M chunks at 64MB as large pages
    constexpr u64 BASE = 0x4000000; // 64MB (below any sane RamEnd? use RAM-agnostic raw map)
    constexpr u64 CH = 1ull << 21;
    NptPerms rwx = {true, true, true};
    Check(c, NT_SUCCESS(v.MapRange(BASE, 4 * CH, rwx)), "npt.map4x2m");

    u64 pde = 0;
    {
        u64* pdePtr = nullptr;
        // reach the PDE through GetPte4k's NOT_SPLIT to prove large-page form
        u64 pte = 0;
        NTSTATUS st = v.GetPte4k(BASE, pte);
        Check(c, st == NPT_STATUS_NOT_SPLIT, "npt.map4x2m.large(pde.ps)");
        (void)pdePtr;
        (void)pde;
    }

    // 2. split the first chunk; 4K walk must resolve identity PFNs
    Check(c, NT_SUCCESS(v.Split2M(BASE)), "npt.split2m");
    Check(c, v.IsSplit2M(BASE), "npt.isSplit(true)");
    Check(c, !v.IsSplit2M(BASE + CH), "npt.isSplit(second chunk still large)");
    NTSTATUS again = v.Split2M(BASE);
    Check(c, again == STATUS_SUCCESS, "npt.split2m.idempotent");

    u64 pte = 0;
    Check(c, NT_SUCCESS(v.GetPte4k(BASE + 0x1234, pte)), "npt.getpte");
    // r36: a PTE stores its 4K page BASE - the intra-page offset (0x234)
    // lives in the VA and can never appear in the PTE. Compare page bases.
    Check(c, NptPaFromPfn(pte) == ((BASE + 0x1234) & ~0xFFFull),
          "npt.pte.identity-pfn");
    Check(c, (pte & NPT_RW) && !(pte & NPT_NX), "npt.pte.rwx bits");

    u64 miss = 0;
    Check(c, v.GetPte4k(BASE + CH + 0x1000, miss) == NPT_STATUS_NOT_SPLIT,
          "npt.getpte(second chunk NOT_SPLIT)");

    // 3. hidden-page swap: point the page at an alien PFN, then back
    u64 origPfn = NptPaFromPfn(pte) >> 12;
    NptPerms rx = {false, true, true};
    Check(c, NT_SUCCESS(v.SwapPage4k(BASE + 0x1000, 0x99999, rx)), "npt.swap");
    Check(c, NT_SUCCESS(v.GetPte4k(BASE + 0x1000, pte)), "npt.swap.getpte");
    // r36: 0x99999 << 12 as int overflows (0x99999000 > INT_MAX) and the
    // negative int sign-extends against the u64 PTE - always unequal. Keep
    // the shift in u64.
    Check(c, NptPaFromPfn(pte) == (0x99999ull << 12), "npt.swap.pfn");
    Check(c, !(pte & NPT_RW) && !(pte & NPT_NX), "npt.swap.rx bits");
    Check(c, NT_SUCCESS(v.SwapPage4k(BASE + 0x1000, origPfn, rwx)), "npt.swap.restore");

    // 4. partial boundary chunk: fresh PDE with only covered 4K present
    constexpr u64 BOUND = BASE + 4 * CH - 0x1000; // 4K before the chunk edge
    Check(c, NT_SUCCESS(v.MapRange(BOUND, 0x2000, rwx)), "npt.boundary.map");
    Check(c, NT_SUCCESS(v.GetPte4k(BOUND, pte)), "npt.boundary.first");
    Check(c, NptPaFromPfn(pte) == BOUND, "npt.boundary.pfn");
    Check(c, v.GetPte4k(BOUND + 0x2000, pte) == NPT_STATUS_NOT_SPLIT ||
                 !(pte & NPT_P),
          "npt.boundary.outside-absent");
    // second 4K inside the crossed chunk boundary
    Check(c, NT_SUCCESS(v.GetPte4k(BOUND + 0x1000, pte)), "npt.boundary.cross");
    Check(c, NptPaFromPfn(pte) == (BOUND + 0x1000), "npt.boundary.cross.pfn");

    // 5. perms: RO page rejects the write bit. NptPerms field order is
    // {Write, Execute, User} - read-only = no RW, NX on (r36: the old
    // initializer {false,true,false} spelled an EXECUTABLE page under this
    // order and npt.ro.bits could never pass).
    NptPerms ro = {false, false, true};
    Check(c, NT_SUCCESS(v.MapRange(BASE + 8 * CH, CH, ro)), "npt.ro.map");
    // a full-chunk map stays a 2M large page: the 4K walk must report
    // NOT_SPLIT until the region is split (r36: the old test asserted
    // NT_SUCCESS here, which the documented large-page behavior denies)
    Check(c, v.GetPte4k(BASE + 8 * CH, pte) == NPT_STATUS_NOT_SPLIT,
          "npt.ro.large-presplit");
    Check(c, NT_SUCCESS(v.Split2M(BASE + 8 * CH)), "npt.ro.split");
    Check(c, NT_SUCCESS(v.GetPte4k(BASE + 8 * CH, pte)), "npt.ro.getpte");
    // 8*CH chunk fresh -> partial? full chunk -> large; split to inspect
    Check(c, NT_SUCCESS(v.Split2M(BASE + 8 * CH)), "npt.ro.split");
    Check(c, NT_SUCCESS(v.GetPte4k(BASE + 8 * CH, pte)), "npt.ro.getpte2");
    Check(c, (pte & NPT_P) && !(pte & NPT_RW) && (pte & NPT_NX), "npt.ro.bits");

    // 5b. negatives: Split2M on unmapped, swap on still-large page
    Check(c, v.Split2M(BASE + 9 * CH) == NPT_STATUS_NOT_MAPPED,
          "npt.split2m.unmapped");
    Check(c, v.SwapPage4k(BASE + CH + 0x40, 0x99998, rwx) ==
                 NPT_STATUS_NOT_SPLIT,
          "npt.swap.large-rejected");

    // 6. split bookkeeping
    Check(c, NT_SUCCESS(v.RefSplit(BASE)), "npt.refsplit");
    Check(c, NT_SUCCESS(v.UnrefSplit(BASE)), "npt.unrefsplit");
    Check(c, v.UnrefSplit(BASE + 6 * CH) == STATUS_NOT_FOUND, "npt.unrefsplit(absent)");

    v.Deinit();
    Check(c, !v.Inited(), "npt.deinit");
}

void TestNptManager(StCtx& c)
{
    NptManager m;
    Check(c, NT_SUCCESS(m.Init()), "nptmgr.init");
    NTSTATUS st = m.FillDefaultView();
    Check(c, NT_SUCCESS(st), "nptmgr.fill");

    // every RAM-covered 2M chunk boundary must resolve
    u64 probe = 0x100000; // 1MB, inside the first RAM range on all systems
    if (m.Ranges().IsRam(probe))
    {
        u64 pte = 0;
        NTSTATUS st2 = m.Default().GetPte4k(probe, pte);
        Check(c, NT_SUCCESS(st2), "nptmgr.default.walk(1MB)");
        if (NT_SUCCESS(st2))
            Check(c, NptPaFromPfn(pte) == probe, "nptmgr.default.identity(1MB)");
    }

    // hook round-trip on our own code page (offline: tables only, the page
    // is never executed through the NPT during the test)
    NptHookManager hooks;
    Check(c, NT_SUCCESS(hooks.Init(&m)), "hook.init");
    u64 target = (u64)&TestNptManager;
    u64 pageVa = target & ~0xFFFull;
    PHYSICAL_ADDRESS tpa = MmGetPhysicalAddress((void*)pageVa);
    u64 pte = 0;
    Check(c, NT_SUCCESS(m.Default().GetPte4k((u64)tpa.QuadPart, pte)) ||
                 m.Default().GetPte4k((u64)tpa.QuadPart, pte) ==
                     NPT_STATUS_NOT_SPLIT,
          "hook.presplit-state");
    Check(c, NT_SUCCESS(hooks.Install(target, target + 0x20,
                                      NptHookManager::MODE_JMP)),
          "hook.install");
    Check(c, NT_SUCCESS(m.Default().GetPte4k((u64)tpa.QuadPart, pte)),
          "hook.pte");
    Check(c, NptPaFromPfn(pte) != (u64)tpa.QuadPart, "hook.pte.hidden-pfn");
    Check(c, hooks.Install(target, target + 0x20,
                           NptHookManager::MODE_JMP) == NPT_HOOK_STATUS_OVERLAP,
          "hook.duplicate-page-rejected");
    Check(c, NT_SUCCESS(hooks.Remove(target)), "hook.remove");
    Check(c, NT_SUCCESS(m.Default().GetPte4k((u64)tpa.QuadPart, pte)) &&
                 NptPaFromPfn(pte) == (u64)tpa.QuadPart,
          "hook.remove.restored");
    hooks.Deinit();

    m.Deinit();
}

} // namespace


void TestInsnLen(StCtx& c)
{
    struct Sample
    {
        const u8* Code;
        u32 Len;
        const char* Name;
    };
#pragma pack(push, 1)
    struct
    {
        u8 mov_eax1[5];
        u8 movabs[10];
        u8 mov_rsp[5];
        u8 sub_rsp[4];
        u8 lea_rsp[5];
        u8 call_rel32[5];
        u8 endbr[4];
        u8 mov_riprel[7];
        u8 cc[1];
        u8 push_rbp[1];
        u8 nop[1];
        u8 sub_rsp32[7];   // 48 81 EC 28 00 00 00
        u8 mov_mem0[8];    // 48 C7 45 F8 00 00 00 00
        u8 jmp_rel8[2];
        u8 jcc_rel8[2];    // 74 11
        u8 jcc_rel32[6];   // 0F 84 11 22 33 44
        u8 ret_insn[1];    // C3
    } s = {
        {0xB8, 0x01, 0x00, 0x00, 0x00},
        {0x48, 0xB8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
        {0x48, 0x89, 0x5C, 0x24, 0x08},
        {0x48, 0x83, 0xEC, 0x28},
        {0x48, 0x8D, 0x64, 0x24, 0x10},
        {0xE8, 0x11, 0x22, 0x33, 0x44},
        {0xF3, 0x0F, 0x1E, 0xFA},
        {0x48, 0x8B, 0x05, 0x11, 0x22, 0x33, 0x44},
        {0xCC},
        {0x55},
        {0x90},
        {0x48, 0x81, 0xEC, 0x28, 0x00, 0x00, 0x00},
        {0x48, 0xC7, 0x45, 0xF8, 0x00, 0x00, 0x00, 0x00},
        {0xEB, 0x0E},
        {0x74, 0x11},
        {0x0F, 0x84, 0x11, 0x22, 0x33, 0x44},
        {0xC3},
    };
#pragma pack(pop)

    Check(c, InsnLen64(s.mov_eax1, 5) == 5, "insn.mov_eax_imm32");
    Check(c, InsnLen64(s.movabs, 10) == 10, "insn.movabs");
    Check(c, InsnLen64(s.mov_rsp, 5) == 5, "insn.mov_rm64");
    Check(c, InsnLen64(s.sub_rsp, 4) == 4, "insn.sub_rsp_imm8");
    Check(c, InsnLen64(s.lea_rsp, 5) == 5, "insn.lea_sib");
    Check(c, InsnLen64(s.call_rel32, 5) == 5, "insn.call_rel32");
    Check(c, InsnLen64(s.endbr, 4) == 4, "insn.endbr64");
    u32 fl = 0;
    Check(c, InsnLen64(s.mov_riprel, 7, &fl) == 7 && (fl & INSN_F_RIPREL),
          "insn.mov_riprel_flagged");
    Check(c, InsnLen64(s.cc, 1) == 1, "insn.int3");
    Check(c, InsnLen64(s.push_rbp, 1) == 1, "insn.push_rbp");
    Check(c, InsnLen64(s.nop, 1) == 1, "insn.nop");
    Check(c, InsnLen64(s.sub_rsp32, 7) == 7, "insn.sub_rsp_imm32_rexw");
    Check(c, InsnLen64(s.mov_mem0, 8) == 8, "insn.mov_rm_imm32_rexw");
    Check(c, InsnLen64(s.jmp_rel8, 2) == 2, "insn.jmp_rel8");
    // r35: control transfers must be flagged (StealLen rejects them) - their
    // PC-relative displacement breaks when re-executed from a trampoline
    fl = 0;
    Check(c, InsnLen64(s.call_rel32, 5, &fl) == 5 && (fl & INSN_F_CTLX),
          "insn.call_rel32_ctlx");
    fl = 0;
    Check(c, InsnLen64(s.jmp_rel8, 2, &fl) == 2 && (fl & INSN_F_CTLX),
          "insn.jmp_rel8_ctlx");
    fl = 0;
    Check(c, InsnLen64(s.jcc_rel8, 2, &fl) == 2 && (fl & INSN_F_CTLX),
          "insn.jcc_rel8_ctlx");
    fl = 0;
    Check(c, InsnLen64(s.jcc_rel32, 6, &fl) == 6 && (fl & INSN_F_CTLX),
          "insn.jcc_rel32_ctlx");
    fl = 0;
    Check(c, InsnLen64(s.ret_insn, 1, &fl) == 1 && (fl & INSN_F_CTLX),
          "insn.ret_ctlx");
    // prefix pile-up must not exceed 15 or loop forever
    u8 many[15] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66,
                   0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90};
    Check(c, InsnLen64(many, 15) > 0, "insn.prefix_pileup");
}


void TestMsrpmShadow(StCtx& c)
{
    Msrpm m;
    Check(c, NT_SUCCESS(m.Init()), "msrpm.init");
    u64 pa0 = m.PhysicalAddress();
    bool r = false, w = false;

    // direct write targets the active page before any build
    Check(c, m.SetIntercept(MSR_VM_HSAVE_PA, true, true, true), "msrpm.set");
    Check(c, m.GetIntercept(MSR_VM_HSAVE_PA, r, w) && r && w, "msrpm.get");

    // build into the shadow, commit, and observe the page rotation
    m.BeginBuild();
    Check(c, m.SetIntercept(MSR_EFER, true, true, true), "msrpm.build.set");
    m.CommitBuild();
    Check(c, m.PhysicalAddress() != pa0, "msrpm.commit.page-flipped");
    Check(c, m.GetIntercept(MSR_VM_HSAVE_PA, r, w) && !r && !w,
          "msrpm.commit.old-bits-gone");
    Check(c, m.GetIntercept(MSR_EFER, r, w) && r && w, "msrpm.commit.new-bits");

    // empty rebuild clears everything
    m.BeginBuild();
    m.CommitBuild();
    Check(c, m.GetIntercept(MSR_EFER, r, w) && !r && !w, "msrpm.rebuild-clears");

    // rotation wraps after three commits: a page retired at commit K comes
    // back as shadow (and gets cleared) only at commit K+2, as active at
    // K+3 - two full cycles of grace (the bound documented in msrpm.h)
    m.BeginBuild();
    m.CommitBuild();
    Check(c, m.PhysicalAddress() == pa0, "msrpm.rotation.wraps");
    Check(c, m.GetIntercept(MSR_EFER, r, w) && !r && !w,
          "msrpm.rotation.cleared");

    // direct writes during an open build must not touch the active page
    m.BeginBuild();
    Check(c, m.SetIntercept(MSR_VM_HSAVE_PA, true, true, true),
          "msrpm.build2.set");
    Check(c, m.GetIntercept(MSR_VM_HSAVE_PA, r, w) && !r && !w,
          "msrpm.build2.active-clean");
    m.CommitBuild();
    Check(c, m.GetIntercept(MSR_VM_HSAVE_PA, r, w) && r && w,
          "msrpm.build2.published");
    m.Deinit();
}

void TestDbgRing(StCtx& c)
{
    svmb::DbgEventRing ring; // namespace not opened for this name
    Check(c, NT_SUCCESS(ring.Init()), "ring.init");
    SVMB_DBG_EVENT e = {};
    e.Type = SvmbDbgEvtBreakpoint;
    e.Rip = 0x1234;
    for (u32 i = 0; i < SVMB_DBG_MAX_EVENTS + 8; ++i)
    {
        e.Extra = i;
        ring.Push(e);
    }
    Check(c, ring.Count() == SVMB_DBG_MAX_EVENTS, "ring.full");
    Check(c, ring.LostCount() == 8, "ring.lost-8");

    SVMB_DBG_EVENT_BUFFER buf = {};
    Check(c, ring.Drain(&buf) == SVMB_DBG_MAX_EVENTS, "ring.drain-all");
    Check(c, buf.Count == SVMB_DBG_MAX_EVENTS, "ring.drain-count");
    Check(c, buf.Events[0].Extra == 0 && buf.Events[63].Extra == 63,
          "ring.fifo-order");
    Check(c, ring.Count() == 0, "ring.empty-after-drain");

    e.Extra = 0xABCD;
    ring.Push(e);
    Check(c, ring.Drain(&buf) == 1 && buf.Events[0].Extra == 0xABCD,
          "ring.push-after-drain");
    // second full cycle forces the Head_ index to wrap
    for (u32 i = 0; i < SVMB_DBG_MAX_EVENTS; ++i)
    {
        e.Extra = 0x10000 + i;
        ring.Push(e);
    }
    Check(c, ring.Drain(&buf) == SVMB_DBG_MAX_EVENTS, "ring.wrap.drain");
    Check(c, buf.Events[0].Extra == 0x10000 &&
                 buf.Events[SVMB_DBG_MAX_EVENTS - 1].Extra ==
                     0x10000 + SVMB_DBG_MAX_EVENTS - 1,
          "ring.wrap.order");
    ring.Deinit();
}


void TestNptMultiView(StCtx& c)
{
    NptManager m;
    Check(c, NT_SUCCESS(m.Init()), "nptmv.init");
    Check(c, NT_SUCCESS(m.FillDefaultView()), "nptmv.fill");

    u32 id = 0;
    Check(c, NT_SUCCESS(m.CreateView(id)) && id != 0, "nptmv.create");
    Check(c, m.View(0)->Pml4Pa() != m.View(id)->Pml4Pa(), "nptmv.distinct-pml4");

    NptPerms rwx = {true, true, true};
    u64 gpa = 0x20000000; // 512MB
    Check(c, NT_SUCCESS(m.View(id)->MapRange(gpa, 0x1000, rwx)), "nptmv.map");
    u64 pte = 0;
    Check(c, m.View(id)->GetPte4k(gpa, pte) == STATUS_SUCCESS, "nptmv.view-walk");
    Check(c, m.Default().GetPte4k(gpa, pte) != STATUS_SUCCESS,
          "nptmv.isolation");

    // callback connectivity: pre-Enable SetActiveView must NOT publish
    struct CbCtx
    {
        u32 Calls;
        u64 Pa;
    } cb = {};
    m.SetApplyCallback([](u64 pml4Pa, void* ctx) {
        static_cast<CbCtx*>(ctx)->Calls++;
        static_cast<CbCtx*>(ctx)->Pa = pml4Pa;
    }, &cb);

    Check(c, NT_SUCCESS(m.SetActiveView(id)) && m.Active() == m.View(id),
          "nptmv.setactive");
    Check(c, cb.Calls == 0, "nptmv.callback-gated-by-enable");
    Check(c, m.NptEnabled() == false, "nptmv.disabled-by-default");
    Check(c, NT_SUCCESS(m.Enable()), "nptmv.enable");
    Check(c, m.NptEnabled() && m.Active()->Pml4Pa() != 0, "nptmv.enabled");

    Check(c, m.DestroyView(id) == STATUS_INVALID_PARAMETER,
          "nptmv.destroy-active-rejected");
    Check(c, NT_SUCCESS(m.SetActiveView(0)) && m.Active() == m.View(0),
          "nptmv.switch-back");
    // Enable() fired the callback once (Calls=1), the switch back to the
    // default view fires it again (Calls=2)
    Check(c, cb.Calls == 2 && cb.Pa == m.View(0)->Pml4Pa(),
          "nptmv.callback-fires-with-pa");
    Check(c, NT_SUCCESS(m.DestroyView(id)), "nptmv.destroy");
    Check(c, m.View(id) == nullptr, "nptmv.destroyed-gone");

    // review-driven negative: Enable without an active view (fresh manager,
    // never SetActiveView'd) must be declined
    {
        NptManager fresh;
        Check(c, NT_SUCCESS(fresh.Init()), "nptmv.fresh.init");
        Check(c, fresh.Enable() == STATUS_DEVICE_NOT_READY,
              "nptmv.enable-without-active");
        fresh.Deinit();
    }
    Check(c, m.SetActiveView(9999) == STATUS_NOT_FOUND,
          "nptmv.setactive-unknown");
    // slot exhaustion: MAX_VIEWS-1 extra views fit, the next one fails
    u32 created = 0;
    for (u32 i = 0; i < NptManager::MAX_VIEWS; ++i)
    {
        u32 nid = 0;
        NTSTATUS st = m.CreateView(nid);
        if (NT_SUCCESS(st))
        {
            ++created;
        }
        else
        {
            Check(c, st == STATUS_INSUFFICIENT_RESOURCES, "nptmv.slots.fail");
            break;
        }
    }
    Check(c, created == NptManager::MAX_VIEWS - 1, // r36: the slot array is
          // MAX_VIEWS-1 entries (the default view lives outside it) and the
          // destroyed view freed its slot - full capacity is creatable
          "nptmv.slots.exhausted");
    m.Deinit();
}

void TestNpfClassify(StCtx& c)
{
    NptManager m;
    Check(c, NT_SUCCESS(m.Init()), "npf.init");
    Check(c, NT_SUCCESS(m.FillDefaultView()), "npf.fill");

    // classification targets a FRESH created view: the default view is
    // pre-filled with RWX large pages over RAM, and MapRange never lowers an
    // existing entry, so an RO map there would be silently dropped
    u32 id = 0;
    Check(c, NT_SUCCESS(m.CreateView(id)), "npf.view");
    NptView& v = *m.View(id);

    NptPerms rwx = {true, true, true};
    u64 ro = 0x8000000; // 128MB
    Check(c, NT_SUCCESS(v.MapRange(ro, 0x1000, {false, true, true})),
          "npf.ro-map");

    // absent + RAM -> lazy identity map
    NpfDecision d = NpfClassify(v, m.Ranges(), 0x1000, 0);
    Check(c, d.Action == NpfAction::MapRam, "npf.absent-ram");
    // absent + far outside RAM -> MMIO bucket
    d = NpfClassify(v, m.Ranges(), 0xFFFFFF000ull, 0);
    Check(c, d.Action == NpfAction::MapNonRam, "npf.absent-nonram");
    // present + write fault on RO leaf -> DenyWrite (deterministic on a
    // fresh view: the RO mapping cannot be silently dropped)
    d = NpfClassify(v, m.Ranges(), ro, NPF_PRESENT | NPF_WRITE);
    Check(c, d.Action == NpfAction::DenyWrite, "npf.denywrite");
    // present + exec fault (leaf is RW non-NX) -> Unhandled (spurious)
    d = NpfClassify(v, m.Ranges(), ro, NPF_PRESENT | NPF_EXECUTE);
    Check(c, d.Action == NpfAction::Unhandled, "npf.spurious");
    // large-page perm fault: leaf walk reports NOT_SPLIT -> Unhandled
    u64 big = ro + (1ull << 21);
    Check(c, NT_SUCCESS(v.MapRange(big, 1ull << 21, rwx)), "npf.large-map");
    d = NpfClassify(v, m.Ranges(), big + 0x40, NPF_PRESENT | NPF_WRITE);
    Check(c, d.Action == NpfAction::Unhandled, "npf.large-notsplit");

    // U/S: map a supervisor (User=false) RO+X page; a user-mode fault on a
    // present supervisor leaf classifies as DenyUser
    u64 sup = 0x10000000; // 256MB
    Check(c, NT_SUCCESS(v.MapRange(sup, 0x1000, {false, true, false})),
          "npf.sup-map");
    d = NpfClassify(v, m.Ranges(), sup, NPF_PRESENT | NPF_USER);
    Check(c, d.Action == NpfAction::DenyUser, "npf.user-vs-supervisor");
    // same leaf, supervisor access without W/X violation bits -> spurious
    d = NpfClassify(v, m.Ranges(), sup, NPF_PRESENT);
    Check(c, d.Action == NpfAction::Unhandled, "npf.supervisor-spurious");
    // user write on the supervisor page: DenyWrite wins (write is checked
    // before U/S - the stronger, more specific violation)
    d = NpfClassify(v, m.Ranges(), sup, NPF_PRESENT | NPF_WRITE | NPF_USER);
    Check(c, d.Action == NpfAction::DenyWrite, "npf.write-beats-user");
    // user fault on a USER leaf (no U/S violation) -> falls through to
    // Unhandled; the ro page is User=true so it must not deny
    d = NpfClassify(v, m.Ranges(), ro, NPF_PRESENT | NPF_USER);
    Check(c, d.Action == NpfAction::Unhandled, "npf.user-on-user-spurious");

    // gate/decline contract: HandleNpfExit declines while NPT is disabled
    // (early-exit path - the dummy context is never dereferenced)
    GuestContext dummy = {};
    Check(c, HandleNpfExit(dummy, &m) == false, "npf.decline-disabled");

    m.Deinit();
}

void TestGuestGpr(StCtx& c)
{
    GuestRegs r = {};
    for (u32 i = 0; i < 16; ++i)
    {
        GuestGpr(&r, i) = 0xA500 + i;
    }
    Check(c, r.Rax == 0xA500 && r.Rcx == 0xA501 && r.Rdx == 0xA502 &&
                 r.Rbx == 0xA503,
          "gpr.low-named");
    Check(c, r.Rsp == 0xA504 && r.Rbp == 0xA505 && r.Rsi == 0xA506 &&
                 r.Rdi == 0xA507,
          "gpr.mid-named");
    Check(c, r.R8 == 0xA508 && r.R9 == 0xA509 && r.R10 == 0xA50A &&
                 r.R11 == 0xA50B,
          "gpr.hi-0-3");
    Check(c, r.R12 == 0xA50C && r.R13 == 0xA50D && r.R14 == 0xA50E &&
                 r.R15 == 0xA50F,
          "gpr.hi-4-7-contiguous");
    // AMD GPR encoding is a 4-bit field: index 16 aliases index 0
    Check(c, &GuestGpr(&r, 0) == &GuestGpr(&r, 16), "gpr.idx-masked");
}

void TestDebuggerPure(StCtx& c)
{
    DrShadow s = {};
    s.Dr[2] = 0x2222;
    s.Dr6 = 0x6666;
    s.Dr7 = 0x7777;

    Check(c, DrResolveRead(7, s, true, false, 0, 0xDEAD) == 0x7777,
          "dr7.hide-shadow");
    Check(c, DrResolveRead(7, s, true, true, 0, 0xDEAD) == 0,
          "dr7.spoof-beats-hide");
    Check(c, DrResolveRead(7, s, false, true, 0, 0xDEAD) == 0,
          "dr7.spoof-real-pass");
    Check(c, DrResolveRead(7, s, false, false, 0, 0xDEAD) == 0xDEAD,
          "dr7.passthrough");
    Check(c, DrResolveRead(2, s, true, false, 0x9999, 0) == 0x2222,
          "dr0-3.hide-shadow");
    Check(c, DrResolveRead(2, s, false, false, 0x9999, 0) == 0x9999,
          "dr0-3.passthrough");
    Check(c, DrResolveRead(6, s, true, false, 0, 0) == 0x6666,
          "dr6.hide-shadow");
    Check(c, DrResolveRead(6, s, false, false, 0, 0) == 0xFFFF0FF0,
          "dr6.initial-value");
    Check(c, DrResolveRead(5, s, true, false, 0x5555, 0x5555) == 0,
          "dr4-5.unsupported-zero");
}

void TestCr3Pure(StCtx& c)
{
    const u64 R = 0x1AB000, K = 0xDEADC0DE;
    Check(c, Cr3ResolveRead(R, K, true, 0) == (R ^ K), "cr3.spoof-all");
    Check(c, Cr3ResolveRead(R, 0, true, 0) == R, "cr3.spoof-needs-key");
    Check(c, Cr3ResolveRead(R, K, false, R) == (R ^ K), "cr3.spoof-target");
    Check(c, Cr3ResolveRead(R, 0, false, R) == R, "cr3.target-needs-key");
    Check(c, Cr3ResolveRead(R, K, false, 0x999000) == R, "cr3.target-miss");
    Check(c, Cr3ResolveRead(R, K, false, 0) == R, "cr3.plain");

    Check(c, Cr3ViewSwitchDecision(0x1000, 0x2000, 0x2000, true) ==
                 Cr3ViewSwitch::Enter,
          "cr3vs.enter");
    Check(c, Cr3ViewSwitchDecision(0x2000, 0x1000, 0x2000, true) ==
                 Cr3ViewSwitch::Leave,
          "cr3vs.leave");
    Check(c, Cr3ViewSwitchDecision(0x1000, 0x3000, 0x2000, true) ==
                 Cr3ViewSwitch::None,
          "cr3vs.unrelated");
    Check(c, Cr3ViewSwitchDecision(0x2000, 0x2000, 0x2000, true) ==
                 Cr3ViewSwitch::None,
          "cr3vs.self-write");
    Check(c, Cr3ViewSwitchDecision(0x1000, 0x2000, 0x2000, false) ==
                 Cr3ViewSwitch::None,
          "cr3vs.mode-off");
    Check(c, Cr3ViewSwitchDecision(0x1000, 0x2000, 0, true) ==
                 Cr3ViewSwitch::None,
          "cr3vs.no-target");
}

void TestNptFillView(StCtx& c)
{
    NptManager m;
    Check(c, NT_SUCCESS(m.Init()), "fillview.init");

    // a non-default view gets the same full-RAM fill as the default
    u32 id = 0;
    Check(c, NT_SUCCESS(m.CreateView(id)), "fillview.create");
    NptView* v = m.View(id);
    Check(c, v != nullptr, "fillview.view");
    Check(c, NT_SUCCESS(m.FillView(*v)), "fillview.fill");
    u64 pte = 0;
    Check(c, NT_SUCCESS(v->GetPte4k(0x100000, pte)), "fillview.pte");
    Check(c, NptPaFromPfn(pte) == 0x100000, "fillview.identity");

    // the default view fill is unaffected by the helper refactor
    Check(c, NT_SUCCESS(m.FillDefaultView()), "fillview.default-ok");
    Check(c, m.FillDefaultView() == STATUS_ALREADY_REGISTERED,
          "fillview.default-idempotent");
    // unknown id yields no view; an un-Init'ed view is rejected
    Check(c, m.View(999) == nullptr, "fillview.unknown-id");
    NptView uninit;
    Check(c, m.FillView(uninit) == STATUS_INVALID_PARAMETER,
          "fillview.uninited");
    m.Deinit();
}

void TestNpfHookSlide(StCtx& c)
{
    NptManager m;
    Check(c, NT_SUCCESS(m.Init()), "slide.init");
    NptHookManager hooks;
    Check(c, NT_SUCCESS(hooks.Init(&m)), "slide.hook-init");

    // hook our own code page (tables only - never executed through the NPT)
    u64 target = (u64)&TestNpfHookSlide;
    u64 pageVa = target & ~0xFFFull;
    PHYSICAL_ADDRESS tpa = MmGetPhysicalAddress((void*)pageVa);
    u64 origPa = (u64)tpa.QuadPart;
    Check(c, NT_SUCCESS(hooks.Install(target, target + 0x20,
                                      NptHookManager::MODE_JMP)),
          "slide.install");

    // NPT still disabled: legacy behavior - hidden page published directly
    u64 pte = 0;
    Check(c, NT_SUCCESS(m.Default().GetPte4k(origPa, pte)), "slide.pte");
    Check(c, NptPaFromPfn(pte) != origPa, "slide.legacy-hidden");
    Check(c, (pte & NPT_RW) && !(pte & NPT_NX), "slide.legacy-rwx");

    NptHook* h = hooks.LookupPage(origPa);
    Check(c, h && h->PagePa == origPa && h->OrigPfn == (origPa >> 12),
          "slide.lookup");
    Check(c, hooks.LookupPage(origPa + 0x1000) == nullptr, "slide.lookup-miss");

    // NPT goes live -> arm: hooked page flips to the DATA state
    Check(c, NT_SUCCESS(m.SetActiveView(0)), "slide.setactive");
    Check(c, NT_SUCCESS(m.Enable()) && m.NptEnabled(), "slide.enable");
    hooks.ArmSlide(m.Default());
    Check(c, NT_SUCCESS(m.Default().GetPte4k(origPa, pte)), "slide.arm-pte");
    Check(c, NptPaFromPfn(pte) == origPa, "slide.arm-orig-pfn");
    Check(c, (pte & NPT_RW) && (pte & NPT_NX), "slide.arm-data-perms");

    // fetch fault -> EXEC state: hidden pfn, executable, no write
    Check(c, HookSlideApply(hooks, m.Default(), origPa,
                            NPF_PRESENT | NPF_EXECUTE),
          "slide.exec-handled");
    Check(c, NT_SUCCESS(m.Default().GetPte4k(origPa, pte)), "slide.exec-pte");
    Check(c, NptPaFromPfn(pte) != origPa, "slide.exec-hidden-pfn");
    Check(c, !(pte & NPT_RW) && !(pte & NPT_NX), "slide.exec-perms");

    // write fault while EXEC-mapped -> back to DATA
    Check(c, HookSlideApply(hooks, m.Default(), origPa,
                            NPF_PRESENT | NPF_WRITE),
          "slide.write-handled");
    Check(c, NT_SUCCESS(m.Default().GetPte4k(origPa, pte)), "slide.write-pte");
    Check(c, NptPaFromPfn(pte) == origPa, "slide.write-orig-pfn");
    Check(c, (pte & NPT_RW) && (pte & NPT_NX), "slide.write-data-perms");

    // user-mode fault (no W/X) declines - not slide business
    Check(c, HookSlideApply(hooks, m.Default(), origPa,
                            NPF_PRESENT | NPF_USER) == false,
          "slide.user-declined");
    // unhooked page declines
    Check(c, HookSlideApply(hooks, m.Default(), origPa + 0x1000,
                            NPF_PRESENT | NPF_EXECUTE) == false,
          "slide.stranger-declined");

    Check(c, NT_SUCCESS(hooks.Remove(target)), "slide.remove");
    Check(c, NT_SUCCESS(m.Default().GetPte4k(origPa, pte)) &&
                 NptPaFromPfn(pte) == origPa,
          "slide.removed-identity");
    hooks.Deinit();
    m.Deinit();
}

void TestCr3Match(StCtx& c)
{
    char stored[16] = {};
    RtlCopyMemory(stored, "cmd.exe", 8);
    Check(c, Cr3ImageMatch(stored, "cmd.exe"), "img.exact");
    Check(c, Cr3ImageMatch(stored, "CMD.EXE"), "img.case-insensitive");
    Check(c, !Cr3ImageMatch(stored, "cmd.exe.bak"), "img.want-longer");
    Check(c, !Cr3ImageMatch(stored, "cmd"), "img.want-shorter");
    Check(c, !Cr3ImageMatch(stored, ""), "img.want-empty");
    Check(c, !Cr3ImageMatch(stored, nullptr), "img.want-null");

    // stored side is a 16-byte NUL-terminated field; a 15-char name matches
    char maxed[16] = {};
    RtlCopyMemory(maxed, "abcdefghijkmnop", 15); // 15 chars, [15] stays NUL
    Check(c, Cr3ImageMatch(maxed, "abcdefghijkmnop"), "img.15-char");
    Check(c, !Cr3ImageMatch(maxed, "abcdefghijkmnopx"), "img.capacity");

    // non-printable bytes in the guest path were stored as '?' and match
    // literally
    char wild[16] = {};
    RtlCopyMemory(wild, "app?.exe", 9);
    Check(c, Cr3ImageMatch(wild, "app?.exe"), "img.literal-question");
    Check(c, !Cr3ImageMatch(wild, "appx.exe"), "img.no-glob");
}

// r88: pure x64 walk math behind the CR3_READVM page-table walker
void TestMemReadPure(StCtx& c)
{
    // index extraction: build a va from chosen per-level indexes
    const u64 va = (0x1AAull << 39) | (0x155ull << 30) | (0x100ull << 21)
                   | (0x0FFull << 12) | 0x123;
    Check(c, ReadVmVaIndex(va, 0) == 0x1AA, "rdvm.idx.pml4");
    Check(c, ReadVmVaIndex(va, 1) == 0x155, "rdvm.idx.pdpt");
    Check(c, ReadVmVaIndex(va, 2) == 0x100, "rdvm.idx.pd");
    Check(c, ReadVmVaIndex(va, 3) == 0x0FF, "rdvm.idx.pt");
    Check(c, ReadVmVaIndex(0, 0) == 0 && ReadVmVaIndex(0, 3) == 0,
          "rdvm.idx.zero-va");
    Check(c, ReadVmVaIndex(va, 4) == 0x1FF, "rdvm.idx.level-guard");

    // frame extraction ignores flag bits (present|rw|us|PS|a|d|nx)
    const u64 entry = 0x000000012345F067ull;
    Check(c, ReadVmEntryFrame(entry) == 0x12345F000ull, "rdvm.frame");
    Check(c, ReadVmEntryFrame(0) == 0, "rdvm.frame.zero");

    // large-page gpa: entry frame aligned down to the size, va offset in
    const u64 e2m = 0x00400000000ull | 0x87; // 16G frame, flag soup
    Check(c, ReadVmLargeGpa(e2m, 0x123456, 21) == 0x400123456ull,
          "rdvm.2m-gpa");
    const u64 e1g = 0x01000000000ull | 0x183;
    Check(c, ReadVmLargeGpa(e1g, 0x0ABCDEF0, 30) == 0x100ABCDEF0ull,
          "rdvm.1g-gpa");

    // 4K page gpa
    Check(c, ReadVmPageGpa(0x12345067ull, 0x89F) == 0x1234589Full,
          "rdvm.4k-gpa");

    // IsKernelVa canonical-only (r85 P2-9 regression guard)
    Check(c, IsKernelVa(0xFFFF800000000000ull), "rdvm.kva.low");
    Check(c, IsKernelVa(0xFFFFFF8000000000ull), "rdvm.kva.high");
    Check(c, !IsKernelVa(0x00007FF000000000ull), "rdvm.kva.user");
    Check(c, !IsKernelVa(0x8000000000000000ull), "rdvm.kva.noncanon");
}

// r89: kill-policy deny list (plan C - system-critical images survive a
// bypass trip even when their CR3 resolves)
void TestKillDenylist(StCtx& c)
{
    Check(c, Cr3RegionKillDenylisted("csrss.exe"), "kill.deny.csrss");
    Check(c, Cr3RegionKillDenylisted("CSRSS.EXE"), "kill.deny.case");
    Check(c, Cr3RegionKillDenylisted("wininit.exe"), "kill.deny.wininit");
    Check(c, Cr3RegionKillDenylisted("winlogon.exe"), "kill.deny.winlogon");
    Check(c, Cr3RegionKillDenylisted("services.exe"), "kill.deny.services");
    Check(c, Cr3RegionKillDenylisted("lsass.exe"), "kill.deny.lsass");
    Check(c, Cr3RegionKillDenylisted("smss.exe"), "kill.deny.smss");
    Check(c, !Cr3RegionKillDenylisted("notepad.exe"), "kill.allow.notepad");
    Check(c, !Cr3RegionKillDenylisted("svmbctl.exe"), "kill.allow.ctl");
    Check(c, !Cr3RegionKillDenylisted(""), "kill.allow.empty");
    Check(c, !Cr3RegionKillDenylisted(nullptr), "kill.allow.null");
    // r96 audit K2: resident-manager images join the deny list
    Check(c, Cr3RegionKillDenylisted("system"), "kill.deny.system");
    Check(c, Cr3RegionKillDenylisted("registry"), "kill.deny.registry");
    Check(c, Cr3RegionKillDenylisted("memcompression"), "kill.deny.memcomp");
}

// r92: the stealth module's CPUID ownership set + scrub transform - the
// exact code its exit handler applies to every owned leaf.
void TestStealthScrub(StCtx& c)
{
    // ownership: leaf 1 + the 0x40000000..0x40000010 signature area
    Check(c, StealthScrubLeaf(1), "stealth.own.leaf1");
    Check(c, StealthScrubLeaf(0x40000000), "stealth.own.lo");
    Check(c, StealthScrubLeaf(0x4000000F), "stealth.own.mid");
    Check(c, StealthScrubLeaf(0x40000010), "stealth.own.tschint");
    Check(c, !StealthScrubLeaf(0), "stealth.own.leaf0");
    Check(c, !StealthScrubLeaf(0x3FFFFFFF), "stealth.own.below");
    Check(c, !StealthScrubLeaf(0x40000011), "stealth.own.above");
    // svmb's own channels must stay reachable with stealth attached:
    // 0x400000FE presence probe, 0x400000FF hypercall vehicle, 0x40000100
    // demo module leaf
    Check(c, !StealthScrubLeaf(0x400000FE), "stealth.allow.probe");
    Check(c, !StealthScrubLeaf(0x400000FF), "stealth.allow.hypercall");
    Check(c, !StealthScrubLeaf(0x40000100), "stealth.allow.demo");
    Check(c, !StealthScrubLeaf(0x80000001), "stealth.allow.extfeats");
    Check(c, !StealthScrubLeaf(0x8000000A), "stealth.allow.svmfeat");

    // leaf 1: ECX bit31 cleared, every other register bit preserved
    u32 r[4] = {0x00000F00, 0x00106500, 0x8FEBFBFF, 0xBFEBFBFF};
    StealthApplyScrub(1, r);
    Check(c, r[2] == (0x8FEBFBFFu & ~(1u << 31)), "stealth.leaf1.ecx31");
    Check(c, r[0] == 0x00000F00 && r[1] == 0x00106500 && r[3] == 0xBFEBFBFF,
          "stealth.leaf1.others");

    // signature leaves: all four registers zeroed
    u32 s[4] = {0x00000010, 'MWRW', 'raVe', 'rewa'};
    StealthApplyScrub(0x40000000, s);
    Check(c, !s[0] && !s[1] && !s[2] && !s[3], "stealth.sig.zeroed");
}

// r100: syscall-audit ring slot math - the mask (r97 signed-modulo class)
// and the commit-marker drain predicate (r100 acceptance P1-1).
void TestSysAuditRing(StCtx& c)
{
    Check(c, SysAuditSlotIndex(0) == 0 && SysAuditSlotIndex(255) == 255,
          "sysaud.ring.idx.first");
    Check(c, SysAuditSlotIndex(256) == 0 && SysAuditSlotIndex(511) == 255,
          "sysaud.ring.idx.wrap");
    Check(c, SysAuditSlotIndex(0x80000000u) == 0,
          "sysaud.ring.idx.signed-wrap");
    Check(c, SysAuditSlotReady(1, 0) && SysAuditSlotReady(42, 41),
          "sysaud.ring.ready");
    Check(c, !SysAuditSlotReady(0, 0) && !SysAuditSlotReady(1, 1),
          "sysaud.ring.notready");
    Check(c, !SysAuditSlotReady(3, 1), "sysaud.ring.hole");
    // r101: the 18362 RUNTIME KiServiceTable law - packed table-relative
    // entries, service = table + (s32(entry) >> 4), low nibble = stack
    // arg count. Pinned against the live kdump: entry[0x55]=0x020b9307
    // unpacks to +0x20b930 (table RVA 0x424c10 -> NtCreateFile 0x630540,
    // args=7); a NEGATIVE entry (e0=0xfced7304) must unpack below the
    // table (arithmetic, not logical shift).
    Check(c, SysAuditServiceVa(0xFFFFF80572624C10ull, 0x020b9307)
                 == 0xFFFFF80572830540ull,
          "sysaud.ssdt.unpack.ntcf");
    Check(c, SysAuditServiceVa(0xFFFFF80572624C10ull, 0x01f88e00)
                 == 0xFFFFF8057281D4F0ull,
          "sysaud.ssdt.unpack.ntop");
    Check(c, SysAuditServiceVa(0x1000, 0x020b9300)
                 == SysAuditServiceVa(0x1000, 0x020b9307),
          "sysaud.ssdt.argnib ignored");
    Check(c, SysAuditServiceVa(0x1000, 0xFFFFFFF0) == 0xFFF,
          "sysaud.ssdt.unpack.negative");
    // r102: behavior window/hash/edge math
    Check(c, SysAuditBehaveWindowOf(0) == 0
                 && SysAuditBehaveWindowOf(9999999) == 0,
          "sysaud.behave.win.first");
    Check(c, SysAuditBehaveWindowOf(10000000) == 1
                 && SysAuditBehaveWindowOf(25999999) == 2,
          "sysaud.behave.win.roll");
    Check(c, SysAuditBehaveSlotFor(0) < SVMB_SYSAUD_BEHAVE_SLOTS
                 && SysAuditBehaveSlotFor(4) == SysAuditBehaveSlotFor(4),
          "sysaud.behave.slot.stable");
    Check(c, SysAuditBehaveSlotFor(0xFFFFFFFF) < SVMB_SYSAUD_BEHAVE_SLOTS,
          "sysaud.behave.slot.range");
    Check(c, !SysAuditBehaveEdge(127, 128) && SysAuditBehaveEdge(128, 128)
                 && !SysAuditBehaveEdge(129, 128),
          "sysaud.behave.edge.exact");
    // r103: (caller,target) cache-slot hash - stable and in range
    Check(c, SysAuditTargetSlotFor(8200, 1028)
                     == SysAuditTargetSlotFor(8200, 1028)
                 && SysAuditTargetSlotFor(8200, 1028)
                        < SVMB_SYSAUD_TARGET_SLOTS,
          "sysaud.target.slot.stable");
    Check(c, SysAuditTargetSlotFor(0xFFFFFFFF, 0xFFFFFFFF)
                 < SVMB_SYSAUD_TARGET_SLOTS,
          "sysaud.target.slot.range");
}

// r88: same-page second-hook target for the hook self-tests. A hard-coded
// +0x40 breaks whenever any insertion shifts the function's in-page offset
// (StealLen must decode a 12-byte prefix at the target) - probe same-page
// candidates around `target` and use the first that installs. Returns 0
// when every candidate fails (the caller's Check then fails loudly).
static u64 PickSecondHookTarget(NptHookManager& hooks, u64 target, u64 pageVa)
{
    static constexpr LONG kOffs[] = {0x40,  -0x40, 0x20,  -0x20,
                                     0x80,  -0x80, 0x100, -0x100};
    for (LONG off : kOffs)
    {
        u64 cand = (u64)((LONG64)target + off);
        if ((cand & ~0xFFFull) != pageVa)
            continue;
        if (NT_SUCCESS(hooks.Install(cand, cand + 0x40,
                                     NptHookManager::MODE_JMP)))
            return cand;
    }
    return 0;
}

void TestNptHookSharedPage(StCtx& c)
{
    NptManager m;
    Check(c, NT_SUCCESS(m.Init()), "shpage.init");
    NptHookManager hooks;
    Check(c, NT_SUCCESS(hooks.Init(&m)), "shpage.hook-init");

    u64 target = (u64)&TestNptHookSharedPage;
    u64 pageVa = target & ~0xFFFull;
    PHYSICAL_ADDRESS tpa = MmGetPhysicalAddress((void*)pageVa);
    u64 origPa = (u64)tpa.QuadPart;

    // two hooks on one page: the second composes into the shared hidden copy
    Check(c, NT_SUCCESS(hooks.Install(target, target + 0x40,
                                      NptHookManager::MODE_JMP)),
          "shpage.h1");
    u64 t2 = PickSecondHookTarget(hooks, target, pageVa);
    Check(c, t2 != 0, "shpage.h2");

    u64 pte = 0;
    Check(c, NT_SUCCESS(m.Default().GetPte4k(origPa, pte)), "shpage.pte");
    Check(c, NptPaFromPfn(pte) != origPa, "shpage.hidden-published");

    // a footprint overlapping h2's patch/steal region must be rejected with
    // the dedicated overlap status (an undecodable target still yields the
    // generic NOT_SUPPORTED - the codes tell the reasons apart).
    // r36 precedence note: an unhookable target prefix (rip-rel or control
    // transfer inside the stolen window) is rejected STATUS_NOT_SUPPORTED
    // BEFORE the overlap check - and this function's code at t2+8 walks
    // into exactly such a prefix, so accept either rejection here; the
    // overlap detector itself is covered by hook.duplicate-page-rejected
    // in TestNptManager (same-target double install).
    NTSTATUS stOverlap = hooks.Install(t2 + 8, t2 + 0x48,
                                       NptHookManager::MODE_JMP);
    Check(c, stOverlap == NPT_HOOK_STATUS_OVERLAP ||
                 stOverlap == STATUS_NOT_SUPPORTED,
          "shpage.overlap-rejected");

    // removing the first hook keeps the page hooked for h2
    Check(c, NT_SUCCESS(hooks.Remove(target)), "shpage.rm1");
    Check(c, NT_SUCCESS(m.Default().GetPte4k(origPa, pte)), "shpage.rm1.pte");
    Check(c, NptPaFromPfn(pte) != origPa, "shpage.rm1.still-hidden");

    // removing the last hook restores the identity mapping
    Check(c, NT_SUCCESS(hooks.Remove(t2)), "shpage.rm2");
    Check(c, NT_SUCCESS(m.Default().GetPte4k(origPa, pte)), "shpage.rm2.pte");
    Check(c, NptPaFromPfn(pte) == origPa, "shpage.rm2.identity");
    Check(c, hooks.Remove(target) == STATUS_NOT_FOUND, "shpage.rm3.absent");

    // regression: Deinit with hooks still installed must reclaim the shared
    // hidden page and split reference exactly once (walks live nodes only)
    Check(c, NT_SUCCESS(hooks.Install(target, target + 0x40,
                                      NptHookManager::MODE_JMP)),
          "shpage.h1b");
    Check(c, NT_SUCCESS(hooks.Install(t2, t2 + 0x40,
                                      NptHookManager::MODE_JMP)),
          "shpage.h2b");
    hooks.Deinit(); // no Remove first
    Check(c, NT_SUCCESS(m.Default().GetPte4k(origPa, pte)),
          "shpage.deinit.pte");
    Check(c, NptPaFromPfn(pte) == origPa, "shpage.deinit.identity");

    m.Deinit();
}

void TestNptHookGraveyard(StCtx& c)
{
    NptManager m;
    Check(c, NT_SUCCESS(m.Init()), "grave.init");
    NptHookManager hooks;
    Check(c, NT_SUCCESS(hooks.Init(&m)), "grave.hook-init");

    u64 target = (u64)&TestNptHookGraveyard;
    u64 pageVa = target & ~0xFFFull;
    PHYSICAL_ADDRESS tpa = MmGetPhysicalAddress((void*)pageVa);
    u64 origPa = (u64)tpa.QuadPart;

    Check(c, NT_SUCCESS(hooks.Install(target, target + 0x40,
                                      NptHookManager::MODE_JMP)),
          "grave.install");
    Check(c, NT_SUCCESS(hooks.Remove(target)), "grave.remove");

    // removal is observable immediately, but the node lives on parked
    Check(c, hooks.LookupPage(origPa) == nullptr, "grave.lookup-cleared");
    Check(c, HookSlideApply(hooks, m.Default(), origPa,
                            NPF_PRESENT | NPF_EXECUTE) == false,
          "grave.slide-declined");
    Check(c, hooks.GraveyardCount() == 1, "grave.parked");
    Check(c, hooks.Remove(target) == STATUS_NOT_FOUND, "grave.remove-absent");

    // a re-install after removal works on a fresh node
    Check(c, NT_SUCCESS(hooks.Install(target, target + 0x40,
                                      NptHookManager::MODE_JMP)),
          "grave.reinstall");
    Check(c, hooks.GraveyardCount() == 1, "grave.count-stable");
    Check(c, NT_SUCCESS(hooks.Remove(target)), "grave.remove2");
    Check(c, hooks.GraveyardCount() == 2, "grave.parked-2");

    // shared-page pair: both parked nodes reference one hidden copy - Deinit
    // must reclaim it exactly once (no double free). Second target via the
    // same-page probe (r88: hard-coded +0x40 was function-layout fragile).
    Check(c, NT_SUCCESS(hooks.Install(target, target + 0x40,
                                      NptHookManager::MODE_JMP)),
          "grave.shared-1");
    u64 t2 = PickSecondHookTarget(hooks, target, pageVa);
    Check(c, t2 != 0, "grave.shared-2");
    Check(c, NT_SUCCESS(hooks.Remove(target)), "grave.shared-rm1");
    Check(c, NT_SUCCESS(hooks.Remove(t2)), "grave.shared-rm2");
    Check(c, hooks.GraveyardCount() == 4, "grave.parked-4");

    hooks.Deinit(); // reclaims both lists, shared hidden freed once
    m.Deinit();
}

void TestIopmRefs(StCtx& c)
{
    Iopm io;
    Check(c, NT_SUCCESS(io.Init()), "iopm.init");

    bool r = false, w = false;
    Check(c, NT_SUCCESS(io.RequirePort(0x3F8, true, true)), "iopm.require");
    Check(c, io.GetPortIntercept(0x3F8, r, w) && r && w, "iopm.bits-set");

    // second require on the same directions: bits stay, refcount doubles
    Check(c, NT_SUCCESS(io.RequirePort(0x3F8, true, true)), "iopm.require2");
    Check(c, io.GetPortIntercept(0x3F8, r, w) && r && w, "iopm.bits-still");

    // first release keeps the interception (count 2 -> 1)
    Check(c, NT_SUCCESS(io.ReleasePort(0x3F8, true, true)), "iopm.release1");
    Check(c, io.GetPortIntercept(0x3F8, r, w) && r && w, "iopm.still-held");

    // second release clears both bits
    Check(c, NT_SUCCESS(io.ReleasePort(0x3F8, true, true)), "iopm.release2");
    Check(c, io.GetPortIntercept(0x3F8, r, w) && !r && !w, "iopm.bits-clear");

    // per-direction independence: read-only require, write over-release.
    // The over-release must leave the read reference and bits untouched
    // (regression: a rollback path here once manufactured phantom counts)
    Check(c, NT_SUCCESS(io.RequirePort(0x60, true, false)), "iopm.ro-require");
    Check(c, io.ReleasePort(0x60, false, true) == STATUS_NOT_FOUND,
          "iopm.over-release");
    Check(c, io.GetPortIntercept(0x60, r, w) && r && !w, "iopm.ro-bits-intact");
    Check(c, io.ReleasePort(0x60, false, true) == STATUS_NOT_FOUND,
          "iopm.over-release-again");
    Check(c, NT_SUCCESS(io.ReleasePort(0x60, true, false)), "iopm.ro-release");
    Check(c, io.GetPortIntercept(0x60, r, w) && !r && !w, "iopm.ro-cleared");

    // same-byte adjacent ports keep isolated bits
    Check(c, NT_SUCCESS(io.RequirePort(0x3F8, true, false)), "iopm.adj-1");
    Check(c, NT_SUCCESS(io.RequirePort(0x3F9, false, true)), "iopm.adj-2");
    Check(c, io.GetPortIntercept(0x3F8, r, w) && r && !w, "iopm.adj-1-bits");
    Check(c, io.GetPortIntercept(0x3F9, r, w) && !r && w, "iopm.adj-2-bits");

    // unrelated ports do not interfere; adjacent bits stay isolated
    Check(c, NT_SUCCESS(io.RequirePort(7, true, false)), "iopm.port7");
    Check(c, NT_SUCCESS(io.RequirePort(8, false, true)), "iopm.port8");
    Check(c, io.GetPortIntercept(7, r, w) && r && !w, "iopm.port7-bits");
    Check(c, io.GetPortIntercept(8, r, w) && !r && w, "iopm.port8-bits");

    Check(c, io.RequirePort(0x10000, true, false) == STATUS_INVALID_PARAMETER,
          "iopm.port-range");
    Check(c, io.ReleasePort(0x999, true, false) == STATUS_NOT_FOUND,
          "iopm.stranger");

    io.Deinit();
}

NTSTATUS RunOfflineSelfTests(SVMB_SELFTEST_RESULT* out)
{
    StCtx c;
    TestPhysRanges(c);
    TestNptView(c);
    TestNptManager(c);
    TestInsnLen(c);
    TestMsrpmShadow(c);
    TestDbgRing(c);
    TestNptMultiView(c);
    TestNpfClassify(c);
    TestGuestGpr(c);
    TestDebuggerPure(c);
    TestCr3Pure(c);
    TestNpfHookSlide(c);
    TestCr3Match(c);
    TestMemReadPure(c);
    TestKillDenylist(c);
    TestStealthScrub(c);
    TestSysAuditRing(c);
    TestNptHookSharedPage(c);
    TestNptHookGraveyard(c);
    TestNptFillView(c);
    TestIopmRefs(c);

    out->Magic = SVMB_PROTO_MAGIC;
    out->Total = c.Total;
    out->Failed = c.Failed;
    out->Reserved = 0;
    strncpy_s(out->LastFail, sizeof(out->LastFail), c.LastFail, _TRUNCATE);

    SVMB_LOGI("selftest: %u checks, %u failed%s%s", c.Total, c.Failed,
              c.Failed ? " first=" : "", c.Failed ? c.LastFail : "");
    return c.Failed ? STATUS_UNSUCCESSFUL : STATUS_SUCCESS;
}

} // namespace svmb
