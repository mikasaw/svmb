#include "modules/cr3_monitor.h"
#include "core/crumbs.h"
#include "core/hypervisor.h"
#include "modules/cr3_seed.h"
#include "modules/dbg_events.h"
#include "modules/mem_read.h"
#include "mm/npf.h"
#include "mm/npt.h"
#include "mm/tlb.h"
#include "platform/offsets.h"
#include "platform/logger.h"
#include "svmb/module_api.h"

namespace
{

using namespace svmb;

struct Cr3MonConfig
{
    volatile LONG ReadSpoof = 0;
    volatile LONG WriteMonitor = 0;
    volatile LONG ProcessView = 0;
    volatile u64 XorKey = 0;
    volatile u64 TargetCr3 = 0;
    char TargetImage[16] = {}; // watch name; written only by the IOCTL gate
    u32 ProcViewId = 0;        // created+filled lazily by the IOCTL gate
};
Cr3MonConfig g_cfg;
volatile LONG64 g_readExits = 0;
volatile LONG64 g_writeExits = 0;
volatile LONG64 g_viewSwitches = 0;
volatile LONG g_switchWarnLogs = 0;
ModuleToken g_cr3Token = 0; // r43: for config-time CR3-intercept arming

// r43: CR3 R/W intercepts are the whole-system context-switch tax on this
// vhv (~2500 exits/s measured, ~26us each) - arming them at module INIT
// starved the guest into timeouts that looked like wedge + self-unload
// (r42/r43). Arm only while a target is actually configured, release when
// the config clears.
void ApplyCr3Intercepts()
{
    Hypervisor* hv = Hypervisor::Instance();
    if (!hv || g_cr3Token == 0)
        return;
    // r43: the CR3-WRITE intercept taxes every context switch on every
    // core - sustained rates hit a death spiral (slower cores -> more
    // switches -> more exits; ~2500 exits/s measured idle-armed, load
    // spikes far beyond) and the guest starves to death (r42 "wedge" +
    // self-unload). Reads are rare and cheap: the spoof needs only the
    // read side. The write side arms ONLY with an explicit
    // EnableWriteMonitor config (and is a known-hazard feature here).
    // r48 LAW (r43's "reads are cheap" falsified): a sustained CR3-READ
    // intercept melts this vhv just like the write side - watch-by-name
    // alone killed guests in <50s (r48 A/B: reproduces on the r46 build).
    // The read side arms ONLY while an actual spoofable target process
    // exists (seeded CR3 + xor key); name-watch keeps everything disarmed.
    bool wantRead = g_cfg.TargetCr3 != 0 && g_cfg.XorKey != 0;
    bool wantWrite = wantRead && g_cfg.WriteMonitor != 0;
    static volatile LONG s_armedR = 0, s_armedW = 0;
    if (wantRead && !InterlockedExchange(&s_armedR, 1))
    {
        NTSTATUS st = hv->Intercepts().RequireCr(g_cr3Token, 3, true, false);
        SVMB_LOGI("cr3mon: arm READ st=%08x tok=%u", st, g_cr3Token);
    }
    else if (!wantRead && InterlockedExchange(&s_armedR, 0))
    {
        NTSTATUS st = hv->Intercepts().ReleaseCr(g_cr3Token, 3, true, false);
        SVMB_LOGI("cr3mon: disarm READ st=%08x", st);
    }
    if (wantWrite && !InterlockedExchange(&s_armedW, 1))
        hv->Intercepts().RequireCr(g_cr3Token, 3, false, true);
    else if (!wantWrite && InterlockedExchange(&s_armedW, 0))
        hv->Intercepts().ReleaseCr(g_cr3Token, 3, false, true);
}

// ---- r47 Route-A sentinel: watched-process tripwires --------------------
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
    void* ObjVa; // r72: the thread OBJECT (not page-aligned); its first
                 // dword is the KTHREAD dispatcher header - a stable
                 // identity for a live object, unlike the page start
                 // (pool header / neighbour tail data that drifts)
    volatile LONG Watermark; // r68: KTHREAD dispatcher-header dword sampled at
                             // arm time. Live thread objects never change it;
                             // a freed+reused frame almost surely does. The
                             // foreign-trip kill-switch and the re-arm DPC
                             // both compare it before trusting the deny.
    volatile LONG64 ResolveTick; // interrupt-time ms of the last resolve;
                                 // the re-arm DPC must not re-deny a page
                                 // whose faulting write has not landed yet
                                 // (r50: NPF/DPC bounce = soft reset)
};
constexpr u32 SENTINEL_MAX = 16;
constexpr ULONG SENTINEL_REARM_MS = 100; // deny-rest window after a resolve
static SentinelPage g_sent[SENTINEL_MAX];
static volatile LONG g_sentArmed = 0;
static volatile LONG64 g_sentTrips = 0;
static volatile LONG64 g_sentReuse = 0; // r68: watermark kill-switch hits
static KDPC g_sentDpc;
static volatile u32 g_watchPid = 0; // single-target design

// r68: GPAs whose deny was just torn down (disarm / kill-switch). A trip
// still in flight for one of them arrives as cb-UNMATCHED after the slot is
// gone; resolving it instead of feeding the generic spin-breaker avoids the
// 256-exit tax and the spurious-fault window during teardown races.
static volatile LONG64 g_disarmedGpa[4] = {};
static volatile LONG g_disarmIdx = 0;
// r74: log cap for the orphan/after-disarm resolve paths (burst-safe)
static volatile LONG g_orphanLogs = 0;

// ---- r75 protected regions: L1 block (guest MM) + L2 sense (NPT) --------
//
// L1 blocking: ZwProtectVirtualMemory(PAGE_READONLY) on the registered
// region in the owning process - the GUEST's own MM delivers the fault
// (VAD=readonly prevents self-heal for private memory). Zero exception
// injection: this vhv drops injected events (the r39 TDO-NMI family), so
// the NPT layer below NEVER blocks - it senses L1 bypasses only.
struct ProtRegion
{
    volatile LONG64 Active; // 0 = free, else first-page GPA
    u32 Id;
    u32 Pid;
    void* BaseVa;
    u64 SizeBytes;
    u32 Pages;
    u32 OrigProtect;
    volatile LONG64 ResolveTick;
    void* PinMdl; // r86 audit P1-3: IoReadAccess page lock held for the
                  // region lifetime - without it the OS can page the
                  // region out, free the frames, and strand the W-deny on
                  // recycled memory (storm + false alerts + sensing loss)
    volatile LONG Policy; // r89: SVMB_PROT_POLICY_* (0 = alert-only)
    // r89 plan C evidence: CR3/RIP/GPA of the latest bypass writer
    volatile LONG64 WriterCr3;
    volatile LONG64 WriterRip;
    volatile LONG64 WriterGpa;
};
constexpr u32 PROT_MAX = 32;
constexpr u32 PROT_MAX_PAGES = 64;
static ProtRegion g_prot[PROT_MAX];
static volatile LONG g_protCount = 0;

// ---- r89 plan C: alert + kill pipeline (docs/L2_BLOCKING_DESIGN.md 3-C) -
// trip (any IRQL) -> KeInsertQueueDpc (any-IRQL legal) -> DPC DISPATCH ->
// IoQueueWorkItem -> PASSIVE workitem terminates the attributed writer.
// The exit path never touches work-queue spinlocks (r77-P0 law), and
// PASSIVE-only ZwTerminateProcess runs on a system worker thread.
// ntifs-only surface (declarations also appear below, near their r75
// peers - identical extern "C" declarations merge)
extern "C" NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId,
                                               PEPROCESS* Process);
extern "C" PCHAR PsGetProcessImageFileName(PEPROCESS Process);
extern "C" NTSTATUS ObOpenObjectByPointer(PVOID Object,
                                          ULONG HandleAttributes,
                                          PVOID PassedAccessState,
                                          ACCESS_MASK DesiredAccess,
                                          POBJECT_TYPE ObjectType,
                                          KPROCESSOR_MODE AccessMode,
                                          PHANDLE Handle);
#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE 0x0001
#endif
static volatile LONG g_killInFlight = 0;   // 0..1: one kill at a time; a
                                           // trip while pending is dropped
                                           // (s_lastKick-style rate limit)
static volatile LONG64 g_killAttempts = 0;
static volatile LONG64 g_killDenylisted = 0;
static volatile LONG64 g_killDropped = 0;
static volatile LONG64 g_suspAttempts = 0;
static volatile LONG64 g_suspDenylisted = 0;
static volatile LONG64 g_suspDropped = 0;
static KDPC g_killDpc;
static PIO_WORKITEM g_killItem = nullptr;
// single-pending handoff (exit path writes, workitem consumes)
static volatile LONG64 g_killCr3 = 0;
static volatile LONG64 g_killRip = 0;
static volatile LONG64 g_killGpa = 0;
static volatile LONG g_killMode = 0;      // SVMB_PROT_POLICY_* of the trip
static volatile LONG64 g_killRegionId = 0;

// ---- r91 plan C-: suspension table + runtime-resolved primitives -------
// PsSuspendProcess/PsResumeProcess ARE exported on 18362 (verified by
// export table - the design doc's "2004+" assumption was wrong), but they
// are absent from ntddk.h and may be missing on other builds: resolve via
// MmGetSystemRoutineAddress, degrade honestly (REGISTER refuses SUSPEND).
typedef NTSTATUS (*PS_SUSPEND_PROCESS_FN)(PEPROCESS Process);
typedef NTSTATUS (*PS_RESUME_PROCESS_FN)(PEPROCESS Process);
static PS_SUSPEND_PROCESS_FN g_pfnSuspendProcess = nullptr;
static PS_RESUME_PROCESS_FN g_pfnResumeProcess = nullptr;

// suspended writers: reversible state, listed via IOCTL_SUSP_LIST and
// released via IOCTL_SUSP_RESUME (both UNGATED - the suspended process
// holds the gate inside its in-flight bypass IOCTL)
struct SuspEntry
{
    u32 Pid;      // 0 = free
    u32 RegionId;
    u64 Tick;
    char Image[16];
};
static SuspEntry g_susp[16];
static KSPIN_LOCK g_suspLock;
static volatile LONG g_suspCount = 0;

// DISPATCH/PASSIVE with g_suspLock held
static SuspEntry* SuspFindLocked(u32 pid)
{
    for (u32 i = 0; i < 16; ++i)
        if (g_susp[i].Pid == pid)
            return &g_susp[i];
    return nullptr;
}

static SuspEntry* SuspFreeLocked()
{
    for (u32 i = 0; i < 16; ++i)
        if (g_susp[i].Pid == 0)
            return &g_susp[i];
    return nullptr;
}

// death notify prune: a suspended process that died by other means (or
// after an external resume+exit) leaves the table
void SuspPrunePid(u32 pid)
{
    KIRQL irql = KeGetCurrentIrql();
    KeAcquireSpinLock(&g_suspLock, &irql);
    SuspEntry* e = SuspFindLocked(pid);
    if (e)
    {
        RtlZeroMemory(e, sizeof(*e));
        InterlockedDecrement(&g_suspCount);
    }
    KeReleaseSpinLock(&g_suspLock, irql);
}

// DISPATCH: hand the kill to a PASSIVE workitem. IoQueueWorkItem takes
// queue spinlocks internally - legal HERE (DISPATCH), never from the exit
// path directly (design doc 3-C-2 / r77-P0 law).
VOID KillWorkItem(PDEVICE_OBJECT, PVOID);

VOID KillDpcRoutine(PKDPC, PVOID, PVOID, PVOID)
{
    CrumbPost(CRUMB_KILL_ENTER);
    if (!g_killItem)
    {
        // pipeline never initialized (no device object): drop and unblock
        InterlockedIncrement64(&g_killDropped);
        InterlockedExchange(&g_killInFlight, 0);
        return;
    }
    IoQueueWorkItem(g_killItem, KillWorkItem, CriticalWorkQueue, nullptr);
}

// PASSIVE: attribute the writer, apply the deny list, then either
// terminate (plan C) or suspend (plan C-, reversible) per the queued mode.
static u32 PolicyResolveWriter(u64 cr3, char* image, bool suspend)
{
    u32 pid = 0;
    Cr3Seed* seed = Cr3SeedInstance();
    // refusal reasons all funnel into the Denylisted stats: pid<=4
    // (r96 audit K2: the guard must cover System itself, pid 4 - until
    // then the shield was only the seed-coverage accident), an image on
    // the deny list, or an unresolvable CR3 (pre-load / spoofed)
    if (!seed || !seed->LookupPidByCr3(cr3, pid, image) || pid <= 4
        || Cr3RegionKillDenylisted(image))
    {
        if (suspend)
            InterlockedIncrement64(&g_suspDenylisted);
        else
            InterlockedIncrement64(&g_killDenylisted);
        SVMB_LOGW("prot: policy skip cr3=%llx pid=%u image='%s' "
                  "(unresolvable/deny-list)",
                  cr3, pid, image);
        return 0;
    }
    return pid;
}

VOID KillWorkItem(PDEVICE_OBJECT, PVOID)
{
    const u64 cr3 = InterlockedExchange64(&g_killCr3, 0);
    const u64 rip = InterlockedExchange64(&g_killRip, 0);
    const u64 gpa = InterlockedExchange64(&g_killGpa, 0);
    const LONG mode = InterlockedExchange(&g_killMode, 0);
    const u64 regionId = InterlockedExchange64(&g_killRegionId, 0);
    const bool suspend = (mode == SVMB_PROT_POLICY_SUSPEND);
    __analysis_assume(cr3 != 0);

    char image[16] = {};
    u32 pid = PolicyResolveWriter(cr3, image, suspend);
    if (pid)
    {
        PEPROCESS proc = nullptr;
        if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid,
                                                  &proc))
            && proc)
        {
            NTSTATUS st = STATUS_SUCCESS;
            bool suspendPlanned = false;
            // r106/K3 (user decision 2026-09-14): re-verify the image
            // against the LIVE EPROCESS before any response. The
            // trip-time image came from CR3 attribution - spoofable
            // (audit K1) - and the pid may have been recycled inside the
            // DPC->workitem window; the deny list is name-based, so the
            // live name is the only trustworthy gate. The live name also
            // becomes the canonical image for the table/log below
            // (r105 shipped the same pattern for the sysalert suspend).
            char liveImage[16] = {};
            RtlCopyMemory(liveImage, PsGetProcessImageFileName(proc), 15);
            liveImage[15] = '\0';
            const bool liveDeny =
                (pid <= 4) || Cr3RegionKillDenylisted(liveImage);
            if (liveDeny)
            {
                InterlockedIncrement64(suspend ? &g_suspDenylisted
                                               : &g_killDenylisted);
                SVMB_LOGE("prot: response REFUSED pid=%u - live EPROCESS "
                          "image '%s' deny-listed (K3)",
                          pid, liveImage);
            }
            else
            {
                RtlCopyMemory(image, liveImage, sizeof(image));
            }
            if (liveDeny)
            {
                // response refused; accounting above, deref at scope end
            }
            else if (suspend && g_pfnSuspendProcess)
            {
                // r96 audit S1: check for a usable slot BEFORE freezing -
                // a full table must not strand the writer frozen-but-
                // unlisted across unload (resume-all sweeps table entries
                // only). Full table = freeze declined, alert-only.
                // r105 (second filler exists: the sysalert suspend): the
                // check->freeze->insert window is no longer exclusive.
                // (a) an ALREADY-LISTED pid must not re-suspend -
                // PsSuspendProcess is cumulative and the single table
                // entry repays only one refcount; (b) the insert-time
                // full-table path now has the r96-ticketed resume belt.
                KIRQL irql = KeGetCurrentIrql();
                KeAcquireSpinLock(&g_suspLock, &irql);
                const bool alreadyListed = SuspFindLocked(pid) != nullptr;
                suspendPlanned = alreadyListed
                                 || SuspFreeLocked() != nullptr;
                KeReleaseSpinLock(&g_suspLock, irql);
                if (alreadyListed)
                {
                    // cumulative-refcount guard: already frozen and
                    // listed - one entry repays one suspend; refresh the
                    // heat only
                    suspendPlanned = false;
                    InterlockedIncrement64(&g_suspDenylisted);
                    SVMB_LOGW("prot: SUSPEND pid=%u already listed - no "
                              "extra freeze refcount",
                              pid);
                }
                else if (!suspendPlanned)
                {
                    InterlockedIncrement64(&g_suspDropped);
                    SVMB_LOGE("prot: SUSPEND declined pid=%u - table FULL, "
                              "no freeze (unload would strand it)",
                              pid);
                }
            }
            if (suspendPlanned)
            {
                // plan C-: reversible - the writer lands in the suspension
                // table; IOCTL_SUSP_RESUME releases it
                bool belt = false;
                st = g_pfnSuspendProcess(proc);
                if (NT_SUCCESS(st))
                {
                    KIRQL irql = KeGetCurrentIrql();
                    KeAcquireSpinLock(&g_suspLock, &irql);
                    SuspEntry* e = SuspFindLocked(pid);
                    if (!e)
                        e = SuspFreeLocked();
                    if (e)
                    {
                        bool fresh = (e->Pid != pid);
                        e->Pid = pid;
                        e->RegionId = (u32)regionId;
                        e->Tick = (u64)KeQueryInterruptTime();
                        RtlCopyMemory(e->Image, image, sizeof(e->Image));
                        e->Image[15] = '\0';
                        if (fresh)
                            InterlockedIncrement(&g_suspCount);
                    }
                    else
                    {
                        // r105 P1-3: the sysalert filler can take the
                        // last slot inside this window - the r96-ticketed
                        // resume belt fires (resume OUTSIDE the lock
                        // below, PsResumeProcess is PASSIVE-only); the
                        // r91 P2-2 stranding shape is gone
                        belt = true;
                        InterlockedIncrement64(&g_suspDropped);
                    }
                    KeReleaseSpinLock(&g_suspLock, irql);
                    if (belt)
                    {
                        g_pfnResumeProcess(proc);
                        SVMB_LOGE("prot: SUSPEND table FULL pid=%u - "
                                  "resume belt fired (no strand)",
                                  pid);
                    }
                    SVMB_LOGW("prot: SUSPEND policy writer pid=%u "
                              "image='%s' cr3=%llx rip=%llx gpa=%llx "
                              "st=%08x",
                              pid, image, cr3, rip, gpa, st);
                }
                else
                {
                    SVMB_LOGE("prot: SUSPEND failed pid=%u st=%08x", pid,
                              st);
                }
            }
            else if (suspend && !liveDeny && !g_pfnSuspendProcess)
            {
                // r96 audit K4: a SUSPEND-region trip must never escalate
                // into termination. REGISTER already refuses SUSPEND when
                // the export is absent - this is the defensive twin for
                // the trip-time gap.
                SVMB_LOGE("prot: SUSPEND primitive unresolved pid=%u - "
                          "no action",
                          pid);
            }
            else if (!suspend && !liveDeny)
            {
                HANDLE hProc = nullptr;
                OBJECT_ATTRIBUTES ob;
                InitializeObjectAttributes(&ob, nullptr, OBJ_KERNEL_HANDLE,
                                           nullptr, nullptr);
                st = ObOpenObjectByPointer(proc, OBJ_KERNEL_HANDLE,
                                           nullptr, PROCESS_TERMINATE,
                                           *PsProcessType, KernelMode,
                                           &hProc);
                if (NT_SUCCESS(st))
                {
                    // deterrence model: the first bypass write already
                    // landed; the kill stops every later one (documented
                    // semantics). STATUS_ACCESS_DENIED reads as an access
                    // violation to any surviving terminator.
                    // K1 DECISION (user, 2026-09-14): the r89 model
                    // stands - ANY attributed bypass writer with a
                    // non-deny-listed LIVE image is responded to;
                    // CR3-spoof misattribution is accepted (audit K1
                    // closed WONTFIX). The K3 live-image gate above
                    // refuses deny-listed names (both response kinds).
                    st = ZwTerminateProcess(hProc, STATUS_ACCESS_DENIED);
                    ObCloseHandle(hProc, KernelMode);
                    SVMB_LOGW("prot: KILL policy writer pid=%u image='%s' "
                              "cr3=%llx rip=%llx gpa=%llx st=%08x",
                              pid, image, cr3, rip, gpa, st);
                }
                else
                {
                    SVMB_LOGE("prot: KILL open failed pid=%u st=%08x", pid,
                              st);
                }
            }
            ObDereferenceObject(proc);
        }
        else
        {
            if (suspend)
                InterlockedIncrement64(&g_suspDenylisted);
            else
                InterlockedIncrement64(&g_killDenylisted);
            SVMB_LOGW("prot: policy skip - pid %u lookup failed", pid);
        }
    }
    InterlockedExchange(&g_killInFlight, 0);
}
static volatile LONG64 g_regionTrips = 0; // r76: L2-bypass alerts on regions
static volatile LONG64 g_wiggleKicks = 0; // r77: shadow-kick count
// r80 wiggle mode: 0=off, 1=back-to-back kick (r77 - the scratch NCr3 may
// never be observed by VMRUN), 2=real wiggle: phase-1 broadcast to the
// scratch view before returning to the guest (the storming write lands on
// identity 2M RWX at the very next VMRUN), phase-2 per-VCPU restore from
// the re-arm DPC. Registry Parameters\WiggleMode.
static volatile LONG g_wiggleMode = 2;
static volatile LONG g_wiggleRestorePending = 0;
static u32 g_protNextId = 1;

// r76 teardown grace: pages whose region deny was just torn down. A write
// racing the unprotect/death-sweep finds no slot to match - the consumer
// must still resolve and swallow the fault or the Release fail-fast fires
// (0xE2 'SVMC'). Mirrors the sentinel's r68 disarm ring.
// r97 (P3-17 closure): capacity raised 256 -> 2048 to cover the full
// worst-case teardown surface (PROT_MAX 32 regions x 64 pages) - a churn
// storm can no longer evict an entry before its racing write arrives.
// Note the eviction consequence was already non-fatal since r85 P1-2
// removed the Release fail-fast: an evicted stale write falls into the
// r45 resolve/heal semantics, so this ring is a consistency grace, not a
// safety cliff. The linear scan stays: the lookup only runs on the rare
// bypass-write NPF exit.
constexpr u32 PROT_DISARM_RING = 2048;
static volatile LONG64 g_regionDisarmed[PROT_DISARM_RING] = {};
static volatile LONG g_regionDisarmIdx = 0;
static void RegionNoteDisarmed(u64 gpa4k)
{
    // r97 review: mask, not modulo - a signed LONG wraps negative after
    // 2^31 notes and % would index OOB; 2048 is a power of two
    LONG k = InterlockedIncrement(&g_regionDisarmIdx)
             & (LONG)(PROT_DISARM_RING - 1);
    InterlockedExchange64(&g_regionDisarmed[k], (LONG64)gpa4k);
}
static bool RegionRecentlyDisarmed(u64 gpa4k)
{
    for (u32 k = 0; k < PROT_DISARM_RING; ++k)
        if (InterlockedCompareExchange64(&g_regionDisarmed[k], 0, 0) ==
            (LONG64)gpa4k)
            return true;
    return false;
}

// NPF-path consumer: a write landed on a protected-region page (rare -
// the guest PTE is readonly, so this is an L1 bypass, e.g. a kernel
// physical-map writer). Resolve + re-arm (sensing semantics) + alert.
bool SentinelRegionConsume(u64 gpa4k, u64 currentCr3, u64 rip);
void SentinelNoteDisarmed(u64 gpa)
{
    LONG k = InterlockedIncrement(&g_disarmIdx) & 3;
    InterlockedExchange64(&g_disarmedGpa[k], (LONG64)gpa);
}
bool SentinelRecentlyDisarmed(u64 gpa)
{
    for (u32 k = 0; k < 4; ++k)
        if (InterlockedCompareExchange64(&g_disarmedGpa[k], 0, 0) ==
            (LONG64)gpa)
            return true;
    return false;
}

// r72: watermark comparison mask. The dispatcher header's Absolute/
// Inserted bytes (1, 3) drift on LIVE objects under scheduling churn -
// r71 saw 8 natural kills in 30 minutes, each costing a sensing slot.
// Only Type (byte 0) and Size (byte 2) are stable object identity; a
// recycled frame almost surely changes those too.
constexpr u32 SENTINEL_WM_MASK = 0x00FF00FF;
bool SentinelWmMatches(u32 cur, LONG wm)
{
    return (cur & SENTINEL_WM_MASK) == ((u32)wm & SENTINEL_WM_MASK);
}

// deferred arm: at process-create notify the initial thread is neither on
// ThreadListHead nor has its thread-notify fired for a just-set watch pid,
// so arming must run ~100ms later, at PASSIVE, via this worker
static KEVENT g_armEvent;
static HANDLE g_armThread = nullptr;
static volatile LONG g_armShutdown = 0;
static volatile LONG g_armPending = 0;
static volatile LONG g_armWorkerLive = 0; // set only after the system
                                          // thread exists (r56: the watch
                                          // IOCTL is reachable pre-attach)
static volatile u32 g_armPid = 0;
static volatile u32 g_armWakes = 0;

// r49 skip-why codes surfaced through CR3_STATS.SentinelDiag high16
enum
{
    kSentWhyArmed = 0,
    kSentWhyEmptyList = 1,
    kSentWhyFull = 2,
    kSentWhyGpa0 = 3,
    kSentWhyNptOff = 4,
    kSentWhyNoView = 5,
    kSentWhyDup = 6,
    kSentWhyMapFail = 7,
    kSentWhyPermFail = 8,
    kSentWhyLookupFail = 9,
    kSentWhyReuse = 10, // r68: deny survived onto a recycled frame
    kSentWhyNoOffsets = 11, // r83: layout unresolved - walk refused
};
static volatile u32 g_lastWhy = kSentWhyArmed;
static bool g_offsetsOk = false; // r83: set once OffsetsResolve succeeds

// r48: thread-create notify is the ONLY reliable arm point - at process
// -create notify time the initial thread is NOT yet on ThreadListHead
// (the r47 offset walk saw an empty list every time). The callback hands
// us the tid; PsLookupThreadByThreadId resolves the object directly.
// documented export; declaration absent from the included headers
extern "C" NTSTATUS PsLookupThreadByThreadId(HANDLE ThreadId,
                                             PETHREAD* Thread);
extern "C" NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId,
                                               PEPROCESS* Process);
// r89 plan C: kill pipeline needs ntifs-only surface (ntddk-only driver);
// PROCESS_TERMINATE mirrors ntifs.h
#ifndef PROCESS_TERMINATE
#define PROCESS_TERMINATE 0x0001
#endif
extern "C" NTSTATUS ObOpenObjectByPointer(PVOID Object, ULONG HandleAttributes,
                                          PVOID PassedAccessState,
                                          ACCESS_MASK DesiredAccess,
                                          POBJECT_TYPE ObjectType,
                                          KPROCESSOR_MODE AccessMode,
                                          PHANDLE Handle);
void SentinelArmPage(void* objVa, u32 pid, const char** why);
void SentinelArmForProcess(PEPROCESS proc, u32 pid);
void SentinelDisarmPage(void* pageVa);

static VOID SentinelThreadNotify(HANDLE tid, HANDLE pid, BOOLEAN created)
{
    u32 p = (u32)(ULONG_PTR)pid;
    if (g_watchPid != p)
        return;
    if (!created)
    {
        // r62 SLOW-MELT FIX: an exiting thread's object page is freed and
        // recycled by the pool while its deny + slot stay live - the
        // process-exit notify only fires at LAST-thread death, so every
        // mid-flight thread exit leaves a denied page on memory that is
        // about to be handed to arbitrary allocations (permanent random
        // NPF traps = the r61 armed-watch slow-form suspect (c): ~12min
        // armed -> all-core soft reset, no dump). Disarm this thread's
        // page now. A co-resident live thread on the same page loses
        // sensing here - correctness over completeness.
        PETHREAD t = nullptr;
        if (NT_SUCCESS(PsLookupThreadByThreadId(tid, &t)))
        {
            SentinelDisarmPage((void*)t);
            ObDereferenceObject(t);
        }
        else
        {
            SVMB_LOGW("sentinel: thread exit lookup fail tid=%llx - page "
                      "deny may linger", (u64)(ULONG_PTR)tid);
        }
        return;
    }
    PETHREAD t = nullptr;
    if (NT_SUCCESS(PsLookupThreadByThreadId(tid, &t)))
    {
        const char* why = nullptr;
        SentinelArmPage((void*)t, p, &why);
        ObDereferenceObject(t);
    }
}

void SentinelRearmPage(NptView* view, u64 gpa4k)
{
    // deny W again (R+X stay); the r46 primitive exactly
    (void)view->SetPerm4k(gpa4k, {false, true, true});
}

// r56 re-arm batch: one IPI broadcast carries every due page's GVA
struct SentinelFlushBatch
{
    void* Va[SENTINEL_MAX];
    u32 Count;
};

extern "C" ULONG_PTR __fastcall SentinelFlushBatchCb(PVOID context,
                                                     ULONG_PTR)
{
    SentinelFlushBatch* b = (SentinelFlushBatch*)context;
    for (u32 i = 0; i < b->Count; ++i)
        TlbInvlpgaLocal((u64)b->Va[i]);
    return 0;
}

// r57: sentinel sensing must survive in-target execution - the per-core
// policy runs the ProcView while the target is current, so a deny armed
// only in the base view goes silent exactly when direction=out=1 trips
// would fire. Every perm op applies to BOTH views (dedup: the ProcView
// may BE the active view when the manager switched globally).
u32 SentinelViews(NptView** views)
{
    u32 n = 0;
    NptManager* npt = NptInstance();
    if (!npt || !npt->NptEnabled())
        return 0;
    if (npt->Active())
        views[n++] = npt->Active();
    if (g_cfg.ProcViewId)
    {
        NptView* pv = npt->View(g_cfg.ProcViewId);
        if (pv && pv != npt->Active())
            views[n++] = pv;
    }
    return n;
}

// DISPATCH_LEVEL: re-arms every armed page (called shortly after each trip)
void SentinelRearmDpc(KDPC*, PVOID, PVOID, PVOID)
{
    // r80 wiggle phase-2: the re-arm DPC is the guaranteed post-VMRUN
    // point of the kick - restore every core to its own pinned view
    // (PublishedNcr3, else manager-active) before re-denying, so the
    // re-arm denies go live on views the cores actually run on.
    // CORRECTNESS ANCHOR: this "post-VMRUN" guarantee rests on the exit
    // handler running with GIF=0 (DPCs cannot be delivered mid-handler;
    // svm_entry's enter_guest loop never stgi). If that invariant ever
    // changes, a DPC could consume pending=0 before the kick and strand
    // every core on the scratch view.
    if (InterlockedExchange(&g_wiggleRestorePending, 0))
    {
        NptManager* npt = NptInstance();
        NptView* a = npt ? npt->Active() : nullptr; // null in the unload
        if (a)                                      // window (r80 review P2)
            TlbWiggleRestore(a->Pml4Pa());
    }
    NptView* views[2] = {};
    u32 nv = SentinelViews(views);
    if (!nv)
        return;
    ULONG now = KeQueryInterruptTime(); // ms-scale
    SentinelFlushBatch batch = {};
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        LONG64 g = g_sent[i].Gpa4k;
        if (!g)
            continue;
        LONG64 tick = g_sent[i].ResolveTick;
        if (now - (ULONG)tick < SENTINEL_REARM_MS)
            continue;
        // r63 TOCTOU FIX: a concurrent resolve (trip on another core) can
        // refresh ResolveTick between the check above and the deny below -
        // the re-deny then lands AFTER that resolve, the faulting write
        // never lands, and the trip/re-deny pair ping-pongs forever (the
        // r63 two-core page-pair livelock). Claim the re-deny by CAS: if
        // any resolve raced in, skip this page this round. ResolveTick now
        // means "last resolve OR last re-deny claim" - either way the page
        // gets a fresh deny-rest window.
        if (InterlockedCompareExchange64(&g_sent[i].ResolveTick,
                                         (LONG64)now, tick) != tick)
            continue;
        // r68 frame-lifetime check: the deny is pinned to the PHYSICAL frame
        // armed at slot-registration time. If the object header no longer
        // matches, the frame was freed/reused while the deny survived (the
        // r62 thread-exit notify covers the common path; this closes the
        // residual races - lookup-fail exits, mid-flight process death).
        // Re-denying a recycled frame is what corrupted an innocent svchost
        // exception frame in the r67 0xEF crash.
        {
            void* pva = g_sent[i].PageVa;
            void* ova = g_sent[i].ObjVa;
            if (ova && pva &&
                !SentinelWmMatches(*(volatile u32*)ova, g_sent[i].Watermark))
            {
                for (u32 v = 0; v < nv; ++v)
                    (void)views[v]->SetPerm4k((u64)g, {true, true, true});
                g_sent[i].PageVa = nullptr;
                g_sent[i].ObjVa = nullptr;
                InterlockedExchange64(&g_sent[i].Gpa4k, 0);
                InterlockedDecrement(&g_sentArmed);
                InterlockedIncrement64(&g_sentReuse);
                g_lastWhy = kSentWhyReuse;
                SentinelNoteDisarmed((u64)g);
                SVMB_LOGW("sentinel: REUSE disarm(rearm) gpa=%llx wm=%08x "
                          "want=%08x",
                          g, *(volatile u32*)ova, (u32)g_sent[i].Watermark);
                continue;
            }
        }
        for (u32 v = 0; v < nv; ++v)
        {
            // MapRange keeps the leaf present in views created after
            // the arm (SetPerm4k needs the 4K split); no-op otherwise
            (void)views[v]->MapRange((u64)g, 0x1000, {true, true, true});
            SentinelRearmPage(views[v], (u64)g);
        }
        if (batch.Count < SENTINEL_MAX)
            batch.Va[batch.Count++] = g_sent[i].PageVa;
    }
    if (batch.Count)
    {
        // r56: a deny flip is inert until every core's NPT TLB drops the
        // cached RW translation - resolve never needs this (failed walks
        // are not cached), re-deny does. r46-r54 were masked by the
        // per-VMRUN FLUSH_ALL that the r55 melt fix removed. Same
        // broadcast the arm path uses.
        KeIpiGenericCall((PKIPI_BROADCAST_WORKER)SentinelFlushBatchCb,
                         (ULONG_PTR)&batch);
        // r69 freeze attack: the r64/r68 soaks froze trips mid-soak while
        // INVLPGA-all-ASID broadcasts kept firing - the nested shadow kept
        // serving pre-deny RW entries (the r57 base disease's mildest
        // symptom; the r67 0xEF was its worst). Arm the r63 one-shot
        // full-flush on every VMCB after each re-deny round: tlbMode=0's
        // semantics are flush-all anyway, this is a plain VMCB store
        // (CLOCK2-safe, no locks), and it is the strongest visibility
        // hammer the guest has. If trips stop freezing, the residual-live
        // deny windows close with them.
        TlbKickFlushAllCores();
    }

    // r75 protected-region re-arm: same deny-rest contract as the sentinel
    // slots. A tripped region page was resolved by the deny callback; the
    // deny goes back up after SENTINEL_REARM_MS. Whole-region re-deny (the
    // tripped page is tracked at region granularity for now).
    ULONG nowP = KeQueryInterruptTime();
    for (u32 i = 0; i < PROT_MAX; ++i)
    {
        LONG64 b = InterlockedCompareExchange64(&g_prot[i].Active, 0, 0);
        if (!b)
            continue;
        LONG64 tick = g_prot[i].ResolveTick;
        if (tick != 0 && nowP - (ULONG)tick < SENTINEL_REARM_MS)
            continue;
        InterlockedExchange64(&g_prot[i].ResolveTick, (LONG64)nowP);
        for (u32 pg = 0; pg < g_prot[i].Pages; ++pg)
        {
            u64 g = (u64)b + pg * 0x1000ull;
            for (u32 v = 0; v < nv; ++v)
            {
                (void)views[v]->MapRange(g, 0x1000, {true, true, true});
                (void)views[v]->SetPerm4k(g, {false, true, true});
            }
        }
        // flush the whole region's GVAs (one broadcast, up to 64 pages)
        for (u32 pg = 0; pg < g_prot[i].Pages; pg += SENTINEL_MAX)
        {
            SentinelFlushBatch rb = {};
            for (u32 k = 0; k < SENTINEL_MAX && pg + k < g_prot[i].Pages; ++k)
                rb.Va[rb.Count++] = (char*)g_prot[i].BaseVa +
                                    ((u64)pg + k) * 0x1000ull;
            if (rb.Count)
                KeIpiGenericCall((PKIPI_BROADCAST_WORKER)SentinelFlushBatchCb,
                                 (ULONG_PTR)&rb);
        }
    }
}

// the deny seam: called from the NPF exit path (>= DISPATCH_LEVEL) for deny
// classifications; returns true when the faulting gpa belongs to the set
// r100 seam state - the svmb-scope setter below writes it, this anon-ns
// consumer reads it
static Cr3SenseCb g_senseCb = nullptr;

bool SentinelDenyCb(void*, GuestContext& g, u64 gpa4k, u64 info1,
                    u64 currentCr3, u64 rip)
{
    // r100: the execute-sense page (syscall audit) consumes its own fetch
    // faults before region processing
    if (g_senseCb && g_senseCb(gpa4k, info1, currentCr3, rip, g))
        return true;

    // r85 audit P2-10: the old CX-based cap read the PRE-increment value
    // (1 after the first call) so the "< 8" never expired - every deny
    // logged. Cap on the incremented value instead.
    static volatile LONG cbEntries = 0;
    if (InterlockedIncrement(&cbEntries) <= 8)
        SVMB_LOGI("sentinel: deny cb gpa=%llx cr3=%llx", gpa4k, currentCr3);

    // r75 protected regions take precedence: a write on a region page is
    // an L1 (guest-MM protection) bypass - resolve it (sensing semantics,
    // the re-arm DPC restores the deny) and alert.
    if (SentinelRegionConsume(gpa4k, currentCr3, rip))
        return true;

    bool ours = false;
    u32 idx = SENTINEL_MAX;
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        if (g_sent[i].Gpa4k == (LONG64)gpa4k)
        {
            ours = true;
            idx = i;
            break;
        }
    }
    if (!ours)
    {
        // r68: a trip for a slot torn down between the deny store and the
        // PTE restore lands here. Resolve it ourselves instead of feeding
        // the generic spin-breaker (256 exits of pure tax per race).
        if (SentinelRecentlyDisarmed(gpa4k))
        {
            NptView* views[2] = {};
            u32 nv = SentinelViews(views);
            for (u32 v = 0; v < nv; ++v)
                (void)views[v]->SetPerm4kNoLock(gpa4k, {true, true, true});
            // r74: capped - a teardown race can emit these in bursts
            if (InterlockedIncrement(&g_orphanLogs) <= 32)
                SVMB_LOGI("sentinel: trip after disarm gpa=%llx - resolved",
                          gpa4k);
            return true;
        }
        // r71: an ORPHAN W-deny - stored in the arm-race window (deny PTE
        // written before the slot registered; the r70 storm logged 5 of
        // these in 22 arms) or left by a restore race. Only the sentinel
        // arms W-denies, so with a watch live an unowned deny can never
        // become valid again: resolve it outright instead of feeding the
        // spin-breaker (Debug tax) or the fail-fast (Production bugcheck).
        // Outside an armed watch the legacy probe semantics stay untouched.
        if (InterlockedCompareExchange(&g_sentArmed, 0, 0) > 0)
        {
            NptView* views[2] = {};
            u32 nv = SentinelViews(views);
            for (u32 v = 0; v < nv; ++v)
                (void)views[v]->SetPerm4kNoLock(gpa4k, {true, true, true});
            // r74: capped - arm-storm windows can orphan several denies
            if (InterlockedIncrement(&g_orphanLogs) <= 32)
                SVMB_LOGW("sentinel: ORPHAN deny resolved gpa=%llx", gpa4k);
            return true;
        }
        SVMB_LOGW("sentinel: cb UNMATCHED gpa=%llx", gpa4k);
        return false;
    }

    // direction hint (telemetry): the write sequence leaving the target has
    // the target still current on this core; entering/unrelated co-resident
    // writes observe a different CR3. r48 turns this into view switching.
    bool outgoing = g_cfg.TargetCr3 != 0 && currentCr3 == g_cfg.TargetCr3;

    // r68 kill-switch (the r67 0xEF fix): a FOREIGN-cr3 write on this frame
    // is legitimate only while the frame still holds the object we armed
    // (clock-ISR KTHREAD accounting writes). Re-reading the dispatcher
    // header costs two instructions; a mismatch means the frame was freed
    // and recycled out from under the deny - resolve the access and tear
    // the deny down for good instead of leaving a live trap on memory that
    // now belongs to an innocent process.
    if (!outgoing)
    {
        void* pva = g_sent[idx].PageVa;
        void* ova = g_sent[idx].ObjVa;
        LONG wm = g_sent[idx].Watermark;
        if (ova && pva)
        {
            u32 cur = *(volatile u32*)ova;
            if (!SentinelWmMatches(cur, wm))
            {
                NptView* views[2] = {};
                u32 nv = SentinelViews(views);
                for (u32 v = 0; v < nv; ++v)
                    (void)views[v]->SetPerm4kNoLock(gpa4k,
                                                    {true, true, true});
                g_sent[idx].PageVa = nullptr;
                g_sent[idx].ObjVa = nullptr;
                InterlockedExchange64(&g_sent[idx].Gpa4k, 0);
                InterlockedDecrement(&g_sentArmed);
                InterlockedIncrement64(&g_sentReuse);
                g_lastWhy = kSentWhyReuse;
                SentinelNoteDisarmed(gpa4k);
                SVMB_LOGW("sentinel: REUSE killswitch gpa=%llx cur=%08x "
                          "want=%08x cr3=%llx",
                          gpa4k, cur, (u32)wm, currentCr3);
                return true;
            }
        }
    }

    // r56: direction in the log line (the log ring is CLOCK2-safe in
    // practice - this line itself fires from clock-ISR trips; the EVENT
    // ring is the one gated to <=DISPATCH below)
    SVMB_LOGI("sentinel: TRIP MATCHED gpa=%llx cr3=%llx out=%u",
              gpa4k, currentCr3, outgoing ? 1u : 0u);
    InterlockedIncrement64(&g_sentTrips);

    // TRIPS ARRIVE AT ANY IRQL: scheduler State writes at DISPATCH, clock
    // tick CycleTime writes at CLOCK2. Everything here must be legal above
    // DISPATCH: interlocked ops, the no-lock PTE store, KeInsertQueueDpc.
    // The clock-level trips skip the event ring (Push takes a spinlock).
    bool atDispatchOrBelow = KeGetCurrentIrql() <= DISPATCH_LEVEL;

    if (atDispatchOrBelow)
    {
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
    }

    // resolve: restore the page so the faulting write re-executes for real
    // r61 MELT FIX (was: Active() only): with the per-core process view
    // live, a trip on the target's core walks the PROCVIEW; allowing only
    // the base leaf left the faulting view denied -> the write NPF'd again
    // forever (single-core exit flood = the r60 loud melt, watch pv).
    // Same dual-view rule as the r57 arm path, resolve side.
    // r63 ORDER: stamp ResolveTick BEFORE the allow-store so a racing
    // RearmDpc's CAS claim sees the fresh tick and skips its re-deny -
    // deny-after-allow ordering is then bounded to one bounce per
    // SENTINEL_REARM_MS window instead of racing per-trip.
    {
        NptView* views[2] = {};
        u32 nv = SentinelViews(views);
        for (u32 i = 0; i < SENTINEL_MAX; ++i)
            if (g_sent[i].Gpa4k == (LONG64)gpa4k)
                g_sent[i].ResolveTick = (LONG64)KeQueryInterruptTime();
        for (u32 v = 0; v < nv; ++v)
            (void)views[v]->SetPerm4kNoLock(gpa4k, {true, true, true});
        if (nv)
        {
            TlbInvlpgaLocal(rip & ~NptView::OFF_4K);
        }
    }
    // bounded sensing window: one DPC (deduplicated while queued) re-arms
    // the deny right after the interrupted write completes
    KeInsertQueueDpc(&g_sentDpc, nullptr, nullptr);
    return true;
}

// r75 protected-region consumer (called from SentinelDenyCb first): a write
// reached a region page WITHOUT the guest MM stopping it = an L1 bypass
// (the guest PTE is readonly, so user/kernel VA writers already faulted).
// Sensing semantics: resolve + re-arm + alert. Never blocks here.
bool SentinelRegionConsume(u64 gpa4k, u64 currentCr3, u64 rip)
{
    for (u32 i = 0; i < PROT_MAX; ++i)
    {
        LONG64 b = InterlockedCompareExchange64(&g_prot[i].Active, 0, 0);
        if (!b || gpa4k < b || gpa4k >= b + (LONG64)g_prot[i].SizeBytes)
            continue;

        {
            NptView* views[2] = {};
            u32 nv = SentinelViews(views);
            for (u32 v = 0; v < nv; ++v)
                (void)views[v]->SetPerm4kNoLock(gpa4k, {true, true, true});
            if (nv)
                TlbInvlpgaLocal(rip & ~NptView::OFF_4K);
        }
        InterlockedExchange64(&g_prot[i].ResolveTick,
                              (LONG64)KeQueryInterruptTime());
        InterlockedIncrement64(&g_regionTrips);
        // r89/r91: opt-in response policy - record writer evidence, then
        // hand kill-or-suspend to the DPC->workitem pipeline (one in
        // flight; extra trips in the window are dropped and counted)
        LONG policy = InterlockedCompareExchange(&g_prot[i].Policy, 0, 0);
        if (policy & (SVMB_PROT_POLICY_KILL | SVMB_PROT_POLICY_SUSPEND))
        {
            InterlockedExchange64(&g_prot[i].WriterCr3, (LONG64)currentCr3);
            InterlockedExchange64(&g_prot[i].WriterRip, (LONG64)rip);
            InterlockedExchange64(&g_prot[i].WriterGpa, (LONG64)gpa4k);
            if (InterlockedCompareExchange(&g_killInFlight, 1, 0) == 0)
            {
                LONG mode = (policy & SVMB_PROT_POLICY_KILL)
                                ? SVMB_PROT_POLICY_KILL
                                : SVMB_PROT_POLICY_SUSPEND;
                InterlockedExchange(&g_killMode, mode);
                InterlockedExchange64(&g_killCr3, (LONG64)currentCr3);
                InterlockedExchange64(&g_killRip, (LONG64)rip);
                InterlockedExchange64(&g_killGpa, (LONG64)gpa4k);
                InterlockedExchange64(&g_killRegionId, (LONG64)g_prot[i].Id);
                if (mode == SVMB_PROT_POLICY_KILL)
                    InterlockedIncrement64(&g_killAttempts);
                else
                    InterlockedIncrement64(&g_suspAttempts);
                KeInsertQueueDpc(&g_killDpc, nullptr, nullptr);
            }
            else
            {
                if (policy & SVMB_PROT_POLICY_KILL)
                    InterlockedIncrement64(&g_killDropped);
                else
                    InterlockedIncrement64(&g_suspDropped);
            }
        }
        KeInsertQueueDpc(&g_sentDpc, nullptr, nullptr);

        SVMB_LOGW("sentinel: REGION trip gpa=%llx cr3=%llx rip=%llx "
                  "(L1 bypass - alert)",
                  gpa4k, currentCr3, rip);
        bool atDispatchOrBelow = KeGetCurrentIrql() <= DISPATCH_LEVEL;
        if (atDispatchOrBelow)
        {
            DbgEventRing* ring = DbgRingInstance();
            if (ring)
            {
                SVMB_DBG_EVENT e = {};
                e.Type = SvmbDbgEvtRegionTrip;
                e.Core = KeGetCurrentProcessorNumberEx(nullptr);
                e.Rip = rip;
                e.Cr3 = currentCr3;
                e.Extra = gpa4k;
                ring->Push(e);
            }
        }
        // r77/r80: the resolve above is invisible to the vhv's nested
        // shadow until an NCr3 value change (r69 law). Mode 2 (default):
        // broadcast every core to the scratch identity view BEFORE
        // returning to the guest - the storming write lands on identity
        // 2M RWX at the very next VMRUN, and the re-arm DPC restores each
        // core to its own pinned view (>=1 real VMRUN on scratch, so the
        // shadow genuinely rebuilds). Mode 1: the r77 back-to-back kick
        // (kept for A/B - the intermediate NCr3 may never be observed).
        {
            static volatile LONG64 s_lastKick = 0;
            LONG64 now = (LONG64)KeQueryInterruptTime();
            LONG64 last = InterlockedCompareExchange64(&s_lastKick, 0, 0);
            if (now - last >= 32)
            {
                if (InterlockedCompareExchange64(&s_lastKick, now, last) ==
                    last)
                {
                    LONG mode = InterlockedCompareExchange(&g_wiggleMode, 0, 0);
                    NptManager* npt = NptInstance();
                    bool kicked = false;
                    if (mode == 1)
                        kicked = NT_SUCCESS(npt->ShadowKick());
                    else if (mode == 2 && npt->NptEnabled())
                    {
                        if (npt->WigglePml4Pa())
                        {
                            TlbWiggleToScratch(npt->WigglePml4Pa());
                            InterlockedExchange(&g_wiggleRestorePending, 1);
                            kicked = true;
                        }
                    }
                    if (kicked)
                        InterlockedIncrement64(&g_wiggleKicks);
                }
            }
        }
        return true;
    }
    // r76: no live slot owns this page - but if its deny was just torn
    // down (unprotect / death sweep / straggler re-arm DPC), the fault is
    // ours: resolve it and swallow (never fail fast on a region frame)
    if (RegionRecentlyDisarmed(gpa4k))
    {
        NptView* views[2] = {};
        u32 nv = SentinelViews(views);
        for (u32 v = 0; v < nv; ++v)
            (void)views[v]->SetPerm4kNoLock(gpa4k, {true, true, true});
        if (nv)
            TlbInvlpgaLocal(rip & ~NptView::OFF_4K);
        SVMB_LOGI("sentinel: region trip after unprotect gpa=%llx - "
                  "resolved", gpa4k);
        return true;
    }
    return false;
}

// IPI context: flush one GVA on every core (r50 arm visibility)
extern "C" ULONG_PTR __fastcall SentinelFlushPageCb(PVOID context,
                                                    ULONG_PTR)
{
    TlbInvlpgaLocal((u64)context);
    return 0;
}

void SentinelArmPage(void* objVa, u32 pid, const char** why)
{
    *why = "full";
    g_lastWhy = kSentWhyFull;
    if (g_sentArmed >= SENTINEL_MAX)
        return;
    void* pageVa = (void*)((u64)objVa & ~0xFFFull);
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(pageVa);
    u64 gpa = pa.QuadPart & ~0xFFFull;
    *why = "gpa0";
    if (!gpa)
        return;
    *why = "npt-off";
    g_lastWhy = kSentWhyGpa0;
    NptManager* npt = NptInstance();
    if (!npt || !npt->NptEnabled())
        return;
    *why = "no-view";
    g_lastWhy = kSentWhyNptOff;
    NptView* view = npt->Active();
    if (!view)
        return;
    *why = "dup";
    g_lastWhy = kSentWhyNoView;
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
        if (g_sent[i].Gpa4k == (LONG64)gpa)
            return; // two thread objects sharing a page: armed once
    g_lastWhy = kSentWhyDup;
    *why = "map-fail";
    if (!NT_SUCCESS(view->MapRange(gpa, 0x1000, {true, true, true})))
        return;
    g_lastWhy = kSentWhyMapFail;
    *why = "perm-fail";
    if (!NT_SUCCESS(view->SetPerm4k(gpa, {false, true, true})))
        return;
    // r57: arm in the ProcView too - while the target is current the
    // per-core policy runs that view, and a base-only deny would go
    // silent exactly when direction=out=1 trips would fire
    if (g_cfg.ProcViewId)
    {
        NptView* pv = npt->View(g_cfg.ProcViewId);
        if (pv && pv != view)
        {
            (void)pv->MapRange(gpa, 0x1000, {true, true, true});
            (void)pv->SetPerm4k(gpa, {false, true, true});
        }
    }
    g_lastWhy = kSentWhyPermFail;
    *why = nullptr;
    g_lastWhy = kSentWhyArmed;

    // r51 ORDERING LAW: the slot MUST be registered before the deny can
    // ever fire - the self-write below trips the seam the instant the IPI
    // flush lands, and an unregistered page means cb-UNMATCHED -> the
    // generic spin-breaker eats it (256 extra exits per arm).
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        if (InterlockedCompareExchange64(&g_sent[i].Gpa4k, 0, 0) == 0)
        {
            g_sent[i].Pid = pid;
            g_sent[i].PageVa = pageVa;
            g_sent[i].ObjVa = objVa;
            // r68/r72: sample the OBJECT header (KTHREAD dispatcher-header
            // dword at the thread object, not the page start - the page
            // start is pool header / neighbour-tail data that drifts)
            // BEFORE the deny flips - stable for a live object, the
            // kill-switch's reference value afterwards
            g_sent[i].Watermark = (LONG) * (volatile u32*)objVa;
            g_sent[i].ResolveTick = 0; // arm-time: writable immediately
            InterlockedExchange64(&g_sent[i].Gpa4k, (LONG64)gpa);
            InterlockedIncrement(&g_sentArmed);
            break;
        }
    }

    // raw ground truth - the leaf must show P|A|D|US and NO RW/NX
    u64 pte = 0;
    if (NT_SUCCESS(view->GetPte4k(gpa, pte)))
        SVMB_LOGI("sentinel: pte gpa=%llx pte=%llx", gpa, pte);

    // the deny is inert until every core's (shadow) TLB drops the old RWX
    // translation - the VMRUN flush-all control is NOT reliable here
    // (r31b). Broadcast an INVLPGA of the page's GVA via IPI; arm is a
    // rare PASSIVE event so the broadcast cost is fine.
    KeIpiGenericCall((PKIPI_BROADCAST_WORKER)SentinelFlushPageCb,
                     (ULONG_PTR)pageVa);

    // decisive self-test: same-value write-back through the fresh deny must
    // trip the seam (one NPF -> resolve -> land). Read-then-write-identical
    // keeps the live object intact while proving the deny is real.
    {
        SVMB_LOGI("sentinel: self-write begin gpa=%llx", gpa);
        u32 orig = *(volatile u32*)pageVa; // read: allowed
        *(volatile u32*)pageVa = orig;     // write: NPFs -> trips -> lands
        SVMB_LOGI("sentinel: self-write done gpa=%llx val=%08x", gpa, orig);
    }
    SVMB_LOGI("sentinel: armed gpa=%llx pid=%u (thread obj page)", gpa,
              pid);
}

typedef PETHREAD (*PsGetNextProcessThreadFn)(PEPROCESS, PETHREAD);
PsGetNextProcessThreadFn g_psnpt = nullptr;

static VOID SentinelArmWorker(PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
    for (;;)
    {
        KeWaitForSingleObject(&g_armEvent, Executive, KernelMode, FALSE,
                              nullptr);
        if (InterlockedCompareExchange(&g_armShutdown, 0, 0))
            PsTerminateSystemThread(STATUS_SUCCESS);
        u32 pid = g_armPid;
        LARGE_INTEGER due;
        due.QuadPart = -100 * 10000; // 100ms: let the thread list populate
        KeDelayExecutionThread(KernelMode, FALSE, &due);
        InterlockedIncrement((volatile LONG*)&g_armWakes);
        // r61 MELT FIX: the watch may have been torn down inside the 100ms
        // window (policytest configures + unwatches in milliseconds, and
        // the exit-notify cleanup runs when the target dies - both BEFORE
        // this worker wakes). Arming a dead watch leaks denies nobody will
        // ever disarm; the pages then get recycled by the pool and become
        // permanent random NPF traps (the r60 melt, policytest alone).
        if (!pid || g_watchPid != pid)
        {
            InterlockedExchange(&g_armPending, 0);
            SVMB_LOGW("sentinel: arm worker stale watch pid=%u - skipped",
                      pid);
            continue;
        }
        PEPROCESS proc = nullptr;
        NTSTATUS lst = PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid,
                                                  &proc);
        if (NT_SUCCESS(lst))
        {
            SVMB_LOGI("sentinel: arm worker lookup ok pid=%u", pid);
            SentinelArmForProcess(proc, pid);
            ObDereferenceObject(proc);
        }
        else
        {
            SVMB_LOGW("sentinel: arm worker lookup fail pid=%u st=%08x",
                      pid, lst);
            g_lastWhy = kSentWhyLookupFail;
        }
        InterlockedExchange(&g_armPending, 0);
    }
}

void SentinelArmForProcess(PEPROCESS proc, u32 pid)
{
    SVMB_LOGI("sentinel: arm proc pid=%u psnpt=%p", pid, g_psnpt);
    u32 armed = 0;
    if (g_psnpt)
    {
        // each thread object's first page holds its KTHREAD (State @ 0x2d is
        // scheduler-written on every switch); thread creation AFTER the walk
        // is r48+ work - the initial thread carries the sensing for now
        for (PETHREAD t = g_psnpt(proc, nullptr); t;)
        {
            PETHREAD next = g_psnpt(proc, t);
            const char* why = nullptr;
            SentinelArmPage((void*)t, pid, &why);
            if (why)
                SVMB_LOGW("sentinel: arm skip pid=%u obj=%llx why=%s", pid,
                          (u64)t, why);
            else
                ++armed;
            ObDereferenceObject(t);
            t = next;
        }
        return;
    }

    // r83: layout offsets are resolved at load (platform/offsets.cpp) -
    // the hardcoded 1903 values were the table of record; an unresolved
    // layout must never be walked (wrong offsets = wrong-memory reads).
    // Retry here too: the load-time resolve may have raced boot churn
    // while this walk has a live referenced target (idempotent, cheap).
    if (!g_offsetsOk)
        g_offsetsOk = NT_SUCCESS(OffsetsResolve());
    if (!g_offsetsOk)
    {
        g_lastWhy = kSentWhyNoOffsets;
        SVMB_LOGW("sentinel: walk skipped pid=%u - offsets unresolved", pid);
        return;
    }
    PLIST_ENTRY head =
        (PLIST_ENTRY)((u8*)proc + Offsets().EprocThreadList);
    __try
    {
        SVMB_LOGI("sentinel: walk pid=%u proc=%llx head=%llx flink=%llx",
                  pid, (u64)proc, (u64)head, (u64)head->Flink);
        if (head->Flink == head)
            g_lastWhy = kSentWhyEmptyList;
        for (PLIST_ENTRY e = head->Flink;
             e && e != head && armed < SENTINEL_MAX;
             e = e->Flink)
        {
            void* t = (u8*)e - Offsets().EthreadListEntry;
            const char* why = nullptr;
            SentinelArmPage(t, pid, &why);
            if (why)
                SVMB_LOGW("sentinel: arm skip pid=%u obj=%llx why=%s", pid,
                          (u64)t, why);
            else
                ++armed;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        SVMB_LOGW("sentinel: thread walk faulted pid=%u armed=%u", pid,
                  armed);
    }
    SVMB_LOGI("sentinel: armed %u pages pid=%u", armed, pid);
}

// r62/r72: disarm the slot(s) of this thread object (called from the
// thread-exit notify, PASSIVE). Matches on ObjVa - the raw ETHREAD pointer
// the notify hands us. THE r72 FINDING: this used to match against the
// PAGE-ALIGNED slot.PageVa, which never equals the raw object pointer, so
// every mid-flight thread exit left its deny live on a frame the pool was
// about to recycle - the r67 0xEF innocent-svchost corruption and the
// r71/r72 natural kill-switch fires all trace back to this no-op disarm.
// Restores RWX in all sentinel views before clearing the slot so a freed
// page never carries a live deny into pool reuse.
void SentinelDisarmPage(void* objVa)
{
    NptView* views[2] = {};
    u32 nv = SentinelViews(views);
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        LONG64 g = InterlockedCompareExchange64(&g_sent[i].Gpa4k, 0, 0);
        if (g && g_sent[i].ObjVa == objVa)
        {
            for (u32 v = 0; v < nv; ++v)
                (void)views[v]->SetPerm4k((u64)g, {true, true, true});
            g_sent[i].PageVa = nullptr;
            g_sent[i].ObjVa = nullptr;
            InterlockedExchange64(&g_sent[i].Gpa4k, 0);
            InterlockedDecrement(&g_sentArmed);
            SentinelNoteDisarmed((u64)g);
            SVMB_LOGI("sentinel: disarm page gpa=%llx (thread exit)", g);
        }
    }
}

static void SentinelRestoreAll(NptView* view, u32 pid)
{
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        LONG64 g = g_sent[i].Gpa4k;
        if (g && (pid == 0 || g_sent[i].Pid == pid))
            if (view)
                (void)view->SetPerm4k((u64)g, {true, true, true});
    }
}

void SentinelDisarmProcess(u32 pid)
{
    // r61: cancel a pending deferred arm for this pid - otherwise the
    // worker re-arms AFTER this disarm runs (the r60 melt orphan denies)
    if (pid != 0 &&
        InterlockedCompareExchange((volatile LONG*)&g_armPid, 0,
                                   (LONG)pid) == (LONG)pid)
        InterlockedExchange(&g_armPending, 0);
    NptView* views[2] = {};
    u32 nv = SentinelViews(views);
    for (u32 v = 0; v < nv; ++v)
        SentinelRestoreAll(views[v], pid);
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        if (g_sent[i].Gpa4k && g_sent[i].Pid == pid)
        {
            SentinelNoteDisarmed((u64)g_sent[i].Gpa4k);
            InterlockedExchange64(&g_sent[i].Gpa4k, 0);
            InterlockedDecrement(&g_sentArmed);
        }
    }
    // an in-flight re-arm DPC may have re-denied a page after the restore
    // above: drain it, then restore once more to close the race
    KeFlushQueuedDpcs();
    for (u32 v = 0; v < nv; ++v)
        SentinelRestoreAll(views[v], pid);
}

void SentinelDisarmAll()
{
    // r61: cancel any pending deferred arm (see SentinelDisarmProcess)
    InterlockedExchange((volatile LONG*)&g_armPid, 0);
    InterlockedExchange(&g_armPending, 0);
    NptView* views[2] = {};
    u32 nv = SentinelViews(views);
    for (u32 v = 0; v < nv; ++v)
        SentinelRestoreAll(views[v], 0);
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        LONG64 g = InterlockedExchange64(&g_sent[i].Gpa4k, 0);
        if (g)
        {
            for (u32 v = 0; v < nv; ++v)
                (void)views[v]->SetPerm4k((u64)g, {true, true, true});
            InterlockedDecrement(&g_sentArmed);
            SentinelNoteDisarmed((u64)g);
        }
    }
    // close the in-flight-DPC re-arm race the same way (see DisarmProcess)
    KeFlushQueuedDpcs();
    for (u32 v = 0; v < nv; ++v)
        SentinelRestoreAll(views[v], 0);
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
        // process going away: its protected regions must release their NPT
        // denies NOW (r76 death sweep - freed frames must not carry live
        // W-denies into pool reuse), then the sentinel tripwires go with
        // the dying thread objects (same fault-trap rule).
        Cr3RegionSweepPid(pid);
        SentinelDisarmProcess(pid);
        if (g_watchPid == pid)
            g_watchPid = 0;
        return;
    }
    if (!g_cfg.TargetImage[0] || !Cr3ImageMatch(image, g_cfg.TargetImage))
        return;
    // r74: switching targets orphans the OUTGOING target's slots. Its
    // future thread exits hit the notify's pid-gate (g_watchPid != p ->
    // early return) and never disarm, so each exited thread leaves a deny
    // on a frame the pool recycles - the natural kill-switch fires these
    // correctly, but they are pure slot loss caused by the switch. Disarm
    // the outgoing target's slots before the switch.
    u32 oldPid = g_watchPid;
    if (oldPid != 0 && oldPid != pid)
        SentinelDisarmProcess(oldPid);
    InterlockedExchange64((volatile LONG64*)&g_cfg.TargetCr3, (LONG64)cr3);
    SVMB_LOGI("cr3 monitor: armed %s pid=%u cr3=%llx", image, pid, cr3);
    g_watchPid = pid; // later threads of this pid arm via thread-notify
    // r56: an already-running target used to stop at this log line - the
    // deferred sentinel walk was only scheduled from OnSeedCreate, so
    // spawn-then-watch flows armed zero pages. Mirror the create-path
    // scheduling; the 100ms worker delay is harmless for a live target.
    if (g_armWorkerLive)
    {
        g_armPid = pid;
        if (InterlockedCompareExchange(&g_armPending, 1, 0) == 0)
            KeSetEvent(&g_armEvent, IO_NO_INCREMENT, FALSE);
    }
    else
    {
        SVMB_LOGW("cr3 monitor: cr3_monitor module not attached - "
                  "sentinel disabled (svmbctl mod attach cr3_monitor)");
    }
    // a seeded target may now satisfy the r48 arming gate (target + key)
    ApplyCr3Intercepts();

    DbgEventRing* ring = DbgRingInstance();
    if (ring)
    {
        SVMB_DBG_EVENT e = {};
        e.Type = SvmbDbgEvtProcessWatch;
        e.Cr3 = cr3;
        e.Extra = pid;
        ring->Push(e);
    }
}

// NPT view flip on context switches in/out of the watched process (M3xM4:
// the process view is a full-RAM canvas that M4 policy can hide pages in).
// Switching requires NPT live; SetActiveView publishes through the apply
// callback (every VMCB's NCr3, observed at the next VMRUN). This is the
// SANCTIONED EXIT-CONTEXT publisher of the activation protocol (npt.h):
// transient per-core NCr3 divergence self-heals on each full republish, and
// per-process page hiding is blocked on the per-core view model (M4
// prerequisite there).
void ApplyViewSwitch(bool enter)
{
    NptManager* npt = NptInstance();
    if (!npt || !npt->NptEnabled())
        return; // switching is meaningless while NPT is offline
    if (enter && g_cfg.ProcViewId == 0)
        return; // view not ready (still being filled) - stay on the current
    u32 id = enter ? g_cfg.ProcViewId : 0;
    NTSTATUS st = npt->SetActiveView(id);
    if (NT_SUCCESS(st))
    {
        InterlockedIncrement64(&g_viewSwitches);
        return;
    }
    if (g_switchWarnLogs < 16)
    {
        InterlockedIncrement(&g_switchWarnLogs);
        SVMB_LOGW("cr3 monitor: view switch to %u failed st=%08x", id, st);
    }
}

bool HandleCrRead(GuestContext& ctx, void*)
{
    InterlockedIncrement64(&g_readExits);
    u32 gpr = ExitCrDrGpr(ctx.Info1); // EXITINFO1[3:0] = destination GPR
    u64 real = ctx.Vmcb()->Save.Cr3;
    GuestGpr(ctx.Regs, gpr) = Cr3ResolveRead(real, g_cfg.XorKey,
                                             g_cfg.ReadSpoof != 0,
                                             g_cfg.TargetCr3);
    ctx.AdvanceRip = true; // read emulated: resume past the mov
    return true;
}

bool HandleCrWrite(GuestContext& ctx, void*)
{
    InterlockedIncrement64(&g_writeExits);
    u32 gpr = ExitCrDrGpr(ctx.Info1); // EXITINFO1[3:0] = source GPR
    u64 curVal = ctx.Vmcb()->Save.Cr3;
    u64 newVal = GuestGpr(ctx.Regs, gpr);
    // emulate the write into the save area; the guest re-executes the mov
    // with CR3 already loaded (VMRUN restores CR3 from the save area)
    ctx.Vmcb()->Save.Cr3 = newVal;

    // process-view switching rides on the emulated context switch
    Cr3ViewSwitch sw = Cr3ViewSwitchDecision(curVal, newVal, g_cfg.TargetCr3,
                                             g_cfg.ProcessView != 0);
    if (sw == Cr3ViewSwitch::Enter)
        ApplyViewSwitch(true);
    else if (sw == Cr3ViewSwitch::Leave)
        ApplyViewSwitch(false);

    ctx.AdvanceRip = true;
    return true;
}

} // namespace

namespace svmb
{

u64 Cr3ResolveRead(u64 real, u64 xorKey, bool readSpoof, u64 targetCr3)
{
    if (readSpoof && xorKey)
        return real ^ xorKey;
    if (targetCr3 && real == targetCr3)
        return real ^ xorKey;
    return real;
}

Cr3ViewSwitch Cr3ViewSwitchDecision(u64 curCr3, u64 newCr3, u64 targetCr3,
                                    bool processViewEnabled)
{
    if (!processViewEnabled || !targetCr3)
        return Cr3ViewSwitch::None;
    bool curIsTarget = (curCr3 == targetCr3);
    bool newIsTarget = (newCr3 == targetCr3);
    if (!curIsTarget && newIsTarget)
        return Cr3ViewSwitch::Enter;
    if (curIsTarget && !newIsTarget)
        return Cr3ViewSwitch::Leave;
    return Cr3ViewSwitch::None;
}

// lazily create + fill the per-process view (IOCTL-gate context only); the
// ProcessView flag goes live only after the view is ready
void EnsureProcessView()
{
    NptManager* npt = NptInstance();
    if (!npt)
    {
        SVMB_LOGW("cr3 monitor: process view requested but NPT unavailable");
        InterlockedExchange(&g_cfg.ProcessView, 0);
        return;
    }
    if (g_cfg.ProcViewId != 0 && npt->View(g_cfg.ProcViewId))
    {
        InterlockedExchange(&g_cfg.ProcessView, 1); // already ready
        return;
    }
    u32 id = 0;
    NTSTATUS st = npt->CreateView(id);
    if (!NT_SUCCESS(st))
    {
        SVMB_LOGW("cr3 monitor: process view create failed st=%08x", st);
        InterlockedExchange(&g_cfg.ProcessView, 0);
        return;
    }
    st = npt->FillView(*npt->View(id));
    if (!NT_SUCCESS(st))
    {
        SVMB_LOGW("cr3 monitor: process view fill failed st=%08x", st);
        npt->DestroyView(id);
        InterlockedExchange(&g_cfg.ProcessView, 0);
        return;
    }
    g_cfg.ProcViewId = id;
    InterlockedExchange(&g_cfg.ProcessView, 1);
    SVMB_LOGI("cr3 monitor: process view %u ready", id);
}

// r53: push the per-core view policy into the hypervisor. When a process
// view exists AND a target CR3 is armed, the running core publishes the
// ProcView NCr3 while the target is current; otherwise cores revert to the
// base view. No-op when NPT is off.
static void ViewPolicyRefresh()
{
    Hypervisor* hv = Hypervisor::Instance();
    NptManager* npt = NptInstance();
    if (!hv || !npt || !npt->NptEnabled())
        return;
    NptView* base = npt->Active();
    if (!base)
        return;
    u64 procPml4 = 0;
    if (g_cfg.ProcessView && g_cfg.ProcViewId != 0 && g_cfg.TargetCr3 != 0)
    {
        NptView* pv = npt->View(g_cfg.ProcViewId);
        if (pv)
            procPml4 = pv->Pml4Pa();
    }
    Hypervisor::SetPerCoreView(procPml4 ? g_cfg.TargetCr3 : 0,
                               procPml4, base->Pml4Pa());
}

void Cr3MonitorConfigure(const SVMB_CR3_CONFIG* cfg)
{
    if (!cfg)
        return;
    InterlockedExchange(&g_cfg.ReadSpoof, cfg->EnableReadSpoof ? 1 : 0);
    InterlockedExchange(&g_cfg.WriteMonitor, cfg->EnableWriteMonitor ? 1 : 0);
    InterlockedExchange64((volatile LONG64*)&g_cfg.XorKey, (LONG64)cfg->XorKey);
    // EnableProcessView: per-process NPT view switching on context switches
    // in/out of the watched process (M3xM4). The view is created+filled once
    // and reused; disabling best-effort returns to the default view.
    if (cfg->EnableProcessView)
    {
        // flag goes live only when the view is actually ready - an Enter
        // decision during the fill window must not switch to view 0
        InterlockedExchange(&g_cfg.ProcessView, 0);
        EnsureProcessView();
    }
    else
    {
        InterlockedExchange(&g_cfg.ProcessView, 0);
        NptManager* npt = NptInstance();
        if (npt && npt->NptEnabled())
            npt->SetActiveView(0); // best-effort: leave the process view
    }

    // target selection priority: image name > pid > raw CR3. The name arms
    // the matching process now (if already seeded) and every later creation
    // through the seed notify hook; pid and raw CR3 are one-shot resolutions.
    Cr3Seed* seed = Cr3SeedInstance();
    char name[16] = {};
    RtlCopyMemory(name, (const void*)cfg->TargetImage, sizeof(name));
    name[15] = 0;

    if (name[0] && seed)
    {
        RtlCopyMemory((void*)g_cfg.TargetImage, name, sizeof(name));
        seed->SetNotifyCallback(OnSeedCreate, nullptr);
        u64 cr3 = 0;
        u32 pid = 0;
        if (seed->LookupByImage(name, cr3, pid))
        {
            InterlockedExchange64((volatile LONG64*)&g_cfg.TargetCr3,
                                  (LONG64)cr3);
            SVMB_LOGI("cr3 monitor: %s armed pid=%u cr3=%llx", name, pid, cr3);
            // r56: an already-running target used to stop at this log
            // line - the deferred sentinel walk was only scheduled from
            // OnSeedCreate, so spawn-then-watch flows armed zero pages.
            // Mirror the create-path scheduling; the 100ms worker delay
            // is harmless for a live target.
            g_watchPid = pid;
            if (g_armWorkerLive)
            {
                g_armPid = pid;
                if (InterlockedCompareExchange(&g_armPending, 1, 0) == 0)
                    KeSetEvent(&g_armEvent, IO_NO_INCREMENT, FALSE);
            }
            else
            {
                SVMB_LOGW("cr3 monitor: cr3_monitor module not attached - "
                          "sentinel disabled (svmbctl mod attach "
                          "cr3_monitor)");
            }
        }
        else
        {
            InterlockedExchange64((volatile LONG64*)&g_cfg.TargetCr3, 0);
            SVMB_LOGW("cr3 monitor: %s not seeded yet - arming on create",
                      name);
        }
    }
    else
    {
        g_cfg.TargetImage[0] = 0;
        g_watchPid = 0;
        SentinelDisarmAll();
        if (seed)
            seed->SetNotifyCallback(nullptr, nullptr);
        u64 cr3 = cfg->TargetCr3;
        if (cfg->TargetPid && seed)
        {
            u64 seeded = 0;
            if (seed->Lookup(cfg->TargetPid, seeded))
                cr3 = seeded;
            else
                SVMB_LOGW("cr3 monitor: pid %u not in seed table",
                          cfg->TargetPid);
        }
        InterlockedExchange64((volatile LONG64*)&g_cfg.TargetCr3, (LONG64)cr3);
    }

    // without any XorKey neither spoof path can produce a fake value
    // (cr3.target-needs-key) - surface it instead of letting the user
    // wonder why nothing spoofs
    if (g_cfg.TargetCr3 && !cfg->XorKey && !cfg->EnableWriteMonitor)
    {
        SVMB_LOGW("cr3 monitor: target armed without XorKey/WriteMonitor - "
                  "reads will not be spoofed");
    }
    ViewPolicyRefresh();
    ApplyCr3Intercepts();
}

void Cr3MonitorStats(SVMB_CR3_STATS* st)
{
    if (!st)
        return;
    st->ReadExitCount = g_readExits;
    st->WriteExitCount = g_writeExits;
    st->WriteApplyCount = g_writeExits; // every intercepted write is emulated
    st->ViewSwitchCount = g_viewSwitches;
    st->MonitoredCount = Cr3SeedInstance() ? Cr3SeedInstance()->Count() : 0;
    st->SentinelTrips = (u32)g_sentTrips;
    st->SentinelArmed = (u32)g_sentArmed;
    st->SentinelDiag = ((u32)g_lastWhy << 16) | ((u32)g_armWakes & 0xFFFF);
    st->SentinelReuse = (u32)g_sentReuse;
    st->NpfHeals = NpfHealCount();
    st->RegionTrips = (u32)g_regionTrips;
    st->WiggleKicks = (u32)g_wiggleKicks;
    st->KillAttempts = (u32)g_killAttempts;
    st->KillDenylisted = (u32)g_killDenylisted;
    st->KillDropped = (u32)g_killDropped;
    st->SuspAttempts = (u32)g_suspAttempts;
    st->SuspDenylisted = (u32)g_suspDenylisted;
    st->SuspDropped = (u32)g_suspDropped;
    st->SuspActive = (u32)g_suspCount;
    MemReadVmStats(st->ReadVmCalls, st->ReadVmPages);
}

// r80 wiggle mode knob (registry Parameters\WiggleMode); see the
// g_wiggleMode comment in the anonymous section for the semantics
void Cr3MonitorSetWiggleMode(u32 mode)
{
    InterlockedExchange(&g_wiggleMode, (LONG)mode);
}

// r71 test hook (IOCTL_CR3_POISON, PASSIVE): corrupt the first armed slot's
// watermark so the kill-switch paths see what they would see after the
// frame was freed and recycled - a header dword that no longer matches.
// F2 fires deterministically: the arm self-write stamped the slot's
// ResolveTick, so the re-arm DPC re-denies it ~100ms later, fails the
// watermark check and tears the slot down (g_sentReuse++). A foreign trip
// on the page lights F1 the same way.
u32 Cr3MonitorPoison()
{
    for (u32 i = 0; i < SENTINEL_MAX; ++i)
    {
        if (InterlockedCompareExchange64(&g_sent[i].Gpa4k, 0, 0) != 0)
        {
            LONG old = g_sent[i].Watermark;
            InterlockedExchange(&g_sent[i].Watermark, (LONG)0xDEADBEEF);
            SVMB_LOGW("sentinel: POISON slot %u gpa=%llx wm=%08x->deadbeef "
                      "(test hook)",
                      i, (u64)g_sent[i].Gpa4k, (u32)old);
            // the re-arm DPC is trip-driven and the trip flow freezes once
            // the shadow caches the armed pages - queue it directly so the
            // F2 watermark check runs without waiting for a natural trip
            KeInsertQueueDpc(&g_sentDpc, nullptr, nullptr);
            return 1;
        }
    }
    SVMB_LOGW("sentinel: POISON found no armed slot");
    return 0;
}

// ---- r75 protected-region public API (IOCTL_CR3_PROT, PASSIVE) ----------

// documented kernel interfaces whose declarations live in ntifs.h; this
// driver is ntddk-only (same situation as PsLookupThreadByThreadId above).
// The APC state is an opaque ≥KAPC_STATE(0x30) scratch - the kernel fills
// it, we never interpret it.
extern "C" NTSTATUS KeStackAttachProcess(void* Process, void* ApcState);
extern "C" void KeUnstackDetachProcess(void* ApcState);
struct alignas(16) OpaqueApcState
{
    UCHAR Bytes[64];
};
extern "C" NTSTATUS ZwProtectVirtualMemory(HANDLE ProcessHandle,
                                           PVOID* BaseAddress,
                                           PSIZE_T RegionSize, ULONG NewProtect,
                                           PULONG OldProtect);

// arm the NPT W-deny on every page of the region + one batch IPI flush
static void Cr3RegionNptArm(ProtRegion* r)
{
    NptView* views[2] = {};
    u32 nv = SentinelViews(views);
    for (u32 pg = 0; pg < r->Pages; ++pg)
    {
        u64 g = (u64)r->Active + pg * 0x1000ull;
        for (u32 v = 0; v < nv; ++v)
        {
            (void)views[v]->MapRange(g, 0x1000, {true, true, true});
            (void)views[v]->SetPerm4k(g, {false, true, true});
        }
    }
    for (u32 pg = 0; pg < r->Pages; pg += SENTINEL_MAX)
    {
        SentinelFlushBatch rb = {};
        for (u32 k = 0; k < SENTINEL_MAX && pg + k < r->Pages; ++k)
            rb.Va[rb.Count++] = (char*)r->BaseVa + ((u64)pg + k) * 0x1000ull;
        if (rb.Count)
            KeIpiGenericCall((PKIPI_BROADCAST_WORKER)SentinelFlushBatchCb,
                             (ULONG_PTR)&rb);
    }
}

// restore guest MM protection + NPT RWX + clear the slot (idempotent)
static void Cr3RegionUnprotectSlot(ProtRegion* r)
{
    // r85 audit P2-8: snapshot the restore parameters BEFORE publishing
    // the slot free - a death-sweep (process notify, outside the IOCTL
    // gate) can race a REGISTER that immediately reuses the slot and
    // overwrites Pid/BaseVa/OrigProtect; restoring from the live slot
    // would then undo the NEW region's L1 protection
    const u32 snapPid = r->Pid;
    const u32 snapId = r->Id;
    void* snapBaseVa = r->BaseVa;
    u64 snapSize = r->SizeBytes;
    u32 snapPages = r->Pages;
    u32 snapOrig = r->OrigProtect;
    PMDL snapPin = (PMDL)r->PinMdl;
    r->PinMdl = nullptr;
    LONG64 b = InterlockedExchange64(&r->Active, 0);
    if (!b)
        return;
    // r76: remember every freed page's GPA BEFORE any restore - a write
    // racing this teardown trips with no slot to match, and the consumer
    // must swallow it via the grace ring instead of failing fast
    for (u32 pg = 0; pg < snapPages; ++pg)
        RegionNoteDisarmed((u64)b + pg * 0x1000ull);
    PEPROCESS proc = nullptr;
    if (NT_SUCCESS(PsLookupProcessByProcessId(
            (HANDLE)(ULONG_PTR)snapPid, &proc)))
    {
        OpaqueApcState apc = {};
        KeStackAttachProcess(proc, &apc);
        PVOID base = snapBaseVa;
        SIZE_T sz = (SIZE_T)snapSize;
        ULONG oldP = 0;
        NTSTATUS st = ZwProtectVirtualMemory(NtCurrentProcess(), &base, &sz,
                                             snapOrig, &oldP);
        KeUnstackDetachProcess(&apc);
        if (!NT_SUCCESS(st))
            SVMB_LOGW("prot: restore protect failed pid=%u st=%08x",
                      snapPid, st);
        ObDereferenceObject(proc);
    }
    NptView* views[2] = {};
    u32 nv = SentinelViews(views);
    for (u32 pg = 0; pg < snapPages; ++pg)
    {
        u64 g = (u64)b + pg * 0x1000ull;
        for (u32 v = 0; v < nv; ++v)
            (void)views[v]->SetPerm4k(g, {true, true, true});
    }
    // r86: release the frame pin LAST - the guest-MM and NPT restores
    // above target the frames we sampled at register time, so the pin
    // must hold them until both restores are done
    if (snapPin)
    {
        MmUnlockPages(snapPin);
        IoFreeMdl(snapPin);
    }
    InterlockedDecrement(&g_protCount);
    SVMB_LOGI("prot: unprotected id=%u gpa=%llx (guest+_pt restored)",
              snapId, b);
}

void Cr3RegionCleanupAll()
{
    for (u32 i = 0; i < PROT_MAX; ++i)
        if (InterlockedCompareExchange64(&g_prot[i].Active, 0, 0) != 0)
            Cr3RegionUnprotectSlot(&g_prot[i]);
}

// r76: the owning process is dying - unprotect its regions NOW. A slot left
// behind keeps its NPT W-deny on frames the pool is about to recycle, and
// region slots have no watermark kill-switch (unlike the sentinel slots),
// so every innocent writer of a recycled frame would trip the region
// consumer forever (log spam + pointless re-arm churn, slot never freed).
void Cr3RegionSweepPid(u32 pid)
{
    u32 n = 0;
    for (u32 i = 0; i < PROT_MAX; ++i)
    {
        if (InterlockedCompareExchange64(&g_prot[i].Active, 0, 0) == 0)
            continue;
        if (g_prot[i].Pid != pid)
            continue;
        Cr3RegionUnprotectSlot(&g_prot[i]);
        ++n;
    }
    if (n)
        SVMB_LOGW("prot: death sweep pid=%u released %u region(s)", pid, n);
}

// r76 L2-bypass probe (IOCTL_CR3_PROBE, PASSIVE): write the magic through a
// kernel MDL mapping of the target VA. The MDL is locked with IoReadAccess
// (succeeds on the region's PAGE_READONLY guest PTE) and mapped into system
// space, where supervisor PTEs ignore the user mapping's protection - the
// classic kernel map-and-write bypass. A live region's NPT W-deny must fire
// on the write (RegionTrips++), resolve in the exit loop, and let it land;
// the L1 guest-MM block itself stays untouched.
u32 Cr3RegionProbe(SVMB_CR3_PROBE* p)
{
    if (!p)
        return 0;
    if (!p->Pid || !p->Base || p->Base & 0xFFFull || p->Size < 8 ||
        p->Size > 0x1000)
    {
        SVMB_LOGW("prot: probe rejected pid=%u base=%llx size=%x", p->Pid,
                  p->Base, p->Size);
        return 0;
    }
    PEPROCESS proc = nullptr;
    if (!NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)p->Pid,
                                               &proc)))
    {
        SVMB_LOGW("prot: probe lookup fail pid=%u", p->Pid);
        return 0;
    }
    // r76: a probe is only meaningful against a LIVE region - without a
    // matching slot the write would land silently (deny already restored)
    // and "success" would be a false positive for the E2E
    bool live = false;
    for (u32 i = 0; i < PROT_MAX; ++i)
    {
        if (InterlockedCompareExchange64(&g_prot[i].Active, 0, 0) == 0)
            continue;
        if (g_prot[i].Pid == p->Pid && (u64)g_prot[i].BaseVa == p->Base)
        {
            live = true;
            break;
        }
    }
    if (!live)
    {
        SVMB_LOGW("prot: probe rejected - no live region pid=%u base=%llx",
                  p->Pid, p->Base);
        ObDereferenceObject(proc);
        return 0;
    }
    u32 written = 0;
    OpaqueApcState apc = {};
    KeStackAttachProcess(proc, &apc);
    PMDL mdl = IoAllocateMdl((PVOID)(ULONG_PTR)p->Base, p->Size, FALSE,
                             FALSE, nullptr);
    if (mdl)
    {
        bool locked = false;
        void* kva = nullptr;
        __try
        {
            MmProbeAndLockPages(mdl, UserMode, IoReadAccess);
            locked = true;
            kva = MmMapLockedPagesSpecifyCache(mdl, KernelMode, MmCached,
                                               nullptr, FALSE,
                                               NormalPagePriority);
            if (kva)
            {
                // best effort: force the system-PTE mapping writable
                // (harmless if it already is)
                (void)MmProtectMdlSystemAddress(mdl, PAGE_READWRITE);
                *(volatile LONG64*)kva = SVMB_PROT_PROBE_MAGIC;
                written = 8;
            }
            else
            {
                SVMB_LOGW("prot: probe kernel map failed pid=%u", p->Pid);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            SVMB_LOGW("prot: probe write faulted pid=%u base=%llx "
                      "(guest MM blocked the kernel mapping?)",
                      p->Pid, p->Base);
        }
        if (kva)
            MmUnmapLockedPages(kva, mdl);
        if (locked)
            MmUnlockPages(mdl);
        IoFreeMdl(mdl);
    }
    else
    {
        SVMB_LOGW("prot: probe mdl alloc failed pid=%u", p->Pid);
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(proc);
    if (written)
    {
        p->OutWritten = written;
        p->OutMagic = SVMB_PROT_PROBE_MAGIC;
        SVMB_LOGW("prot: PROBE pid=%u base=%llx wrote magic via kernel map "
                  "(region trip expected)", p->Pid, p->Base);
    }
    return written;
}

u32 Cr3RegionAction(SVMB_CR3_PROT* p)
{
    if (!p)
        return 0;

    if (p->Action == SVMB_PROT_ACTION_REGISTER)
    {
        u64 pages = p->Size / 0x1000;
        // r85 audit P3-15: Size must be an exact page multiple - the old
        // truncated Pages while storing raw SizeBytes, so the consumer
        // range outgrew the armed range
        if (!p->Pid || !p->Base || !pages || pages > PROT_MAX_PAGES ||
            p->Base & 0xFFFull || p->Size & 0xFFFull)
        {
            SVMB_LOGW("prot: register rejected pid=%u base=%llx size=%llx",
                      p->Pid, p->Base, p->Size);
            return 0;
        }
        PEPROCESS proc = nullptr;
        if (!NT_SUCCESS(PsLookupProcessByProcessId(
                (HANDLE)(ULONG_PTR)p->Pid, &proc)))
        {
            SVMB_LOGW("prot: register lookup fail pid=%u", p->Pid);
            return 0;
        }
        // r76: stamp the id BEFORE publishing the slot - the old code read
        // the (still-zero) IOCTL input field here and only assigned
        // g_protNextId afterwards, so every slot carried Id=0 and the
        // unprotect log printed "id=0" forever.
        u32 regionId = g_protNextId++;
        for (u32 i = 0; i < PROT_MAX; ++i)
        {
            if (InterlockedCompareExchange64(&g_prot[i].Active, 0, 0) != 0)
                continue;
            OpaqueApcState apc = {};
            KeStackAttachProcess(proc, &apc);
            PVOID base = (PVOID)(ULONG_PTR)p->Base;
            // r86 audit P1-3: pin the region's frames BEFORE anything else
            // - an IoReadAccess lock faults the pages in and holds their
            // PFNs for the slot lifetime, so the OS can neither strand the
            // W-deny on a recycled frame (storm + false alerts) nor move
            // the region to a frame with no deny (silent sensing loss).
            // IoReadAccess (not IoWriteAccess) is required: the caller may
            // legitimately re-protect an already-readonly page.
            PMDL pin = IoAllocateMdl(base, (ULONG)p->Size, FALSE, FALSE,
                                     nullptr);
            if (!pin)
            {
                KeUnstackDetachProcess(&apc);
                SVMB_LOGW("prot: register rejected - pin mdl alloc failed "
                          "pid=%u",
                          p->Pid);
                ObDereferenceObject(proc);
                return 0;
            }
            bool pinned = false;
            __try
            {
                MmProbeAndLockPages(pin, UserMode, IoReadAccess);
                pinned = true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // bad VA (kernel address / no-access page): the probe
                // faults, nothing is locked - reject loudly
                SVMB_LOGW("prot: register rejected - pin failed pid=%u "
                          "base=%llx (probe fault)",
                          p->Pid, p->Base);
            }
            if (!pinned)
            {
                IoFreeMdl(pin);
                KeUnstackDetachProcess(&apc);
                ObDereferenceObject(proc);
                return 0;
            }
            SIZE_T sz = (SIZE_T)p->Size;
            ULONG oldP = 0;
            NTSTATUS st = ZwProtectVirtualMemory(NtCurrentProcess(), &base,
                                                 &sz, PAGE_READONLY, &oldP);
            if (NT_SUCCESS(st))
            {
                // sample the physical frames from the PINNED MDL's PFN
                // array (r86 review P2): sampling the VA instead would let
                // a same-process thread move the VA->frame binding between
                // pin and ZwProtect (WRITECOPY COW only), arming deny on a
                // frame the MDL does not hold. PFN-derived GPAs are the
                // pinned frames by construction.
                u64 gpaBase = 0;
                u64 gpas[PROT_MAX_PAGES];
                PPFN_NUMBER pfns = MmGetMdlPfnArray(pin);
                for (u32 pg = 0; pg < (u32)pages; ++pg)
                {
                    gpas[pg] = ((u64)pfns[pg]) << 12;
                    if (pg == 0)
                        gpaBase = gpas[pg];
                }
                // r76: a not-present page has no physical address (0). A
                // region on it would publish Active=0 - invisible to the
                // consumer, the sweep AND the list - and worse, the arm
                // loop would deny physical page 0. Reject before any
                // state is touched (the guest MM restore below undoes the
                // PAGE_READONLY on the rejection path).
                bool backed = true;
                for (u32 pg = 0; pg < (u32)pages; ++pg)
                    if (!gpas[pg])
                    {
                        backed = false;
                        break;
                    }
                if (!backed)
                {
                    // r86: locked pages are present by construction, so a
                    // zero GPA here is a structural anomaly - reject with
                    // the full teardown (protect + pin); the protect
                    // restore must run while still attached (r86 review
                    // P3: NtCurrentProcess is the IOCTL caller after
                    // detach, not the region owner)
                    PVOID rb = base;
                    SIZE_T rsz = (SIZE_T)p->Size;
                    ULONG rold = 0;
                    ZwProtectVirtualMemory(NtCurrentProcess(), &rb, &rsz,
                                           oldP, &rold);
                    KeUnstackDetachProcess(&apc);
                    MmUnlockPages(pin);
                    IoFreeMdl(pin);
                    SVMB_LOGW("prot: register rejected - zero GPA pid=%u "
                              "base=%llx",
                              p->Pid, (u64)base);
                    ObDereferenceObject(proc);
                    return 0;
                }
                // r76: publish the slot BEFORE arming the denies - a trip
                // racing the arm loop must find a matching slot, otherwise
                // it falls through to the Release fail-fast (0xE2 'SVMC')
                g_prot[i].Pid = p->Pid;
                g_prot[i].Id = regionId;
                g_prot[i].BaseVa = base;
                g_prot[i].SizeBytes = p->Size;
                g_prot[i].Pages = (u32)pages;
                g_prot[i].OrigProtect = oldP;
                g_prot[i].ResolveTick = 0;
                g_prot[i].PinMdl = pin;
                // policy lands before the Active publish: a trip racing the
                // arm loop must see the final policy, not a stale zero.
                // r91: unknown bits are rejected LOUD (r89 InFlags
                // convention - never silently mask what the caller asked
                // for); SUSPEND additionally requires the runtime-resolved
                // primitive - refuse loudly instead of alerting forever.
                if (p->Policy
                    & ~(SVMB_PROT_POLICY_KILL | SVMB_PROT_POLICY_SUSPEND))
                {
                    PVOID rb = base;
                    SIZE_T rsz = (SIZE_T)p->Size;
                    ULONG rold = 0;
                    ZwProtectVirtualMemory(NtCurrentProcess(), &rb, &rsz,
                                           oldP, &rold);
                    KeUnstackDetachProcess(&apc);
                    MmUnlockPages(pin);
                    IoFreeMdl(pin);
                    ObDereferenceObject(proc);
                    SVMB_LOGW("prot: unknown policy bits %08x refused",
                              p->Policy);
                    return 0;
                }
                if ((p->Policy & SVMB_PROT_POLICY_KILL) && !g_killItem)
                {
                    // r96 audit K5: mirror the SUSPEND honesty - a KILL
                    // region must not register when its work item never
                    // allocated (the trip would silently degrade to
                    // alert-only)
                    PVOID rb = base;
                    SIZE_T rsz = (SIZE_T)p->Size;
                    ULONG rold = 0;
                    ZwProtectVirtualMemory(NtCurrentProcess(), &rb, &rsz,
                                           oldP, &rold);
                    KeUnstackDetachProcess(&apc);
                    MmUnlockPages(pin);
                    IoFreeMdl(pin);
                    ObDereferenceObject(proc);
                    SVMB_LOGW("prot: KILL policy refused - kill work item "
                              "unavailable");
                    return 0;
                }
                if ((p->Policy & SVMB_PROT_POLICY_SUSPEND)
                    && !g_pfnSuspendProcess)
                {
                    PVOID rb = base;
                    SIZE_T rsz = (SIZE_T)p->Size;
                    ULONG rold = 0;
                    ZwProtectVirtualMemory(NtCurrentProcess(), &rb, &rsz,
                                           oldP, &rold);
                    KeUnstackDetachProcess(&apc);
                    MmUnlockPages(pin);
                    IoFreeMdl(pin);
                    ObDereferenceObject(proc);
                    SVMB_LOGW("prot: SUSPEND policy refused - "
                              "PsSuspendProcess unavailable");
                    return 0;
                }
                g_prot[i].Policy =
                    (LONG)(p->Policy
                           & (SVMB_PROT_POLICY_KILL
                              | SVMB_PROT_POLICY_SUSPEND));
                g_prot[i].WriterCr3 = 0;
                g_prot[i].WriterRip = 0;
                g_prot[i].WriterGpa = 0;
                InterlockedExchange64(&g_prot[i].Active, (LONG64)gpaBase);
                InterlockedIncrement(&g_protCount);
                // dual-view deny W (r57 rule), after the slot is published
                for (u32 pg = 0; pg < (u32)pages; ++pg)
                {
                    NptView* views[2] = {};
                    u32 nv = SentinelViews(views);
                    for (u32 v = 0; v < nv; ++v)
                    {
                        (void)views[v]->MapRange(gpas[pg], 0x1000,
                                                 {true, true, true});
                        (void)views[v]->SetPerm4k(gpas[pg],
                                                  {false, true, true});
                    }
                }
                KeUnstackDetachProcess(&apc);
                p->OutId = regionId;
                p->OutOrigProtect = oldP;
                ObDereferenceObject(proc);
                SVMB_LOGW("prot: REGISTER id=%u pid=%u base=%llx pages=%u "
                          "oldProt=%08x (L1 readonly + L2 deny)",
                          p->OutId, p->Pid, gpaBase, (u32)pages, oldP);
                return 1;
            }
            MmUnlockPages(pin);
            IoFreeMdl(pin);
            KeUnstackDetachProcess(&apc);
            SVMB_LOGW("prot: ZwProtect failed pid=%u st=%08x", p->Pid, st);
            ObDereferenceObject(proc);
            return 0;
        }
        SVMB_LOGW("prot: region table full");
        ObDereferenceObject(proc);
        return 0;
    }

    if (p->Action == SVMB_PROT_ACTION_UNPROTECT)
    {
        u32 n = 0;
        if (p->InUnprotectId)
        {
            // r78: unprotect by region id - the multi-target workflow
            // addresses regions by the id REGISTER returned. Ids are
            // unique among live slots (monotonic g_protNextId under the
            // IOCTL gate); the u32 wrap-around needs 2^32 registers and
            // is not a real scenario at 32 slots.
            for (u32 i = 0; i < PROT_MAX; ++i)
            {
                if (InterlockedCompareExchange64(&g_prot[i].Active, 0, 0) ==
                    0)
                    continue;
                if (g_prot[i].Id != p->InUnprotectId)
                    continue;
                Cr3RegionUnprotectSlot(&g_prot[i]);
                ++n;
            }
            p->OutId = n;
            return n;
        }
        for (u32 i = 0; i < PROT_MAX; ++i)
        {
            LONG64 b = InterlockedCompareExchange64(&g_prot[i].Active, 0, 0);
            if (!b)
                continue;
            // match by pid (+base when given); Base=0 → all of this pid
            if (g_prot[i].Pid != p->Pid)
                continue;
            if (p->Base != 0 && (u64)g_prot[i].BaseVa != p->Base)
                continue;
            Cr3RegionUnprotectSlot(&g_prot[i]);
            ++n;
        }
        p->OutId = n;
        return n;
    }

    if (p->Action == SVMB_PROT_ACTION_LIST)
    {
        u32 n = 0;
        for (u32 i = 0; i < PROT_MAX && n < SVMB_PROT_MAX_ENTRIES; ++i)
        {
            LONG64 b = InterlockedCompareExchange64(&g_prot[i].Active, 0, 0);
            if (!b)
                continue;
            p->Entries[n].Id = g_prot[i].Id;
            p->Entries[n].Pid = g_prot[i].Pid;
            p->Entries[n].Base = (u64)g_prot[i].BaseVa;
            p->Entries[n].Size = g_prot[i].SizeBytes;
            // r91: echo the live policy (r89 P3-6 - ctl must be able to
            // verify a KILL/SUSPEND registration actually stuck)
            p->Entries[n].Policy =
                (u32)InterlockedCompareExchange(&g_prot[i].Policy, 0, 0);
            ++n;
        }
        p->OutCount = g_protCount;
        return n;
    }
    return 0;
}

// r76: the NPF deny consumer must be live for the WHOLE driver lifetime,
// not just while the module is attached. The protected-region L2 sensing
// (and the orphan-resolve grace paths) are reachable from plain IOCTLs on a
// bare deployment (svc start, no `mod attach cr3_monitor`) - with the
// callback unregistered, the first region deny fell through to the Release
// fail-fast (0xE2 'SVMC', the r76 23:19 BSOD and the r75-era 22:47 BSOD).
// DriverEntry calls this before NPT consumers can exist.
//
// The death notify lives here for the same reason: OnSeedCreate (which
// sweeps the sentinel) only fires when the module is attached, so a
// bare deployment kept a dead owner's region denies live on freed frames.
static VOID Cr3RegionDeathNotify(PEPROCESS, HANDLE pid,
                                 PPS_CREATE_NOTIFY_INFO info);

static VOID Cr3RegionDeathNotify(PEPROCESS, HANDLE pid,
                                 PPS_CREATE_NOTIFY_INFO info)
{
    if (!info)
    {
        // termination: region sweep + r91 suspend-table prune (a suspended
        // writer killed/resumed-and-exited by other means must not linger)
        // + r91 gate rescue (acceptance P1-1): a plan-C- victim frozen
        // inside DevCtrlLocked holds the gate; killing it (taskkill /F on
        // a "hung" tool) must not wedge every gated IOCTL forever
        Cr3RegionSweepPid((u32)(ULONG_PTR)pid);
        SuspPrunePid((u32)(ULONG_PTR)pid);
        IoGateOwnerDied((u32)(ULONG_PTR)pid);
    }
}

void Cr3MonitorLoadInit()
{
    KeInitializeDpc(&g_sentDpc, SentinelRearmDpc, nullptr);
    NpfSetDenyCallback(SentinelDenyCb, nullptr);
    KeInitializeDpc(&g_killDpc, KillDpcRoutine, nullptr);
    // r83: resolve the system layout BEFORE anything touches EPROCESS/
    // ETHREAD fields (seed notify + sentinel walk both consume it; both
    // degrade loudly when this fails)
    g_offsetsOk = NT_SUCCESS(OffsetsResolve());
    NTSTATUS st = PsSetCreateProcessNotifyRoutineEx(Cr3RegionDeathNotify,
                                                    FALSE);
    SVMB_LOGI("prot: death notify st=%08x offsets=%s", st,
              g_offsetsOk ? "resolved" : "UNRESOLVED");
}

void Cr3MonitorLoadDeinit()
{
    PsSetCreateProcessNotifyRoutineEx(Cr3RegionDeathNotify, TRUE);
}

// ---- r100 execute-sense seam (used by SentinelDenyCb above) -------------
// svmb-scope so the sysaudit module can register its consumer. The state
// is a single optional callback - the seam itself is lock-free.

void Cr3MonitorSetSenseCb(Cr3SenseCb cb)
{
    g_senseCb = cb;
}

u32 Cr3MonitorArmViews(NptView* views[2])
{
    return SentinelViews(views);
}

// ---- r89 plan C public pipeline (docs/L2_BLOCKING_DESIGN.md 3-C) -------

// deny list for the kill policy: system-critical images must never be
// terminated even when their CR3 resolves (csrss/wininit/winlogon/
// services/lsass/smss). r96 audit K2: also the resident-manager images
// System / Registry / MemCompression (pid<=4 covers System by pid, the
// name entries cover a recycled-pid future). Pure, offline-tested.
bool Cr3RegionKillDenylisted(const char* image)
{
    static const char* const kDeny[] = {
        "csrss.exe",  "wininit.exe", "winlogon.exe",
        "services.exe", "lsass.exe", "smss.exe",
        "system", "registry", "memcompression",
    };
    if (!image)
        return false;
    for (const char* d : kDeny)
    {
        if (Cr3ImageMatch(image, d))
            return true;
    }
    return false;
}

// called from DriverEntry right after the control device exists: the kill
// workitem is device-tied; the suspend primitives resolve here so a
// missing export degrades REGISTER honestly (acceptance-friendly)
void Cr3RegionKillInit(PDEVICE_OBJECT dev)
{
    UNICODE_STRING fnSus = RTL_CONSTANT_STRING(L"PsSuspendProcess");
    UNICODE_STRING fnRes = RTL_CONSTANT_STRING(L"PsResumeProcess");
    g_pfnSuspendProcess =
        (PS_SUSPEND_PROCESS_FN)MmGetSystemRoutineAddress(&fnSus);
    g_pfnResumeProcess =
        (PS_RESUME_PROCESS_FN)MmGetSystemRoutineAddress(&fnRes);
    KeInitializeSpinLock(&g_suspLock);
    if (dev)
        g_killItem = IoAllocateWorkItem(dev);
    SVMB_LOGI("prot: policy pipeline %s suspend=%s resume=%s",
              g_killItem ? "ready" : "UNAVAILABLE (no work item)",
              g_pfnSuspendProcess ? "yes" : "NO",
              g_pfnResumeProcess ? "yes" : "NO");
}

// PASSIVE: release every suspended writer (reversibility default on
// unload - a suspended process must never be stranded by driver removal)
static VOID SuspResumeAll()
{
    for (u32 i = 0; i < 16; ++i)
    {
        KIRQL irql = KeGetCurrentIrql();
        KeAcquireSpinLock(&g_suspLock, &irql);
        u32 pid = g_susp[i].Pid;
        if (pid)
        {
            g_susp[i].Pid = 0;
            InterlockedDecrement(&g_suspCount);
        }
        KeReleaseSpinLock(&g_suspLock, irql);
        if (pid)
        {
            PEPROCESS proc = nullptr;
            if (NT_SUCCESS(PsLookupProcessByProcessId(
                    (HANDLE)(ULONG_PTR)pid, &proc))
                && proc)
            {
                if (g_pfnResumeProcess)
                    (void)g_pfnResumeProcess(proc);
                ObDereferenceObject(proc);
                SVMB_LOGW("prot: resume-all released pid=%u", pid);
            }
        }
    }
}

// driver-unload hygiene: claim the work item atomically (acceptance P2-1:
// a racing KillDpcRoutine must never see a freed item), drain a queued DPC,
// and wait bounded for an in-flight kill. A kill still in flight past the
// wait LEAKS the item (one allocation, never freed - the safe side; the
// driver is going away).
void Cr3RegionKillDeinit()
{
    KeRemoveQueueDpc(&g_killDpc); // drop a queued, unrun DPC
    SuspResumeAll(); // r91: never strand a plan-C- suspended process
    PIO_WORKITEM item =
        (PIO_WORKITEM)InterlockedExchangePointer((PVOID*)&g_killItem,
                                                 nullptr);
    if (!item)
        return;
    for (int i = 0; i < 1000; ++i)
    {
        if (!InterlockedCompareExchange(&g_killInFlight, 0, 0))
        {
            IoFreeWorkItem(item);
            SuspResumeAll(); // r91 P2-1: a workitem that flew AFTER the
                             // first sweep may have suspended someone new
            return;
        }
        LARGE_INTEGER ms1;
        ms1.QuadPart = -10000; // 1ms
        KeDelayExecutionThread(KernelMode, FALSE, &ms1);
    }
    // still in flight after 1s (ZwTerminateProcess stalled): leak `item`
    // rather than free it under the worker; best effort - release anyone
    // the in-flight item managed to suspend
    SuspResumeAll();
}

// r105: suspend a pid on a sysaudit behavior alert (the response path is
// opt-in via Parameters\BehaveResponse=1; default is telemetry-only).
// Shares the r91 suspension table/locks/resume-all with the kill path,
// honors the S1 invariant (usable slot checked BEFORE the freeze) and
// adds the r96-ticketed RESUME BELT: this is a SECOND table filler (the
// kill workitem is the first), so the non-atomic check->freeze->insert
// window is closed by undoing the freeze when the insert finds no slot -
// a full table can no longer strand a frozen-but-unlisted process.
// RegionId carries the SYSALERT sentinel so `cr3 suspended` shows the
// provenance. Returns the pid on success, 0 on refusal (guards, missing
// primitive, table full).
u32 Cr3RegionSuspendPidSysalert(u32 pid, const char* image)
{
    if (!pid || pid <= 4 || Cr3RegionKillDenylisted(image))
    {
        InterlockedIncrement64(&g_suspDenylisted);
        return 0;
    }
    if (!g_pfnSuspendProcess || !g_pfnResumeProcess)
        return 0;
    PEPROCESS proc = nullptr;
    if (!NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid,
                                               &proc))
        || !proc)
        return 0;
    // r105 acceptance P1-5: EXECUTE-time identity - the enqueue-time
    // image may belong to a recycled pid by the time the workitem runs.
    // EPROCESS ImageFileName is 15 chars, not guaranteed NUL-terminated.
    char liveImage[16] = {};
    RtlCopyMemory(liveImage, PsGetProcessImageFileName(proc), 15);
    liveImage[15] = '\0';
    if (pid <= 4 || Cr3RegionKillDenylisted(liveImage))
    {
        InterlockedIncrement64(&g_suspDenylisted);
        ObDereferenceObject(proc);
        return 0;
    }
    // r105 acceptance P0-1/P1-1: decide under the lock, ACT unlocked -
    // PsSuspendProcess/PsResumeProcess are PASSIVE-only and the spinlock
    // raises to DISPATCH. Also: a table-full decline has no freeze to
    // undo, so the belt must not fire there (P1-1 - the first draft
    // resumed a process it never suspended).
    KIRQL irql = KeGetCurrentIrql();
    KeAcquireSpinLock(&g_suspLock, &irql);
    SuspEntry* e = SuspFindLocked(pid);
    const bool alreadyListed = (e != nullptr);
    const bool mayFreeze = alreadyListed || SuspFreeLocked() != nullptr;
    KeReleaseSpinLock(&g_suspLock, irql);
    NTSTATUS st = STATUS_SUCCESS;
    bool frozen = false;
    if (alreadyListed)
    {
        // cumulative-refcount guard: PsSuspendProcess is cumulative per
        // call and the single table entry repays exactly one - a repeat
        // offender stays frozen by the FIRST suspend; refresh the heat
        // only
        InterlockedIncrement64(&g_suspDenylisted);
        SVMB_LOGW("prot: sysalert pid=%u already listed - no extra "
                  "freeze refcount",
                  pid);
    }
    else if (mayFreeze)
    {
        st = g_pfnSuspendProcess(proc);
        if (NT_SUCCESS(st))
        {
            frozen = true;
            KeAcquireSpinLock(&g_suspLock, &irql);
            e = SuspFindLocked(pid);
            if (!e)
                e = SuspFreeLocked();
            if (e)
            {
                const bool fresh = (e->Pid != pid);
                e->Pid = pid;
                e->RegionId = 0xFFFFFFFF; // SYSALERT sentinel
                e->Tick = (u64)KeQueryInterruptTime();
                RtlCopyMemory(e->Image, liveImage, sizeof(e->Image));
                e->Image[15] = '\0';
                if (fresh)
                    InterlockedIncrement(&g_suspCount);
            }
            KeReleaseSpinLock(&g_suspLock, irql);
            SVMB_LOGW("prot: sysalert SUSPEND pid=%u image='%s' st=%08x",
                      pid, liveImage, st);
        }
        else
        {
            SVMB_LOGW("prot: sysalert SUSPEND failed pid=%u st=%08x", pid,
                      st);
        }
    }
    else
    {
        // table full, pid unlisted: decline with NO freeze - nothing to
        // undo (the belt exists for the freeze-then-lose-the-slot race,
        // handled below)
        InterlockedIncrement64(&g_suspDropped);
        SVMB_LOGE("prot: sysalert SUSPEND declined pid=%u - table FULL, "
                  "no freeze",
                  pid);
    }
    if (frozen)
    {
        // the insert may still have lost the last slot to the other
        // filler - resume belt, OUTSIDE the lock (PASSIVE-only call;
        // r105 acceptance P0-1)
        KeAcquireSpinLock(&g_suspLock, &irql);
        e = SuspFindLocked(pid);
        KeReleaseSpinLock(&g_suspLock, irql);
        if (!e)
        {
            g_pfnResumeProcess(proc);
            InterlockedIncrement64(&g_suspDropped);
            st = STATUS_INSUFFICIENT_RESOURCES;
            SVMB_LOGE("prot: sysalert SUSPEND table FULL pid=%u - resume "
                      "belt fired (no strand)",
                      pid);
        }
    }
    ObDereferenceObject(proc);
    return NT_SUCCESS(st) ? pid : 0;
}

// ---- r91 plan C- IOCTL helpers (ungated fast paths) --------------------

// IOCTL_SUSP_LIST: fill the suspension table snapshot
u32 Cr3RegionSuspList(SVMB_SUSP* p)
{
    if (!p)
        return 0;
    KIRQL irql = KeGetCurrentIrql();
    KeAcquireSpinLock(&g_suspLock, &irql);
    u32 n = 0;
    for (u32 i = 0; i < 16 && n < SVMB_SUSP_MAX_ENTRIES; ++i)
    {
        if (!g_susp[i].Pid)
            continue;
        p->Entries[n].Pid = g_susp[i].Pid;
        p->Entries[n].RegionId = g_susp[i].RegionId;
        p->Entries[n].Tick = g_susp[i].Tick;
        RtlCopyMemory(p->Entries[n].Image, g_susp[i].Image, 16);
        ++n;
    }
    KeReleaseSpinLock(&g_suspLock, irql);
    p->OutCount = n;
    p->SuspAvailable = (g_pfnSuspendProcess && g_pfnResumeProcess) ? 1 : 0;
    return 1;
}

// IOCTL_SUSP_RESUME: release one suspended writer (pid != 0) or all.
// r91 acceptance P2-2: a pid whose table slot was lost (16-slot overflow)
// is STILL resumable - the direct fallback resumes it without a record.
u32 Cr3RegionSuspResume(SVMB_SUSP* p)
{
    if (!p)
        return 0;
    const u32 pid = p->Pid;
    u32 resumed = 0;
    for (u32 i = 0; i < 16; ++i)
    {
        KIRQL irql = KeGetCurrentIrql();
        KeAcquireSpinLock(&g_suspLock, &irql);
        u32 victim = 0;
        if (g_susp[i].Pid && (pid == 0 || g_susp[i].Pid == pid))
        {
            victim = g_susp[i].Pid;
            g_susp[i].Pid = 0;
            InterlockedDecrement(&g_suspCount);
        }
        KeReleaseSpinLock(&g_suspLock, irql);
        if (!victim)
            continue;
        PEPROCESS proc = nullptr;
        if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)victim,
                                                  &proc))
            && proc)
        {
            if (g_pfnResumeProcess)
            {
                (void)g_pfnResumeProcess(proc);
                ++resumed;
                SVMB_LOGW("prot: SUSP resume pid=%u", victim);
            }
            ObDereferenceObject(proc);
        }
    }
    if (pid != 0 && resumed == 0)
    {
        // not in the table: resume directly (harmless no-op when the
        // target is not actually suspended) - closes the table-overflow
        // unrecoverable-frozen hole
        PEPROCESS proc = nullptr;
        if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)pid,
                                                  &proc))
            && proc)
        {
            if (g_pfnResumeProcess)
            {
                (void)g_pfnResumeProcess(proc);
                ++resumed;
                SVMB_LOGW("prot: SUSP resume (direct) pid=%u", pid);
            }
            ObDereferenceObject(proc);
        }
    }
    p->OutCount = resumed;
    return resumed || pid == 0 ? 1 : 0;
}

NTSTATUS Cr3MonInit(const SvmbApi* api)
{
    UNICODE_STRING fnName = RTL_CONSTANT_STRING(L"PsGetNextProcessThread");
    g_psnpt = (PsGetNextProcessThreadFn)MmGetSystemRoutineAddress(&fnName);
    SVMB_LOGI("sentinel: psnpt=%p", g_psnpt);
    NTSTATUS tnSt = PsSetCreateThreadNotifyRoutine(SentinelThreadNotify);
    SVMB_LOGI("sentinel: thread notify st=%08x", tnSt);
    KeInitializeEvent(&g_armEvent, SynchronizationEvent, FALSE);
    OBJECT_ATTRIBUTES ob;
    InitializeObjectAttributes(&ob, nullptr, OBJ_KERNEL_HANDLE, nullptr,
                               nullptr);
    NTSTATUS thSt = PsCreateSystemThread(&g_armThread, THREAD_ALL_ACCESS,
                                         &ob, nullptr, nullptr,
                                         SentinelArmWorker, nullptr);
    SVMB_LOGI("sentinel: arm worker st=%08x", thSt);
    if (NT_SUCCESS(thSt))
    {
        InterlockedExchange(&g_armWorkerLive, 1);
        // r56: a watch issued before this attach could have latched
        // g_armPending=1 into the void (no worker to clear it) - that
        // stale gate would swallow the first post-attach KeSetEvent.
        InterlockedExchange(&g_armPending, 0);
    }
    // r43: do NOT require the CR3 intercept here. Attaching the module used
    // to tax every context switch on every core (~2500 exits/s on this vhv)
    // and starve the guest into timeouts that looked like wedge + self-
    // unload. Handlers register now; the intercept arms lazily from
    // Cr3MonitorConfigure (ApplyCr3Intercepts) only while a target exists.
    g_cr3Token = api->Token;
    NTSTATUS st = api->RegisterExitHandler(api, vmexit::CR_READ(3), HandleCrRead,
                                  nullptr, 5);
    if (!NT_SUCCESS(st))
        return st;
    return api->RegisterExitHandler(api, vmexit::CR_WRITE(3), HandleCrWrite,
                                    nullptr, 5);
}

void Cr3MonStop()
{
    PsRemoveCreateThreadNotifyRoutine(SentinelThreadNotify);
    InterlockedExchange(&g_armWorkerLive, 0);
    if (g_armThread)
    {
        InterlockedExchange(&g_armShutdown, 1);
        KeSetEvent(&g_armEvent, IO_NO_INCREMENT, FALSE);
        PVOID h = g_armThread;
        g_armThread = nullptr;
        ZwClose(*(PHANDLE)&h);
    }
    SentinelDisarmAll();
    g_readExits = 0;
    g_writeExits = 0;
    // r43: drop the config-time CR3 intercept if one is armed (unwind
    // before the module's token goes away with the ReleaseOwner pass)
    Hypervisor* hv = Hypervisor::Instance();
    if (hv && g_cr3Token != 0)
        hv->Intercepts().ReleaseCr(g_cr3Token, 3, true, true);
}

SVMB_DEFINE_MODULE(ModuleCr3Monitor, "cr3_monitor", 0, Cr3MonInit, Cr3MonStop)

} // namespace svmb
