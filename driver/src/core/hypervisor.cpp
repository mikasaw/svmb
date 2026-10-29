#include "core/hypervisor.h"
#include "core/crumbs.h"
#include "hw/segments.h"
#include "mm/npf.h"
#include "mm/tlb.h"
#include "hw/svm_defs.h"
#include "platform/logger.h"
#include "platform/util.h"
#include "core/nested.h"         // r166: L2 exit reflection gate
#include "platform/marker.h"


namespace svmb
{

Hypervisor* Hypervisor::gInstance = nullptr;

// see hypervisor.h - registry-overridable diagnosis switches
volatile LONG GuestCpuidHideEnabled = 1;
volatile LONG TscProbeEnabled = 0;
// 2026-09-06 bisect: MSR VM_HSAVE_PA interception on the guest caused
// vCPU shutdown / Triple fault under VMware nested AMD-V (reproduced: HideCpuid=0
// + ProtectHsave=1 still crashed at vcpu-0 02:40:47). The host CPU apparently
// rejects guest writes to MSR VM_HSAVE_PA once the L1 hypervisor has already
// loaded an HSAVE_PA. Default off; the registry switch remains for forensics.
volatile LONG HsaveProtectionEnabled = 0;

namespace
{

struct MachineFrame
{
    u64 Rip;
    u64 Cs;
    u64 EFlags;
    u64 OldRsp;
    u64 Ss;
};

// Set by CheckCpu() when CPUID.8000000A.EDX.NRIPS is absent (QEMU TCG).
// SvmbVmExitEntry reconstructs Ctrl.NRip for every intercept-class exit.
static bool s_noNripsCompat = false;

// VMEXIT class table: whether NRip points at the NEXT instruction (advance)
// or at the faulting instruction (re-execute)
bool IsFaultClassExit(u64 exit)
{
    if (exit >= vmexit::EXCEPTION(0) && exit <= vmexit::EXCEPTION(31))
        return true;
    return exit == vmexit::NPF || exit == vmexit::SHUTDOWN || exit == vmexit::INTR ||
           exit == vmexit::NMI || exit == vmexit::SMI || exit == vmexit::INIT ||
           exit == vmexit::VINTR;
}

// TCG NRIP fallback: byte length of the instruction that raised an
// intercept-class exit. Closed set - svmb only ever sees exits its own
// intercept bitmap enables (baseline CPUID/MSR/VMRUN plus module-added
// CR/DR/VMMCALL/RDTSCP-family bits), and every one of those instructions
// is fixed-length with no prefix games in the kernel paths we trap.
// Returns 0 for exits with no confident length (variable/prefixed);
// those are not in svmb's intercept set today and are logged loudly.
static u32 InterceptInstrLen(u64 exit)
{
    u32 lo = (u32)exit;
    switch (exit)
    {
    case vmexit::CPUID:            return 2;  // 0F A2
    case vmexit::MSR:              return 2;  // 0F 30 / 0F 32 (RDMSR/WRMSR)
    case vmexit::INTN:             return 1;  // 0xCC int3 (0xCD imm8 = 2 -
                                              // HandleSwintExit advances by
                                              // its own byte-read; this entry
                                              // only keeps the no-NRIP
                                              // reconstruction quiet)
    case vmexit::RDTSC:            return 2;  // 0F 31
    case vmexit::RDPMC:            return 2;  // 0F 33
    case vmexit::INVD:             return 2;  // 0F 08
    case vmexit::WBINVD:           return 2;  // 0F 09
    case vmexit::PAUSE:            return 2;  // F3 90
    case vmexit::HLT:              return 1;  // F4
    case vmexit::ICEBP:            return 1;  // F1
    case vmexit::VMRUN:            return 3;  // 0F 01 D8
    case vmexit::VMMCALL:          return 3;  // 0F 01 D9
    case vmexit::VMLOAD:           return 3;  // 0F 01 DA
    case vmexit::VMSAVE:           return 3;  // 0F 01 DB
    case vmexit::STGI:             return 3;  // 0F 01 DC
    case vmexit::CLGI:             return 3;  // 0F 01 DD
    case vmexit::SKINIT:           return 3;  // 0F 01 DE
    case vmexit::INVLPGA:          return 3;  // 0F 01 DF
    case vmexit::RDTSCP:           return 3;  // 0F 01 F9
    case vmexit::MONITOR:          return 3;  // 0F 01 C8
    case vmexit::MWAIT:            return 3;  // 0F 01 C9
    case vmexit::XSETBV:           return 3;  // 0F 01 D6
    case vmexit::EFER_WRITE_TRAP:  return 2;  // underlying WRMSR
    default:
        // MOV CRn / MOV DRn: 0F 20/22 / 0F 21/23 + modrm (reg-reg, no SIB)
        if ((lo >= (u32)vmexit::CR_READ(0)  && lo <= (u32)vmexit::CR_WRITE(15)) ||
            (lo >= (u32)vmexit::DR_READ(0)  && lo <= (u32)vmexit::DR_WRITE(15)) ||
            (lo >= (u32)vmexit::CR_WRITE_TRAP(0) && lo <= (u32)vmexit::CR_WRITE_TRAP(15)) ||
            (lo >= (u32)vmexit::IDTR_READ && lo <= (u32)vmexit::TR_READ) ||   // SIDT/SGDT/SLDT/STR
            (lo >= (u32)vmexit::IDTR_WRITE && lo <= (u32)vmexit::TR_WRITE))    // LIDT/LGDT/LLDT/LTR
            return 3;
        return 0;
    }
}

} // namespace

bool Hypervisor::TcgCompat() { return s_noNripsCompat; }

SvmCheckResult Hypervisor::CheckCpu()
{
    // SVM availability probe. Every branch logs the *reason* at the point
    // of rejection so that "SVM feature is not supported" never reaches
    // the user without a specific cause. The four CPUID/MSR queries and
    // the resulting decision are logged once per Start().
    int out[4] = {};
    __cpuid(out, 0);
    char vendor[13] = {};
    memcpy(vendor + 0, &out[1], 4);
    memcpy(vendor + 4, &out[3], 4);
    memcpy(vendor + 8, &out[2], 4);
    if (strcmp(vendor, "AuthenticAMD") != 0)
    {
        SVMB_LOGE("svm check: not AMD cpu, vendor='%.12s'", vendor);
        return SvmCheckResult::NotAmd;
    }

    __cpuidex(out, CPUID_EXT_FEATURES, 0);
    SVMB_LOGI("svm check: CPUID.80000001.ECX=%08x (SVM=%u)",
              (u32)out[2], (unsigned)((out[2] >> SVM_CPUID_BIT) & 1));
    if (!(out[2] & (1 << SVM_CPUID_BIT)))
    {
        SVMB_LOGE("svm check: SVM CPUID bit is 0 - L1 hypervisor or BIOS "
                  "disabled SVM. ECX=%08x", (u32)out[2]);
        return SvmCheckResult::NoSvmBit;
    }

    // VM_CR.SVMDIS bit is the only firmware-level disable we honor; SVMDIS_LOCK
    // is a separate lockout bit that VMware Workstation (vhv.enable=TRUE) sets
    // to 1 to keep the L1 hypervisor exclusive, but it does NOT prevent guest
    // VMRUN. Round 10 (2026-09-07) cross-tested with a reference educational hypervisor on the same VM
    // confirmed that VMRUN works with SVMDIS_LOCK=1.
    u64 vmcr = __readmsr(MSR_VM_CR);
    if (vmcr & (1ull << VM_CR_SVMDIS))
    {
        SVMB_LOGE("svm check: VM_CR.SVMDIS=1 - SVM disabled by firmware");
        return SvmCheckResult::SvmdisSet;
    }

    __cpuidex(out, CPUID_SVM_FEATURES, 0);
    if (!(out[3] & (1 << NPT_CPUID_BIT)))
    {
        SVMB_LOGE("svm check: NPT not supported (CPUID.8000000A.EDX=%08x)",
                  (u32)out[3]);
        return SvmCheckResult::NoNpt;
    }
    // QEMU TCG's SVM emulation has no NRIP-save: do_vmexit() writes
    // Save.Rip (start of the intercepted instruction) but never
    // Ctrl.NRip (tcg/system/svm_helper.c). svmb's intercept set is closed
    // and every member is a fixed-length instruction, so the exit path
    // reconstructs NRip from an exit-code -> instruction-length table
    // (see SvmbVmExitEntry) instead of refusing to virtualize.
    if (!(out[3] & (1 << NRIPS_CPUID_BIT)))
    {
        s_noNripsCompat = true;
        SVMB_LOGW("svm check: NRIPS not supported (CPUID.8000000A.EDX=%08x)"
                  " - TCG fallback: fixed-length nrip advance",
                  (u32)out[3]);
    }

    SVMB_LOGI("svm check: OK (AMD cpu, SVM bit=1, VM_CR.SVMDIS=0, NPT supported, NRIPS=%u)",
              (unsigned)!s_noNripsCompat);
    return SvmCheckResult::Ok;
}


NTSTATUS Hypervisor::Start()
{
    if (Running_)
        return STATUS_ALREADY_REGISTERED;

    SvmCheckResult check = CheckCpu();
    if (check != SvmCheckResult::Ok)
    {
        SVMB_LOGE("SVM check failed: %u (AMD CPU + SVM/NVBit in BIOS/VMware required)", (u32)check);
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    NTSTATUS status;
    CoreCount_ = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    TraceMarker("start-enter"); // r141: death-proof progress channel

    // single failure exit: anything below that fails runs Stop(), which is
    // safe pre-enter (all vcpu states are Off -> devirtualize pass no-ops)
#define SVMB_START_FAIL(res) { Stop(); return (res); }

    status = Msrpm_.Init();
    if (!NT_SUCCESS(status))
        return status; // nothing allocated yet

    status = Dispatcher_.Init();
    if (!NT_SUCCESS(status))
        SVMB_START_FAIL(status)

    status = Intercepts_.Init();
    if (!NT_SUCCESS(status))
        SVMB_START_FAIL(status)

    status = Hypercalls_.Init();
    if (!NT_SUCCESS(status))
        SVMB_START_FAIL(status)

    status = Modules_.Init(this);
    if (!NT_SUCCESS(status))
        SVMB_START_FAIL(status)

    // ---- wire built-in handlers ----
    Hypercalls_.Register(HC_PROBE, Hypercall::HcProbe, nullptr, CORE_TOKEN);
    Hypercalls_.Register(HC_VERSION, Hypercall::HcVersion, nullptr, CORE_TOKEN);
    Hypercalls_.Register(HC_EXIT_VMM, Hypercall::HcExitVmm, nullptr, CORE_TOKEN);

    Dispatcher_.Register(vmexit::CPUID, HandleCpuidExit, &Hypercalls_, CORE_TOKEN, 0);
    Dispatcher_.Register(vmexit::MSR, HandleMsrExit, nullptr, CORE_TOKEN, 0);
    Dispatcher_.Register(vmexit::VMMCALL, HandleVmmcallExit, &Hypercalls_, CORE_TOKEN, 0);
    Dispatcher_.Register(vmexit::VMRUN, HandleVmrunExit, nullptr, CORE_TOKEN, 0);
    // r168: explicit #GP refusal - native VMLOAD/VMSAVE would rewrite the
    // real save area through a VMCB svmb does not own (see nested.cpp)
    Dispatcher_.Register(vmexit::VMLOAD, NestedVmloadExit, nullptr,
                         CORE_TOKEN, 0);
    Dispatcher_.Register(vmexit::VMSAVE, NestedVmloadExit, nullptr,
                         CORE_TOKEN, 0);
    // r176: INVLPGA intercept - H's "flush nested translations" is the
    // nested-shadow invalidation contract (wipe all composed windows +
    // TLB-kick live sessions; the native invalidation is re-issued
    // host-side so non-nested callers stay transparent). Bit armed under
    // TcgCompat in InterceptManager::ApplyToVcpu.
    Dispatcher_.Register(vmexit::INVLPGA, NestedInvlpgaExit, nullptr,
                         CORE_TOKEN, 0);
    // r140 TCG compat: hlt emulation exit (bit armed only when TcgCompat()
    // - see InterceptManager::ApplyCore). Handler itself is inert unless
    // the guest actually takes HLT exits.
    Dispatcher_.Register(vmexit::HLT, HandleHltExit, nullptr, CORE_TOKEN, 0);
    // M3 skeleton: NPF handler declines while NPT is disabled, so
    // registering it unconditionally is safe for the crash-line work
    Dispatcher_.Register(vmexit::NPF, HandleNpfExit, NptInstance(),
                         CORE_TOKEN, 0);
    Hypercalls_.Register(HC_UPDATE_BARRIER, Hypercall::HcUpdateBarrier,
                         nullptr, CORE_TOKEN);

    Intercepts_.SetMsrpm(&Msrpm_);
    // protect EFER / VM_CR / VM_HSAVE_PA from guest tampering (always-on;
    // HSAVE can be disabled via Parameters\ProtectHsave for bisecting)
    Intercepts_.RequireMsr(CORE_TOKEN, MSR_EFER, true, true);
    Intercepts_.RequireMsr(CORE_TOKEN, MSR_VM_CR, true, true);
    if (HsaveProtectionEnabled)
        Intercepts_.RequireMsr(CORE_TOKEN, MSR_VM_HSAVE_PA, true, true);

    gInstance = this;

    // ---- allocate per-core contexts (page-aligned blobs) ----
    Vcpus_ = (VcpuContext**)AllocNonPaged(sizeof(VcpuContext*) * CoreCount_, TAG_VCPU);
    if (!Vcpus_)
        SVMB_START_FAIL(STATUS_INSUFFICIENT_RESOURCES)
    RtlZeroMemory(Vcpus_, sizeof(VcpuContext*) * CoreCount_);

    SIZE_T blobSize = sizeof(VcpuContext) + PAGE_SIZE; // slack for manual align
    for (u32 i = 0; i < CoreCount_; ++i)
    {
        void* raw = AllocNonPaged(blobSize, TAG_VCPU);
        if (!raw)
            SVMB_START_FAIL(STATUS_INSUFFICIENT_RESOURCES)
        u64 aligned = ((u64)(ULONG_PTR)raw + PAGE_SIZE - 1) & ~(u64)(PAGE_SIZE - 1);
        VcpuContext* v = (VcpuContext*)aligned;
        RtlZeroMemory(v, sizeof(VcpuContext));
        // round-27 exp3: VMCB/HSAVE get their own physically-contiguous
        // pages (svm-base reference pattern) instead of riding inside the
        // per-core pool blob. Tests whether VMware's L1 cares about the
        // physical neighbourhood of the VMCB/HSAVE frames it reads on
        // every VMRUN crossing.
        v->GuestVmcb = (VMCB*)AllocContiguous(sizeof(VMCB), TAG_VCPU);
        v->HostVmcb = (VMCB*)AllocContiguous(sizeof(VMCB), TAG_VCPU);
        v->HSave = (u8*)AllocContiguous(PAGE_SIZE, TAG_VCPU);
        if (!v->GuestVmcb || !v->HostVmcb || !v->HSave)
            SVMB_START_FAIL(STATUS_INSUFFICIENT_RESOURCES)
        if ((MmGetPhysicalAddress(v->GuestVmcb).QuadPart & (PAGE_SIZE - 1)) ||
            (MmGetPhysicalAddress(v->HostVmcb).QuadPart & (PAGE_SIZE - 1)) ||
            (MmGetPhysicalAddress(v->HSave).QuadPart & (PAGE_SIZE - 1)))
        {
            SVMB_LOGE("exp3: non-page-aligned contiguous PA cpu=%u", i);
            SVMB_START_FAIL(STATUS_HV_OPERATION_FAILED)
        }
        v->GuestVmcbRaw = v->GuestVmcb;
        v->HostVmcbRaw = v->HostVmcb;
        v->HSaveRaw = v->HSave;
        v->Info.RawAlloc = raw;
        v->Info.CpuIndex = i;
        v->Info.Hv = this;
        v->Info.State = (LONG)VcpuState::Off;
        v->Info.ExitTraceLeft = 200; // early-exit forensics budget (round-13:
                                    // was 2048, which flooded KDNET - see
                                    // SvmbVmExitEntry note; earlier hunts:
                                    // 64 ran out ~25s in, before the fatal
                                    // exit, so 16 stays diagnostic-only.
                                    // r140: 16 -> 200 on the QEMU serial
                                    // bench (no KDNET attached there) so
                                    // the death-window exits stay visible)
        Vcpus_[i] = v;
    }

    // ---- enter virtualization on every core ----
    // EnterCore handles its own enter-once guard and returns STATUS_SUCCESS
    // both for the host-side enter and the subsequent guest re-entry
    Running_ = true;
    SVMB_LOGI("r13-start: Running_=true, RunOnEachCore(%u cores) about to vmrun", CoreCount_);
    status = RunOnEachCore([this](u32 idx) -> NTSTATUS {
        return EnterCore(idx);
    });

    if (!NT_SUCCESS(status))
    {
        SVMB_LOGE("enter virtualization failed on some core: %08x", status);
        Stop();
        return status;
    }

    SVMB_LOGI("virtualization active on %u cores", CoreCount_);
    TraceMarker("start-active");
    // r141: per-core VMCB physical addresses on the death-proof channel -
    // when the bench freezes, QMP `xp` on these PAs reads each core's
    // LAST exit (Ctrl.ExitCode / ExitInfo1..2 / NRip / Save.Rip) straight
    // out of the frozen VMCBs. EnterCore itself runs at IPI level (file
    // IO impossible there), so harvest from the PASSIVE orchestrator.
    {
        char buf[192];
        int n = 0;
        n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, "vmcb-pas:");
        for (u32 i = 0; i < CoreCount_ && i < 8; ++i)
            n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE, " %llx",
                             Vcpus_[i] ? Vcpus_[i]->Info.GuestVmcbPa : 0ull);
        TraceMarker(buf);
    }
    // r142: NPT-state marker. The r141 frozen VMCBs all carried
    // NCr3=0x1589f3000, yet the page at that PA read back as kernel pool
    // data, not a PML4 - either those VMCBs were stale or the NPT root got
    // recycled between generations. Pin the truth at start: the default
    // view's root PA, its first two entries (the [0,1TB) identity fill must
    // leave pml4[0] pointing at a PDPT), and the NCr3 the first VMCB was
    // actually configured with.
    {
        NptManager* npt = NptInstance();
        if (npt && npt->Active())
        {
            char buf[160];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                        "npt-state: pml4pa=%llx pe0=%llx pe1=%llx ncr3=%llx",
                        npt->Active()->Pml4Pa(), npt->Active()->Pml4Pe(0),
                        npt->Active()->Pml4Pe(1),
                        (Vcpus_ && Vcpus_[0]) ? Vcpus_[0]->GuestVmcb->Ctrl.NCr3 : 0ull);
            TraceMarker(buf);
        }
        else
            TraceMarker("npt-state: no-active-view");
    }
    // r143 start-kick: the gen-2 re-entry is where the QEMU TCG
    // interrupt-delivery machinery historically desyncs (all cores lose
    // CPU_INTERRUPT_HARD while deliverable IRRs pile up - the r142/r143
    // freeze portraits). A real KeIpiGenericCall pushes every core through
    // the complete, genuine interrupt delivery path (IRR->ISR->handler->
    // EOI) right after virtualization goes active, flushing any state the
    // stop/start transition broke. Noop callback: the traffic itself is
    // the medicine.
    {
        KeIpiGenericCall([](ULONG_PTR) { return 0ull; }, 0);
        TraceMarker("start-kick-done");
    }
    return STATUS_SUCCESS;
#undef SVMB_START_FAIL
}

NTSTATUS Hypervisor::EnterCore(u32 cpuIdx)
{
    VcpuContext* vcpu = Vcpus_[cpuIdx];
    if (!vcpu)
        return STATUS_INVALID_PARAMETER;

    // per-core SVM sanity gate: CheckCpu() runs on the IOCTL thread but
    // vmrun fires on each core individually. Re-probe on the core that is
    // about to vmrun and bail out cleanly if the L1 hypervisor disabled SVM
    // between Start() and now. Mirrors the reference educational hypervisor's CheckSVM.
    {
        int c[4] = {};
        __cpuidex(c, CPUID_EXT_FEATURES, 0);
        u64 vmcr = __readmsr(MSR_VM_CR);
        if (!(c[2] & (1u << SVM_CPUID_BIT)) || (vmcr & (1ull << VM_CR_SVMDIS)))
        {
            SVMB_LOGE("per-core svm gate FAILED cpu=%u - skipping vmrun", cpuIdx);
            return STATUS_HV_FEATURE_UNAVAILABLE;
        }
    }

    // phase-1 register save: captures this thread's state and plants the
    // guest resume point. After vmrun the thread resumes at the instruction
    // following this call AS GUEST, so everything below must be guarded.
    _svmb_save_or_load_regs(&vcpu->Regs);

    // enter exactly once per core (reference-hypervisor technique): the guest re-entry
    // of this thread lands here with State==Entering/Guest and falls through
    LONG expected = (LONG)VcpuState::Off;
    if (InterlockedCompareExchange(&vcpu->Info.State, (LONG)VcpuState::Entering, expected) != expected)
        return STATUS_SUCCESS; // guest re-entry: this core is already virtualized

    ConfigureVmcb(vcpu);

    SVMB_LOGI("r13-entercore: cpu=%u ConfigureVmcb done, NP_ENABLE=%llu NCr3=%llx "
              "InterceptOpcode1=%08x InterceptOpcode2=%08x InterceptExc=%08x "
              "TlbControl=%u GuestAsid=%u",
              cpuIdx,
              (u64)vcpu->GuestVmcb->Ctrl.Np.Data,
              vcpu->GuestVmcb->Ctrl.NCr3,
              vcpu->GuestVmcb->Ctrl.InterceptOpcode1,
              vcpu->GuestVmcb->Ctrl.InterceptOpcode2,
              vcpu->GuestVmcb->Ctrl.InterceptException,
              (u32)vcpu->GuestVmcb->Ctrl.TlbControl,
              vcpu->GuestVmcb->Ctrl.GuestAsid);

    // 2026-09-07 fix: turn host SVME on FIRST so VM_HSAVE_PA wrmsr actually
    // lands. Native AMD rejects VM_HSAVE_PA wrmsr when SVME=0 with #GP
    // (APM vol.2 15.5); VMware's L1 transparently swallows that #GP without
    // forwarding our wrmsr, so the MSR stayed at its L1 default (often 0 or
    // unaligned) and vmrun later faulted inside the asm loop with
    // ExitCode=0xFF..FF.
    u64 efer = __readmsr(MSR_EFER);
    __writemsr(MSR_EFER, efer | (1ull << EFER_SVME));
    u64 hsavePa = MmGetPhysicalAddress(vcpu->HSave).QuadPart;
    __writemsr(MSR_VM_HSAVE_PA, hsavePa);

    // FillGuestState runs with host SVME=1; the only MSR reads it performs
    // (EFER/PAT/STAR/etc.) are not in our MSRPM intercept mask so the L1
    // never traps them. Capturing guest state AFTER host SVME is fine.
    FillGuestState(vcpu, &vcpu->Regs);
    vcpu->GuestVmcb->Save.Efer |= (1ull << EFER_SVME); // VMRUN validity requires it

    if (TscProbeEnabled)
    {
        // r92 vhv-honor probe: guest RDTSC shows raw+OFFSET iff VMware's
        // nested SVM honors the VMCB TscOffset field. Probe only - a
        // permanent non-zero offset would skew the guest's time sources,
        // so this knob goes back to 0 right after the measurement.
        // VERDICT (r92): the vhv IGNORES TscOffset (user-vs-driver RDTSC
        // diff ~0 with the offset applied) - TSC compensation through this
        // field is bare-metal-only. Note for any future post-VMRUN
        // TscOffset write: this vhv drops control-area writes unless
        // Ctrl.CleanBits.Bits = 0 (r38 law).
        vcpu->GuestVmcb->Ctrl.TscOffset = TSC_PROBE_OFFSET;
        SVMB_LOGI("tsc-probe: cpu=%u offset=%llx", cpuIdx,
                  (unsigned long long)TSC_PROBE_OFFSET);
    }

    // host state = guest snapshot (thin hypervisor: shared CR3/IDT/GDT)
    vcpu->HostVmcb->Save = vcpu->GuestVmcb->Save;

    vcpu->Info.GuestVmcbPa = MmGetPhysicalAddress(vcpu->GuestVmcb).QuadPart;
    vcpu->Info.HostVmcbPa = MmGetPhysicalAddress(vcpu->HostVmcb).QuadPart;
    vcpu->Info.VmmStackTop = (u64)vcpu->VmmStack + sizeof(vcpu->VmmStack);

    vcpu->Info.State = (LONG)VcpuState::Guest;

    SVMB_LOGI("vmrun cpu=%u vmcb=%llx EFER=%llx (SVME=%u) ExitCode_pre=%llx",
              cpuIdx, vcpu->Info.GuestVmcbPa,
              (u64)__readmsr(MSR_EFER),
              (unsigned)((__readmsr(MSR_EFER) >> EFER_SVME) & 1),
              vcpu->GuestVmcb->Ctrl.ExitCode);
    SVMB_LOGI("r13-vmrun-fire: cpu=%u about to _svmb_vmm_loop", cpuIdx);

    _svmb_vmm_loop(vcpu, vcpu->Info.GuestVmcbPa, vcpu->Info.HostVmcbPa,
                   (void*)vcpu->Info.VmmStackTop);

    // r13 life-cycle beacon: every time the asm VMM loop returns to this
    // point on this core we know the host kernel has control back. Reading
    // VMCB state right after the loop catches VMRUN failures (ExitCode !=
    // 0x69/expected) before any of the SVMB_LOGE below could fire if the
    // return path took a different exit. Round-13 forward-portal: even if
    // VMware vhv panics the L1 mid-VMRUN we never reach this line - so the
    // ABSENCE of a "r13-vmloop-return" line is itself a strong signal.
    SVMB_LOGI("r13-vmloop-return: cpu=%u State=%ld ExitCode=%llx",
              cpuIdx, (LONG)vcpu->Info.State,
              vcpu->GuestVmcb->Ctrl.ExitCode);

    // Two ways here:
    //  - self-exit (Stop set State=Leaving; asm shut SVME off and unwound):
    //    State is already Leaving/Off, EFER.SVME is already cleared by the
    //    asm path. Just retire the state machine.
    //  - VMRUN failure (vmrun_failed): asm cleared nothing; clean up here.
    LONG st = vcpu->Info.State;
    __writemsr(MSR_EFER, __readmsr(MSR_EFER) & ~(1ull << EFER_SVME));
    if (st == (LONG)VcpuState::Leaving)
    {
        InterlockedExchange(&vcpu->Info.State, (LONG)VcpuState::Off);
        SVMB_LOGI("core %u self-exited VMM", cpuIdx);
        return STATUS_SUCCESS;
    }
    InterlockedExchange(&vcpu->Info.State, (LONG)VcpuState::Off);
    u64 exitCode = vcpu->GuestVmcb->Ctrl.ExitCode;
    // Diagnostic: VMCB's ExitCode on a vmrun_failed path is whatever the
    // driver put there. RtlZeroMemory clears the 4 KB VMCB control area,
    // but a few bytes past the field we use stay 0xFF..FF (RtlZeroMemory
    // operates in 4 KB blocks so the trailing 0xFF is uninitialised padding).
    // If vmrun had actually serviced a #VMEXIT, ExitInfo1/ExitInfo2/NRip
    // would be populated; if vmrun never executed, they stay 0. We log
    // those two values plus InterceptOpcode2 (the bit that catches VMRUN
    // nesting violations) so future bisects can tell the failure mode
    // without dumping the whole 4 KB control area.
    SVMB_LOGE("core %u VMRUN failed, exit=%llx ExitInfo1=%llx "
              "InterceptOpcode2=%08x EFER_AFTER=%llx",
              cpuIdx, exitCode,
              vcpu->GuestVmcb->Ctrl.ExitInfo1,
              (u32)vcpu->GuestVmcb->Ctrl.InterceptOpcode2,
              __readmsr(MSR_EFER));
    return STATUS_HV_OPERATION_FAILED;
}

// r58 A/B knobs (registry Parameters\TlbMode / Parameters\GuestAsid,
// set via SetTlbKnobs from DriverEntry, consumed in ConfigureVmcb).
// tlbMode 0 = flush-all armed (r54 semantics); 1 = never arm it
// (reference semantics). GuestAsid defaults to 1 (the reference uses the same).
static ULONG s_tlbMode = 0;
static ULONG s_guestAsid = 1;
// r148: per-exit CR8 re-poll gate (see the r148 SUSPEND note at the call
// site). Default OFF for the A/B verdict; Parameters\Cr8Repoll=1 restores
// the r143 behavior if the freeze family comes back without it.
static volatile LONG s_cr8Repoll = 0;

// r157 death-window probe (layout documented in hypervisor.h)
volatile LONG64 g_stepProbe[8] = { 0x31504644, 0, 0, 0, 0, 0, 0, 0 };

void Hypervisor::ConfigureVmcb(VcpuContext* vcpu)
{
    VMCB& vmcb = *vcpu->GuestVmcb;
    RtlZeroMemory(&vmcb, sizeof(vmcb));

    if (vcpu->Info.CpuIndex == 0)
    {
        SVMB_LOGI("r13-cfgvmcb: cpu0 ConfigureVmcb enter (vmcb=%p)", (void*)&vmcb);
        SVMB_LOGI("cfgvmcb knobs: tlbMode=%u asid=%u", s_tlbMode, s_guestAsid);
    }

    Intercepts_.ApplyToVcpu(vcpu); // fills intercept fields + msrpm PA

    // round-13 align with the reference hypervisor: do NOT force #DE/#BP/#UD intercepts.
    // The earlier comment claimed VMware vhv raises unhandled #GP/#UD if we
    // don't intercept them; cross-testing again on 2026-09-07 with NPT off
    // shows VMware vhv panic with "vcpu-N:Invalid VMCB" before any #DE/#BP
    // /#UD ever fires - the panic is on the VMCB-shape itself, not on the
    // exception. The reference hypervisor leaves InterceptException=0 by
    // default and only sets BP/UD/DB when an optional plugin is wired in.
    // Keep this line deliberately empty; re-introducing exception intercepts
    // must be bisected first against the VMware vhv panic signature.

    vmcb.Ctrl.GuestAsid = (u32)s_guestAsid;
    vmcb.Ctrl.TlbControl = 0; // flush-all on first vmrun

    // NPT integration: when an NPT view is active the active PML4 becomes
    // NCr3 and we mark NP_ENABLE so vmrun walks the guest physical
    // address through the second-level page tables. Without NPT, vmrun
    // would force guest physical == host physical - on VMware vhv the L1
    // hypervisor sees those addresses mapping to its own memory and
    // triggers an automatic guest reset (CRASH_DEBUG_LOG round 10). When
    // no view is active (bare-metal bring-up path), NPT stays off.
    NptManager* npt = NptInstance();
    if (npt && npt->NptEnabled() && npt->Active())
    {
        vmcb.Ctrl.Np.Data |= 1; // NP_ENABLE
        vmcb.Ctrl.NCr3 = npt->Active()->Pml4Pa();
        // r58 A/B: tlbMode 1 = reference semantics (never arm flush-all);
        // 0 (default) = the r54 flush-all-armed configuration
        if (s_tlbMode == 0)
            vmcb.Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
    }
    else
    {
        vmcb.Ctrl.Np.Data = 0;
        vmcb.Ctrl.NCr3 = 0;
    }
}

void Hypervisor::FillGuestState(VcpuContext* vcpu, const GuestRegs* saved)
{
    VmcbSave& s = vcpu->GuestVmcb->Save;
    u64 gdtBase = 0, idtBase = 0;
    u16 gdtLimit = 0, idtLimit = 0;
    u16 trSel = 0, ldtrSel = 0;

    _svmb_sgdt(&gdtBase, &gdtLimit);
    _svmb_sidt(&idtBase, &idtLimit);
    _svmb_str(&trSel);
    _svmb_sldt(&ldtrSel);

    s.Gdtr.Base = gdtBase;  s.Gdtr.Limit = gdtLimit; s.Gdtr.Selector = 0;
    s.Idtr.Base = idtBase;  s.Idtr.Limit = idtLimit; s.Idtr.Selector = 0;

    struct SegJob { u16 sel; SegmentReg* out; bool fromMsrBase; u32 baseMsr; };
    SegJob jobs[] = {
        { _svmb_es(), &s.Es, false, 0 },
        { _svmb_cs(), &s.Cs, false, 0 },
        { _svmb_ss(), &s.Ss, false, 0 },
        { _svmb_ds(), &s.Ds, false, 0 },
        { _svmb_fs(), &s.Fs, true, MSR_FS_BASE },
        { _svmb_gs(), &s.Gs, true, MSR_GS_BASE },
        { ldtrSel,    &s.Ldtr, false, 0 },
        { trSel,      &s.Tr, false, 0 },
    };
    for (const SegJob& j : jobs)
    {
        j.out->Selector = j.sel;
        j.out->Attribute = GetSegmentAttribute(j.sel, gdtBase).AsUInt16;
        // round-18 VMCB-diff root cause: the reference hypervisor zeroes ALL segment limits
        // in the VMCB (its GetSegmentLimit2 returns 0 in long mode) and is
        // STABLE on this VMware vhv VM, while svmb's "architecturally
        // correct" nonzero limits correlate 1:1 with the delayed hard
        // reset (round-15 bisect: CPUID exits necessary+sufficient; the
        // per-exit vmsave/vmload/vmrun round-trip re-arms the shadow
        // segment state each time). HW ignores limits in long mode; the
        // old "LimitLow=0 -> vmrun FF..FF" failure (2026-09-07) is now
        // attributed to the same-era EFER/asm bugs, not the limit itself.
        j.out->Limit = 0;
        j.out->Base = j.fromMsrBase ? __readmsr(j.baseMsr) : GetSegmentBase(j.sel, gdtBase);
    }
    // LDTR/TR bases come from the descriptor (system segments keep real base)
    s.Ldtr.Base = GetSegmentBase(ldtrSel, gdtBase);
    s.Tr.Base = GetSegmentBase(trSel, gdtBase);
    // Same fix for the system segments above
    s.Ldtr.Limit = GetSegmentLimitForVmcb(ldtrSel, gdtBase);
    s.Tr.Limit = GetSegmentLimitForVmcb(trSel, gdtBase);

    s.Efer = __readmsr(MSR_EFER);
    s.Cr0 = __readcr0();
    s.Cr2 = __readcr2();
    s.Cr3 = __readcr3();
    s.Cr4 = __readcr4();
    // round-18 reference parity: the reference leaves DR6/DR7 at 0 (zero-init) in
    // the VMCB and is stable; svmb's DR6=FFFF0FF0/DR7=0x400 diverged in the
    // round-18 VMCB byte diff. Zero them to match.
    s.Dr7 = 0;
    s.Dr6 = 0;
    s.GPat = __readmsr(0x277); // IA32_MSR_PAT

    s.Star = __readmsr(MSR_STAR);
    s.Lstar = __readmsr(MSR_LSTAR);
    s.Cstar = __readmsr(MSR_CSTAR);
    s.Sfmask = __readmsr(MSR_SFMASK);
    s.KernelGsBase = __readmsr(MSR_KERNEL_GS_BASE);
    s.SysenterCs = __readmsr(0x174);
    s.SysenterEsp = __readmsr(0x175);
    s.SysenterEip = __readmsr(0x176);

    // guest must start phase-2 with rax = &Regs so the asm restore branch
    // fires (asm contract: if_load_regs tests rax against the image pointer)
    s.Rax = (u64)saved;
    s.Rflags = saved->RFlags;
    s.Rsp = saved->Rsp;
    s.Rip = saved->Rip;      // = if_load_regs address (asm contract)
    s.Cpl = (u8)(_svmb_cs() & 3);
}

// IPI broadcast callback: runs on every core at IPI level. Its only job is
// to be an event that forces each guest core out of HLT/WAIT and through a
// VMEXIT, letting the VMM loop see State==Leaving and self-exit.
ULONG_PTR Hypervisor::IpiNudgeCallback(ULONG_PTR Context)
{
    UNREFERENCED_PARAMETER(Context);
    // r28b: a bare IPI does not #VMEXIT (INTR is not in our intercept set;
    // most exits here are VMware-L1-policy CPUIDs), so the old empty
    // callback nudged nothing. CPUID always exits under VMware vhv and the
    // callback runs KERNEL-mode - exactly the context the r28 Leaving
    // devirt gate waits for. Executing it here makes Stop() single-phase:
    // every core hits its devirt inside this broadcast.
    int regs[4] = {};
    __cpuid(regs, 0);
    return 0;
}NTSTATUS Hypervisor::Stop()
{
    TraceMarker("stop-enter"); // r141: death-proof progress channel
    if (!Running_ && !CoreCount_)
        return STATUS_SUCCESS;

    // ask every still-virtualized core to devirtualize; the exiting thread
    // on that core resumes transparently at the VMMCALL return address.
    // Never break early: one refusal must not strand the remaining cores on
    // memory we are about to free. (Vcpus_ can still be null when Start()
    // failed during context allocation and rolled back into here.)
    // 2026-09-07 bisect C: SELF-EXIT. The old flow pinned the IOCTL thread to
    // each guest-state core and issued a VMMCALL from outside; under VMware
    // nested AMD-V that handoff wedges RunOnEachCore (deadlock, no crash,
    // KDNET unreachable - see round 9). New flow: mark each core Leaving,
    // then WAIT for the core's own VMM loop to notice (checked at the top of
    // the asm loop after every VMEXIT) and unwind itself. No cross-core
    // VMMCALL, no affinity switching, nothing to deadlock on. An idle core
    // with no VMEXITs is nudged by asking it to reschedule; worst case the
    // wait times out and the hard gate below reports what remains.
    NTSTATUS devirtStatus = STATUS_SUCCESS;
    if (Vcpus_)
    {
        for (u32 i = 0; i < CoreCount_; ++i)
        {
            VcpuContext* v = Vcpus_[i];
            if (!v)
                continue;
            LONG expected = (LONG)VcpuState::Guest;
            InterlockedCompareExchange(&v->Info.State, (LONG)VcpuState::Leaving,
                                       expected);
        }

        // wait (bounded) for every core to devirtualize. r28: the Leaving
        // exit resumes the interrupted thread in place (svm_entry.asm), so
        // each core drains on its NEXT NATURAL exit - an idle core with no
        // interceptable traffic can take a while, hence the 15s budget.
        // Cores that miss the deadline stay marked Leaving and devirt
        // lazily later; that is safe now (no thread context discarded).
        KeIpiGenericCall(&Hypervisor::IpiNudgeCallback, 0);        for (int spin = 0; spin < 300; ++spin) // ~15s worst case
        {
            bool allOff = true;
            for (u32 i = 0; Vcpus_ && i < CoreCount_; ++i)
            {
                VcpuContext* v = Vcpus_[i];
                if (v && v->Info.State != (LONG)VcpuState::Off)
                {
                    allOff = false;
                    break;
                }
            }
            if (allOff)
                break;
            LARGE_INTEGER delay;
            delay.QuadPart = -500000; // 50ms
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }
        for (u32 idx = 0; idx < CoreCount_; ++idx)
        {
            VcpuContext* v = Vcpus_[idx];
            SVMB_LOGI("stop: core %u self-exit state=%ld", idx,
                      v ? (LONG)v->Info.State : -1L);
        }
    }

    // hard gate: never free per-core resources while a core is still in SVM.
    // Keeping everything allocated makes a retry Stop() possible (the devirt
    // pass above runs for Guest-state cores regardless of Running_)
    for (u32 i = 0; Vcpus_ && i < CoreCount_; ++i)
    {
        if (Vcpus_[i] && Vcpus_[i]->Info.State != (LONG)VcpuState::Off)
        {
            SVMB_LOGE("stop aborted: core %u still in SVM (state %ld, devirt err %08x)",
                      i, Vcpus_[i]->Info.State, devirtStatus);
            // keep Running_ = true: the hypervisor is still (partially) live.
            // Clearing it here would let a subsequent start create a second
            // instance on top of cores that never left SVM.
            return STATUS_HV_OPERATION_FAILED;
        }
    }

    Running_ = false;

    // detach modules first: their handlers/requirements die with the hypervisor
    Modules_.Deinit();

    // 2026-09-06 fix (BSOD 0x1A in MiDeleteFinalPageTables + 0x50 PAGE_FAULT):
    // we explicitly DO NOT free the per-vcpu raw allocation or the Vcpus_ array
    // here. The 11-page VcpuContext (GuestVmcb/HostVmcb/HSave/Info/Regs/VmmStack)
    // holds 4 KB page frames that can be referenced by stale PTE entries
    // in any user-mode process that ever issued an IOCTL (svmbctl etc.).
    // ExFreePoolWithTag returns those frames to NT's pool; the next allocator
    // can hand them to a process page table, and when the process exits
    // MiDeleteFinalPageTables hits our stale 0xDEAD-fill PTE and BSODs.
    //
    // cost: 11 * 4096 * CoreCount bytes (264 KB on 6 cores) per start cycle.
    // tradeoff: tiny leak vs. host BSOD on every svmbctl exit. We choose leak.
    if (Vcpus_)
    {
        for (u32 i = 0; i < CoreCount_; ++i)
        {
            if (Vcpus_[i])
            {
                Vcpus_[i]->Info.State = (LONG)VcpuState::Off;
                Vcpus_[i] = nullptr;
            }
        }
        // Intentionally do NOT FreeNonPaged(Vcpus_, TAG_VCPU).
        // The pool block lives until OS process teardown cleans it up.
        Vcpus_ = nullptr;
    }
    CoreCount_ = 0;

    Intercepts_.Deinit();
    Hypercalls_.Deinit();
    Dispatcher_.Deinit();
    Msrpm_.Deinit();
    gInstance = nullptr;

    SVMB_LOGI("hypervisor stopped (vCPU pages leaked for safe teardown)");
    TraceMarker("stop-done");
    return STATUS_SUCCESS;
}

u64 Hypervisor::TotalExitCount() const
{
    u64 total = 0;
    for (u32 i = 0; i < CoreCount_; ++i)
        if (Vcpus_[i])
            total += Vcpus_[i]->Info.ExitCount;
    return total;
}

// ---- r53 per-core process view policy ----
// cr3_monitor binds a target CR3 to a process-view PML4 (SetPerCoreView);
// each core's own exit path then publishes the matching NCr3 into its own
// VMCB (single writer, no locks). basePml4 == 0 disables the policy.
static u64 s_viewTargetCr3 = 0;
static u64 s_viewProcPml4 = 0;
static u64 s_viewBasePml4 = 0;

void Hypervisor::SetPerCoreView(u64 targetCr3, u64 procPml4, u64 basePml4)
{
    s_viewTargetCr3 = targetCr3;
    s_viewProcPml4 = procPml4;
    s_viewBasePml4 = basePml4;
}

u64 Hypervisor::ViewTargetCr3() { return s_viewTargetCr3; }
u64 Hypervisor::ViewProcPml4() { return s_viewProcPml4; }
u64 Hypervisor::ViewBasePml4() { return s_viewBasePml4; }

// r58 A/B knobs (registry: Parameters\TlbMode / Parameters\GuestAsid,
// consumed in ConfigureVmcb). tlbMode 0 = flush-all armed per the r54
// semantics; 1 = never arm it (reference semantics). GuestAsid defaults to
// 1 (the reference uses the same).
void Hypervisor::SetTlbKnobs(ULONG tlbMode, ULONG guestAsid)
{
    s_tlbMode = tlbMode;
    s_guestAsid = guestAsid;
    SVMB_LOGI("tlb knobs: tlbMode=%u asid=%u", s_tlbMode, s_guestAsid);
}

void Hypervisor::SetCr8Repoll(ULONG enable)
{
    s_cr8Repoll = enable ? 1 : 0;
    SVMB_LOGI("cr8-repoll knob: %u", (ULONG)s_cr8Repoll);
}


ULONG Hypervisor::TlbModeKnob()
{
    return s_tlbMode;
}

// ---- exit path ----

void ApplyTfFixup(GuestContext& ctx)
{
    if (ctx.EventInjected)
        return;
    if (ctx.Regs->RFlags & (1ull << EFLAGS_TF))
        InjectException(ctx, EXC_DB);
}

extern "C" void SvmbFillMachineFrame(void* frame, void* vcpuPtr)
{
    MachineFrame* mf = (MachineFrame*)frame;
    VcpuContext* vcpu = (VcpuContext*)vcpuPtr;
    mf->Rip = vcpu->GuestVmcb->Ctrl.NRip;
    mf->Cs = vcpu->GuestVmcb->Save.Cs.Selector;
    mf->Ss = vcpu->GuestVmcb->Save.Ss.Selector;
    mf->OldRsp = vcpu->GuestVmcb->Save.Rsp;
    mf->EFlags = (u32)vcpu->GuestVmcb->Save.Rflags;
}

extern "C" void SvmbVmExitEntry(void* vcpuPtr, GuestRegs* regs)
{
    VcpuContext* vcpu = (VcpuContext*)vcpuPtr;

    VMCB& vmcb = *vcpu->GuestVmcb;

    // r113: bracket the whole exit handler - the r112 freeze died after
    // exit-epilogue, so the surviving window is [vmrun-pre -> HERE] vs
    // [HERE -> the rest of the handler]
    CrumbPost(CRUMB_EXIT_ENTRY);

    // TCG compat: with no NRIP-save the VMCB arrives with NRip=0 and
    // Save.Rip = start of the intercepted instruction. Reconstruct NRip
    // here, once, before every consumer (exit trace, dispatch AdvanceRip,
    // freeze machine frame, hypercall resume contract) reads it. Written
    // back into the VMCB so the reflection paths see it too.
    // Must NOT be gated on NRip==0: TCG never writes the field, so on the
    // 2nd+ exit it still holds whatever WE wrote back last time (stale
    // nrip from a previous rip = rewind livelock, seen live as 16x
    // "rip=...675a nrip=...6718" on serial).
    if (s_noNripsCompat && !IsFaultClassExit(vmcb.Ctrl.ExitCode))
    {
        u32 len = InterceptInstrLen(vmcb.Ctrl.ExitCode);
        if (len != 0)
            vmcb.Ctrl.NRip = vmcb.Save.Rip + len;
        else
            SVMB_LOGE("nrip-fallback: no instr length for exit %llx (rip=%llx)",
                      vmcb.Ctrl.ExitCode, vmcb.Save.Rip);
    }

    // round-16 CPUID short-circuit REMOVED: v24 proved the reset is not in
    // the C++ dispatch layer (short-circuit still died). Round 18 found the
    // real divergent state instead - nonzero segment limits + DR6/DR7 in
    // the VMCB (the reference zeroes both) - fixed in FillGuestState, so the
    // normal dispatcher path is restored.

    // r187b heartbeat: one gate-free UART line per core per 2^16 exits
    // (2^20 proved unreachable before a wedge - the r187 livelock froze
    // the machine at only a few thousand exits). During a healthy run
    // these land every few seconds; during a wedge their silence
    // distinguishes "vCPUs still exiting" from "frozen mid-exit" - and
    // the last exit code printed is the wedge-point witness.
    {
        static volatile LONG s_hb[NESTED_MAX_CORES];
        static volatile LONG s_hbLast[NESTED_MAX_CORES];
        u32 hbCpu = vcpu->Info.CpuIndex < NESTED_MAX_CORES
                        ? vcpu->Info.CpuIndex : 0;
        LONG n = InterlockedIncrement(&s_hb[hbCpu]);
        InterlockedExchange(&s_hbLast[hbCpu],
                            (LONG)(u32)vmcb.Ctrl.ExitCode);
        if ((n & 0xFFFF) == 0)
            LogSerialTraceF("HB cpu=%u n=%ld last=%lx",
                            hbCpu, n, s_hbLast[hbCpu]);
    }

    // early-exit forensics: trace the first exits after each enter so a
    // vCPU shutdown leaves the exact fatal exit in the log/serial record.
    // Budget is per-Start (ExitTraceLeft, see Start()). Round-13: capped at
    // 16 per core - CPUID is intercepted and Windows kernels CPUID constantly,
    // so a 2048 budget flooded KDNET with DbgPrint packets and the guest
    // crawled to a standstill while the debugger was idle (each packet waits
    // for the debugger's ack). 16 traces still capture the fatal-exit window.
    if (vcpu->Info.ExitTraceLeft > 0)
    {
        InterlockedDecrement(&vcpu->Info.ExitTraceLeft);
        SVMB_LOGW("exit trace: code=%llx rip=%llx nrip=%llx cpl=%u svme=%u info1=%llx",
                  vmcb.Ctrl.ExitCode, vmcb.Save.Rip, vmcb.Ctrl.NRip,
                  (u32)vmcb.Save.Cpl,
                  (unsigned)((vmcb.Save.Efer >> EFER_SVME) & 1),
                  vmcb.Ctrl.ExitInfo1);
    }

    // r142/r143: QEMU TCG HARD-bit desync repair. x86_cpu_exec_interrupt
    // clears CPU_INTERRUPT_HARD *before" confirming deliverability; when the
    // racing LAPIC state answers "blocked", the bit stays clear while a
    // deliverable IRR bit may sit pending, and nothing re-runs
    // apic_update_irq until the next APIC state change. r143 taught the
    // harder lesson: gating on "every 1024th exit" starves exactly when it
    // matters - the post-cycle freeze caught all six cores in long
    // user-mode stretches with ZERO exits, so the gate never fired and the
    // desync persisted for the whole freeze. Re-poll on EVERY exit now: a
    // same-value CR8 rewrite routes through cpu_set_apic_tpr ->
    // apic_set_tpr -> apic_update_irq unconditionally (no same-value
    // short-circuit, vapic inactive on this guest), while real hardware
    // and VMware vhv see a TPR no-op.
    //
    // r148 SUSPEND: the per-exit host-side CR8 write is the last standing
    // suspect for the post-cycle triple-fault family (5/5 forged-RIP at
    // wrong_base+0x5f137a + one silent reset; hunt7 cleared the VMCB state,
    // the MSR path, and the exit dispatcher - the crash context looks like a
    // half-swapped injection, and this write interleaves with QEMU's
    // clear-HARD-then-check-deliverability race on every exit). Disabled
    // for the A/B verdict; re-enable via Parameters\Cr8Repoll=1 if the
    // freeze family returns without it.
    ++vcpu->Info.ExitCount;
    if (s_cr8Repoll)
        __writecr8(__readcr8());

    // r108 root fix: consume THIS core's pending TLB kick here, on the
    // owning CPU, where writing our own VMCB is safe. Same semantics as
    // the old cross-CPU write (the next VMRUN flushes all) without the
    // VMRUN-boundary race.
    if (InterlockedExchange(&vcpu->Info.PendingTlbKick, 0))
    {
        vmcb.Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
        vmcb.Ctrl.CleanBits.Bits = 0; // else the vmx drops the write
    }

    // r123 (B7): consume THIS core's pending view switch. The view
    // publish and wiggle paths used to cross-write every VMCB - the
    // exact race class r108 rooted out for the kick (lost CleanBits
    // updates at the VMRUN boundary). The calling core applied its own
    // switch synchronously at publish time; every other core lands
    // here, still before its next VMRUN - the effective timing is the
    // same, minus the cross-CPU VMCB write.
    if (InterlockedExchange(&vcpu->Info.PendingViewSwitch, 0))
    {
        vmcb.Ctrl.NCr3 = vcpu->Info.PendingViewPml4;
        vmcb.Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
        vmcb.Ctrl.CleanBits.Bits = 0;
        // NOTE: PublishedNcr3 deliberately NOT touched - it is the r53
        // policy's single-writer bookkeeping (and the wiggle-restore
        // target source); the historical cross-write paths never
        // updated it either
    }

    // r175: SWINT punt re-arm (invariant restore). HandleSwintExit punts
    // kernel-mode software interrupts natively by clearing this VMCB's
    // INTN bit (op1 bit21), so the re-executed instruction raises through
    // the guest's own IDT. ApplyToVcpu maintains the invariant
    // (TCG + BP face armed) => bit21 set, so a live punt is exactly the
    // state "BP exception bit set, INTN bit missing" - restore the bit
    // here on the owning core, before this exit's own VMRUN, and software
    // interrupts intercept again from the next raise on. A face teardown
    // meanwhile rewrote both fields wholesale, leaves them consistent, and
    // correctly skips this restore.
    // r187: NEVER while an L2 session runs on this core. H's own #BP
    // request (the reference hypervisor arms vector 3, hexc=8) makes the condition true
    // for the L2's VMCB too, and the re-arm silently undid the accept's
    // INTN-bit clear on every entry - the L2's softint then exited 0x75,
    // reflected into the reference hypervisor's exit-switch default (__debugbreak ->
    // FAST_FAIL_INCORRECT_STACK 0x139, minidump-proven twice).
    if (!NestedActive(vcpu->Info.CpuIndex) &&
        s_noNripsCompat &&
        (vmcb.Ctrl.InterceptOpcode1 & (1u << 21)) == 0 &&
        (vmcb.Ctrl.InterceptException & (1u << EXC_BP)) != 0)
    {
        vmcb.Ctrl.InterceptOpcode1 |= 1u << 21;
        vmcb.Ctrl.CleanBits.Bits = 0;
    }

    regs->Rip = vmcb.Save.Rip;
    regs->Rsp = vmcb.Save.Rsp;
    regs->Rax = vmcb.Save.Rax;
    regs->RFlags = vmcb.Save.Rflags;

    // r55 armed FLUSH_ALL once and cleared it here after the first
    // crossing. REVERTED in r57: the clear bought minutes, not stability -
    // the guest still livelocked (Mm working-set IPI storm, kd-verified)
    // and eventually triple-faulted. Conclusion: per-entry flush-all is
    // load-bearing on this vhv (it compensates a nested-TLB weakness we
    // have not isolated yet); keep the r54 semantics and the 30-90s
    // window workflow (atomic one-shot bats) until the vhv TLB behavior
    // is characterized against svm-base.

    // asm contract: Extra1/Extra2 carry the devirtualize request examined by
    // the restore path right after this function returns. Clear them on every
    // exit or the stale phase-1 resume rip would spuriously devirtualize (and
    // ret to a null rip) on the very first exit. Only HC_EXIT_VMM re-sets them.
    regs->Extra1 = 0;
    regs->Extra2 = 0;

    // r166 nested v1: while L2 runs on this core, EVERY exit reflects into
    // H's VMCB and H resumes past its VMRUN - module dispatch never sees L2
    // exits (see core/nested.cpp). Placed after the per-exit housekeeping
    // (NRip reconstruction above is what fills H's Ctrl.NRip) and after the
    // regs IN-sync so the write-back below re-syncs the restored H values.
    // r172: NPF exits in an NP session go to the shadow composer FIRST -
    // composed entries re-enter L2 (the faulting access retries) instead
    // of reflecting; H's-walk-fault NPFs still reflect.
    // r186 intent gate: only exits H's own VMCB armed reflect. The rest
    // self-serve inline in nested.cpp (hlt nap, cpuid/msr passthrough) -
    // NEVER the L1 dispatch below (its post-dispatch machinery - view
    // publish, TLB mode, the r143 interrupt repair - assumes L1
    // semantics; the s2 freeze came from falling through into it).
    // Reflecting unordered exits hands H surprises it never asked for -
    // the r185 reference-hypervisor wedge (forced INTR at L2's first instruction ->
    // its generic leave path -> machine starved; the reference hypervisor's exit switch
    // literally KeBugChecks on unhandled exits).
    if (NestedActive(vcpu->Info.CpuIndex))
    {
        // r200: close the intr-delivery world window at the earliest exit,
        // before any nested routing (compose must not run under identity)
        NestedIntrDeliveryWindowClose(vcpu);
        if (vmcb.Ctrl.ExitCode == vmexit::NPF &&
            NestedFor(vcpu->Info.CpuIndex).NestedNp)
        {
            if (NestedShadowCompose(vcpu->Info.CpuIndex,
                                    vmcb.Ctrl.ExitInfo2,
                                    vmcb.Ctrl.ExitInfo1))
                return;
            SVMB_LOGI("shadow: npf reflect gpa=%llx info1=%llx rip=%llx",
                      (unsigned long long)vmcb.Ctrl.ExitInfo2,
                      (unsigned long long)vmcb.Ctrl.ExitInfo1,
                      (unsigned long long)vmcb.Save.Rip);
            NestedReflect(vcpu, regs);
            return;
        }
        if (NestedExitOrderedByH(vcpu->Info.CpuIndex,
                                 vmcb.Ctrl.ExitCode))
        {
            NestedReflect(vcpu, regs);
            return;
        }
        // r186b: unordered - serve inline (closed mini-dispatch; never
        // the L1 pipeline - the s2 freeze came from falling through)
        NestedSelfServeExit(vcpu, regs);
        return;
    }

    GuestContext ctx = {};
    ctx.Vcpu = vcpu;
    ctx.Regs = regs;
    ctx.Exit = vmcb.Ctrl.ExitCode;
    ctx.Info1 = vmcb.Ctrl.ExitInfo1;
    ctx.Info2 = vmcb.Ctrl.ExitInfo2;
    ctx.IntInfo = vmcb.Ctrl.ExitIntInfo;
    ctx.NRip = vmcb.Ctrl.NRip;
    ctx.AdvanceRip = !IsFaultClassExit(ctx.Exit);
    ctx.EventInjected = false;

    Hypervisor::Instance()->Dispatcher().Dispatch(ctx);

    ApplyTfFixup(ctx);

    vmcb.Save.Rip = regs->Rip;
    vmcb.Save.Rsp = regs->Rsp;
    vmcb.Save.Rax = regs->Rax;
    vmcb.Save.Rflags = regs->RFlags;

    InterlockedIncrement64(&vcpu->Info.ExitCount);

    // r45 exit census: owning core writes its own slot (single-writer, no
    // atomics). Dense 0x00..0xFF, NPF and everything else collapse into the
    // two trailing slots (svmb_protocol.h SVMB_XP_*).
    {
        const u64 exitReason = vmcb.Ctrl.ExitCode;
        u32 slot = SVMB_XP_OTHER;
        if (exitReason < SVMB_XP_DENSE)
            slot = (u32)exitReason;
        else if (exitReason == 0x400) // VMEXIT_NPF
            slot = SVMB_XP_NPF;
        ++vcpu->Info.ExitHist[slot];
    }

    // r53 per-core view publish: single writer (the owning core), so the
    // compare-and-publish needs no locks. CleanBits=0 (r38 law) so the vmx
    // honors the NCr3 update.
    // r122b RESOLVED (was the r122 open issue): the runtime CR3 equals
    // EPROCESS+0x28 DTB exactly on this guest (no PCID variance; the
    // +0x388 UserDirectoryTableBase candidate reads 0 - no KVA shadow).
    // The real blocker was ENGAGEMENT: the policy evaluates only at an
    // exit in the target's context, and this vhv delivers no INTR exits,
    // so a zero-exit workload (no trapping syscall, no NPF, no CPUID)
    // never presents the target CR3. Any SYSCALLING process engages for
    // free (kernel-mode execution runs under the target's DTB and hits
    // the intercepted CPUID/MSR baseline constantly); synthetic pure
    // user-mode loops need an explicit fuse (a __cpuid spin).
    if (s_viewBasePml4)
    {
        u64 want = s_viewBasePml4;
        if (s_viewTargetCr3 && vmcb.Save.Cr3 == s_viewTargetCr3)
        {
            want = s_viewProcPml4;
            static volatile LONG s_pubDiag = 0;
            if (want != vcpu->Info.PublishedNcr3 &&
                InterlockedIncrement(&s_pubDiag) <= 4)
                SVMB_LOGI("viewpol PUBLISH: cpu=%u cr3=%llx",
                          vcpu->Info.CpuIndex, vmcb.Save.Cr3);
        }
        if (want && want != vcpu->Info.PublishedNcr3)
        {
            vmcb.Ctrl.NCr3 = want;
            vmcb.Ctrl.TlbControl = TLB_CTL_FLUSH_ALL; // drop stale roots
            vmcb.Ctrl.CleanBits.Bits = 0;
            vcpu->Info.PublishedNcr3 = want;
        }
        // r61: the switch flush above is STICKY - TlbControl is re-evaluated
        // on every VMRUN and nothing reset it, so one view switch silently
        // degraded that core to permanent flush-all, breaking the tlbMode=1
        // semantics and making per-core TLB cadence diverge. One-shot:
        // restore the configured mode on the first non-switch exit.
        else if (s_tlbMode == 1 && vmcb.Ctrl.TlbControl != 0)
        {
            vmcb.Ctrl.TlbControl = 0;
            vmcb.Ctrl.CleanBits.Bits = 0; // else the vmx drops the write
        }
    }

    // round-21 race-window experiment: the reference's loop runs a
    // CompareGenericRegisters pass (~0.5us: 16 movaps + compares + call)
    // between exit handling and vmrun re-entry; svmb re-enters immediately.
    // Round 15-20 eliminated every static difference, so if the delayed
    // hard reset is a race against VMware's L1 nested-emulation bookkeeping,
    // this pacing should eliminate it. 2us chosen as clearly larger than
    // the reference's compare.
    KeStallExecutionProcessor(2);
    // r112 death-point split: the r111 freeze's cpu4 died AFTER npf-tail
    // with no further hops - this station brackets the remaining window
    // ([exit-epilogue] -> asm loop -> VMRUN -> next exit's first hop)
    CrumbPost(CRUMB_EXIT_EPILOGUE);
}

} // namespace svmb

// r113: C-linkage beacon for the asm loop (svmb::CrumbPostD is namespaced;
// the asm cannot spell the mangled name). tag in rcx, data in rdx.
extern "C" void SvmbCrumbPostD(LONG tag, unsigned long long data)
{
    svmb::CrumbPostD(tag, (svmb::u64)data);
}


