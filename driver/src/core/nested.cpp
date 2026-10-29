// svmb - r166 nested virtualization v1 implementation. See nested.h.
#include "core/nested.h"
#include "core/vlog.h" // r181: H-behavior recorder hooks
#include "core/exit_dispatcher.h"
#include "core/hypervisor.h"
#include "core/vcpu.h"
#include "hw/svm_defs.h"
#include "mm/tlb.h" // TLB_CTL_FLUSH_ALL (the shadow-swap flush)
#include "platform/logger.h"

namespace svmb
{
// r183: defined later in this file (the accept path calls the scan
// before its definition point)
static bool ShadowScanHooks(u32 cpuIndex);
static u64* MapGuestPageU64(u64 gpa);
static volatile LONG g_intrPuntEnabled = 0; // r197 gate (setter below)

// local mirror of hypervisor.cpp's class table (its linkage is not
// exported): fault-class exits carry no advance-NRip semantics
static bool FaultClassExit(u64 exit)
{
    if (exit >= vmexit::EXCEPTION(0) && exit <= vmexit::EXCEPTION(31))
        return true;
    return exit == vmexit::NPF || exit == vmexit::SHUTDOWN ||
           exit == vmexit::INTR || exit == vmexit::NMI ||
           exit == vmexit::SMI || exit == vmexit::INIT ||
           exit == vmexit::VINTR;
}

static VMCB* MapGuestPage(u64 pa)
{
    PHYSICAL_ADDRESS p = {};
    p.QuadPart = pa;
    return (VMCB*)MmGetVirtualForPhysical(p);
}

// guest RAM identity cap (svmb's NPT fill range; also bounds the PA the
// VMM may dereference through the direct map)
static bool PaPlausible(u64 pa)
{
    return pa != 0 && (pa & 0xFFFull) == 0 && pa < (1ull << 40);
}

// r197: nested-session INTR punt gate (Parameters\NestedIntrPunt, default
// 0). With the gate on, OwnIdt (in-place, e.g. the reference hypervisor) sessions ALSO arm
// ic1 bit0 - a pending external interrupt exits (0x60) to svmb instead of
// being pushed into the L2 by TCG, and the self-serve case drains it into
// the L1 context (the months-stable native delivery path). Motivation:
// r195/r196 - both nested-freeze death vehicles trace to TCG's
// guest-in-guest event push (cookie overwrite / delivery-time fault ->
// 0x7F double fault).
extern "C" LONG svmbNestedIntrPunt(LONG on)
{
    return InterlockedExchange(&g_intrPuntEnabled, on);
}

bool NestedVmrunExit(GuestContext& ctx)
{
    VcpuContext* vcpu = ctx.Vcpu;
    VMCB& vmcb = *ctx.Vmcb();
    NestedState& ns = NestedFor(ctx.CpuIndex());
    u64 pa = ctx.Regs->Rax;

    // L3 attempt: L2 itself executed VMRUN. v1 refuses - inject #GP into
    // L2 and step past the VMRUN (fault semantics would re-execute it
    // forever, the r140 anti-nesting lesson).
    if (ns.Active)
    {
        SVMB_LOGI("nested: L3 vmrun refused vmcbPa=%llx rip=%llx",
                  (unsigned long long)pa,
                  (unsigned long long)vmcb.Save.Rip);
        InjectEvent(ctx, EXC_GP, EVT_EXCEPTION, false, 0);
        ctx.AdvanceRip = true;
        return true;
    }

    // v1 contract validation (see nested.h). Any miss keeps the r140
    // anti-nesting semantics: #BP with softint delivery, skip the VMRUN.
    VMCB* h = nullptr;
    bool accept = PaPlausible(pa);
    if (accept)
    {
        // PFN-backed guest RAM only (debugger.cpp precedent); the direct
        // map is valid at any IRQL
        h = MapGuestPage(pa);
        // r172: NP sessions are accepted too - they run on the shadow
        // composer (the "refuse NpEnable" era ends). Other v1 contract
        // checks unchanged.
        accept = h && h->Save.Rip && h->Save.Rsp && h->Save.Cr3 &&
                 h->Save.Rflags;
    }
    if (!accept)
    {
        SVMB_LOGI("nested: vmrun reject vmcbPa=%llx (v1 contract unmet)",
                  (unsigned long long)pa);
        InjectEvent(ctx, EXC_BP, EVT_SOFTINT, false, 0);
        ctx.AdvanceRip = true;
        return true;
    }

    // ---- accept: H enters L2 ----
    // Stash H: the full save area (Rip bumped past the VMRUN - on real
    // hardware VMRUN saves the next-instruction host RIP into the host
    // save area, so H resumes AFTER its VMRUN) and the whole GPR frame.
    ns.HSave = vmcb.Save;
    ns.HSave.Rip = ctx.NRip; // reconstructed NRip = VMRUN + 3 (r13 law)
    ns.HRegs = *ctx.Regs;
    ns.SaveIc1 = vmcb.Ctrl.InterceptOpcode1;
    ns.SaveIc2 = vmcb.Ctrl.InterceptOpcode2;
    ns.SaveExc = vmcb.Ctrl.InterceptException;
    // r186: stash H's OWN request - the reflect gate serves only these
    ns.HIc1 = h->Ctrl.InterceptOpcode1;
    ns.HIc2 = h->Ctrl.InterceptOpcode2;
    ns.HExc = h->Ctrl.InterceptException;
    // r186c/r188/r189: sticky first-sight in-place latch, EARLY (the
    // r189 shadow-init identity prefill consumes it below). Both older
    // tests were brittle: the raw Idtr check broke on the second accept
    // (the reflect's full-save copy echoes the skeleton IDTR back into
    // H's VMCB), the CR3-equality check broke when H's handler context
    // switched threads between the reflect and its re-VMRUN (the reference hypervisor
    // relaunching a user thread -> its #PF reflected into its exit-
    // switch default). The FIRST observation of a VMCB precedes any
    // contamination: the reference hypervisor's guestVmcb re-stores a real Idtr per
    // launch, test toys are zeroed before use - both stay truthful.
    {
        static struct { u64 pa; u32 own; } s_idtLatch[8];
        u32 slot = 0xFF;
        for (u32 i = 0; i < 8; ++i)
        {
            if (s_idtLatch[i].pa == pa)
            {
                slot = i;
                break;
            }
        }
        if (slot == 0xFF)
        {
            for (u32 i = 0; i < 8; ++i)
            {
                if (s_idtLatch[i].pa == 0)
                {
                    slot = i;
                    s_idtLatch[i].pa = pa;
                    s_idtLatch[i].own =
                        (h->Save.Idtr.Limit || h->Save.Idtr.Base) ? 1 : 0;
                    break;
                }
            }
            if (slot == 0xFF)
            {
                slot = 0; // cache full: recycle slot 0 (bench-scale)
                s_idtLatch[0].pa = pa;
                s_idtLatch[0].own =
                    (h->Save.Idtr.Limit || h->Save.Idtr.Base) ? 1 : 0;
            }
        }
        ns.OwnIdt = s_idtLatch[slot].own;
    }
    ns.SaveTscOffset = vmcb.Ctrl.TscOffset;
    ns.HvmcbPa = pa;
    // r168b: an EVENTINJ armed for L1 is armed for THE NEXT VMRUN - which
    // accept just made L2's. Firing an L1 event into L2 delivers through
    // the skeleton L1 IDT on L2 state (Windows handlers + L2 CR3/RIP =
    // the wedge family). Stash and clear; reflect re-arms it for L1.
    ns.SaveEventInj = vmcb.Ctrl.EventInj;
    ns.PendingL1Event = vmcb.Ctrl.EventInj.F.Valid ? 1 : 0;
    vmcb.Ctrl.EventInj.F.Valid = 0;
    // r168b: same hazard class - a pending VINTR (svmb arms VIntr for L1
    // interrupt delivery) fires ctl_has_irq at the L2-entry VMRUN and
    // QEMU delivers L1's interrupt INTO L2. Stash and clear; reflect
    // restores. The interrupt-shadow word rides along (delivery-block
    // state belongs to L1's delivery, not L2's).
    ns.SaveVIntr = vmcb.Ctrl.VIntr.Data;
    ns.SaveIntStatus = vmcb.Ctrl.GuestIntStatus;
    vmcb.Ctrl.VIntr.Data = 0;
    vmcb.Ctrl.GuestIntStatus = 0;
    // r181: the third-party-H behavior recorder - session accept with
    // H's intent snapshot; internally diffs against the previous accept
    // on this core and emits intercept/injection/control-change records
    VlogAccept(ctx.CpuIndex(), pa, h->Save.Rip, h->Save.Cr3,
               h->Ctrl.Np.F.NpEnable, ns.PendingL1Event,
               h->Ctrl.InterceptOpcode1, h->Ctrl.InterceptOpcode2,
               h->Ctrl.InterceptException, h->Ctrl.NCr3,
               h->Ctrl.TscOffset, h->Ctrl.EventInj.Data);
    // r172: NP session - swap svmb's NPT root for the shadow root. The
    // shadow starts EMPTY each session (fresh accept = fresh world view;
    // per-entry full rebuild is the v0 invalidation policy) and composes
    // lazily from H's nested NPT on NPF. Non-NP sessions keep svmb's NCr3
    // (the v1 single-level ride).
    ns.SaveNcr3 = vmcb.Ctrl.NCr3;
    ns.NestedNp = h->Ctrl.Np.F.NpEnable ? 1 : 0;
    ns.HNptRoot = h->Ctrl.NCr3;
    if (ns.NestedNp && NestedShadowInit(&ns))
    {
        vmcb.Ctrl.NCr3 = ns.ShadowPml4Pa;
        // r163 law (the silent-coast lesson, biting live): a root swap
        // with no flush serves STALE composite entries built under the
        // real NCR3 - the walk's A/D write then hits old user-perm
        // entries and NPFs P|W|U (SMAP-shaped). Flush ALL at the swap.
        vmcb.Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
    }
    // r184: scan AFTER the world init (the r183 pre-Init placement made
    // the first scan's fills collide with the world-change zeroing and
    // left scanDirty hot on every retry), self-core only (the accept is
    // the one point where this core's L2 is provably not running), and
    // flush when anything materialized - a rematerialized PD link swap
    // must not be shadowed by a stale 2M blanket translation
    if (ShadowScanHooks(ctx.CpuIndex()))
    {
        vmcb.Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
        vmcb.Ctrl.CleanBits.Bits = 0;
    }

    // Install L2 - skeleton-inherit: segs/CR0/CR4/EFER/CPL/DRs stay L1's
    // (valid by construction); H supplies the program state + intercepts.
    // svmb's control area is authoritative except the OR-ed H bitmaps -
    // its NCR3/NP stays (L2 rides svmb's single NPT), MSRPM/IOPM stay.
    //
    // r167 force-reflect safety net, independent of what H asked for:
    //  - exception bitmap ALL-ONES. The skeleton gave L2 the L1 IDTR, so
    //    any natively-delivered L2 exception would dispatch into the
    //    Windows kernel on L2's CR3/RIP - a bugcheck. Every L2 exception
    //    exits and reflects instead; H owns the emulation.
    //  - ic1 bit0 (INTR). Same class of hazard on the interrupt side:
    //    an interrupt delivered into L2 would run the L1 IDT with L2
    //    state. With INTR armed, a pending interrupt exits (0x60) and
    //    reflects - the real nested-SVM contract (H sees the intr exit
    //    and retries its VMRUN).
    // r168: TSC offset compose - L2's rdtsc must carry both levels
    // (svmb's base + whatever H asked for its guest).
    vmcb.Ctrl.TscOffset += h->Ctrl.TscOffset;
    // r168: EVENTINJ passthrough - H injects an event into L2 by arming
    // its VMCB's EVENTINJ (hardware-consumed semantics: transferred into
    // the real VMCB, the Valid bit cleared in H's copy just as hardware
    // clears the field after injection). A pending svmb/L1 event wins -
    // transferring over it would lose it.
    if (h->Ctrl.EventInj.F.Valid && !vmcb.Ctrl.EventInj.F.Valid)
    {
        vmcb.Ctrl.EventInj = h->Ctrl.EventInj;
        h->Ctrl.EventInj.F.Valid = 0;
    }
    // r168: IDTR override - H may give L2 its own IDT (needed for any
    // deliverable event; the skeleton L1 IDTR would dispatch L2 state
    // into Windows handlers). Zero = keep the skeleton. (The in-place
    // OwnIdt latch runs EARLY, right after the intent stash - the r189
    // shadow-init identity prefill consumes it.)
    if (h->Save.Idtr.Limit || h->Save.Idtr.Base)
        vmcb.Save.Idtr = h->Save.Idtr;
    vmcb.Save.Rip = h->Save.Rip;
    vmcb.Save.Rsp = h->Save.Rsp;
    vmcb.Save.Rflags = h->Save.Rflags;
    vmcb.Save.Rax = h->Save.Rax; // the one GPR SVM loads from the VMCB
    vmcb.Save.Cr3 = h->Save.Cr3;
    // r167 force-reflect safety net, skeleton-only (r186): with no H IDT
    // every L2 exception/intr must exit+reflect (native delivery would
    // dispatch through the L1 IDT on L2 state = bugcheck). With H's own
    // IDT installed the correct nested contract is H's request alone -
    // natively delivered L2 events go through L2's own IDT.
    vmcb.Ctrl.InterceptException =
        ns.OwnIdt ? ns.HExc : 0xFFFFFFFFu;
    vmcb.Ctrl.InterceptOpcode1 |=
        h->Ctrl.InterceptOpcode1 |
        ((ns.OwnIdt && !g_intrPuntEnabled) ? 0 : (1u << 0)); // r197 gate
    // r186d: for in-place sessions, svmb's own INTN punt bit (ic1 bit21,
    // the r175 machinery) must also stay OUT of the L2's set: an L2
    // kernel softint (int 2e family) exiting 0x75 and reflecting would
    // land in the reference hypervisor's exit-switch default = KeBugCheck (proven live:
    // the first SWINT reflect froze the machine after all six cores had
    // launched). The in-place L2 delivers softints natively through its
    // own IDT - correct and free.
    if (ns.OwnIdt)
        vmcb.Ctrl.InterceptOpcode1 &= ~(1u << 21);
    vmcb.Ctrl.InterceptOpcode2 |= h->Ctrl.InterceptOpcode2 |
                                   ic2::VMLOAD | ic2::VMSAVE |
                                   ic2::MWAIT;
    // r187b: MWAIT armed as svmb's own structural need for the L2. The
    // bench CPU model advertises MONITOR/MWAIT, so the L2 OS idles via
    // mwait - executed natively it parks the TCG vCPU thread, and a
    // parked core never takes the pending IRR (the r142 wake-path bug
    // family). Idle cores parked forever = every IPI rendezvous in the
    // L2 OS hangs with zero vmexits (the r187 livelock: 20+ min frozen
    // com1, no bugcheck, all cores in the freeze handshake). The
    // self-serve handler nap+retries it into a poll-wait.
    vmcb.Ctrl.CleanBits.Bits = 0; // we just dirtied ctrl + save

    // Ride the exit-entry write-back channel: SvmbVmExitEntry copies
    // Regs->Rip/Rsp/Rax/Rflags into the save area after dispatch - make
    // it copy the L2 values (and do not AdvanceRip over them).
    ctx.Regs->Rip = vmcb.Save.Rip;
    ctx.Regs->Rsp = vmcb.Save.Rsp;
    ctx.Regs->Rax = vmcb.Save.Rax;
    ctx.Regs->RFlags = vmcb.Save.Rflags;
    ctx.AdvanceRip = false;

    ns.Active = 1;
    SVMB_LOGI("nested: accept hvmcb=%llx hva=%p -> L2 rip=%llx rsp=%llx cr3=%llx "
              "ownIdt=%u hi1=%08x hexc=%08x",
              (unsigned long long)pa, (void*)h,
              (unsigned long long)h->Save.Rip,
              (unsigned long long)h->Save.Rsp,
              (unsigned long long)h->Save.Cr3,
              ns.OwnIdt, ns.HIc1, ns.HExc);
    return true;
}

// r186 intent gate: an L2 exit reflects to H only when H's own VMCB armed
// it (the ic1/ic2/exception-bitmap contract, mapping law ic1 bit n <->
// exit 0x60+n). Exits H never ordered are svmb's to serve through the
// normal module dispatch - silently handing H exits it didn't request is
// the r185 the reference hypervisor wedge (our forced INTR exit at the L2's first
// instruction drove its handler down the generic leave path and starved
// the machine). The r167 skeleton net keeps reflecting INTR/exceptions
// when H supplied no L2 IDT (native delivery would use the wrong IDT).
bool NestedExitOrderedByH(u32 cpuIndex, u64 e)
{
    NestedState& ns = NestedFor(cpuIndex);
    if (e >= 0x60 && e <= 0x7F)
    {
        if (e == 0x60 && !ns.OwnIdt)
            return true; // skeleton IDT: intr must exit, H retries VMRUN
        return (ns.HIc1 >> (u32)(e - 0x60)) & 1;
    }
    if (e >= 0x80 && e <= 0x9F)
        return (ns.HIc2 >> (u32)(e - 0x80)) & 1;
    if (e >= 0x40 && e <= 0x5F)
        return !ns.OwnIdt || (((ns.HExc >> (u32)(e - 0x40)) & 1) != 0);
    if (e == 0x400)
        return ns.NestedNp != 0; // H's NPT request owns its NPFs; svmb's
                                 // own NPT faults (NP-less session) are
                                 // ours to serve
    return true; // unknown codes: conservative reflect
}

// r186b: closed self-service for exits H never ordered. The L1 dispatch
// pipeline (view publish, TLB-mode bookkeeping, the r143 interrupt
// repair) is off-limits here - it assumes L1 semantics and froze the
// bench when unordered L2 exits fell through into it (the s2 lesson).
// Reflecting instead is equally fatal for real H's: the reference hypervisor's exit
// switch ends in KeBugCheck(MANUALLY_INITIATED_CRASH) on any exit it
// didn't arm for. Scope v0: the proven-safe trio; everything else
// reflects (conservative - no real driver hits those today).
void NestedSelfServeExit(VcpuContext* vcpu, GuestRegs* regs)
{
    VMCB& vmcb = *vcpu->GuestVmcb;
    const u32 ec = (u32)vmcb.Ctrl.ExitCode;
    // r199 (B): every self-serve exit except the punt itself re-arms the
    // INTR bit - the punt's unarm must cover exactly ONE L2 re-entry
    bool retire = false;
    // r199 (B): every self-serve exit except the punt itself re-arms the
    // INTR bit - the punt's unarm must cover exactly ONE L2 re-entry
    if (g_intrPuntEnabled && ec != 0x60 && !(vmcb.Ctrl.InterceptOpcode1 & 1u))
    {
        vmcb.Ctrl.InterceptOpcode1 |= 1u;
        vmcb.Ctrl.CleanBits.Bits = 0;
    }
    switch (ec)
    {
    case 0x60: // r200 (door 6): INTR punt with a world swap - the single-
               // variable A/B against r199. r199 = controlled entry
               // delivery through the ACTIVE SHADOW root -> 0x139 death.
               // The L1's delivery has been stable for months through the
               // STATIC identity root. This punt swaps NCR3 to that root
               // for the delivery entry (the reference hypervisor's nested table is
               // identity anyway, so every non-hook page - stacks, IDT,
               // page tables - lands at the SAME physical target), the
               // pending vector delivers through the months-proven
               // translation, and the next exit (any class) closes the
               // window (NestedIntrDeliveryWindowClose: shadow back, bit0
               // re-armed). Exposure: the L2 runs under identity until
               // the next exit - H's remapped (hook) slots serve identity
               // for that stretch; two TLB flushes per interrupt.
    {
        static volatile LONG s_punts;
        LONG n = InterlockedIncrement(&s_punts);
        // r201: name the pending vector(s). r198/r199/r200 all died at
        // EXACTLY punt 4 across three different delivery behaviors - one
        // specific pending event looks lethal in any shape. x2APIC MSR
        // read (IRR 0x820-0x827, PPR 0x80A); host-mode RDMSR executes
        // without interception and without needing GIF.
        u32 vec = 0, ppr = 0;
        u64 apicBase = __readmsr(0x1B);
        bool x2 = (apicBase & (1ull << 10)) != 0;
        u32 irr[8] = {};
        if (x2)
        {
            ppr = (u32)__readmsr(0x80A);
            for (int r = 0; r < 8; ++r)
                irr[r] = (u32)__readmsr(0x820 + (unsigned)r);
        }
        else
        {
            // r201: the bench guest runs xAPIC - IRR lives at MMIO
            // 0xFEE00000+0x200 (PPR +0xA0). Map once, lazily (map calls
            // need IRQL <= DISPATCH; punts land at plenty of such
            // moments during the healthy window).
            static void* s_apicVa;
            static volatile LONG s_apicMapState; // 0 untried 1 ok 2 fail
            if (!s_apicMapState && KeGetCurrentIrql() <= DISPATCH_LEVEL)
            {
                PHYSICAL_ADDRESS apa = {};
                apa.QuadPart = 0xFEE00000ull;
                s_apicVa = MmMapIoSpace(apa, 0x1000, MmNonCached);
                s_apicMapState = s_apicVa ? 1 : 2;
            }
            if (s_apicMapState == 1)
            {
                volatile u32* apic = (volatile u32*)s_apicVa;
                ppr = apic[0xA0 >> 2];
                for (int r = 0; r < 8; ++r)
                    irr[r] = apic[(0x200 + r * 0x10) >> 2];
            }
        }
        for (int r = 7; r >= 0; --r)
        {
            if (irr[r])
            {
                unsigned long b = 0;
                _BitScanReverse(&b, irr[r]);
                vec = (u32)(r * 32 + b);
                break;
            }
        }
        if (n <= 16 || (n & 0xFFF) == 0)
            SVMB_LOGI("nested: intr punt n=%ld x2=%u vec=%02x ppr=%02x "
                      "irr7..0=%08x %08x %08x %08x %08x %08x %08x %08x "
                      "rip=%llx",
                      n, (u32)x2, vec, ppr,
                      irr[7], irr[6], irr[5], irr[4],
                      irr[3], irr[2], irr[1], irr[0],
                      (unsigned long long)regs->Rip);
        NestedState& ns = NestedFor(vcpu->Info.CpuIndex);
        vmcb.Ctrl.InterceptOpcode1 &= ~(1u << 0);
        if (ns.NestedNp && !ns.IntrDeliveryWorld)
        {
            vmcb.Ctrl.NCr3 = ns.SaveNcr3;             // L1-stable root
            vmcb.Ctrl.TlbControl = TLB_CTL_FLUSH_ALL; // swap = flush (r163)
            ns.IntrDeliveryWorld = 1;
        }
        vmcb.Ctrl.CleanBits.Bits = 0;
        return; // fault-class, no advance - re-execute the interrupted L2 RIP
    }
    case 0x78: // hlt - r140 semantics: nap a bounded slice, retire the
               // instruction; the idle loop re-hlts on its next pass
        KeStallExecutionProcessor(100);
        retire = true;
        break;
    case 0x72: // cpuid passthrough - L2 is H's guest, not our stealth
               // target: raw leaf, no mask
    {
        int c[4] = {};
        __cpuidex(c, (int)regs->Rax, (int)regs->Rcx);
        regs->Rax = (u32)c[0];
        regs->Rbx = (u32)c[1];
        regs->Rcx = (u32)c[2];
        regs->Rdx = (u32)c[3];
        retire = true;
        break;
    }
    case 0x7C: // rdmsr passthrough (the reference hypervisor arms this itself - this arm
               // only serves other H shapes that left it to us)
    {
        u64 v = ReadGuestMsrPassthrough((u32)regs->Rcx);
        regs->Rax = (u32)v;
        regs->Rdx = (u32)(v >> 32);
        retire = true;
        break;
    }
    case 0x75: // SWINT - svmb's own INTN punt bit (r175 machinery)
               // firing inside an in-place L2: whatever re-armed it, the
               // L2's softint must deliver NATIVELY through its own IDT
               // (reflecting it lands in the reference hypervisor's exit-switch default =
               // KeBugCheck - proven live, twice). Self-healing punt:
               // re-clear the bit and retry the instruction (fault-style,
               // no advance). The re-arm needs the BP bit in the bitmap,
               // which an HExc-driven session never has - one punt per
               // softint, not a loop.
        vmcb.Ctrl.InterceptOpcode1 &= ~(1u << 21);
        vmcb.Ctrl.CleanBits.Bits = 0;
        return; // regs->Rip already the int's own RIP: re-execute natively
    case 0x8B: // mwait - poll-wait emulation. Native mwait parks the TCG
               // vCPU thread and a parked core never takes its pending
               // IRR (r142 family) - the r187 livelock was idle cores
               // parked in mwait while the L2 OS's IPI rendezvous waited
               // for them forever. Nap a bounded slice and RETRY the
               // mwait (no advance - retiring it would fake a wake).
        KeStallExecutionProcessor(100);
        return;
    default:
        NestedReflect(vcpu, regs);
        return;
    }
    if (retire)
    {
        // trap-class: resume at NRip (the entry housekeeping
        // reconstructed it; fall back to +2 if a path left it zero)
        regs->Rip = vmcb.Ctrl.NRip ? vmcb.Ctrl.NRip : regs->Rip + 2;
        vmcb.Save.Rip = regs->Rip;
        vmcb.Save.Rax = regs->Rax;
        vmcb.Save.Rflags = regs->RFlags;
        vmcb.Ctrl.CleanBits.Bits = 0; // we dirtied the save area
    }
}

void NestedIntrDeliveryWindowClose(VcpuContext* vcpu)
{
    NestedState& ns = NestedFor(vcpu->Info.CpuIndex);
    if (!ns.IntrDeliveryWorld)
        return;
    ns.IntrDeliveryWorld = 0;
    VMCB& vmcb = *vcpu->GuestVmcb;
    vmcb.Ctrl.NCr3 = ns.ShadowPml4Pa;             // shadow back
    vmcb.Ctrl.InterceptOpcode1 |= 1u << 0;        // re-arm the punt bit
    vmcb.Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
    vmcb.Ctrl.CleanBits.Bits = 0;
}

void NestedReflect(VcpuContext* vcpu, GuestRegs* regs)
{
    VMCB& vmcb = *vcpu->GuestVmcb;
    NestedState& ns = NestedFor(vcpu->Info.CpuIndex);
    ns.Active = 0;

    // L2's exit record first - the save-area restore below clobbers it
    const u64 exitCode = vmcb.Ctrl.ExitCode;
    const u64 exitInfo1 = vmcb.Ctrl.ExitInfo1;
    const u64 exitInfo2 = vmcb.Ctrl.ExitInfo2;
    const u64 exitIntInfo = vmcb.Ctrl.ExitIntInfo;
    // r168: fault-class exits carry no meaningful NRip on this NRIPS=0
    // bench (the entry-time reconstruction skips them) - zero it in H's
    // copy so stale values never masquerade as one (H hand-computes).
    const u64 exitNRip = FaultClassExit(exitCode) ? 0 : vmcb.Ctrl.NRip;
    const VmcbSave l2Save = vmcb.Save;
    // r181: every reflected L2 exit is the third-party H's to handle -
    // the exit record IS its virtualization-operations telemetry
    VlogReflect(vcpu->Info.CpuIndex, exitCode, exitInfo1, exitInfo2,
                l2Save.Rip);

    // restore L1/H FIRST (intercepts back to svmb's, save area from the
    // stash, GPR frame from the stash - the exit-entry write-back then
    // re-syncs Rip/Rsp/Rax/Rflags from regs into the save area, same
    // values); H's VMCB writes are second so even a vanished H page
    // still resumes L1 sanely.
    vmcb.Ctrl.InterceptOpcode1 = ns.SaveIc1;
    vmcb.Ctrl.InterceptOpcode2 = ns.SaveIc2;
    vmcb.Ctrl.InterceptException = ns.SaveExc;
    vmcb.Ctrl.TscOffset = ns.SaveTscOffset; // r168 compose unwind
    vmcb.Ctrl.NCr3 = ns.SaveNcr3;           // r172 shadow unwind
    // r168b: give L1 its pending event back (armed for the resume VMRUN)
    if (ns.PendingL1Event)
        vmcb.Ctrl.EventInj = ns.SaveEventInj;
    vmcb.Ctrl.VIntr.Data = ns.SaveVIntr;
    vmcb.Ctrl.GuestIntStatus = ns.SaveIntStatus;
    vmcb.Save = ns.HSave;
    vmcb.Ctrl.CleanBits.Bits = 0;
    *regs = ns.HRegs;

    VMCB* h = MapGuestPage(ns.HvmcbPa);
    if (h)
    {
        // exit record + L2's state land in H's VMCB (the nested-SVM ABI)
        h->Ctrl.ExitCode = exitCode;
        h->Ctrl.ExitInfo1 = exitInfo1;
        h->Ctrl.ExitInfo2 = exitInfo2;
        h->Ctrl.ExitIntInfo = exitIntInfo;
        h->Ctrl.NRip = exitNRip;
        h->Save = l2Save;
        SVMB_LOGI("nested: reflect exit=%llx L2rip=%llx -> H rip=%llx",
                  (unsigned long long)exitCode,
                  (unsigned long long)l2Save.Rip,
                  (unsigned long long)ns.HSave.Rip);
    }
}

// ---- r172 nested-NPT shadow v0 ----
// Shadow layout: a 32MB fine-grain window at SHADOW_WINDOW_BASE (4K PTs,
// composed lazily from H's nested NPT) sitting INSIDE a full-4GB blanket
// of 2MB IDENTITY large pages. The blanket exists because L2's own CR3
// walk pages and any other sparse GPAs must translate through the shadow
// too (the first run NPF'd on a walk-table page at 0x7b05c000 - far
// outside any window); identity is semantically safe: svmb's real NPT is
// identity-RWX anyway. The window's non-identity compose is the actual
// feature under test. Perms v0: permissive (P|RW|US, NX off).
// Per-session invalidation: zero the window PTs (blanket persists).
constexpr u64 SHADOW_WINDOW_BASE = 0x400000; // (vestigial: window slots are
                                            // GPA-based since r173)
constexpr u32 SHADOW_PT_SLOTS = 64;          // 64 PTs = 128MB window (r176:
                                            // 16 -> 64; PD0 holds 512 slots)
constexpr u32 SHADOW_PD_COUNT = 8;           // 8GB blanket (PCI-hole
                                            // reclaim puts RAM above 4G)

constexpr u32 HOOK_SLOT_CAP = 64; // r183: materialized fine-grain slots
                                   // per core (H-hooked 2MB regions; the
                                   // pool PTs are pre-allocated so the
                                   // exit-context path never allocates)

struct ShadowPool
{
    void* raw;      // one allocation, page-array base (aligned inside)
    u64 pml4Pa;
    u64 lastRoot;   // r173: H's nested root the window was built for
    u64 lastVmcb;   // r178: + H's VMCB PA - nonpaged-pool LIFO reuse can
                    // hand a NEW world the SAME root PA (the freed
                    // tables' frames recycled); the pair makes a stale
                    // cross-test window far less reachable, and the
                    // tests' teardown INVLPGA closes it outright
    u64 pdptPa;     // r176: link targets cached so invalidation can
    u64 pdPa[8];    // rebuild PDPT links without Mm calls at exit IRQL
    u64* hookPt[HOOK_SLOT_CAP]; // r183: pool PTs for materialized slots
    u32 hookSlot[HOOK_SLOT_CAP];
    u32 hookCount;  // live materializations (slots >= 64 window-region)
    volatile LONG scanDirty; // r183: H's tables may have new hooks -
                             // rescan at the next accept. Set on world
                             // change and on every INVLPGA (the reference hypervisor
                             // pattern edits its NPT then INVLPGAs; real
                             // SVM semantics require the flush too)
};
static ShadowPool s_shadow[NESTED_MAX_CORES];

// ---- r183: hook-slot materialization (full-RAM shadow coverage) ----
// H's NPT structure tells us its intent: a 2MB slot it left as a large
// identity page is blanket material; a slot it SPLIT into a PT is a
// hooked region (the reference hypervisor shape - it splits exactly the slots it
// hooks). We mirror that granularity lazily: scan H's tables, give every
// H-split slot a pre-allocated shadow PT filled from H's PT entries, and
// swap our blanket PD link for a PT link on that slot. Everything else
// stays on the zero-fault 2MB identity blanket. Rescans are dirty-flag
// gated (world change + INVLPGA - the reference hypervisor pattern edits its tables
// then INVLPGAs, which real SVM semantics require anyway).
static u64* ShadowPdsOf(ShadowPool& sp)
{
    u64 base = ((u64)(ULONG_PTR)sp.raw + 0xFFFull) & ~0xFFFull;
    return (u64*)(base + 2 * PAGE_SIZE);
}

static u64* ShadowPtsOf(ShadowPool& sp)
{
    u64 base = ((u64)(ULONG_PTR)sp.raw + 0xFFFull) & ~0xFFFull;
    return (u64*)(base + (2 + SHADOW_PD_COUNT) * PAGE_SIZE);
}

static void ShadowDematerializeAll(u32 cpuIndex)
{
    ShadowPool& sp = s_shadow[cpuIndex];
    if (!sp.raw || !sp.hookCount)
        return;
    u64* pds = ShadowPdsOf(sp);
    for (u32 i = 0; i < sp.hookCount; ++i)
    {
        u32 slot = sp.hookSlot[i];
        if (slot >= SHADOW_PT_SLOTS) // window slots zero with the window
        {
            u32 j = slot >> 9, k = slot & 0x1FF;
            pds[j * 512 + k] = (((u64)j << 30) | ((u64)k << 21)) | 0xE7;
            RtlZeroMemory(sp.hookPt[i], PAGE_SIZE);
        }
    }
    sp.hookCount = 0;
}

// materialize one 2MB slot from H's PT page; false = out of pool slots
static bool ShadowMaterializeSlot(u32 cpuIndex, u32 slot, u64 hPtPa)
{
    ShadowPool& sp = s_shadow[cpuIndex];
    if (sp.hookCount >= HOOK_SLOT_CAP)
        return false;
    u64* hpt = MapGuestPageU64(hPtPa & ~0xFFFull);
    if (!hpt)
        return false;
    u64* pt;
    if (slot < SHADOW_PT_SLOTS)
        pt = ShadowPtsOf(sp) + slot * 512; // window slot: reuse its PT
    else
    {
        pt = sp.hookPt[sp.hookCount];
        sp.hookSlot[sp.hookCount] = slot;
    }
    constexpr u64 hFlagMask = (1u << 1) | (1u << 2) | (1ull << 63);
    u64 slotGpaBase = (u64)slot << 21;
    for (u32 i = 0; i < 512; ++i)
    {
        u64 e = hpt[i];
        pt[i] = (e & 1)
                    ? ((e & 0x000FFFFFFFFFF000ull) | (e & hFlagMask) | 0x61)
                    : 0; // absent in H = absent here: NPF -> compose -> reflect
        // r183 diag: record every NON-IDENTITY entry (the actual hooks)
        u64 gpa = slotGpaBase + ((u64)i << 12);
        if ((e & 1) && (e & 0x000FFFFFFFFFF000ull) != gpa)
            VlogCompose(cpuIndex, gpa, e & 0x000FFFFFFFFFF000ull,
                        e & hFlagMask, 4);
    }
    if (slot >= SHADOW_PT_SLOTS)
    {
        u64 ptPa = MmGetPhysicalAddress(pt).QuadPart;
        ShadowPdsOf(sp)[(slot >> 9) * 512 + (slot & 0x1FF)] = ptPa | 0x7;
        ++sp.hookCount;
    }
    // window slots (<64) only get their PT filled - the window wipe
    // already owns their lifecycle, no registry entry needed
    return true;
}

// r183: the scan is GATED - default OFF. The first bring-up destabilized
// the regression faces (s7 death) and wedges on the second scan inside a
// live accept after an INVLPGA sequence; until r184 roots those, the
// existing nesting faces must run bit-identical to r182. Flip on with
// the shadow-scan ctl (probe Iterations bit12 on stage 32).
static volatile LONG g_hookScanEnabled = 0;

extern "C" LONG svmbHookScanEnable(LONG on)
{
    return InterlockedExchange(&g_hookScanEnabled, on);
}

// returns true when anything was (re)materialized - the caller applies
// a TLB flush so a rematerialized PD link swap cannot be shadowed by a
// stale cached 2M blanket translation
static bool ShadowScanHooks(u32 cpuIndex)
{
    ShadowPool& sp = s_shadow[cpuIndex];
    if (!g_hookScanEnabled || !sp.raw || !sp.scanDirty)
        return false;
    sp.scanDirty = 0;
    ShadowDematerializeAll(cpuIndex);
    NestedState& ns = NestedFor(cpuIndex);
    if (!ns.NestedNp || !ns.HNptRoot)
        return false;
    // walk H's nested NPT: PML4 -> PDPT -> PDs; a PD entry that LINKS a
    // PT (not PS-large) = H split that 2MB slot = hook region. H's
    // low-half identity maps ride PML4 entry [0].
    u64* pml4 = MapGuestPageU64(ns.HNptRoot);
    if (!pml4)
        return false;
    u32 materialized = 0, full = 0;
    u64 pdptPa = pml4[0] & 0x000FFFFFFFFFF000ull;
    u64* pdpt = MapGuestPageU64(pdptPa);
    if (!pdpt)
        return false;
    for (u32 j = 0; j < SHADOW_PD_COUNT; ++j) // our blanket span = 8GB
    {
        u64 pdpte = pdpt[j];
        if (!(pdpte & 1) || (pdpte & 0x80))
            continue; // absent or H maps a whole 1GB large - blanket ok
        u64* pd = MapGuestPageU64(pdpte & 0x000FFFFFFFFFF000ull);
        if (!pd)
            continue;
        for (u32 k = 0; k < 512; ++k)
        {
            u64 pde = pd[k];
            if (!(pde & 1) || (pde & 0x80))
                continue; // absent or 2M large: blanket is correct
            // H split this slot - materialize it from H's PT
            if (!ShadowMaterializeSlot(cpuIndex, j * 512 + k, pde))
            {
                ++full;
                continue;
            }
            ++materialized;
        }
    }
    if (materialized || full)
        SVMB_LOGI("shadow: scan hooks cpu=%u materialized=%u poolFull=%u",
                  cpuIndex, materialized, full);
    return materialized != 0;
}

bool NestedShadowInit(NestedState* ns)
{
    ShadowPool& sp = s_shadow[ns - &NestedFor(0)];
    if (!sp.raw)
    {
        // pml4 + pdpt + 8 pds + 64 window pts + 64 hook-slot pts
        const u32 pages =
            2 + SHADOW_PD_COUNT + SHADOW_PT_SLOTS + HOOK_SLOT_CAP;
        sp.raw = AllocNonPaged((pages + 1) * PAGE_SIZE, TAG_SVMB);
        if (!sp.raw)
            return false;
        u64 base = ((u64)(ULONG_PTR)sp.raw + 0xFFFull) & ~0xFFFull;
        u64* pml4 = (u64*)base;
        u64* pdpt = (u64*)(base + PAGE_SIZE);
        u64* pds = (u64*)(base + 2 * PAGE_SIZE);
        u64* pts = (u64*)(base + (2 + SHADOW_PD_COUNT) * PAGE_SIZE);
        u64* hookPts = pts + SHADOW_PT_SLOTS * 512;
        for (u32 i = 0; i < HOOK_SLOT_CAP; ++i)
            sp.hookPt[i] = hookPts + i * 512;
        sp.hookCount = 0;
        sp.scanDirty = 1;
        sp.pml4Pa = MmGetPhysicalAddress(pml4).QuadPart;
        u64 pdptPa = MmGetPhysicalAddress(pdpt).QuadPart;
        sp.pdptPa = pdptPa;
        pml4[0] = pdptPa | 0x7; // US! QEMU walks the NPT as MMU_USER64 (excp_helper.c case MMU_NESTED_IDX) and probes PTE pages with MMU_DATA_STORE - link entries without US fault P|W|U (r173)
        for (u32 j = 0; j < SHADOW_PD_COUNT; ++j)
        {
            u64* pd = pds + j * 512;
            sp.pdPa[j] = MmGetPhysicalAddress(pd).QuadPart;
            pdpt[j] = sp.pdPa[j] | 0x7;
            for (u32 i = 0; i < 512; ++i)
            {
                if (j == 0 && i < SHADOW_PT_SLOTS)
                    pd[i] = MmGetPhysicalAddress(
                                pts + i * 512).QuadPart | 0x7;
                else // 2MB identity blanket (P|RW|US|A|D|PS) - A/D
                       // pre-set: the guest-walk NPF decode showed QEMU
                       // faults walk accesses on A=0 entries
                    pd[i] = (((u64)j << 30) | ((u64)i << 21)) | 0xE7;
            }
        }
    }
    // r173: the window persists across H's retries (the r172 per-accept
    // zeroing let the interrupt storm starve L2 - every retry re-faulted,
    // re-composed, and got preempted again before one instruction ran).
    // Reset ONLY when H switches its world (= a new view): same-root
    // re-VMRUN keeps the composed entries. r178: the root PA alone is not
    // a world identity - pool reuse can hand a rebuilt world the same
    // root PA - so the H vmcb PA joins the key.
    u64 base = ((u64)(ULONG_PTR)s_shadow[ns - &NestedFor(0)].raw +
                0xFFFull) & ~0xFFFull;
    u64* pts = (u64*)(base + (2 + SHADOW_PD_COUNT) * PAGE_SIZE);
    if (sp.lastRoot != ns->HNptRoot || sp.lastVmcb != ns->HvmcbPa)
    {
        RtlZeroMemory(pts, (u64)SHADOW_PT_SLOTS * PAGE_SIZE);
        ShadowDematerializeAll((u32)(ns - &NestedFor(0)));
        sp.lastRoot = ns->HNptRoot;
        sp.lastVmcb = ns->HvmcbPa;
        sp.scanDirty = 1;
        // r189: an in-place session prefills the window with identity
        // 4K entries. The L1's interrupt delivery is stable because its
        // stack pages ride the identity blanket (never fault); an
        // in-place L2's stack pages can land in the lazy window, and a
        // cold stack page faulting DURING QEMU's delivery push loses
        // the interrupt (r168 seam family) - IRRs pile up, the OS
        // freezes in IPI waits (three identical wedges: all-core
        // rendezvous, ISR=0x209 never EOId, no dump). the reference hypervisor's NPT is
        // identity at init so the prefill is exact; its later NPT edits
        // still ride the INVLPGA wipe/rescan contract.
        if (ns->OwnIdt)
        {
            for (u32 i = 0; i < SHADOW_PT_SLOTS; ++i)
                for (u32 k = 0; k < 512; ++k)
                    pts[i * 512 + k] =
                        ((((u64)i << 21) | ((u64)k << 12))) | 0x67;
        }
    }
    ns->ShadowPml4Pa = sp.pml4Pa;
    return true;
}

static u64* MapGuestPageU64(u64 gpa)
{
    return (u64*)MapGuestPage(gpa & ~0xFFFull);
}

bool NestedShadowCompose(u32 cpuIndex, u64 l2Gpa, u64 npfInfo1)
{
    NestedState& ns = NestedFor(cpuIndex);
    // the PTs are wired at pd[0..63] = GPA slots counted from ZERO -
    // the r172 window-base-relative math wrote PT slot (gpa-4MB)>>21
    // while the walk reads slot gpa>>21: an off-by-window bug that
    // looped compose-fault-compose forever. Window = GPA < 128MB.
    if (!ns.NestedNp || l2Gpa >= (u64)SHADOW_PT_SLOTS * 0x200000)
    {
        // r172 diag: dump the shadow's own view of this GPA
        ShadowPool* dsp = &s_shadow[cpuIndex];
        u64* dpml4 = dsp->raw
            ? (u64*)((((u64)(ULONG_PTR)dsp->raw + 0xFFFull) &
                      ~0xFFFull))
            : nullptr;
        u64 pml4e = dpml4 ? dpml4[(l2Gpa >> 39) & 0x1FF] : 0;
        u64* dpdpt = dpml4
            ? (u64*)MapGuestPage(pml4e & ~0xFFFull) : nullptr;
        u64 pdpte = dpdpt ? dpdpt[(l2Gpa >> 30) & 0x1FF] : 0;
        SVMB_LOGI("shadow: decline gpa=%llx np=%u pml4e=%llx pdpte=%llx "
                  "shadowNcr3=%llx",
                  (unsigned long long)l2Gpa, ns.NestedNp,
                  (unsigned long long)pml4e,
                  (unsigned long long)pdpte,
                  (unsigned long long)ns.ShadowPml4Pa);
        return false; // outside the v1 window: reflect to H
    }
    ShadowPool& sp = s_shadow[cpuIndex];
    if (!sp.raw)
    {
        SVMB_LOGI("shadow: decline gpa=%llx (no pool)",
                  (unsigned long long)l2Gpa);
        return false;
    }

    // software-walk H's nested NPT (L1-GPAs via the direct map)
    u64 e = ns.HNptRoot;
    u64 leafEnt = 0;   // r176: H's level-3 entry verbatim (perms source)
    u64 largeEnt = 0;  // r176: H's large-page entry if one terminated
    u32 largeLevel = 0;
    for (u32 level = 0; level < 4; ++level)
    {
        u64* tbl = MapGuestPageU64(e);
        if (!tbl)
        {
            SVMB_LOGI("shadow: walk map fail lvl=%u e=%llx", level,
                      (unsigned long long)e);
            return false;
        }
        u32 idx = (u32)((l2Gpa >> (39 - 9 * level)) & 0x1FF);
        u64 ent = tbl[idx];
        if (!(ent & 1))
        {
            // r181: a GUEST-WALK access (bit32/33 - the L2 page-table
            // walk touching L2's own PT/CR3 frames) to a GPA H's nested
            // NPT does not map falls back to IDENTITY - this was the
            // r172 blanket's job, which the r176 window expansion
            // (16->64 slots) quietly ate inside [0,128MB): a walk page
            // landing in-window met an empty PT and the decline broke
            // the very first L2 fetch (live bite: walk page at
            // 0x7674000, EXT-H bailed on the reflected NPF). Real
            // (non-walk) accesses to unmapped GPAs still reflect - that
            // is H's fault to run.
            if (npfInfo1 & ((1ull << 32) | (1ull << 33)))
            {
                u64 wbase = ((u64)(ULONG_PTR)sp.raw + 0xFFFull) &
                            ~0xFFFull;
                u64* wpts = (u64*)(wbase +
                                   (2 + SHADOW_PD_COUNT) * PAGE_SIZE);
                u64* wpt = wpts + (l2Gpa >> 21) * 512;
                wpt[(l2Gpa >> 12) & 0x1FF] =
                    l2Gpa & ~0xFFFull | 0x67; // identity, P|RW|US|A|D
                VlogCompose(cpuIndex, l2Gpa, l2Gpa & ~0xFFFull, 0x67, 3);
                return true;
            }
            SVMB_LOGI("shadow: walk !P lvl=%u idx=%u ent=%llx root=%llx "
                      "gpa=%llx",
                      level, idx, (unsigned long long)ent,
                      (unsigned long long)ns.HNptRoot,
                      (unsigned long long)l2Gpa);
            return false; // H's walk faults: reflect the NPF to H
        }
        if (level < 3 && (ent & 0x80))
        {
            largeEnt = ent;
            largeLevel = level;
            break;
        }
        if (level == 3)
            leafEnt = ent;
        e = ent & 0x000FFFFFFFFFF000ull;
    }
    u64 base = ((u64)(ULONG_PTR)sp.raw + 0xFFFull) & ~0xFFFull;
    u64* pts = (u64*)(base + (2 + SHADOW_PD_COUNT) * PAGE_SIZE);
    // r176 perms: H's RW/US/NX ride through verbatim (P|A|D forced - A/D
    // pre-set kills the QEMU walk-probe fault storm; the r173 US law
    // applies to LINK entries, leaves mirror H's intent: an H-NX page
    // executing in L2 NPFs back to H, an H-RO page writing in L2 NPFs
    // back to H - correct nesting semantics instead of v0's permissive
    // blanket).
    constexpr u64 hFlagMask = (1u << 1) | (1u << 2) | (1ull << 63);
    // r176 fault-class validation: a composed leaf that would STILL fault
    // this exact access is H's fault to handle - composing it and resuming
    // would loop the NPF forever. H's perms mirror through verbatim, so
    // exec-on-NX / write-on-RO / user-on-supervisor reflect to H.
    auto permsDeny = [&](u64 fl) -> bool {
        if ((npfInfo1 & NPF_EXECUTE) && (fl & (1ull << 63)))
            return true;
        if ((npfInfo1 & NPF_WRITE) && !(fl & (1u << 1)))
            return true;
        if ((npfInfo1 & (1ull << 2)) && !(fl & (1u << 2))) // user access
            return true;
        return false;
    };
    if (largeEnt)
    {
        if (permsDeny(largeEnt & hFlagMask))
        {
            SVMB_LOGI("shadow: perms deny (large) gpa=%llx info1=%llx "
                      "fl=%llx",
                      (unsigned long long)l2Gpa,
                      (unsigned long long)npfInfo1,
                      (unsigned long long)(largeEnt & hFlagMask));
            return false;
        }
        if (largeLevel == 2)
        {
            // H maps this 2MB slot with one large entry: express it as a
            // 512-entry fill of the shadow PT slot (window slots are PT
            // links, not PS-capable). One fill covers the whole slot -
            // later faults in the same 2MB hit present entries.
            u64 t2m = (largeEnt & 0x000FFFFFFFFFF000ull) & ~0x1FFFFFull;
            u64 fl = largeEnt & hFlagMask;
            u64 ptIdx = l2Gpa >> 21;
            u64* pt = pts + ptIdx * 512;
            for (u32 i = 0; i < 512; ++i)
                pt[i] = (t2m + ((u64)i << 12)) | fl | 0x61;
            SVMB_LOGI("nested: shadow compose 2M gpa=%llx -> %llx",
                      (unsigned long long)l2Gpa,
                      (unsigned long long)t2m);
            VlogCompose(cpuIndex, l2Gpa, t2m, fl | 0x61, 1);
            return true;
        }
        if (largeLevel == 1)
        {
            // H maps a whole 1GB: the shadow PDPT slot becomes the large
            // leaf itself (PDPT links only matter as walk probes - a PS
            // leaf ends the walk). GB0 is declined - the window lives
            // there and its PD0 link must survive. ShadowInvalidateAll
            // rebuilds these links when H revokes the mapping.
            if (((l2Gpa >> 30) & 0x1FF) == 0)
            {
                SVMB_LOGI("shadow: 1G large at GB0 declined (window)");
                return false;
            }
            u64 t1g = (largeEnt & 0x000FFFFFFFFFF000ull) & ~0x3FFFFFFFull;
            u64* pdpt = (u64*)(base + PAGE_SIZE);
            pdpt[(l2Gpa >> 30) & 0x1FF] =
                t1g | (largeEnt & hFlagMask) | 0xE1; // P|A|D|PS
            SVMB_LOGI("nested: shadow compose 1G gpa=%llx -> %llx",
                      (unsigned long long)l2Gpa,
                      (unsigned long long)t1g);
            VlogCompose(cpuIndex, l2Gpa, t1g,
                        (largeEnt & hFlagMask) | 0xE1, 2);
            return true;
        }
        SVMB_LOGI("shadow: large page lvl=%u (512GB - declined)",
                  largeLevel);
        return false; // 512GB large page: reflect to H
    }
    // compose: shadow PT entry maps l2Gpa -> H's target L1-GPA
    if (permsDeny(leafEnt & hFlagMask))
    {
        SVMB_LOGI("shadow: perms deny gpa=%llx info1=%llx fl=%llx",
                  (unsigned long long)l2Gpa,
                  (unsigned long long)npfInfo1,
                  (unsigned long long)(leafEnt & hFlagMask));
        return false;
    }
    u64 ptIdx = l2Gpa >> 21; // PD slot index (from GPA zero)
    u64* pt = pts + ptIdx * 512;
    pt[(l2Gpa >> 12) & 0x1FF] = e | (leafEnt & hFlagMask) | 0x61;
    SVMB_LOGI("nested: shadow compose gpa=%llx -> %llx",
              (unsigned long long)l2Gpa, (unsigned long long)e);
    VlogCompose(cpuIndex, l2Gpa, e, (leafEnt & hFlagMask) | 0x61, 0);
    return true;
}

// r176: wipe every composed shadow window in the machine. Zeroing another
// core's PTs races that core's compose benignly (anything composed after
// the wipe re-walked H's CURRENT tables); lastRoot=0 makes the next accept
// re-init too. Blanket untouched.
static void ShadowInvalidateAll()
{
    for (u32 i = 0; i < NESTED_MAX_CORES; ++i)
    {
        ShadowPool& sp = s_shadow[i];
        if (!sp.raw)
            continue;
        u64 base = ((u64)(ULONG_PTR)sp.raw + 0xFFFull) & ~0xFFFull;
        u64* pdpt = (u64*)(base + PAGE_SIZE);
        u64* pts = (u64*)(base + (2 + SHADOW_PD_COUNT) * PAGE_SIZE);
        RtlZeroMemory(pts, (u64)SHADOW_PT_SLOTS * PAGE_SIZE);
        // r176: a composed 1G large leaf replaced a PDPT link - restore
        // the blanket link (cached PAs; no Mm calls at exit IRQL)
        for (u32 j = 1; j < SHADOW_PD_COUNT; ++j)
            if (pdpt[j] & 0x80) // PS bit set = was made a large leaf
                pdpt[j] = sp.pdPa[j] | 0x7;
        // r183: hook slots die too - their truth source (H's tables)
        // changed; the INVLPGA caller rescans and rematerializes
        ShadowDematerializeAll(i);
        sp.lastRoot = 0;
        sp.lastVmcb = 0; // r178: full teardown - both key halves die
        sp.scanDirty = 1;
    }
    // cores with live sessions may hold cached composites: flush them at
    // their own exit entry through the r108 pending-kick protocol (never
    // cross-write a VMCB)
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || !hv->IsRunning())
        return;
    hv->ForEachVcpu([](VcpuContext* v) {
        if (NestedActive(v->Info.CpuIndex))
            InterlockedExchange(&v->Info.PendingTlbKick, 1);
    });
}

bool NestedInvlpgaExit(GuestContext& ctx, void* ud)
{
    UNREFERENCED_PARAMETER(ud);
    // re-issue the invalidation natively from the host side (same
    // physical TLB, ASID-tagged): non-nested callers - the driver's own
    // guest-context hook paths - keep their semantics transparently
    _svmb_invlpga(ctx.Regs->Rax, (u32)ctx.Regs->Rcx);
    ShadowInvalidateAll(); // wipes + dematerializes + scanDirty on every
    // core + kicks live ones; r184: NO inline cross-core rescan here -
    // mutating another core's pool while its L2 runs it as the live NPT
    // root is the r108 race class. Each core rescans at its own next
    // accept (the one point its L2 is provably parked)
    SVMB_LOGI("nested: invlpga rip=%llx gva=%llx asid=%u -> shadows wiped",
              (unsigned long long)ctx.Regs->Rip,
              (unsigned long long)ctx.Regs->Rax,
              (u32)ctx.Regs->Rcx);
    ctx.AdvanceRip = true;
    return true;
}

bool NestedVmloadExit(GuestContext& ctx, void* ud)
{
    UNREFERENCED_PARAMETER(ud);
    // r182: VMLOAD/VMSAVE EMULATION (replaces the r168 #GP refusal).
    // A real-H-shaped driver - the reference hypervisor asm pattern - executes
    // vmload guestVmcb BEFORE its vmrun (lining the CPU save area up
    // from its VMCB) and vmsave guestVmcb / vmload hostVmcb around every
    // exit; the old refusal killed its launch at the very first vmload.
    //
    // Emulation (operand RAX = VMCB PA, APM field set):
    //   VMLOAD: copy segments (ES..GS/GDTR/LDTR/IDTR/TR), EFER and CPL
    //           from H's VMCB into the REAL save area
    //   VMSAVE: copy the set from the real save area back into H's VMCB
    // RIP/RSP/RFLAGS/CRs are NOT part of the VMLOAD set - they ride the
    // VMRUN/reflect contract untouched. For the in-place H shape this
    // serves, L1 segments == L2 segments (skeleton-inherit), so both
    // directions are value-preserving.
    u64 pa = ctx.Regs->Rax & ~0xFFFull;
    const bool isLoad = ctx.Exit == vmexit::VMLOAD;
    if (!pa || !PaPlausible(pa))
    {
        SVMB_LOGW("nested: %s bad pa=%llx rip=%llx -> #GP",
                  isLoad ? "vmload" : "vmsave",
                  (unsigned long long)pa,
                  (unsigned long long)ctx.Regs->Rip);
        InjectEvent(ctx, EXC_GP, EVT_EXCEPTION, false, 0);
        ctx.AdvanceRip = true;
        return true;
    }
    VMCB* h = MapGuestPage(pa);
    if (!h)
    {
        SVMB_LOGW("nested: %s map fail pa=%llx -> #GP",
                  isLoad ? "vmload" : "vmsave",
                  (unsigned long long)pa);
        InjectEvent(ctx, EXC_GP, EVT_EXCEPTION, false, 0);
        ctx.AdvanceRip = true;
        return true;
    }
    VMCB& real = *ctx.Vmcb();
    auto copySet = [](VMCB& to, const VMCB& from)
    {
        to.Save.Es = from.Save.Es;
        to.Save.Cs = from.Save.Cs;
        to.Save.Ss = from.Save.Ss;
        to.Save.Ds = from.Save.Ds;
        to.Save.Fs = from.Save.Fs;
        to.Save.Gs = from.Save.Gs;
        to.Save.Gdtr = from.Save.Gdtr;
        to.Save.Ldtr = from.Save.Ldtr;
        to.Save.Idtr = from.Save.Idtr;
        to.Save.Tr = from.Save.Tr;
        to.Save.Cpl = from.Save.Cpl;
    };
    if (isLoad)
    {
        copySet(real, *h);
        // EFER: H's copy rides EXCEPT the architecture-critical bits -
        // LME/LMA/NXE/SVME stay the real VMCB's (a zeroed or partial H
        // VMCB must never strip long mode off the machine: the first
        // emulation run did exactly that and took QEMU down with it)
        constexpr u64 keep = (1ull << 8) | (1ull << 10) | (1ull << 11) |
                             (1ull << EFER_SVME); // NXE|LME|LMA|SVME
        real.Save.Efer = (h->Save.Efer & ~keep) | (real.Save.Efer & keep);
    }
    else
    {
        copySet(*h, real);
        h->Save.Efer = real.Save.Efer;
    }
    SVMB_LOGI("nested: %s emulated pa=%llx rip=%llx",
              isLoad ? "vmload" : "vmsave",
              (unsigned long long)pa,
              (unsigned long long)ctx.Regs->Rip);
    ctx.AdvanceRip = true;
    return true;
}


// ---- r171 per-H virtual SVM view ----
struct HViewSlot
{
    volatile LONG Active;
    u64 Cr3;
    u64 Efer;   // virtual: H's belief, seeded from the real masked view
    u64 Hsave;  // virtual: H's own claimed HSAVE PA
    u64 VmCr;   // virtual
};
static HViewSlot s_hview[4];

static HViewSlot* HViewFind(u64 cr3)
{
    for (u32 i = 0; i < 4; ++i)
        if (s_hview[i].Active && s_hview[i].Cr3 == cr3)
            return &s_hview[i];
    return nullptr;
}

bool NestedHViewActive(u64 cr3) { return HViewFind(cr3) != nullptr; }

void NestedHViewRegister(u64 cr3)
{
    for (u32 i = 0; i < 4; ++i)
    {
        if (InterlockedCompareExchange(&s_hview[i].Active, 1, 0) == 0)
        {
            s_hview[i].Cr3 = cr3;
            LogSerialTraceF("R185HREG slot=%u cr3=%016llx", i, cr3);
            VlogRecord(6, 0, 0x186, i, cr3, 0);
            // seed: the stealth face's answers, SVME clear (H has not
            // enabled yet), HSAVE none
            u64 efer = ReadGuestMsrPassthrough(MSR_EFER);
            s_hview[i].Efer = efer & ~(1ull << EFER_SVME);
            s_hview[i].Hsave = 0;
            u64 vmcr = ReadGuestMsrPassthrough(MSR_VM_CR);
            s_hview[i].VmCr = vmcr & ~(1ull << VM_CR_SVMDIS);
            return;
        }
    }
}

void NestedHViewUnregister(u64 cr3)
{
    HViewSlot* v = HViewFind(cr3);
    if (v)
        InterlockedExchange(&v->Active, 0);
}

u64 NestedHViewServe(u64 cr3, bool isWrite, u32 msr, u64 value,
                     bool* served)
{
    *served = false;
    HViewSlot* v = HViewFind(cr3);
    if (!v)
        return 0;
    u64* dst = msr == MSR_EFER ? &v->Efer
              : msr == MSR_VM_HSAVE_PA ? &v->Hsave
              : msr == MSR_VM_CR ? &v->VmCr : nullptr;
    if (!dst)
        return 0; // not an SVM-config MSR: fall to the default handling
    *served = true;
    if (msr == MSR_EFER)
    {
        if (isWrite)
            v->Efer = value; // H's belief state - real MSR untouched
        return v->Efer;
    }
    if (isWrite)
        *dst = value;
    return *dst;
}

} // namespace svmb
