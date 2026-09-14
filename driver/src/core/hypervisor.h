// svmb - hypervisor lifecycle: capability check, per-core context allocation,
// enter/exit virtualization, built-in exit handler wiring.
#ifndef SVMB_HYPERVISOR_H
#define SVMB_HYPERVISOR_H

#include "core/vcpu.h"
#include "core/exit_dispatcher.h"
#include "core/intercept_manager.h"
#include "core/hypercall.h"
#include "core/module.h"
#include "hw/msrpm.h"
#include "platform/base.h"

namespace svmb
{

enum class SvmCheckResult : u32
{
    Ok = 0,
    NotAmd = 1,
    NoSvmBit = 2,
    SvmdisSet = 3,       // SVM disabled (BIOS or another VMM holds it)
    NoNpt = 4,
    AlreadyVirtualized = 5,
    NoNrips = 6,         // no NRIP auto-save: exit-path rip advance impossible
};

constexpr u32 SVMB_DRIVER_VERSION = 0x00010000;

// runtime A/B switches for diagnosing "vCPU shutdown on start" (set from
// registry in DriverEntry, see main.cpp). Defaults preserve full protection.
extern volatile LONG GuestCpuidHideEnabled;  // P0-1: mask SVM/NPT CPUID bits from the guest
extern volatile LONG HsaveProtectionEnabled; // P2-6: intercept VM_HSAVE_PA read/write
// r92 vhv-honor probe: when set at DriverEntry, every guest VMCB starts with
// a large TscOffset. Guest RDTSC then shows raw+offset IFF VMware's nested
// SVM honors the VMCB field - the probe that decides whether TSC
// compensation is buildable in this topology (round 92).
extern volatile LONG TscProbeEnabled;
constexpr u64 TSC_PROBE_OFFSET = 0x4000000000ull; // ~91s @3GHz: dwarfs the kd-vs-tool readout skew

class Hypervisor
{
public:
    static Hypervisor* Instance() { return gInstance; }

    NTSTATUS Start();  // PASSIVE_LEVEL
    NTSTATUS Stop();   // PASSIVE_LEVEL
    bool     IsRunning() const { return Running_; }

    SvmCheckResult CheckCpu(); // callable without starting

    // subsystem access
    ExitDispatcher& Dispatcher() { return Dispatcher_; }
    InterceptManager& Intercepts() { return Intercepts_; }
    Hypercall& Hypercalls() { return Hypercalls_; }
    ModuleManager& Modules() { return Modules_; }
    Msrpm& GetMsrpm() { return Msrpm_; }
    u32 CoreCount() const { return CoreCount_; }
    VcpuContext* Vcpu(u32 idx) { return (idx < CoreCount_) ? Vcpus_[idx] : nullptr; }

    template <typename Fn>
    void ForEachVcpu(Fn&& fn)
    {
        for (u32 i = 0; i < CoreCount_; ++i)
            if (Vcpus_[i])
                fn(Vcpus_[i]);
    }

    u64 TotalExitCount() const;

    // r53 per-core process view policy: when active, every core publishes
    // ProcPml4 into its own VMCB NCr3 while the guest's current CR3 equals
    // TargetCr3, and BasePml4 otherwise - lazily, at each core's own exit.
    // SetPerCoreView(0, 0, basePml4) reverts all cores to the base view.
    static void SetPerCoreView(u64 targetCr3, u64 procPml4, u64 basePml4);
    // r58 A/B knobs (registry TlbMode / GuestAsid), consumed by
    // ConfigureVmcb: tlbMode 0 = flush-all armed, 1 = reference semantics
    static void SetTlbKnobs(ULONG tlbMode, ULONG guestAsid);
    static ULONG TlbModeKnob();
    static u64 ViewTargetCr3();
    static u64 ViewProcPml4();
    static u64 ViewBasePml4();


private:
    static Hypervisor* gInstance;

    NTSTATUS EnterCore(u32 cpuIdx);   // runs on the target core
    void FillGuestState(VcpuContext* vcpu, const GuestRegs* saved);
    void ConfigureVmcb(VcpuContext* vcpu);
    static ULONG_PTR IpiNudgeCallback(ULONG_PTR Context);

    VcpuContext** Vcpus_ = nullptr;
    u32 CoreCount_ = 0;
    bool Running_ = false;

    Msrpm Msrpm_;
    ExitDispatcher Dispatcher_;
    InterceptManager Intercepts_;
    Hypercall Hypercalls_;
    ModuleManager Modules_;
};

// exit shim called from asm (svm_entry.asm)
extern "C" void SvmbVmExitEntry(void* vcpu, GuestRegs* regs);
extern "C" void SvmbFillMachineFrame(void* machineFrame, void* vcpu);

// TF preservation: after any exit, if the guest had TF set and no event was
// injected, deliver #DB so single-stepping survives interception
// (anti-detection, see howtohypervise.blogspot.com "common oversight").
void ApplyTfFixup(GuestContext& ctx);

} // namespace svmb

#endif // SVMB_HYPERVISOR_H
