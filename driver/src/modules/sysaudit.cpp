// svmb - r101 syscall audit v3: EXECUTE-SENSE syscall auditing.
// (r99 verdict: the v1 inline NPT hook on MmCopyVirtualMemory tripped
// PatchGuard 0x109 - kernel-byte modification is unusable on PG-protected
// functions.) v2 modifies ZERO guest bytes:
//   ARMED  : watched functions' 4K pages are X-deny in the NPT (both views,
//            then an IPI/KICK flush - r56: a deny flip is inert until every
//            core's NPT TLB drops the cached translation)
//   trip   : the entry fetch faults -> the consumer records the call args
//            (still live in the guest registers at entry-fetch) with caller
//            attribution (cr3->pid/image, r91-hardened reverse lookup)
//   resolve: the page reopens (X allowed) and the faulting instruction
//            re-executes natively - zero skipped bytes (r56: failed walks
//            are not cached, so no flush is needed on resolve)
//   re-arm : a timer re-denies 250ms later (+ the same flush)
// Watched targets (r101 v3, five sense pages): MmCopyVirtualMemory (the
// large-copy funnel for NtRead/WriteVirtualMemory; SMALL copies take the
// in-function pool path and are NOT sensed), NtCreateFile (exported,
// high-frequency file-open audit), and the SSDT-derived NtOpenProcess /
// NtReadVirtualMemory / NtWriteVirtualMemory pages - the small-copy
// blind spot closed. SSDT law (offline-verified against the pulled guest
// image + live kdump, 18362.592, SHA256 2ea1e2a2...): KiServiceTable sits
// at RVA 0x424C10; the on-disk initializer holds ABSOLUTE image RVAs, but
// at boot the table is REWRITTEN into packed table-relative form -
// service = table + (s32(entry) >> 4), low nibble = stack-arg count.
// (The r101 WIP failed because it assumed table-relative WITHOUT the
// >>4/unpack - every candidate failed the anchor.) All four SSN->RVA
// constants are re-verified against the running image at attach; a
// mismatch refuses to arm (stage 3) instead of X-denying a wrong page.
// The table itself is only ever READ (writes there are 0x109/0xa
// territory). PatchGuard-invisible by
// construction: the guest's view of the page bytes never changes.
// Lock-free ring (r99 lesson: no spinlocks on paths a freeze/kill
// policy can strand mid-hold); overflow claims write tombstones so the
// drain's sequence continuity never wedges (r100 acceptance P1-1).
#include "modules/sysaudit.h"
#include "modules/cr3_monitor.h"
#include "modules/cr3_seed.h"
#include "modules/npt_hook_mgr.h"
#include "hw/svm_defs.h"
#include "mm/tlb.h"
#include "svmb/module_api.h"
#include "platform/saferead.h"
#include "core/crumbs.h"

// documented kernel interfaces whose declarations live in ntifs.h -
// this driver is ntddk-only (mem_read.cpp / cr3_monitor.cpp pattern)
extern "C" NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId,
                                               PEPROCESS* Process);
extern "C" VOID KeStackAttachProcess(void* Process, void* ApcState);
extern "C" void KeUnstackDetachProcess(void* ApcState);
extern "C" NTSTATUS MmCopyVirtualMemory(PEPROCESS SourceProcess,
                                        void* SourceAddress,
                                        PEPROCESS TargetProcess,
                                        void* TargetAddress, SIZE_T Size,
                                        int PreviousMode, SIZE_T* ReturnSize);

namespace svmb
{

constexpr u32 RING_SLOTS = 256; // power of two - slot math uses masks

// ---- pure slot math (offline-tested; header-declared) ---------------------

u32 SysAuditSlotIndex(u32 seq)
{
    return seq & (RING_SLOTS - 1);
}

bool SysAuditSlotReady(u64 commit, LONG d)
{
    return commit == (u64)d + 1;
}

// r101 runtime KiServiceTable law (18362/19H1, kdump-verified live): the
// on-disk initializer holds ABSOLUTE image RVAs (entry[0x55]=0x630540),
// but KiSystemStartup rewrites the table at boot into PACKED TABLE-
// RELATIVE form: service = table + (s32(entry) >> 4), low nibble = stack
// arg count in qwords (entry[0x55]=0x020b9307 -> NtCreateFile, args=7).
u64 SysAuditServiceVa(u64 tableVa, u32 entry)
{
    return tableVa + (((LONG64)(LONG)entry) >> 4);
}

// ---- r102 behavior math ----------------------------------------------------

// interrupt time is 100ns units; the analytics window is 1s
u64 SysAuditBehaveWindowOf(u64 tick)
{
    return tick / (SVMB_SYSAUD_BEHAVE_WIN_MS * 10000ull);
}

// slot probe start. Note: with a 16-slot mask, any multiplicative hash
// degenerates (2654435761 mod 16 == 1) - this is an identity map on
// pid mod 16; the linear probe absorbs the collisions and correctness
// never depended on hash quality
u32 SysAuditBehaveSlotFor(u32 pid)
{
    return (pid * 2654435761u) & (SVMB_SYSAUD_BEHAVE_SLOTS - 1);
}

bool SysAuditBehaveEdge(u64 count, u64 threshold)
{
    return count == threshold;
}

// r103 (caller,target) cache-slot hash probe start. Like the caller
// hash this degenerates under the 32-slot mask - the linear probe is
// the real mechanism; correctness never depends on hash quality
u32 SysAuditTargetSlotFor(u32 callerPid, u32 targetPid)
{
    return ((callerPid * 0x9E3779B1u) ^ (targetPid * 0x85EBCA6Bu))
           & (SVMB_SYSAUD_TARGET_SLOTS - 1);
}

namespace
{

constexpr LONG SENSE_REARM_MS = 250;

// five sense pages: the two exported targets + the three SSDT-derived
// Nt* pages (FnIds 0,1,3,4,5 - LOST=2 is a tombstone, not a page)
constexpr u32 SENSE_COUNT = SVMB_SYSAUD_MAX_TARGETS - 1;

// r101 SSDT law, offline-verified against the guest image (ntoskrnl.exe
// 18362.592, SHA256 2ea1e2a2945fc4808b9fef1701bcfb5430bd1d7ccc9b9321986b1
// 2f807b53c37) and cross-checked against the r99 crash dump's symbols
// (nt base fffff800`48800000, PDB F3A4F64B...) and its Zw* stub bytes
// (ZwCreateFile mov eax,55h; ZwOpenProcess mov eax,26h):
constexpr u32 SSDT_TABLE_RVA = 0x424C10;
constexpr u32 SSDT_NTCF_SSN = 0x55;
constexpr u32 SSDT_NTCF_RVA = 0x630540;
struct SsdtAnchor
{
    u32 Ssn;
    u32 Rva;
    u32 FnId;
};
constexpr SsdtAnchor SSDT_ANCHORS[3] = {
    {0x26, 0x61D4F0, SVMB_SYSAUD_FN_NTOP}, // NtOpenProcess
    {0x3F, 0x622A80, SVMB_SYSAUD_FN_NTRD}, // NtReadVirtualMemory
    {0x3A, 0x6D8D30, SVMB_SYSAUD_FN_NTWR}, // NtWriteVirtualMemory
};

// lock-free ring: producers claim monotonic sequence numbers, the slot is
// committed by writing Commit = seq+1 LAST (x86 TSO store order = release);
// the drain consumes strictly in order and marks slots empty. An overflow
// claim (ring full) writes a FN_LOST tombstone instead of dropping the
// sequence - a discarded sequence would wedge the drain on a Commit hole
// forever (r100 acceptance P1-1).
struct Ring
{
    SVMB_SYSAUD_ENTRY Slots[RING_SLOTS] = {};
    volatile LONG W = 0;  // claimed entries (monotonic)
    volatile LONG D = 0;  // drained entries (monotonic)
    volatile LONG64 Total = 0;
    volatile LONG64 Lost = 0;
    volatile LONG64 Noise = 0; // non-target fetches on the armed pages
};

Ring g_Ring = {};
bool g_Armed = false;

// r102 alert thresholds - registry-overridable (Parameters\BehaveRdThs /
// BehaveWrThs / BehaveOpThs, read once at DriverEntry; 0 keeps the
// protocol default). Deliberately high by default: the X-deny sensor
// samples (r100 law - cached TLB translations slide, so the sensed
// density is a few entries/second/page on this vhv), a threshold is a
// sensed-burst alarm, not a true-syscall-rate meter.
u32 g_BehaveRdThs = SVMB_SYSAUD_BEHAVE_RD_THS;
u32 g_BehaveWrThs = SVMB_SYSAUD_BEHAVE_WR_THS;
u32 g_BehaveOpThs = SVMB_SYSAUD_BEHAVE_OP_THS;
// r105 response knob (Parameters\BehaveResponse): 0 = telemetry only
// (DEFAULT - an alert logs and flags, nothing happens to the caller),
// 1 = suspend the alerting caller via the r91 suspension infrastructure
// (reversible: `cr3 resume all` releases it; guards + resume belt in
// cr3_monitor.Cr3RegionSuspendPidSysalert). The kill-vs-suspend policy
// decision stays with the user (K1/K3 pending); suspend is the safe
// side per the r96 K4 law.
u32 g_BehaveResponse = 0;

// r102: per-caller cross-process syscall rates - the behavior-analytics
// MVP (README gap). Lock-free by r99 law: CAS slot claims, per-window
// counters with CAS rollover, exact edge-triggered alerts. Approximate
// under 6-vCPU NPF concurrency (lost increments possible, never
// double-alerts); TELEMETRY ONLY - response stays a pending policy
// decision (K1/K3).
struct CallerSlot
{
    volatile LONG Pid;      // 0 = free; claimed via InterlockedCAS
    volatile LONG Alerted;  // alert raised in the current window
    volatile LONG64 Rd;     // lifetime attributed counts
    volatile LONG64 Wr;
    volatile LONG64 Op;
    volatile LONG64 CurrRd; // current-window counts
    volatile LONG64 CurrWr;
    volatile LONG64 CurrOp;
    volatile LONG64 Window; // window index Curr* belong to
    volatile LONG64 PeakRd; // lifetime max within-window rate
    volatile LONG64 PeakWr;
    char Image[16];
};
struct Behavior
{
    CallerSlot Slots[SVMB_SYSAUD_BEHAVE_SLOTS] = {};
    volatile LONG64 Alerts = 0;
    volatile LONG64 Dropped = 0; // unattributed (pid=0) entries
    volatile LONG64 TableFull = 0;
    volatile LONG64 ExcludedNtcf = 0; // r103: ambient NTCF, by design
    volatile LONG64 TargetsFull = 0;  // r104: target rows refused
    // r103 per-(caller,target) rows - filled only after the deferred
    // resolver converges on the pair (the EDR "who reads whom" view)
    struct TargetRow
    {
        volatile LONG CallerPid; // 0 = free
        volatile LONG TargetPid;
        volatile LONG64 Rd;
        volatile LONG64 Wr;
        volatile LONG64 Op; // r104: NtOpenProcess (workitem-resolved)
    };
    TargetRow T[SVMB_SYSAUD_TARGET_SLOTS] = {};
};
Behavior g_Behave = {};

// r101 v3: FIVE sense pages - the two exported targets (r100) plus the
// three SSDT-derived Nt* pages (small-copy blind spot).
struct SensePage
{
    u64 Gpa;    // armed 4K page (0 = none)
    u64 Rip;    // the watched function entry
    u32 FnId;   // SVMB_SYSAUD_FN_*
};
SensePage g_Sense[SENSE_COUNT] = {};
u32 g_FailStage = 0;    // 0=none 1=export/gpa 2=no views 3=ssdt identity
KDPC g_SenseDpc;
KTIMER g_SenseTimer;

SvmbApi g_Api = {}; // BY-VALUE copy: BuildApi hands Init a stack local

// ---- r103 deferred target resolution --------------------------------------
//
// The Nt* entries carry the TARGET as a caller-owned process HANDLE
// (rcx), and exit context can neither touch the caller's handle table
// nor fault on user memory - so the (caller, handle) pair is queued
// here and resolved at PASSIVE in a single-flight work item:
// PsLookupProcessByProcessId(caller) -> KeStackAttachProcess ->
// ObReferenceObjectByHandle(*PsProcessType) -> PsGetProcessId.
// A resolved cache (single PASSIVE writer, lock-free exit-context
// readers - benign approximation) stamps later entries' DstProcess and
// feeds the per-(caller,target) counters. Negative results back off
// (5s) and die after 3 tries - a probe that closes notepad must not
// retry forever.

constexpr u32 RES_PENDING_SLOTS = 64; // power of two
constexpr u32 RES_CACHE_SLOTS = 32;   // power of two
constexpr u32 RES_MAX_TRIES = 3;
constexpr LONG64 RES_BACKOFF_TICKS = 5 * 10000000ll; // 5s of interrupt time

constexpr LONG RES_KIND_HANDLE = 0; // NTRD/NTWR: rcx is a process handle
constexpr LONG RES_KIND_CID = 1;    // NTOP: r9 is a user CLIENT_ID*
constexpr LONG RES_KIND_SUSPEND = 2; // r105: suspend the alerting caller

struct ResPending
{
    volatile LONG CallerPid;
    volatile LONG Kind;     // RES_KIND_*
    volatile LONG Commit;   // seq+1 written LAST (r100 P1-1 law)
    volatile LONG64 Addr;   // handle value / CLIENT_ID pointer
};
struct ResCacheEntry
{
    volatile LONG CallerPid;   // 0 = free
    volatile LONG HandleLo;    // user handles are 32-bit significant
    volatile LONG TargetPid;   // valid when State == RESOLVED
    volatile LONG State;       // 0 empty 1 resolved 2 failed-backoff 3 dead
    volatile LONG Tries;
    volatile LONG64 Tick;      // last attempt (backoff reference)
};
struct Responder
{
    ResPending P[RES_PENDING_SLOTS] = {};
    volatile LONG W = 0;    // producer claims (exit ctx)
    volatile LONG R = 0;    // resolver consumed
    ResCacheEntry C[RES_CACHE_SLOTS] = {};
    volatile LONG64 Resolved = 0;
    volatile LONG64 ResFail = 0;
    volatile LONG64 ResDropped = 0;
    void* WorkItem = nullptr;    // IoAllocateWorkItem at res-init
    volatile LONG Queued = 0;    // single-flight guard
};
Responder g_Res = {};

// cache row probe: find (pid, handle) or claim a free row. SINGLE
// WRITER (the PASSIVE work item - single-flight), so plain writes are
// safe; exit context only ever READS fields where tearing degrades to
// a stale stamp (telemetry approximation)
void BehaveTargetCount(u32 pid, u32 targetPid, u32 fnId);

ResCacheEntry* ResCacheClaim(u32 pid, u32 handleLo)
{
    const u32 start = SysAuditTargetSlotFor(pid, handleLo);
    for (u32 k = 0; k < RES_CACHE_SLOTS; ++k)
    {
        ResCacheEntry* c = &g_Res.C[(start + k) & (RES_CACHE_SLOTS - 1)];
        const LONG p = c->CallerPid;
        if (p == (LONG)pid && c->HandleLo == (LONG)handleLo)
            return c;
        if (!p)
        {
            c->CallerPid = (LONG)pid;
            c->HandleLo = (LONG)handleLo;
            return c;
        }
    }
    // r103 acceptance P2-2: no free row - recycle the first DEAD row
    // (State==3, terminal negative verdict) in the probe chain. Stale
    // pairs must not make the cache permanently full; recycled rows
    // re-resolve from scratch. Single-writer (PASSIVE workitem), so
    // plain writes are safe.
    for (u32 k = 0; k < RES_CACHE_SLOTS; ++k)
    {
        ResCacheEntry* c = &g_Res.C[(start + k) & (RES_CACHE_SLOTS - 1)];
        if (c->State == 3)
        {
            c->CallerPid = (LONG)pid;
            c->HandleLo = (LONG)handleLo;
            c->TargetPid = 0;
            c->State = 0;
            c->Tries = 0;
            c->Tick = 0;
            return c;
        }
    }
    return nullptr; // 32 live pairs and no dead rows: this pair never converges
}

// exit-context read of the cache (u32 fields; stale reads are fine)
u32 ResCacheLookup(u32 pid, u32 handleLo)
{
    const u32 start = SysAuditTargetSlotFor(pid, handleLo);
    for (u32 k = 0; k < RES_CACHE_SLOTS; ++k)
    {
        const ResCacheEntry* c =
            &g_Res.C[(start + k) & (RES_CACHE_SLOTS - 1)];
        if (c->CallerPid == (LONG)pid && c->HandleLo == (LONG)handleLo)
            return (c->State == 1) ? (u32)c->TargetPid : 0;
        if (!c->CallerPid)
            break; // empty row in the probe chain: not cached
    }
    return 0;
}

// producers (exit ctx): claim a sequence, write the pair, commit LAST.
// Overflow claims write a tombstone (pid=0 + commit) so the resolver's
// strict order never wedges on a Commit hole (r100 P1-1 law).
void ResEnqueue(u32 pid, u64 addr, LONG kind)
{
    const LONG seq = InterlockedIncrement(&g_Res.W) - 1;
    const LONG r = g_Res.R;
    ResPending* p = &g_Res.P[(ULONG)seq & (RES_PENDING_SLOTS - 1)];
    if ((ULONG)(seq - r) < RES_PENDING_SLOTS)
    {
        p->CallerPid = (LONG)pid;
        p->Kind = kind;
        p->Addr = (LONG64)addr;
        p->Commit = seq + 1;
    }
    else
    {
        p->CallerPid = 0; // tombstone
        p->Kind = 0;
        p->Addr = 0;
        p->Commit = seq + 1;
        InterlockedIncrement64(&g_Res.ResDropped);
    }
}

// r105: CID negative memo - a bogus/unreadable CLIENT_ID pointer would
// otherwise retry (and trip the COPY page) on every NTOP entry. Same
// backoff/dead semantics as the handle cache; single-writer (workitem).
struct ResCidFail
{
    volatile LONG CallerPid;
    volatile LONG PtrLo;
    volatile LONG Tries;
    volatile LONG64 Tick;
};
ResCidFail g_ResCidFail[8] = {};

// returns false when this (pid, ptr) pair is backed-off or dead
bool ResCidFailBlocked(u32 pid, u32 ptrLo, u64 now)
{
    for (u32 i = 0; i < 8; ++i)
    {
        const ResCidFail* f = &g_ResCidFail[i];
        if (f->CallerPid == (LONG)pid && f->PtrLo == (LONG)ptrLo)
        {
            if (f->Tries >= RES_MAX_TRIES)
                return true;
            if ((LONG64)now - f->Tick < RES_BACKOFF_TICKS)
                return true;
        }
    }
    return false;
}

void ResCidFailRecord(u32 pid, u32 ptrLo, u64 now)
{
    ResCidFail* slot = &g_ResCidFail[0];
    for (u32 i = 0; i < 8; ++i)
    {
        ResCidFail* f = &g_ResCidFail[i];
        if (f->CallerPid == (LONG)pid && f->PtrLo == (LONG)ptrLo)
        {
            slot = f;
            break;
        }
        if (!f->CallerPid)
            slot = f;
    }
    slot->CallerPid = (LONG)pid;
    slot->PtrLo = (LONG)ptrLo;
    slot->Tick = (LONG64)now;
    if (slot->Tries == 0)
        slot->Tries = 1;
    else if (slot->CallerPid != (LONG)pid || slot->PtrLo != (LONG)ptrLo)
        slot->Tries = 1; // r105 acceptance P2: a taken-over row starts
                         // its own budget (stale Tries would kill the
                         // new pair after one failure)
}

// PASSIVE: resolve one pending entry.
//   HANDLE kind: (caller, handle) through the negative cache - handles
//   are semantically stable, so the cache key is sound.
//   CID kind: read CLIENT_ID.UniqueProcess (arg3 pointer's first qword)
//   out of the caller's user space with MmCopyVirtualMemory - the OS
//   primitive handles arbitrary/paged-out pointers, which raw __try
//   touches cannot (the r89 MiUserFault law). NO cache: a CLIENT_ID
//   pointer is just a stack buffer address, reused for different
//   targets, so pointer-keyed caching would be semantically wrong; the
//   resolution lands directly in the per-target counters. Side effect,
//   documented: MmCopyVirtualMemory's own entry trips the COPY sense
//   page once per call - a pid=0 COPY entry in the ring (the resolver
//   runs in System context), honest self-telemetry.
void ResResolvePair(u32 pid, LONG kind, u64 addr, u64 now)
{
    if (kind == RES_KIND_CID)
    {
        if (ResCidFailBlocked(pid, (u32)(ULONG_PTR)addr, now))
            return;
        void* caller = nullptr;
        if (!NT_SUCCESS(PsLookupProcessByProcessId(
                (HANDLE)(ULONG_PTR)pid, (PEPROCESS*)&caller))
            || !caller)
        {
            InterlockedIncrement64(&g_Res.ResFail);
            return;
        }
        u64 cidPid = 0;
        SIZE_T copied = 0;
        const NTSTATUS st = MmCopyVirtualMemory(
            (PEPROCESS)caller, (void*)(ULONG_PTR)addr,
            (PEPROCESS)PsGetCurrentProcess(), &cidPid, sizeof(cidPid),
            KernelMode, &copied);
        ObDereferenceObject(caller);
        if (NT_SUCCESS(st) && copied == sizeof(cidPid) && cidPid
            && cidPid <= 0xFFFFFFFEull)
        {
            BehaveTargetCount(pid, (u32)cidPid, SVMB_SYSAUD_FN_NTOP);
            InterlockedIncrement64(&g_Res.Resolved);
        }
        else
        {
            ResCidFailRecord(pid, (u32)(ULONG_PTR)addr, now);
            InterlockedIncrement64(&g_Res.ResFail);
        }
        return;
    }

    ResCacheEntry* c = ResCacheClaim(pid, (u32)(ULONG_PTR)addr);
    if (!c)
        return;
    if (c->State == 1 || c->State == 3)
        return; // converged (either way)
    if (c->State == 2 && (LONG64)now - c->Tick < RES_BACKOFF_TICKS)
        return; // still backing off
    if (c->Tries >= RES_MAX_TRIES)
    {
        c->State = 3;
        return;
    }
    InterlockedIncrement(&c->Tries);
    c->Tick = (LONG64)now;

    void* caller = nullptr;
    if (!NT_SUCCESS(PsLookupProcessByProcessId(
            (HANDLE)(ULONG_PTR)pid, (PEPROCESS*)&caller))
        || !caller)
    {
        InterlockedIncrement64(&g_Res.ResFail);
        c->State = 2; // caller gone: backoff (the pid may be recycled)
        return;
    }
    void* target = nullptr;
    UCHAR apc[64]; // >= KAPC_STATE (0x30), opaque - mem_read.cpp pattern
    NTSTATUS st;
    KeStackAttachProcess(caller, apc);
    st = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)addr, 0,
                                   *PsProcessType, KernelMode, &target,
                                   nullptr);
    KeUnstackDetachProcess(apc);
    ObDereferenceObject(caller);
    if (NT_SUCCESS(st) && target)
    {
        const HANDLE tid = PsGetProcessId((PEPROCESS)target);
        ObDereferenceObject(target);
        c->TargetPid = (LONG)(ULONG_PTR)tid;
        c->State = 1;
        InterlockedIncrement64(&g_Res.Resolved);
    }
    else
    {
        InterlockedIncrement64(&g_Res.ResFail);
        c->State = 2; // stale/closing handle: backoff, bounded by tries
    }
}

VOID ResWorkItem(PDEVICE_OBJECT, PVOID)
{
    CrumbPost(CRUMB_RESOLVER_ENTER);
    const u64 now = KeQueryInterruptTime();
    u32 n = 0;
    while (g_Res.R != g_Res.W && n < 16)
    {
        const LONG seq = g_Res.R;
        ResPending* p = &g_Res.P[(ULONG)seq & (RES_PENDING_SLOTS - 1)];
        if (p->Commit != (LONG)(seq + 1))
        {
            if ((ULONG)p->Commit < (ULONG)(seq + 1))
                break; // producer mid-write; the next pass takes it
            // r103 acceptance P1-1: the head was LAPPED - the slot
            // already carries a LATER entry's commit, so this
            // sequence's pair is gone. Touch NOTHING in the slot (the
            // later entry still owns it) and advance instead of
            // breaking - a break here let the head self-requeue
            // forever on a fully-lapped ring.
            InterlockedIncrement64(&g_Res.ResDropped);
            InterlockedIncrement(&g_Res.R);
            ++n;
            continue;
        }
        if (p->CallerPid)
        {
            if (p->Kind == RES_KIND_SUSPEND)
            {
                // r105: the alert response executes here - PASSIVE,
                // single-flight, reversible (r91 table + resume-all)
                if (!Cr3RegionSuspendPidSysalert((u32)p->CallerPid,
                                                 "sysaudit-alert"))
                    InterlockedIncrement64(&g_Res.ResFail);
            }
            else
                ResResolvePair((u32)p->CallerPid, p->Kind, (u64)p->Addr,
                               now);
        }
        p->Commit = 0;
        InterlockedIncrement(&g_Res.R);
        ++n;
    }
    // single-flight: clear the guard, THEN re-check - a producer that
    // raced in during the drain requeues (the item perpetuates only
    // while pending work exists). CriticalWorkQueue via IoQueueWorkItem
    // runs the callback at PASSIVE (IO_WORKITEM_ROUTINE contract) -
    // queue-type naming notwithstanding.
    InterlockedExchange(&g_Res.Queued, 0);
    if (g_Res.WorkItem && g_Res.R != g_Res.W
        && !InterlockedExchange(&g_Res.Queued, 1))
        IoQueueWorkItem((PIO_WORKITEM)g_Res.WorkItem, ResWorkItem,
                        CriticalWorkQueue, nullptr);
}

// called from the sense re-arm DPC (250ms cadence while armed) - the
// only kick the resolver needs
void ResKick()
{
    if (g_Res.WorkItem && g_Res.R != g_Res.W
        && !InterlockedExchange(&g_Res.Queued, 1))
        IoQueueWorkItem((PIO_WORKITEM)g_Res.WorkItem, ResWorkItem,
                        CriticalWorkQueue, nullptr);
}

// ---- the consumer ---------------------------------------------------------

void ResEnqueue(u32 pid, u64 addr, LONG kind);

// find this pid's slot or claim a free one. CAS-convergent: both CPUs
// hashing the same pid walk the identical probe order, and the loser
// observes prev == pid and reuses the winner's slot - two slots for one
// pid is impossible (slots are never freed, so no ABA either)
CallerSlot* BehaveSlotFor(u32 pid, const char* image)
{
    const u32 start = SysAuditBehaveSlotFor(pid);
    for (u32 k = 0; k < SVMB_SYSAUD_BEHAVE_SLOTS; ++k)
    {
        CallerSlot* s =
            &g_Behave.Slots[(start + k) & (SVMB_SYSAUD_BEHAVE_SLOTS - 1)];
        const LONG prev = InterlockedCompareExchange(&s->Pid, (LONG)pid, 0);
        if (prev == 0)
        {
            // claim won: stamp the image once
            RtlCopyMemory((void*)s->Image, image, sizeof(s->Image));
            s->Image[sizeof(s->Image) - 1] = 0;
            return s;
        }
        if (prev == (LONG)pid)
            return s;
    }
    return nullptr;
}

// roll the per-caller window forward (CAS: one CPU zeros, the rest race
// benignly - an increment lost in the seam is acceptable and documented)
void BehaveRoll(CallerSlot* s, u64 w)
{
    const LONG64 old = s->Window;
    if (old == (LONG64)w)
        return;
    if (InterlockedCompareExchange64(&s->Window, (LONG64)w, old) == old)
    {
        InterlockedExchange64(&s->CurrRd, 0);
        InterlockedExchange64(&s->CurrWr, 0);
        InterlockedExchange64(&s->CurrOp, 0);
        InterlockedExchange(&s->Alerted, 0);
    }
}

// r105: the single alert grant (exactly one caller per edge, guarded by
// the Alerted exchange at each site). Telemetry always; the response
// (suspend the alerting caller) fires only with BehaveResponse=1 and
// only for guarded callers - the suspend request rides the resolver's
// pending ring as RES_KIND_SUSPEND and executes at PASSIVE via the r91
// suspension infrastructure (reversible: `cr3 resume all`).
void BehaveAlertFired(u32 pid, const char* image, const char* what,
                      LONG64 n, u32 ths)
{
    InterlockedIncrement64(&g_Behave.Alerts);
    g_Api.LogError("sysaudit-behave ALERT: pid=%lu (%s) %s burst "
                   "%lld/window >= %u",
                   (unsigned long)pid, image ? image : "", what,
                   (long long)n, (unsigned)ths);
    if (g_BehaveResponse == SVMB_SYSAUD_BEHAVE_RESP_SUSPEND && pid > 4
        && !Cr3RegionKillDenylisted(image))
        ResEnqueue(pid, 0, RES_KIND_SUSPEND);
}

// one attributed cross-process entry into the analytics table
void BehaveCount(u32 pid, const char* image, u32 fnId, u64 tick)
{
    if (!pid)
    {
        InterlockedIncrement64(&g_Behave.Dropped);
        return;
    }
    CallerSlot* s = BehaveSlotFor(pid, image);
    if (!s)
    {
        InterlockedIncrement64(&g_Behave.TableFull);
        return;
    }
    BehaveRoll(s, SysAuditBehaveWindowOf(tick));
    if (fnId == SVMB_SYSAUD_FN_NTRD)
    {
        InterlockedIncrement64(&s->Rd);
        const LONG64 n = InterlockedIncrement64(&s->CurrRd);
        // check-then-exchange can regress the peak under races (telemetry
        // approximation, same class as the counters)
        if (n > s->PeakRd)
            InterlockedExchange64(&s->PeakRd, n);
        // exact edge: exactly one incrementer observes the threshold
        // value, and the Alerted exchange guarantees a single alert
        // per window even across rollovers
        if (SysAuditBehaveEdge((u64)n, g_BehaveRdThs)
            && !InterlockedExchange(&s->Alerted, 1))
            BehaveAlertFired(pid, s->Image, "RD", n, g_BehaveRdThs);
    }
    else if (fnId == SVMB_SYSAUD_FN_NTWR)
    {
        InterlockedIncrement64(&s->Wr);
        const LONG64 n = InterlockedIncrement64(&s->CurrWr);
        if (n > s->PeakWr) // approximate, see the Rd note
            InterlockedExchange64(&s->PeakWr, n);
        if (SysAuditBehaveEdge((u64)n, g_BehaveWrThs)
            && !InterlockedExchange(&s->Alerted, 1))
            BehaveAlertFired(pid, s->Image, "WR", n, g_BehaveWrThs);
    }
    else if (fnId == SVMB_SYSAUD_FN_NTOP)
    {
        InterlockedIncrement64(&s->Op);
        const LONG64 n = InterlockedIncrement64(&s->CurrOp);
        if (SysAuditBehaveEdge((u64)n, g_BehaveOpThs)
            && !InterlockedExchange(&s->Alerted, 1))
            BehaveAlertFired(pid, s->Image, "open-process", n,
                             g_BehaveOpThs);
    }
}

// r103 per-(caller,target) lifetime counters (exit ctx, CAS claim like
// the caller table). Unresolved targets are not counted here - they
// join after the deferred resolver converges on the pair. Race note
// (r103 acceptance P3): the CAS winner stamps TargetPid after the
// claim, so a concurrent same-pair CPU can see TargetPid=0, mismatch,
// and claim a DUPLICATE row for the same pair - the TargetPid re-check
// prevents misfiling into ANOTHER pair's row; a split/inflated count
// is accepted telemetry approximation.
void BehaveTargetCount(u32 pid, u32 targetPid, u32 fnId)
{
    const u32 start = SysAuditTargetSlotFor(pid, targetPid);
    for (u32 k = 0; k < SVMB_SYSAUD_TARGET_SLOTS; ++k)
    {
        Behavior::TargetRow* t =
            &g_Behave.T[(start + k) & (SVMB_SYSAUD_TARGET_SLOTS - 1)];
        LONG prev = InterlockedCompareExchange(
            &t->CallerPid, (LONG)pid, 0);
        if (prev && prev != (LONG)pid)
            continue; // probed row belongs to another pair
        if (!prev)
            InterlockedExchange(&t->TargetPid, (LONG)targetPid);
        else if ((u32)t->TargetPid != targetPid)
            continue;
        if (fnId == SVMB_SYSAUD_FN_NTRD)
            InterlockedIncrement64(&t->Rd);
        else if (fnId == SVMB_SYSAUD_FN_NTWR)
            InterlockedIncrement64(&t->Wr);
        else if (fnId == SVMB_SYSAUD_FN_NTOP)
            InterlockedIncrement64(&t->Op);
        return;
    }
    // table full (r104: counted, was silent): target rows are
    // best-effort telemetry, no tombstone needed
    InterlockedIncrement64(&g_Behave.TargetsFull);
}

// the consumer runs in NPF-exit context at the guest's IRQL - the call
// args are still in the registers (entry-fetch fault, no instruction ran)
bool SenseConsume(u64 gpa4k, u64 info1, u64 cr3, u64 rip, GuestContext& g)
{
    if (!(info1 & NPF_EXECUTE))
        return false; // data faults on an open/deny page are not ours
    CrumbPost(CRUMB_CONSUMER_ENTER);

    // r101: match by the watched ENTRY RIP, not the page - two targets
    // can share one 4K page (MmCopyVirtualMemory +0x622cb0 and
    // NtReadVirtualMemory +0x622a80 both live in nt page 0x622000). The
    // first-draft GPA match classified every NtReadVM fetch as COPY-page
    // noise: the probe's 220 reads all vanished into Noise (live-observed
    // noise=42, target ntrd=0).
    i32 hit = -1;
    for (u32 i = 0; i < SENSE_COUNT; ++i)
        if (g_Sense[i].Gpa && rip == g_Sense[i].Rip)
        {
            hit = (i32)i;
            break;
        }
    if (hit < 0)
    {
        bool ours = false;
        for (u32 i = 0; i < SENSE_COUNT; ++i)
            if (g_Sense[i].Gpa && gpa4k == g_Sense[i].Gpa)
            {
                ours = true;
                break;
            }
        if (!ours)
            return false; // not an armed page
        // a non-target function on an armed page - counted, not recorded
        InterlockedIncrement64(&g_Ring.Noise);
        CrumbPost(CRUMB_CONSUMER_NOISE);
    }
    else
    {
        InterlockedIncrement64(&g_Ring.Total);
        const u32 fnId = g_Sense[hit].FnId;
        SVMB_SYSAUD_ENTRY e = {};
        e.Tick = KeQueryInterruptTime();
        e.FnId = fnId;
        if (fnId == SVMB_SYSAUD_FN_COPY)
        {
            e.SrcProcess = g.Regs->Rcx; // MmCopyVirtualMemory(SourceProcess,
            e.SrcAddress = g.Regs->Rdx; //  SourceAddress, TargetProcess,
            e.DstProcess = g.Regs->R8;  //  TargetAddress, BufferSize, ...)
            e.DstAddress = g.Regs->R9;
            e.Size = 0; // stack args not captured at fault time (v2 scope)
        }
        else
        {
            // syscall entry (19H1 dispatch, dump-verified): KiSystemCall64
            // restores rcx = arg0 (the user's `mov r10, rcx` is undone at
            // KiSystemCall64+0x24) and rdx/r8/r9 carry arg1..3. r10 is
            // DISPATCH SCRATCH at this point (the service address, live
            // saves show it >>16-shifted) - not an argument, not recorded.
            //   NtRd/NtWr (ProcessHandle rcx, BaseAddress rdx, Buffer r8,
            //          *ByteCount r9)
            //   NtCF/NtOp (PHANDLE* rcx, DesiredAccess rdx, OBJATTR* r8,
            //          IOSTATUS/CLIENT_ID* r9)
            e.SrcProcess = g.Regs->Rcx;
            e.SrcAddress = g.Regs->Rdx;
            e.DstProcess = 0; // the caller's own process (pid/image column)
            e.DstAddress = g.Regs->R8;
            e.Size = g.Regs->R9;
        }
        if (Cr3SeedInstance())
            Cr3SeedInstance()->LookupPidByCr3(cr3, e.Pid, e.Image);
        CrumbPost(CRUMB_CONSUMER_HIT);

        // r102 behavioral analytics: attributed cross-process entries
        // only (NtCF is ambient file I/O, excluded by design - r103
        // counts the exclusion instead of swallowing it)
        if (fnId == SVMB_SYSAUD_FN_NTCF)
        {
            InterlockedIncrement64(&g_Behave.ExcludedNtcf);
        }
        else if (fnId == SVMB_SYSAUD_FN_NTRD ||
                 fnId == SVMB_SYSAUD_FN_NTWR ||
                 fnId == SVMB_SYSAUD_FN_NTOP)
        {
            BehaveCount(e.Pid, e.Image, fnId, e.Tick);
            // r103: defer the target-handle resolution to PASSIVE, and
            // stamp what the cache already knows (NTRD/NTWR only -
            // NtOpenProcess's rcx is an out-PHANDLE, its target hides
            // behind the user-mode CLIENT_ID pointer, unreadable from
            // exit context without faulting). pid=0 entries have no
            // caller process to attach - the r103 acceptance P2-1 guard
            // keeps them out of the resolver entirely (they are already
            // counted in Dropped by BehaveCount).
            if (e.Pid && (fnId == SVMB_SYSAUD_FN_NTRD ||
                          fnId == SVMB_SYSAUD_FN_NTWR))
            {
                const u32 tpid = ResCacheLookup(e.Pid, (u32)g.Regs->Rcx);
                e.DstProcess = tpid;
                ResEnqueue(e.Pid, g.Regs->Rcx, RES_KIND_HANDLE);
                if (tpid)
                    BehaveTargetCount(e.Pid, tpid, fnId);
            }
            else if (e.Pid && fnId == SVMB_SYSAUD_FN_NTOP)
            {
                // r104: the target hides in the user-mode CLIENT_ID
                // (arg3, r9) - resolved in the workitem via
                // MmCopyVirtualMemory, counted straight into the
                // per-target table (no per-entry stamp: the ring entry
                // is long gone by convergence). ResKick immediately:
                // attackers are short-lived processes - waiting for the
                // 250ms DPC heartbeat loses the race to process exit
                // (first E2E: the probe's powershell was gone before
                // the heartbeat, PsLookupProcessByProcessId failed,
                // resfail=1 op=0)
                ResEnqueue(e.Pid, g.Regs->R9, RES_KIND_CID);
                ResKick();
            }
        }

        // r100 acceptance P3-2: mask, not modulo (signed wrap safety)
        const LONG seq = InterlockedIncrement(&g_Ring.W) - 1;
        const LONG d = g_Ring.D;
        if ((ULONG)(seq - d) < RING_SLOTS)
        {
            SVMB_SYSAUD_ENTRY* s = &g_Ring.Slots[SysAuditSlotIndex((ULONG)seq)];
            s->Commit = 0; // invalidate first (reused slot)
            s->Tick = e.Tick;
            s->SrcProcess = e.SrcProcess;
            s->SrcAddress = e.SrcAddress;
            s->DstProcess = e.DstProcess;
            s->DstAddress = e.DstAddress;
            s->Size = e.Size;
            s->Pid = e.Pid;
            s->FnId = e.FnId;
            RtlCopyMemory(s->Image, e.Image, sizeof(s->Image));
            // commit LAST (x86 TSO store order = release): the drain only
            // accepts a slot whose Commit equals its sequence
            s->Commit = (u64)seq + 1;
        }
        else
        {
            // r100 acceptance P1-1: an overflow claim still consumes its
            // sequence - write a tombstone so the drain's strict order
            // never wedges on a Commit hole
            SVMB_SYSAUD_ENTRY* s = &g_Ring.Slots[SysAuditSlotIndex((ULONG)seq)];
            s->Commit = 0;
            s->Tick = e.Tick;
            s->SrcProcess = 0;
            s->SrcAddress = 0;
            s->DstProcess = 0;
            s->DstAddress = 0;
            s->Size = 0;
            s->Pid = 0;
            s->FnId = SVMB_SYSAUD_FN_LOST;
            RtlZeroMemory(s->Image, sizeof(s->Image));
            s->Commit = (u64)seq + 1;
            // r101 acceptance P2-1: make the drop measurable - the
            // tombstone keeps the drain alive but is invisible in the
            // entries, so the Lost counter is the only trace
            InterlockedIncrement64(&g_Ring.Lost);
        }
    }

    // resolve: open the page; the faulting fetch re-executes natively.
    // r100 acceptance P2-2: SetPerm4kNoLock - exit-path legal at any IRQL
    // (r63: spurious denies arrive at CLOCK_LEVEL); the page is 4K-split
    // at arm time, so this is a single aligned store. No flush needed on
    // resolve (r56: failed walks are not cached).
    NptManager* npt = NptInstance();
    NptView* views[2] = {};
    const u32 nv = npt ? Cr3MonitorArmViews(views) : 0;
    for (u32 v = 0; v < nv; ++v)
        (void)views[v]->SetPerm4kNoLock(gpa4k, {true, true, true});
    CrumbPost(CRUMB_CONSUMER_RESOLVED);

    // re-arm: re-deny after the deny-rest window (sentinel r68 pattern).
    // KeSetTimer is <=DISPATCH legal; this consumer only ever runs for
    // entry-fetch faults of the watched functions (<=APC_LEVEL callers).
    LARGE_INTEGER due;
    due.QuadPart = -(LONG64)SENSE_REARM_MS * 10000;
    KeSetTimer(&g_SenseTimer, due, &g_SenseDpc);
    CrumbPost(CRUMB_CONSUMER_DONE);
    return true;
}

VOID SenseRedenyDpc(KDPC*, PVOID, PVOID, PVOID)
{
    CrumbPost(CRUMB_DPC_ENTER);
    NptManager* npt = NptInstance();
    NptView* views[2] = {};
    const u32 nv = npt ? Cr3MonitorArmViews(views) : 0;
    for (u32 v = 0; v < nv; ++v)
        for (u32 i = 0; i < SENSE_COUNT; ++i)
            if (g_Sense[i].Gpa)
                (void)views[v]->SetPerm4k(g_Sense[i].Gpa,
                                          {true, true, false});
    CrumbPost(CRUMB_DPC_UNDENIED);
    // r100 acceptance P2-1 (r56 law): the re-deny is inert until every
    // core's NPT TLB drops the cached translation - hammer visibility
    if (nv)
    {
        CrumbPost(CRUMB_DPC_PREKICK);
        TlbKickFlushAllCores();
        CrumbPost(CRUMB_DPC_POSTKICK);
    }
    // r103: the re-arm cadence doubles as the resolver's heartbeat
    ResKick();
}

} // namespace

NTSTATUS SysAuditInit(const SvmbApi* api)
{
    g_Api = *api;
    g_Armed = false;
    g_FailStage = 0;
    RtlZeroMemory(&g_Ring, sizeof(g_Ring));
    RtlZeroMemory(&g_Behave, sizeof(g_Behave));
    RtlZeroMemory(&g_Sense, sizeof(g_Sense));

    UNICODE_STRING nmCopy;
    RtlInitUnicodeString(&nmCopy, L"MmCopyVirtualMemory");
    UNICODE_STRING nmFile;
    RtlInitUnicodeString(&nmFile, L"NtCreateFile");
    g_Sense[0].Rip = (u64)MmGetSystemRoutineAddress(&nmCopy);
    g_Sense[1].Rip = (u64)MmGetSystemRoutineAddress(&nmFile);
    g_Sense[0].FnId = SVMB_SYSAUD_FN_COPY;
    g_Sense[1].FnId = SVMB_SYSAUD_FN_NTCF;
    if (!g_Sense[0].Rip || !g_Sense[1].Rip)
    {
        g_Api.LogError("sysaudit: target exports missing (%llx %llx)",
                       g_Sense[0].Rip, g_Sense[1].Rip);
        g_FailStage = 1;
        return STATUS_NOT_SUPPORTED;
    }

    // r101: derive the SSDT targets. nt base comes from the exported
    // NtCreateFile anchor (RVA 0x630540 in the pinned image); the runtime
    // table entries are PACKED table-relative (service = table +
    // (s32(e) >> 4), kdump-verified) and must resolve ALL FOUR SSNs to
    // exactly nt_base + the pinned RVAs, or the running image is not
    // 18362.592 -> refuse to arm rather than X-deny the wrong pages (a
    // wrongly denied page is a freeze generator, not a log line).
    const u64 ntBase = g_Sense[1].Rip - SSDT_NTCF_RVA;
    const u64 tblVa = ntBase + SSDT_TABLE_RVA;
    u32 e = 0;
    bool ssdtOk = !(ntBase & 0xFFF) &&
                  SafRead32(tblVa + 4ull * SSDT_NTCF_SSN, &e) &&
                  SysAuditServiceVa(tblVa, e) == g_Sense[1].Rip;
    for (u32 i = 0; ssdtOk && i < 3; ++i)
    {
        if (!SafRead32(tblVa + 4ull * SSDT_ANCHORS[i].Ssn, &e) ||
            SysAuditServiceVa(tblVa, e) != ntBase + SSDT_ANCHORS[i].Rva)
        {
            ssdtOk = false;
            break;
        }
        g_Sense[2 + i].Rip = ntBase + SSDT_ANCHORS[i].Rva;
        g_Sense[2 + i].FnId = SSDT_ANCHORS[i].FnId;
    }
    if (!ssdtOk)
    {
        u32 eNt = 0;
        (void)SafRead32(tblVa + 4ull * SSDT_NTCF_SSN, &eNt);
        g_Api.LogError("sysaudit: ssdt identity mismatch nt=%llx tbl=%llx "
                       "e55=%x (unpack %llx, want %llx)",
                       ntBase, tblVa, eNt,
                       SysAuditServiceVa(tblVa, eNt), g_Sense[1].Rip);
        g_FailStage = 3;
        return STATUS_NOT_SUPPORTED;
    }

    KeInitializeTimer(&g_SenseTimer);
    KeInitializeDpc(&g_SenseDpc, SenseRedenyDpc, nullptr);

    NptManager* npt = NptInstance();
    NptView* views[2] = {};
    const u32 nv = npt ? Cr3MonitorArmViews(views) : 0;
    if (!nv)
    {
        g_Api.LogError("sysaudit: no arm views (NPT off)");
        g_FailStage = 2;
        return STATUS_DEVICE_NOT_READY;
    }

    // r100 acceptance P3-1: resolve ALL GPAs before arming any - no
    // partial arm with a denied page and no consumer
    for (u32 i = 0; i < SENSE_COUNT; ++i)
    {
        const u64 pageVa = g_Sense[i].Rip & ~0xFFFull;
        PHYSICAL_ADDRESS pa = MmGetPhysicalAddress((void*)pageVa);
        g_Sense[i].Gpa = pa.QuadPart & ~0xFFFull;
        if (!g_Sense[i].Gpa)
        {
            g_Api.LogError("sysaudit: target %u page gpa=0 (rip %llx)", i,
                           g_Sense[i].Rip);
            g_FailStage = 1;
            return STATUS_NOT_SUPPORTED;
        }
    }
    for (u32 i = 0; i < SENSE_COUNT; ++i)
        for (u32 v = 0; v < nv; ++v)
        {
            (void)views[v]->MapRange(g_Sense[i].Gpa, 0x1000,
                                     {true, true, true});
            (void)views[v]->SetPerm4k(g_Sense[i].Gpa, {true, true, false});
        }
    // r56 law: the deny flips are inert until the TLBs drop the cached
    // translations - hammer visibility before the consumer goes live
    TlbKickFlushAllCores();

    Cr3MonitorSetSenseCb(SenseConsume);
    g_Armed = true;
    g_Api.LogInfo("sysaudit: armed copy=%llx file=%llx ntop=%llx ntrd=%llx "
                  "ntwr=%llx views=%u",
                  g_Sense[0].Rip, g_Sense[1].Rip, g_Sense[2].Rip,
                  g_Sense[3].Rip, g_Sense[4].Rip, nv);
    return STATUS_SUCCESS;
}

void SysAuditStop()
{
    Cr3MonitorSetSenseCb(nullptr);
    // r100 acceptance P3-4: capture the pages and disarm the consumer+
    // state BEFORE cancelling the timer - a DPC already dequeued must not
    // re-deny after the reopen
    u64 pages[SENSE_COUNT] = {};
    for (u32 i = 0; i < SENSE_COUNT; ++i)
    {
        pages[i] = g_Sense[i].Gpa;
        g_Sense[i].Gpa = 0;
    }
    KeCancelTimer(&g_SenseTimer);
    // leave the pages fully open
    NptManager* npt = NptInstance();
    NptView* views[2] = {};
    const u32 nv = npt ? Cr3MonitorArmViews(views) : 0;
    for (u32 v = 0; v < nv; ++v)
        for (u32 i = 0; i < SENSE_COUNT; ++i)
            if (pages[i])
                (void)views[v]->SetPerm4k(pages[i], {true, true, true});
    g_Armed = false;
    g_Api.LogInfo("sysaudit: detached, total=%llu lost=%llu noise=%llu",
                  (unsigned long long)g_Ring.Total,
                  (unsigned long long)g_Ring.Lost,
                  (unsigned long long)g_Ring.Noise);
}

void SysAuditDrain(SVMB_SYSCALL_AUDIT* out)
{
    if (!out)
        return;
    u32 n = 0;
    while (n < SVMB_SYSAUD_MAX_ENTRIES)
    {
        const LONG d = g_Ring.D;
        const LONG w = g_Ring.W;
        if (d >= w)
            break;
        SVMB_SYSAUD_ENTRY* s = &g_Ring.Slots[SysAuditSlotIndex((ULONG)d)];
        if (!SysAuditSlotReady(s->Commit, d))
        {
            if ((u64)s->Commit > (u64)d + 1)
            {
                // r103 P1-1 class (pre-existing latent hole since r100):
                // the head was LAPPED - the slot carries a LATER entry's
                // commit, so the sequences d..owner-1 were overwritten
                // while undrained. Skip to the owner, account the loss,
                // keep draining - a plain break here wedged the drain
                // forever on a fully-lapped ring.
                const LONG owner = (LONG)s->Commit - 1;
                InterlockedAdd64(&g_Ring.Lost,
                                 (LONG64)(owner - (LONG)g_Ring.D));
                InterlockedExchange(&g_Ring.D, owner);
                continue;
            }
            break; // not committed yet (or mid-write) - done for now
        }
        if (s->FnId != SVMB_SYSAUD_FN_LOST)
        {
            // FN_LOST tombstones satisfy the sequence without emitting
            if (n < SVMB_SYSAUD_MAX_ENTRIES)
                out->Entries[n++] = *s;
        }
        s->Commit = 0; // consumed
        InterlockedIncrement(&g_Ring.D);
    }
    out->OutCount = n;
    out->Total = g_Ring.Total;
    out->Lost = g_Ring.Lost;
    out->Noise = g_Ring.Noise;
    out->Armed = g_Armed ? 1 : 0;
    out->Resolved = g_Sense[0].Rip ? 1 : 0;
    out->FailStage = g_FailStage;
    out->Target = g_Sense[0].Rip;
    out->TargetFile = g_Sense[1].Rip;
    out->TargetRd = 0;
    for (u32 i = 0; i < SENSE_COUNT; ++i)
        // gate on Gpa: a partial SSDT mismatch leaves validated Rips on
        // slots that never armed - "0 = not armed" per the protocol
        if (g_Sense[i].FnId == SVMB_SYSAUD_FN_NTRD && g_Sense[i].Gpa)
            out->TargetRd = g_Sense[i].Rip;
}

void SysAuditSetBehaveThresholds(u32 rd, u32 wr, u32 op)
{
    if (rd)
        g_BehaveRdThs = rd;
    if (wr)
        g_BehaveWrThs = wr;
    if (op)
        g_BehaveOpThs = op;
}

void SysAuditSetBehaveResponse(u32 v)
{
    g_BehaveResponse = v ? 1 : 0;
}

// r103: give the resolver its work item (DriverEntry-time, like
// Cr3RegionKillInit - the resolver outlives attach cycles; without a
// device it simply never kicks and targets stay unresolved)
void SysAuditResInit(void* deviceObject)
{
    if (deviceObject)
        g_Res.WorkItem = IoAllocateWorkItem((PDEVICE_OBJECT)deviceObject);
}

// r103 acceptance P1-2: driver-unload hygiene, mirroring
// Cr3RegionKillDeinit - claim the item atomically (a racing ResKick
// must never see a freed item; the null claim also permanently
// disarms ResKick/ResWorkItem requeue), then wait bounded for an
// in-flight drain. Still in flight past the wait: LEAK the item (one
// allocation - the safe side; the driver is going away).
void SysAuditResDeinit()
{
    void* item =
        InterlockedExchangePointer(&g_Res.WorkItem, nullptr);
    if (!item)
        return;
    for (int i = 0; i < 1000; ++i)
    {
        if (!InterlockedCompareExchange(&g_Res.Queued, 0, 0))
        {
            IoFreeWorkItem((PIO_WORKITEM)item);
            return;
        }
        LARGE_INTEGER ms1;
        ms1.QuadPart = -10000; // 1ms
        KeDelayExecutionThread(KernelMode, FALSE, &ms1);
    }
    // leak-on-timeout, Cr3RegionKillDeinit semantics
}

void SysAuditBehaviorSnapshot(SVMB_SYSAUD_BEHAVIOR* out)
{
    if (!out)
        return;
    RtlZeroMemory(out, sizeof(*out));
    u32 n = 0;
    for (u32 i = 0; i < SVMB_SYSAUD_BEHAVE_SLOTS; ++i)
    {
        CallerSlot* s = &g_Behave.Slots[i];
        const LONG pid = s->Pid;
        if (!pid)
            continue;
        // approximate under concurrency: a torn row (fields read across
        // a live window rollover) is possible and acceptable - the
        // snapshot is telemetry, not accounting
        SVMB_SYSAUD_CALLER* c = &out->Callers[n++];
        c->Pid = (svmb_u32)pid;
        c->Alerted = s->Alerted ? 1u : 0u;
        c->Rd = s->Rd;
        c->Wr = s->Wr;
        c->Op = s->Op;
        c->CurrRd = s->CurrRd;
        c->CurrWr = s->CurrWr;
        c->CurrOp = s->CurrOp;
        c->PeakRd = s->PeakRd;
        c->PeakWr = s->PeakWr;
        RtlCopyMemory(c->Image, (const void*)s->Image, sizeof(c->Image));
    }
    out->OutCount = n;
    out->WindowMs = SVMB_SYSAUD_BEHAVE_WIN_MS;
    out->RdThresh = g_BehaveRdThs;
    out->WrThresh = g_BehaveWrThs;
    out->OpThresh = g_BehaveOpThs;
    out->Alerts = g_Behave.Alerts;
    out->Dropped = g_Behave.Dropped;
    out->TableFull = g_Behave.TableFull;
    // r103 target side
    u32 tn = 0;
    for (u32 i = 0; i < SVMB_SYSAUD_TARGET_SLOTS; ++i)
    {
        const Behavior::TargetRow* t = &g_Behave.T[i];
        const LONG pid = t->CallerPid;
        if (!pid)
            continue;
        SVMB_SYSAUD_TARGET* r = &out->Targets[tn++];
        r->CallerPid = (svmb_u32)pid;
        r->TargetPid = (svmb_u32)t->TargetPid;
        r->Rd = t->Rd;
        r->Wr = t->Wr;
        r->Op = t->Op;
    }
    out->TargetCount = tn;
    out->ResTargetSlots = SVMB_SYSAUD_TARGET_SLOTS;
    out->Resolved = g_Res.Resolved;
    out->ResFail = g_Res.ResFail;
    out->ResDropped = g_Res.ResDropped;
    out->ExcludedNtcf = g_Behave.ExcludedNtcf;
    out->TargetsFull = g_Behave.TargetsFull;
}

SVMB_DEFINE_MODULE(ModuleSysAudit, "sysaudit", 10, SysAuditInit, SysAuditStop)

} // namespace svmb
