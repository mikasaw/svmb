#include "core/hypervisor.h"
#include "hw/segments.h"
#include "mm/npf.h"
#include "mm/tlb.h"
#include "hw/svm_defs.h"
#include "platform/logger.h"
#include "platform/util.h"

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

} // namespace

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
    // the whole exit path resumes the guest via VMCB.NRip; without NRIPS
    // support NRip stays 0 and every intercept would send rip to address 0
    if (!(out[3] & (1 << NRIPS_CPUID_BIT)))
    {
        SVMB_LOGE("svm check: NRIPS not supported (CPUID.8000000A.EDX=%08x)",
                  (u32)out[3]);
        return SvmCheckResult::NoNrips;
    }

    SVMB_LOGI("svm check: OK (AMD cpu, SVM bit=1, VM_CR.SVMDIS=0, NPT+NRIPS supported)");
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
        v->Info.ExitTraceLeft = 16; // early-exit forensics budget (round-13:
                                    // was 2048, which flooded KDNET - see
                                    // SvmbVmExitEntry note; earlier hunts:
                                    // 64 ran out ~25s in, before the fatal
                                    // exit, so 16 stays diagnostic-only)
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

    // round-16 CPUID short-circuit REMOVED: v24 proved the reset is not in
    // the C++ dispatch layer (short-circuit still died). Round 18 found the
    // real divergent state instead - nonzero segment limits + DR6/DR7 in
    // the VMCB (the reference zeroes both) - fixed in FillGuestState, so the
    // normal dispatcher path is restored.

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

    // r108 root fix: consume THIS core's pending TLB kick here, on the
    // owning CPU, where writing our own VMCB is safe. Same semantics as
    // the old cross-CPU write (the next VMRUN flushes all) without the
    // VMRUN-boundary race.
    if (InterlockedExchange(&vcpu->Info.PendingTlbKick, 0))
    {
        vmcb.Ctrl.TlbControl = TLB_CTL_FLUSH_ALL;
        vmcb.Ctrl.CleanBits.Bits = 0; // else the vmx drops the write
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
    if (s_viewBasePml4)
    {
        u64 want = s_viewBasePml4;
        if (s_viewTargetCr3 && vmcb.Save.Cr3 == s_viewTargetCr3)
            want = s_viewProcPml4;
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
}

} // namespace svmb
