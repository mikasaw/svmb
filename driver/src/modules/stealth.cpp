// svmb - r92 stealth module. CPUID exit handler scrubbing the loudest
// virtualization tells the core passthrough still answers with native
// (VMware) content:
//   leaf 0x1                          ECX bit31 "hypervisor present"
//   leaves 0x40000000..0x40000010     hypervisor signature leaves
//                                     ("VMwareVMware" at 0x40000000, Hv#1,
//                                     the 0x40000010 TSC/bus-frequency
//                                     hint, ...)
// Complements the compiled-in hideCpuidBits core behavior (SVM/NPT feature
// bits at 0x80000001/0x8000000A, VM_CR.SVMDIS), which stays untouched.
//
// Deliberately out of scope: svmb's own channels all sit above the scrub
// range and stay reachable with stealth attached - presence probe leaf
// 0x400000FE (HYPERCALL_CPUID_LEAF - 1), the hypercall vehicle leaf
// 0x400000FF, and the demo module's 0x40000100.
//
// Lifecycle IS the switch (no knob, no IOCTL): `svmbctl mod attach stealth`
// enables, `mod detach stealth` restores the native answers because
// ModuleManager revokes the handler. No boot auto-attach on purpose:
// Windows calibrates TSC-related paths against the hypervisor leaves at
// boot, and cold-boot scrubbing of 0x40000010 is unvalidated (r92 scope
// note in tests/CRASH_DEBUG_LOG.md).
#include "modules/stealth.h"
#include "svmb/module_api.h"

namespace svmb
{

namespace
{

volatile LONG64 g_scrubHits = 0;

// function-table COPY, not a pointer: BuildApi hands Init a stack local, so
// keeping `api` itself for Stop() would dangle. The vtable pointers stay
// valid for the driver's lifetime.
SvmbApi g_api = {};

bool StealthCpuidHandler(GuestContext& ctx, void* userData)
{
    UNREFERENCED_PARAMETER(userData);
    const u32 leaf = (u32)ctx.Regs->Rax;
    if (!StealthScrubLeaf(leaf))
        return false; // not ours: let the chain continue (core emulates)

    int out[4] = {};
    __cpuidex(out, (int)leaf, (int)ctx.Regs->Rcx);
    u32 res[4] = {(u32)out[0], (u32)out[1], (u32)out[2], (u32)out[3]};
    StealthApplyScrub(leaf, res);
    ctx.Regs->Rax = res[0];
    ctx.Regs->Rbx = res[1];
    ctx.Regs->Rcx = res[2];
    ctx.Regs->Rdx = res[3];
    InterlockedIncrement64(&g_scrubHits);
    ctx.AdvanceRip = true;
    return true;
}

NTSTATUS StealthInit(const SvmbApi* api)
{
    g_api = *api;
    g_scrubHits = 0;
    // priority 10 > core CPUID handler (0): owned leaves never reach it
    return g_api.RegisterExitHandler(&g_api, vmexit::CPUID,
                                     StealthCpuidHandler, nullptr, 10);
}

void StealthStop()
{
    // Detach runs at PASSIVE_LEVEL; the counter read is approximate if a
    // CPUID exit lands concurrently - good enough for a summary line.
    g_api.LogInfo("stealth: detached, scrub hits=%lld",
                  (long long)InterlockedCompareExchange64(&g_scrubHits, 0, 0));
    g_api = {};
}

} // namespace

SVMB_DEFINE_MODULE(ModuleStealth, "stealth", 10, StealthInit, StealthStop)

// ---- pure scrub core (offline-tested) -----------------------------------

bool StealthScrubLeaf(u32 leaf)
{
    if (leaf == 1)
        return true;
    // 0x40000010 included on purpose: VMware answers it with the TSC/bus
    // frequency hint even though it is above the standard hypercall area.
    // The range deliberately stops well below svmb's own channels
    // (0x400000FE probe / 0x400000FF hypercall / 0x40000100 demo).
    return leaf >= 0x40000000 && leaf <= 0x40000010;
}

void StealthApplyScrub(u32 leaf, u32 res[4])
{
    if (leaf == 1)
    {
        res[2] &= ~(1u << 31);
        return;
    }
    res[0] = 0;
    res[1] = 0;
    res[2] = 0;
    res[3] = 0;
}

} // namespace svmb
