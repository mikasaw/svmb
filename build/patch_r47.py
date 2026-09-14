import io

p = "driver/src/modules/cr3_monitor.cpp"
s = io.open(p, encoding="utf-8").read()

# 1) includes
old = """#include "modules/cr3_monitor.h"
#include "core/hypervisor.h"
#include "modules/cr3_seed.h"
#include "modules/dbg_events.h"
#include "mm/npt.h"
#include "platform/logger.h"
#include "svmb/module_api.h"
"""
new = """#include "modules/cr3_monitor.h"
#include "core/hypervisor.h"
#include "modules/cr3_seed.h"
#include "modules/dbg_events.h"
#include "mm/npf.h"
#include "mm/npt.h"
#include "mm/tlb.h"
#include "platform/logger.h"
#include "svmb/module_api.h"
"""
assert s.count(old) == 1
s = s.replace(old, new)

# 2) sentinel registry + primitives, replacing the notify hook
old = """// seed notify hook: a freshly created process matching the watch name arms
// the target on the fly. Torn-read window on TargetImage vs a concurrent
// configure is accepted (worst case: one process is not matched; both
// threads run at PASSIVE and the config write is a one-shot 16-byte copy).
void OnSeedCreate(u32 pid, u64 cr3, const char* image, void*)
{
    if (!g_cfg.TargetImage[0] || !Cr3ImageMatch(image, g_cfg.TargetImage))
        return;
    InterlockedExchange64((volatile LONG64*)&g_cfg.TargetCr3, (LONG64)cr3);
    SVMB_LOGI("cr3 monitor: armed %s pid=%u cr3=%llx", image, pid, cr3);
"""
new = """// ---- r47 Route-A sentinel: watched-process tripwires --------------------
//
// Instead of the CR3-WRITE intercept (whole-system context-switch tax, the
// r43 death spiral), NPT-deny WRITES to the watched process's thread-object
// pages: the scheduler writes KTHREAD.State on every switch in/out, so one
// write-NPF per scheduling burst is the switch sensor. The deny seam
// (NpfSetDenyCallback) records + resolves immediately (r45 semantics - the
// instruction re-executes, nothing is skipped) and re-arms after a bounded
// window. Protection and sensing are strictly per-target.
struct SentinelPage
{
    volatile LONG64 Gpa4k; // 0 = free slot
    u32 Pid;
    void* PageVa;
};
constexpr u32 SENTINEL_MAX = 16;
constexpr LONG64 SENTINEL_REARM_MS = 100;
static SentinelPage g_sent[SENTINEL_MAX];
static volatile LONG g_sentArmed = 0;
static volatile LONG64 g_sentTrips = 0;
static KTIMER g_sentTimer;
static KDPC g_sentDpc;

void SentinelRearmPage(NptView* view, u64 gpa4k)
{
    // deny W again (R+X stay); the r46 primitive exactly
    (void)view->SetPerm4k(gpa4k, {false, true, true});
}

void SentinelRearmDpc(KDPC*, PVOID, PVOID, PVOID)
{
    NptManager* npt = NptInstance();
    if (!npt || !npt->NptEnabled())
        return;
    NptView* view = npt->Active();
    if (!view)
        return;
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        LONG64 g = g_sent[i].Gpa4k;
        if (g)
            SentinelRearmPage(view, (u64)g);
    }
}

// the deny seam: called from the NPF exit path (>= DISPATCH_LEVEL) for deny
// classifications; returns true when the faulting gpa belongs to the set
bool SentinelDenyCb(void*, u64 gpa4k, u64, u64 currentCr3, u64 rip)
{
    bool ours = false;
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        if (g_sent[i].Gpa4k == (LONG64)gpa4k)
        {
            ours = true;
            break;
        }
    }
    if (!ours)
        return false;

    InterlockedIncrement64(&g_sentTrips);

    // direction hint (telemetry): the write sequence leaving the target has
    // the target still current on this core; entering/unrelated co-resident
    // writes observe a different CR3. r48 turns this into view switching.
    bool outgoing = g_cfg.TargetCr3 != 0 && currentCr3 == g_cfg.TargetCr3;

    DbgEventRing* ring = DbgRingInstance();
    if (ring)
    {
        SVMB_DBG_EVENT e = {};
        e.Type = SvmbDbgEvtSentinelTrip;
        e.Core = KeGetCurrentProcessorNumberEx(nullptr);
        e.Rip = rip;
        e.Cr3 = currentCr3;
        e.Extra = (outgoing ? 1ull : 0ull) << 63 | gpa4k;
        ring->Push(e);
    }

    // resolve: restore the page so the faulting write re-executes for real
    NptManager* npt = NptInstance();
    if (npt && npt->NptEnabled())
    {
        if (NptView* view = npt->Active())
        {
            (void)view->SetPerm4k(gpa4k, {true, true, true});
            TlbInvlpgaLocal(rip & ~NptView::OFF_4K);
        }
    }
    // bounded sensing window: re-arm the deny after the switch burst
    LARGE_INTEGER due;
    due.QuadPart = -SENTINEL_REARM_MS * 10000; // relative, 100ms
    KeSetTimer(&g_sentTimer, due, &g_sentDpc);
    return true;
}

void SentinelArmPage(void* objVa, u32 pid)
{
    if (g_sentArmed >= SENTINEL_MAX)
        return;
    void* pageVa = (void*)((u64)objVa & ~0xFFFull);
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(pageVa);
    u64 gpa = pa.QuadPart & ~0xFFFull;
    if (!gpa)
        return;
    NptManager* npt = NptInstance();
    if (!npt || !npt->NptEnabled())
        return;
    NptView* view = npt->Active();
    if (!view)
        return;
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
        if (g_sent[i].Gpa4k == (LONG64)gpa)
            return; // two thread objects sharing a page: armed once
    if (!NT_SUCCESS(view->MapRange(gpa, 0x1000, {true, true, true})))
        return;
    if (!NT_SUCCESS(view->SetPerm4k(gpa, {false, true, true})))
        return;
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        if (InterlockedCompareExchange64(&g_sent[i].Gpa4k, 0, 0) == 0)
        {
            g_sent[i].Pid = pid;
            g_sent[i].PageVa = pageVa;
            InterlockedExchange64(&g_sent[i].Gpa4k, (LONG64)gpa);
            InterlockedIncrement(&g_sentArmed);
            SVMB_LOGI("sentinel: armed gpa=%llx pid=%u (thread obj page)",
                      gpa, pid);
            return;
        }
    }
}

void SentinelArmForProcess(PEPROCESS proc, u32 pid)
{
    // each thread object's first page holds its KTHREAD (State @ 0x2d is
    // scheduler-written on every switch); thread creation AFTER the walk is
    // r48+ work - for now the initial thread carries the sensing
    for (PETHREAD t = PsGetNextProcessThread(proc, nullptr); t;)
    {
        PETHREAD next = PsGetNextProcessThread(proc, t);
        SentinelArmPage((void*)t, pid);
        ObDereferenceObject(t);
        t = next;
    }
}

void SentinelDisarmProcess(u32 pid)
{
    NptManager* npt = NptInstance();
    NptView* view = npt && npt->NptEnabled() ? npt->Active() : nullptr;
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        if (g_sent[i].Gpa4k && g_sent[i].Pid == pid)
        {
            if (view)
                (void)view->SetPerm4k((u64)g_sent[i].Gpa4k,
                                      {true, true, true});
            InterlockedExchange64(&g_sent[i].Gpa4k, 0);
            InterlockedDecrement(&g_sentArmed);
        }
    }
}

void SentinelDisarmAll()
{
    NptManager* npt = NptInstance();
    NptView* view = npt && npt->NptEnabled() ? npt->Active() : nullptr;
    KeCancelTimer(&g_sentTimer);
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        LONG64 g = InterlockedExchange64(&g_sent[i].Gpa4k, 0);
        if (g)
        {
            if (view)
                (void)view->SetPerm4k((u64)g, {true, true, true});
            InterlockedDecrement(&g_sentArmed);
        }
    }
    SVMB_LOGI("sentinel: disarmed all");
}

// seed notify hook: a freshly created process matching the watch name arms
// the target on the fly (and its sentinel pages). Torn-read window on
// TargetImage vs a concurrent configure is accepted (worst case: one
// process is not matched; both threads run at PASSIVE and the config write
// is a one-shot 16-byte copy).
void OnSeedCreate(u32 pid, u64 cr3, const char* image, void*, PEPROCESS proc,
                  bool created)
{
    if (!created)
    {
        // process going away: its thread objects will be freed - the
        // tripwires must go with them (a deny left on recycled pool memory
        // is a live fault trap)
        SentinelDisarmProcess(pid);
        return;
    }
    if (!g_cfg.TargetImage[0] || !Cr3ImageMatch(image, g_cfg.TargetImage))
        return;
    InterlockedExchange64((volatile LONG64*)&g_cfg.TargetCr3, (LONG64)cr3);
    SVMB_LOGI("cr3 monitor: armed %s pid=%u cr3=%llx", image, pid, cr3);
    if (proc)
        SentinelArmForProcess(proc, pid);
"""
assert s.count(old) == 1
s = s.replace(old, new)

# 3) unwatch clear path -> disarm all
old = """    else
    {
        g_cfg.TargetImage[0] = 0;
        if (seed)
            seed->SetNotifyCallback(nullptr, nullptr);"""
new = """    else
    {
        g_cfg.TargetImage[0] = 0;
        SentinelDisarmAll();
        if (seed)
            seed->SetNotifyCallback(nullptr, nullptr);"""
assert s.count(old) == 1
s = s.replace(old, new)

# 4) init: timer/dpc + deny seam registration; stop: disarm
old = """NTSTATUS Cr3MonInit(const SvmbApi* api)
{"""
new = """NTSTATUS Cr3MonInit(const SvmbApi* api)
{
    KeInitializeTimer(&g_sentTimer);
    KeInitializeDpc(&g_sentDpc, SentinelRearmDpc, nullptr);
    NpfSetDenyCallback(SentinelDenyCb, nullptr);"""
assert s.count(old) == 1
s = s.replace(old, new)

old = """void Cr3MonStop()
{
    g_readExits = 0;
    g_writeExits = 0;"""
new = """void Cr3MonStop()
{
    SentinelDisarmAll();
    g_readExits = 0;
    g_writeExits = 0;"""
assert s.count(old) == 1
s = s.replace(old, new)

# 5) stats: expose trips in Reserved (no layout change)
old = """    st->MonitoredCount = Cr3SeedInstance() ? Cr3SeedInstance()->Count() : 0;
}"""
new = """    st->MonitoredCount = Cr3SeedInstance() ? Cr3SeedInstance()->Count() : 0;
    st->Reserved = (u32)g_sentTrips; // r47: sentinel trips (low 32 bits)
}"""
assert s.count(old) == 1
s = s.replace(old, new)

io.open(p, "w", encoding="utf-8", newline="\n").write(s)
print("cr3_monitor.cpp OK")
