// debugger module (M4): intercepts #BP/#DB/#UD, reports each hit to
// the dbg event ring, and transparently reinjects so the guest's own
// handlers keep working. Attach = intercepts on; detach = off (the module
// framework revokes the exception intercepts automatically).
//
// Reinjection semantics per vector:
//   #BP (int3): TRAP-like - the saved rip already points past the int3;
//       reinject without advancing (r38: the old "skip the int3" advance
//       skipped one LIVE instruction in whatever process hit the int3 and
//       killed VMware Tools)
//   #DB (trap-like): rip already past the trigger - reinject, no advance
//   #UD (fault): rip at the invalid instruction - reinject, no advance
//
// DR shadowing: with HideDr, DR0-3/DR6/DR7 writes land in a per-CPU shadow
// and reads return it - the guest never sees (or clobbers) the real debug
// registers; hardware breakpoints set outside keep working. SpoofDr7Zero
// makes DR7 reads return 0. MTF single-step: see DebuggerArmSingleStep.
#include "modules/debugger.h"
#include "modules/dbg_events.h"
#include "core/hypervisor.h"
#include "svmb/module_api.h"
#include "platform/logger.h"
#include "platform/util.h"

namespace
{

using namespace svmb;

constexpr u32 MAX_DBG_CPUS = 256;

DrShadow g_drShadow[MAX_DBG_CPUS];
volatile LONG g_hideDr = 0;
volatile LONG g_spoofDr7 = 0;
volatile LONG g_stepArmed = 0;
// r39: opt-in exception-arming state (DebuggerConfigure / DebuggerStop)
volatile LONG g_excArmedBp = 0, g_excArmedDb = 0, g_excArmedUd = 0;
ModuleToken g_token = 0; // 0 until the module is attached

u64 RealDr(u32 dr)
{
    switch (dr)
    {
    case 0: return _svmb_read_dr0();
    case 1: return _svmb_read_dr1();
    case 2: return _svmb_read_dr2();
    case 3: return _svmb_read_dr3();
    case 7: return _svmb_read_dr7();
    default: return 0;
    }
}

void RealSetDr(u32 dr, u64 v)
{
    switch (dr)
    {
    case 0: _svmb_write_dr0(v); break;
    case 1: _svmb_write_dr1(v); break;
    case 2: _svmb_write_dr2(v); break;
    case 3: _svmb_write_dr3(v); break;
    case 7: _svmb_write_dr7(v); break;
    default: break;
    }
}

bool HandleExceptionExit(GuestContext& ctx, void*)
{
    u32 vector = (u32)(ctx.Exit - vmexit::EXCEPTION(0));
    SVMB_DBG_EVENT e = {};
    switch (vector)
    {
    case EXC_BP:
        e.Type = SvmbDbgEvtBreakpoint;
        break;
    case EXC_DB:
        e.Type = SvmbDbgEvtSingleStep;
        break;
    case EXC_UD:
        e.Type = SvmbDbgEvtInvalidOpcode;
        break;
    default:
        return false;
    }

    // MTF single-step: the armed step arrives as a #DB-class exit - consume
    // it (disarm, report, re-execute) without delivering a #DB to the guest
    if (vector == EXC_DB && InterlockedExchange(&g_stepArmed, 0) == 1)
    {
        e.Extra = 1; // marker: MTF-consumed step
        e.Core = ctx.CpuIndex();
        e.Rip = ctx.Regs->Rip;
        e.Cr3 = ctx.GuestCr3();
        DbgRingInstance()->Push(e);
        Hypervisor::Instance()->Intercepts().ReleaseMtf(g_token);
        return true; // rip unchanged: the stepped instruction re-executes
    }

    e.Core = ctx.CpuIndex();
    e.Rip = ctx.Regs->Rip;
    e.Rsp = ctx.Regs->Rsp;
    e.Cr3 = ctx.GuestCr3();
    DbgRingInstance()->Push(e);
    // r39: exception exits are rare and were invisible to every forensics
    // channel (ring drains via IOCTL, which dies exactly when these exits
    // happen) - mirror them into the kd-visible DbgPrint stream; the count
    // answers the Push-ran-but-Drain-saw-0 question in-band.
    SVMB_LOGW("exc exit: vec=%u rip=%llx cnt=%u",
              vector, (unsigned long long)ctx.Regs->Rip,
              (unsigned)(DbgRingInstance() ? DbgRingInstance()->Count() : 0));

    if (vector == EXC_BP)
        ctx.AdvanceRip = false; // int3 is TRAP-like: the saved rip already
                                // points PAST the int3 - advancing would
                                // skip one live instruction in whatever
                                // process hit it (r38: that is exactly how
                                // vmtoolsd died). Redeliver at the saved
                                // rip = standard int3 semantics.
    else
        ctx.AdvanceRip = false; // faults re-execute at the saved rip
    ReinfectExitEvent(ctx);
    return true;
}

bool HandleDrRead(GuestContext& ctx, void*)
{
    u32 dr = (u32)(ctx.Exit - vmexit::DR_READ(0));
    u32 gpr = ExitCrDrGpr(ctx.Info1); // destination GPR
    u32 cpu = CurrentCpuIndex();
    bool hide = g_hideDr != 0;
    bool spoof = g_spoofDr7 != 0;

    // hardware values are fetched only when hiding leaves them exposed
    u64 realDr = 0, realDr7 = 0;
    if (!hide)
    {
        if (dr == 7 && !spoof)
            realDr7 = _svmb_read_dr7();
        else if (dr <= 3)
            realDr = RealDr(dr);
    }
    GuestGpr(ctx.Regs, gpr) = DrResolveRead(dr, g_drShadow[cpu], hide, spoof,
                                            realDr, realDr7);
    ctx.AdvanceRip = true; // read emulated: resume past the mov
    return true;
}

bool HandleDrWrite(GuestContext& ctx, void*)
{
    u32 dr = (u32)(ctx.Exit - vmexit::DR_WRITE(0));
    u32 gpr = ExitCrDrGpr(ctx.Info1); // source GPR
    u32 cpu = CurrentCpuIndex();
    u64 val = GuestGpr(ctx.Regs, gpr);
    bool hide = g_hideDr != 0;

    if (dr <= 3)
    {
        if (hide)
            g_drShadow[cpu].Dr[dr] = val; // swallow into the shadow
        else
            RealSetDr(dr, val);
    }
    else if (dr == 7)
    {
        if (hide)
            g_drShadow[cpu].Dr7 = val;
        else
            _svmb_write_dr7(val);
    }
    else if (dr == 6)
    {
        if (hide)
            g_drShadow[cpu].Dr6 = val;
    }
    ctx.AdvanceRip = true; // write emulated: resume past the mov
    return true;
}

} // namespace

namespace svmb
{

u64 DrResolveRead(u32 dr, const DrShadow& shadow, bool hide, bool spoofDr7,
                  u64 realDr, u64 realDr7)
{
    if (dr == 7)
        return spoofDr7 ? 0 : (hide ? shadow.Dr7 : realDr7);
    if (dr <= 3)
        return hide ? shadow.Dr[dr] : realDr;
    if (dr == 6)
        return hide ? shadow.Dr6 : 0xFFFF0FF0ull; // power-on DR6
    return 0;
}

namespace
{

// republish DR intercepts for the current config (no-op before attach: the
// token is 0 and Init runs this once attached)
void ApplyDrIntercepts()
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || g_token == 0)
        return;
    bool hide = g_hideDr != 0;
    bool spoof = g_spoofDr7 != 0;
    for (u32 d = 0; d <= 3; ++d)
    {
        hv->Intercepts().ReleaseDr(g_token, d, true, true); // NOT_FOUND ignored
        if (hide)
            hv->Intercepts().RequireDr(g_token, d, true, true);
    }
    hv->Intercepts().ReleaseDr(g_token, 7, true, true);
    if (hide || spoof)
        hv->Intercepts().RequireDr(g_token, 7, true, true);
}

} // namespace

void DebuggerConfigure(const SVMB_DBG_CONFIG* cfg)
{
    if (!cfg)
        return;
    InterlockedExchange(&g_hideDr, cfg->HideDr ? 1 : 0);
    InterlockedExchange(&g_spoofDr7, cfg->SpoofDr7Zero ? 1 : 0);
    ApplyDrIntercepts();

    // r39: exception intercepts are OPT-IN via DBG_CONFIG, default OFF.
    // On the VMware vhv (vhv.enable=TRUE) any armed exception vector dies
    // by hypervisor NMI abort (bugcheck 0x80 'TDO') the first time it
    // fires - WHEA NMI source count=1 at the #UD reinjection RIP (r39,
    // twice reproduced, stack captured live in kd). Flags remain for
    // real-metal SVM tests.
    static const u32 vectors[3] = { EXC_BP, EXC_DB, EXC_UD };
    volatile LONG* armed[3] = { &g_excArmedBp, &g_excArmedDb, &g_excArmedUd };
    const u32 want[3] = { cfg->EnableBp ? 1u : 0u,
                          cfg->EnableDb ? 1u : 0u,
                          cfg->EnableUd ? 1u : 0u };
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || g_token == 0)
        return;
    InterceptManager& ic = hv->Intercepts();
    for (u32 i = 0; i < 3; ++i)
    {
        if (want[i] && !InterlockedExchange(armed[i], 1))
            ic.RequireException(g_token, vectors[i]);
        else if (!want[i] && InterlockedExchange(armed[i], 0))
            ic.ReleaseException(g_token, vectors[i]);
    }
}

bool DebuggerArmSingleStep()
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || g_token == 0)
        return false;
    NTSTATUS st = hv->Intercepts().RequireMtf(g_token);
    if (!NT_SUCCESS(st))
        return false;
    InterlockedExchange(&g_stepArmed, 1);
    return true;
}

NTSTATUS DebuggerInit(const SvmbApi* api)
{
    g_token = api->Token;
    NTSTATUS st;
    // r39: arm NOTHING in the exception vector set at attach. On this
    // VMware vhv any armed exception vector dies by NMI abort (0x80) the
    // first time it fires (see DebuggerConfigure) - and #BP/#DB additionally
    // collide with the always-on KDNET kernel debugger. The exit handlers
    // stay registered: harmless while nothing is armed, and the SVMB face
    // activates the moment DBG_CONFIG opts in.
    st = api->RegisterExitHandler(api, vmexit::EXCEPTION(EXC_UD),
                                  HandleExceptionExit, nullptr, 5);
    if (!NT_SUCCESS(st))
        return st;
    // r41: DR handlers were NEVER registered (only BP/DB/UD were) - with
    // the DR face armed the exits fell to the default policy, which
    // AdvanceRip'd PAST the mov without writing the destination GPR, so
    // the caller saw stale-register garbage instead of the spoof value.
    for (u32 d = 0; d < 8; ++d)
    {
        st = api->RegisterExitHandler(api, vmexit::DR_READ(d),
                                      HandleDrRead, nullptr, 5);
        if (!NT_SUCCESS(st))
            return st;
        st = api->RegisterExitHandler(api, vmexit::DR_WRITE(d),
                                      HandleDrWrite, nullptr, 5);
        if (!NT_SUCCESS(st))
            return st;
    }
    // DR intercepts are config-driven (see ApplyDrIntercepts) - nothing to
    // register until HideDr/SpoofDr7Zero is set via IOCTL_DBG_CONFIG
    ApplyDrIntercepts();
    return STATUS_SUCCESS;
}

void DebuggerStop()
{
    g_stepArmed = 0;
    // r39: release any DBG_CONFIG-armed exception intercepts so the
    // UnregisterOwner fold after module stop starts from a clean slate
    Hypervisor* hv = Hypervisor::Instance();
    if (hv && g_token != 0)
    {
        InterceptManager& ic = hv->Intercepts();
        if (InterlockedExchange(&g_excArmedBp, 0))
            ic.ReleaseException(g_token, EXC_BP);
        if (InterlockedExchange(&g_excArmedDb, 0))
            ic.ReleaseException(g_token, EXC_DB);
        if (InterlockedExchange(&g_excArmedUd, 0))
            ic.ReleaseException(g_token, EXC_UD);
    }
}


SVMB_DEFINE_MODULE(ModuleDebugger, "debugger", 0, DebuggerInit, DebuggerStop)

} // namespace svmb
