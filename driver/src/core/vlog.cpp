// svmb - r181 vlog recorder implementation (see vlog.h).
#include "core/vlog.h"
#include "platform/util.h"
#include "hw/svm_defs.h"

namespace svmb
{

namespace
{
constexpr u32 RING_CAP = 8192; // 8192 * 64B = 512KB nonpaged
constexpr u32 MAX_CORES_VL = 64;

struct Rec
{
    u64 tsc;
    u64 a, b, c, d;
    u32 seq;
    u8 kind, cpu;
    u16 pad;
};

struct VlogState
{
    Rec* ring = nullptr;
    volatile LONG lock = 0;      // spinlock (exit-context safe)
    volatile LONG64 head = 0;    // monotonically growing write index
    volatile LONG64 readIdx = 0; // consume index (drain reads)
    u64 kindCount[6] = {};
    u64 exitHist[128] = {};      // 64 hashed (bucket,count) pairs
    volatile LONG mask = -1;     // all kinds on
    volatile LONG tryLockDropped = 0; // r188: records skipped by the
                                      // try-lock (contention beats spin)
    // per-core last-accept snapshot for H control diffing
    struct Last
    {
        u64 vmcbPa;
        u32 ic1, ic2, exc;
        u64 ncr3, tscOff, inj;
        bool valid;
    };
    Last last[MAX_CORES_VL] = {};
};
VlogState g_vl;
} // namespace

bool VlogInit()
{
    if (g_vl.ring)
        return true;
    g_vl.ring = (Rec*)AllocNonPaged(RING_CAP * sizeof(Rec), TAG_SVMB);
    if (!g_vl.ring)
        return false;
    RtlZeroMemory(g_vl.ring, RING_CAP * sizeof(Rec));
    return true;
}

void VlogDeinit()
{
    if (g_vl.ring)
        FreeNonPaged(g_vl.ring, TAG_SVMB, RING_CAP * sizeof(Rec));
    g_vl.ring = nullptr;
    RtlZeroMemory(&g_vl.last, sizeof(g_vl.last));
}

void VlogRecord(u8 kind, u8 cpu, u64 a, u64 b, u64 c, u64 d)
{
    if (!g_vl.ring || kind < 1 || kind > 6)
        return;
    if ((g_vl.mask & (1 << (kind - 1))) == 0)
        return;
    // r188: TRY-LOCK only, never spin. The freeze IPI can preempt an
    // exit path inside this critical section and a core frozen in the
    // rendezvous never releases the lock - every other core's exit
    // path then spins here forever (the r187 rendezvous self-deadlock:
    // the bugcheck dump path runs as L2 through the shadow, its exits
    // pile onto the held lock, the freeze never completes, zero exits
    // machine-wide). A diagnostic record lost to contention is the
    // cheap outcome; count it and move on.
    if (InterlockedCompareExchange(&g_vl.lock, 1, 0) != 0)
    {
        InterlockedIncrement(&g_vl.tryLockDropped);
        return;
    }
    LONG64 h = ++g_vl.head;
    Rec& r = g_vl.ring[(u32)(h - 1) & (RING_CAP - 1)];
    r.tsc = __rdtsc();
    r.a = a;
    r.b = b;
    r.c = c;
    r.d = d;
    r.seq = (u32)h;
    r.kind = kind;
    r.cpu = cpu;
    r.pad = 0;
    g_vl.kindCount[kind - 1]++;
    // exit histogram: bucket by exit code class (top bits identify the
    // family, low 8 bits collapse per-vector noise; NPF/exceptions are
    // kept exact where they matter)
    if (kind == VLOG_REFLECT)
    {
        u64 e = a;
        // NPFs bucket as one family; every other exit code is distinct
        // and meaningful (0x72 CPUID, 0x78 HLT, 0x40+N exceptions...)
        u64 bucket = (e >= 0x400 && e <= 0x4FF) ? 0x400 : e;
        // linear probe into a small exact-bucket table (records are
        // dominated by very few buckets)
        for (u32 i = 0; i < 64; ++i)
        {
            u64 k = (bucket + i * 0x9E37ull) % 64;
            if (g_vl.exitHist[k * 2] == bucket || g_vl.exitHist[k * 2] == 0)
            {
                g_vl.exitHist[k * 2] = bucket;
                g_vl.exitHist[k * 2 + 1]++;
                break;
            }
        }
    }
    InterlockedExchange(&g_vl.lock, 0);
}

void VlogAccept(u32 cpu, u64 hvmcbPa, u64 rip, u64 cr3, u32 np,
                u32 pendingL1, u32 hIc1, u32 hIc2, u32 hExc, u64 hNcr3,
                u64 hTscOff, u64 hEventInj)
{
    if (!g_vl.ring)
        return;
    VlogRecord(VLOG_ACCEPT, (u8)cpu, hvmcbPa, rip, cr3,
               (u64)np | ((u64)pendingL1 << 1));
    if (cpu >= MAX_CORES_VL)
        return;
    VlogState::Last& L = g_vl.last[cpu];
    if (!L.valid || L.ic1 != hIc1 || L.ic2 != hIc2 || L.exc != hExc)
        VlogRecord(VLOG_INTENT, (u8)cpu, hIc1, hIc2, hExc, 0);
    if (hEventInj & 0x80000000ull) // Valid bit armed by H
        VlogRecord(VLOG_HINJ, (u8)cpu, hEventInj, 0, 0, 0);
    if (L.valid && L.ncr3 != hNcr3)
        VlogRecord(VLOG_HCTRL, (u8)cpu, 1, L.ncr3, hNcr3, 0);
    if (L.valid && L.tscOff != hTscOff)
        VlogRecord(VLOG_HCTRL, (u8)cpu, 2, L.tscOff, hTscOff, 0);
    L.vmcbPa = hvmcbPa;
    L.ic1 = hIc1;
    L.ic2 = hIc2;
    L.exc = hExc;
    L.ncr3 = hNcr3;
    L.tscOff = hTscOff;
    L.inj = hEventInj;
    L.valid = true;
}

void VlogReflect(u32 cpu, u64 exitCode, u64 info1, u64 info2, u64 l2rip)
{
    VlogRecord(VLOG_REFLECT, (u8)cpu, exitCode, info1, info2, l2rip);
}

void VlogCompose(u32 cpu, u64 gpa, u64 pa, u64 perms, u32 level)
{
    VlogRecord(VLOG_COMPOSE, (u8)cpu, gpa, pa, perms, level);
}

u32 VlogRead(void* out, u32 maxRecords, u32* droppedOut)
{
    if (!g_vl.ring || !out)
        return 0;
    u8* dst = (u8*)out;
    u32 n = 0;
    while (InterlockedCompareExchange(&g_vl.lock, 1, 0) != 0)
        YieldProcessor();
    LONG64 head = g_vl.head;
    LONG64 avail = head - g_vl.readIdx;
    if (avail > (LONG64)RING_CAP)
    {
        if (droppedOut)
            *droppedOut = (u32)(avail - RING_CAP);
        g_vl.readIdx = head - RING_CAP; // skip the overwritten tail
        avail = RING_CAP;
    }
    if (avail > (LONG64)maxRecords)
        avail = maxRecords;
    for (LONG64 i = g_vl.readIdx; i < g_vl.readIdx + avail; ++i)
    {
        Rec& r = g_vl.ring[(u32)i & (RING_CAP - 1)];
        // wire layout mirrors Rec exactly (see SVMB_VLOG_REC)
        RtlCopyMemory(dst, &r, sizeof(Rec));
        dst += sizeof(Rec);
        ++n;
    }
    g_vl.readIdx += avail;
    InterlockedExchange(&g_vl.lock, 0);
    return n;
}

void VlogStats(u64 kindCount[6], u64 topExits[8][2])
{
    for (u32 k = 0; k < 6; ++k)
        kindCount[k] = g_vl.kindCount[k];
    // selection-sort the top 8 histogram buckets
    u64 tmp[64][2];
    RtlCopyMemory(tmp, g_vl.exitHist, sizeof(tmp));
    for (u32 i = 0; i < 8; ++i)
    {
        u32 best = i;
        for (u32 j = i + 1; j < 64; ++j)
            if (tmp[j][1] > tmp[best][1])
                best = j;
        u64 bk = tmp[i][0], bn = tmp[i][1];
        tmp[i][0] = tmp[best][0];
        tmp[i][1] = tmp[best][1];
        tmp[best][0] = bk;
        tmp[best][1] = bn;
        topExits[i][0] = tmp[i][0];
        topExits[i][1] = tmp[i][1];
    }
}

void VlogSetMask(u32 kindMask)
{
    InterlockedExchange(&g_vl.mask, (LONG)kindMask);
}

void VlogReset()
{
    while (InterlockedCompareExchange(&g_vl.lock, 1, 0) != 0)
        YieldProcessor();
    g_vl.head = 0;
    g_vl.readIdx = 0;
    RtlZeroMemory(g_vl.kindCount, sizeof(g_vl.kindCount));
    RtlZeroMemory(g_vl.exitHist, sizeof(g_vl.exitHist));
    RtlZeroMemory(g_vl.last, sizeof(g_vl.last));
    InterlockedExchange(&g_vl.lock, 0);
}

} // namespace svmb
