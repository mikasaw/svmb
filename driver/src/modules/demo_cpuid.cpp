// demo_cpuid - example svmb module.
// Demonstrates the extension contract: this file + one registry entry in
// src/core/module.cpp is everything needed to add a new interception.
//
// Behavior: answers CPUID leaf 0x40000100 with an "SVMBHV" signature and
// counts the hits. The handler runs at a higher priority than the core's
// CPUID passthrough; unrecognized leaves fall through to the core.
#include "svmb/module_api.h"

namespace
{

using namespace svmb;

SvmbApi gApi = {}; // BY-VALUE copy (r96 audit ST3): BuildApi hands Init a
                   // stack local - storing the pointer would dangle (the
                   // stealth module sets the precedent)
volatile LONG64 gHits = 0;

constexpr u32 DEMO_CPUID_LEAF = 0x40000100;

bool DemoCpuidHandler(GuestContext& ctx, void* userData)
{
    UNREFERENCED_PARAMETER(userData);
    if ((u32)ctx.Regs->Rax != DEMO_CPUID_LEAF)
        return false; // not ours: let the chain continue

    ctx.Regs->Rax = 'B' << 24 | 'M' << 16 | 'V' << 8 | 'S'; // "SVMB"
    ctx.Regs->Rbx = 0x00010000;                              // demo version
    ctx.Regs->Rcx = (u32)ctx.CpuIndex();
    ctx.Regs->Rdx = InterlockedIncrement64(&gHits);
    ctx.AdvanceRip = true;
    return true;
}

NTSTATUS DemoInit(const SvmbApi* api)
{
    gApi = *api;
    gHits = 0;
    // priority 10 > core passthrough (0): we see the leaf first
    return gApi.RegisterExitHandler(&gApi, vmexit::CPUID, DemoCpuidHandler,
                                    nullptr, 10);
}

void DemoStop()
{
    gApi = {};
}

} // namespace

namespace svmb
{
SVMB_DEFINE_MODULE(ModuleDemoCpuid, "demo_cpuid", 0, DemoInit, DemoStop)
}
