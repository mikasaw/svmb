// svmb - offsets.cpp: see offsets.h. All scanning is bounded ([0, 0x400) /
// [0, 0x800), 8-byte aligned) and wrapped in __try - a wrong guess must
// fail loudly, never corrupt.
#include "platform/offsets.h"
#include "platform/logger.h"

namespace svmb
{

namespace
{

SysOffsets g_offsets = {};
// r83 review P2: published via InterlockedExchange AFTER the offset
// stores (interlocked = full barrier on x64) so any concurrent reader
// that observes "resolved" also observes the offsets; readers use
// InterlockedCompareExchange for the acquire side.
volatile LONG g_resolved = 0;
ULONG g_build = 0;

struct KnownBuild
{
    ULONG build;
    u32 dtb;
    u32 head;
    u32 entry;
};
// layouts of record (campaign-measured on 1903; 1909 shares 1836x layout)
constexpr KnownBuild kKnown[] = {
    {18362, 0x28, 0x488, 0x6b8},
    {18363, 0x28, 0x488, 0x6b8},
};

// template scan: find the one 8-aligned qword in [base, span) that
// equals `value`; false on zero or multiple matches
bool FindUniqueQword(const void* base, u32 span, u64 value, u32* offOut)
{
    const u64* p = (const u64*)base;
    u32 hits = 0;
    u32 hitOff = 0;
    __try
    {
        for (u32 o = 0; o + 8 <= span; o += 8)
        {
            if (p[o / 8] == value)
            {
                ++hits;
                hitOff = o;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    if (hits != 1)
        return false;
    *offOut = hitOff;
    return true;
}

// r83 CRITICAL: reading freed NonPaged pool does NOT raise a catchable
// AV - the MM bugchecks 0x50 directly (the r83 kd catch proved it), so
// SEH provides NO protection for speculative dereferences. Every deref
// of an unvalidated pointer must go through this gate.
bool SafeKernelQword(u64 addr, u64* out)
{
    if (addr < 0xFFFF800000000000ull)
        return false;
    if (!MmIsAddressValid((void*)(ULONG_PTR)addr))
        return false;
    *out = *(u64*)(ULONG_PTR)addr;
    return true;
}

} // namespace

ULONG OffsetsBuild()
{
    return g_build;
}

const SysOffsets& Offsets()
{
    return g_offsets;
}

NTSTATUS OffsetsResolve()
{
    if (InterlockedCompareExchange(&g_resolved, 0, 0))
        return STATUS_SUCCESS;

    RTL_OSVERSIONINFOW vi = {};
    vi.dwOSVersionInfoSize = sizeof(vi);
    if (NT_SUCCESS(RtlGetVersion(&vi)))
        g_build = vi.dwBuildNumber;

    PEPROCESS proc = PsGetCurrentProcess(); // System at DriverEntry
    if (!proc)
        return STATUS_UNSUCCESSFUL;

    // --- EprocDtb: the directory base we are running on is stored in our
    // own EPROCESS; __readcr3 carries low-bit flags, the field does not.
    u64 cr3 = __readcr3() & ~0xFFFull;
    u32 dtb = 0;
    if (!FindUniqueQword(proc, 0x400, cr3, &dtb))
    {
        SVMB_LOGE("offsets: DTB scan failed (cr3=%llx) - layout unknown",
                  cr3);
        return STATUS_UNSUCCESSFUL;
    }

    // --- EthreadListEntry + EprocThreadList: 1903 does not export
    // PsGetNextProcessThread (r47), so we cannot ask for the first
    // thread. The CURRENT thread sits in ThreadListHead's circular list
    // like every other thread, so: find candidate LIST_ENTRY offsets on
    // this thread (doubly-linked consistency), then walk Blink backwards
    // - the list whose head lives INSIDE EPROCESS is ThreadListHead.
    // Every dereference below is gated by SafeKernelQword (0x50 law).
    const u8* t = (const u8*)PsGetCurrentThread();
    if (!t)
    {
        SVMB_LOGE("offsets: no current thread - layout unknown");
        return STATUS_UNSUCCESSFUL;
    }
    u32 entry = 0;
    u32 head = 0;
    u32 hits = 0;
    u32 candO[4] = {};
    u32 candH[4] = {};    // 0x800 span: 1836x ETHREAD is ~0x890 - a smaller-build ETHREAD
    // would over-read into adjacent pool, but every deref is gated
    // (SafeKernelQword) and consistency rejects the noise
    for (u32 o = 0; o + 16 <= 0x800 && hits < 4; o += 8)
    {
        u64 flink = 0;
        u64 blink = 0;
        if (!SafeKernelQword((u64)(ULONG_PTR)t + o, &flink))
            continue;
        if (!SafeKernelQword((u64)(ULONG_PTR)t + o + 8, &blink))
            continue;
        if (blink < 0xFFFF800000000000ull || flink < 0xFFFF800000000000ull)
            continue;
        // doubly-linked consistency: *Blink == this entry's Flink slot
        u64 back = 0;
        if (!SafeKernelQword(blink, &back))
            continue;
        if (back != (u64)(ULONG_PTR)t + o)
            continue;
        // phase 2: follow Blink backwards until it lands inside EPROCESS
        u64 b = blink;
        const u8* cur = t;
        bool found = false;
        for (u32 step = 0; step < 512; ++step)
        {
            if (b > (u64)proc && b < (u64)proc + 0x800)
            {
                entry = o;
                head = (u32)(b - (u64)proc);
                found = true;
                break;
            }
            if (b < 0xFFFF800000000000ull)
                break;
            u64 prevFlink = 0;
            u64 prevBlink = 0;
            if (!SafeKernelQword(b, &prevFlink))
                break;
            if (!SafeKernelQword(b + 8, &prevBlink))
                break;
            if (prevFlink != (u64)(ULONG_PTR)cur + o)
                break; // list consistency broken - not a real list
            cur = (const u8*)(ULONG_PTR)(b - o);
            b = prevBlink;
        }
        if (found)
        {
            // structural rule: the thread-list head never lives inside
            // the EPROCESS fixed-header region (first 0x100 bytes: object
            // header + basic fields incl. the DTB just verified) on any
            // build - the r83 scan needed exactly this to reject the
            // shadow-DTB-adjacent false list at +0x30
            if (head >= 0x100)
            {
                if (hits < 4)
                {
                    candO[hits] = entry;
                    candH[hits] = head;
                }
                ++hits;
            }
        }
    }
    if (hits != 1)
    {
        SVMB_LOGE("offsets: thread-list scan failed (hits=%u) cand="
                  "%x@%x %x@%x %x@%x %x@%x - layout unknown",
                  hits, candO[0], candH[0], candO[1], candH[1], candO[2],
                  candH[2], candO[3], candH[3]);
        return STATUS_UNSUCCESSFUL;
    }

    g_offsets.EprocDtb = dtb;
    g_offsets.EprocThreadList = head;
    g_offsets.EthreadListEntry = entry;
    InterlockedExchange(&g_resolved, 1);

    // cross-check against the table of record (scan wins on mismatch)
    const char* verdict = "UNKNOWN-BUILD";
    for (const KnownBuild& k : kKnown)
    {
        if (k.build == g_build)
        {
            verdict = (k.dtb == dtb && k.head == head && k.entry == entry)
                          ? "MATCH"
                          : "MISMATCH-TRUST-SCAN";
            break;
        }
    }
    SVMB_LOGW("offsets: build=%u dtb=%x head=%x entry=%x vs-table=%s",
              g_build, dtb, head, entry, verdict);
    return STATUS_SUCCESS;
}

} // namespace svmb
