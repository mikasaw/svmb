// svmb - mem_read.cpp: r87 hypervisor-level process memory read, r88
// strategy B (fault-in) + hardening. CR3 from the seed table -> manual x64
// four-level page-table walk performed entirely with physical reads
// (MmCopyMemory MM_COPY_MEMORY_PHYSICAL) -> payload copy. No process attach
// (strategy A), no handles, no guest kernel APIs, and no guest VA is ever
// dereferenced (the 0x50 freed-pool law cannot bite here). Reads never NPF
// on the default RWX NPT view, so the operation is invisible to the guest
// AND bypasses this driver's own L1/L2 protection - by design: the device
// handle is the trust root (docs/SECURITY_AUDIT.md).
//
// Scope notes (docs/HYPERVISOR_MEM_READ.md):
// - 4.2: the walk yields the target process's true frames; NPT hook
//   remapping is bypassed, hook records are not consulted. Each chunk is a
//   fresh walk, so per-chunk snapshots may span concurrent page-table
//   updates (documented semantics, not a bug).
// - TOCTOU: after Lookup the target may die and its page-table frames get
//   reused; a present-PTE read of a recycled frame then yields plausible
//   garbage WITHOUT the PARTIAL flag (r87 acceptance P3-3).
// - 3.3 strategy B (r88, SVMB_READVM_F_FAULTIN): attach the target and
//   read-touch every page in the range so demand-paged content resolves.
//   The touch is SEH-guarded (NOACCESS/guard pages AV cleanly), and the
//   trace is documented: working-set growth, one-shot guard flags consumed.
//   A READ touch never fires copy-on-write.
#include "modules/mem_read.h"
#include "modules/cr3_seed.h"
#include "modules/npt_hook_mgr.h"
#include "core/npt_probe.h"
#include "mm/npt.h"
#include "platform/logger.h"

// documented kernel interfaces whose declarations live in ntifs.h; this
// driver is ntddk-only (same pattern as the r75 declarations in
// cr3_monitor.cpp; KeStackAttachProcess fills the opaque APC state, we
// never interpret it).
extern "C" NTSTATUS PsLookupProcessByProcessId(HANDLE ProcessId,
                                               PEPROCESS* Process);
extern "C" NTSTATUS KeStackAttachProcess(void* Process, void* ApcState);
extern "C" void KeUnstackDetachProcess(void* ApcState);
struct alignas(16) ReadVmApcState
{
    UCHAR Bytes[64]; // >= KAPC_STATE (0x30)
};

namespace svmb
{

// ---- pure x64 walk math (offline-tested, offline_tests.cpp) ------------

u32 ReadVmVaIndex(u64 va, u32 level)
{
    // level 0 = PML4 (bits 39-47) .. 3 = PT (bits 12-20)
    static constexpr u32 kShift[4] = {39, 30, 21, 12};
    return level < 4 ? (u32)((va >> kShift[level]) & 0x1FFull) : 0x1FF;
}

u64 ReadVmEntryFrame(u64 entry)
{
    return entry & 0x000FFFFFFFFFF000ull; // 52-bit physical frame
}

u64 ReadVmLargeGpa(u64 entry, u64 va, u32 shift)
{
    const u64 offMask = (1ull << shift) - 1;
    return (entry & 0x000FFFFFFFFFF000ull & ~offMask) | (va & offMask);
}

u64 ReadVmPageGpa(u64 entry, u64 va)
{
    return (entry & 0x000FFFFFFFFFF000ull) | (va & 0xFFFull);
}

namespace
{

constexpr u64 PTE_PRESENT = 0x1ull;
constexpr u64 PTE_PS = 0x80ull; // PDE/PDPTE large-page flag

volatile LONG64 g_readVmCalls = 0;
volatile LONG64 g_readVmPages = 0;

// physical read; MmCopyMemory is the only memory API touched. *got receives
// the bytes actually transferred on any outcome.
bool PhysRead(u64 pa, void* dst, u32 len, SIZE_T& got)
{
    got = 0;
    MM_COPY_ADDRESS src = {};
    src.PhysicalAddress.QuadPart = static_cast<LONGLONG>(pa);
    return NT_SUCCESS(
               MmCopyMemory(dst, src, len, MM_COPY_MEMORY_PHYSICAL, &got))
           && got == len;
}

bool PhysReadQword(u64 pa, u64& out)
{
    SIZE_T got = 0;
    return PhysRead(pa, &out, sizeof(out), got);
}

// translate one user VA to a GPA; false = some level not present or a
// page-table frame outside the firmware RAM map (a poisoned guest page
// table must not steer MmCopyMemory into MMIO - r87 acceptance P3-1).
bool WalkVa(const PhysRanges* ranges, u64 cr3, u64 va, u64& gpaOut)
{
    const u64 pml4Pa = ReadVmEntryFrame(cr3);
    if (!pml4Pa || !ranges->IsRam(pml4Pa))
        return false;
    u64 pml4e;
    if (!PhysReadQword(pml4Pa + (u64)ReadVmVaIndex(va, 0) * 8, pml4e)
        || !(pml4e & PTE_PRESENT))
        return false;
    const u64 pdptPa = ReadVmEntryFrame(pml4e);
    if (!pdptPa || !ranges->IsRam(pdptPa))
        return false;
    u64 pdpte;
    if (!PhysReadQword(pdptPa + (u64)ReadVmVaIndex(va, 1) * 8, pdpte)
        || !(pdpte & PTE_PRESENT))
        return false;
    if (pdpte & PTE_PS) // 1G page
    {
        gpaOut = ReadVmLargeGpa(pdpte, va, 30);
        return true;
    }
    const u64 pdPa = ReadVmEntryFrame(pdpte);
    if (!pdPa || !ranges->IsRam(pdPa))
        return false;
    u64 pde;
    if (!PhysReadQword(pdPa + (u64)ReadVmVaIndex(va, 2) * 8, pde)
        || !(pde & PTE_PRESENT))
        return false;
    if (pde & PTE_PS) // 2M page
    {
        gpaOut = ReadVmLargeGpa(pde, va, 21);
        return true;
    }
    const u64 ptPa = ReadVmEntryFrame(pde);
    if (!ptPa || !ranges->IsRam(ptPa))
        return false;
    u64 pte;
    if (!PhysReadQword(ptPa + (u64)ReadVmVaIndex(va, 3) * 8, pte)
        || !(pte & PTE_PRESENT))
        return false;
    gpaOut = ReadVmPageGpa(pte, va);
    return true;
}

// r88/r89 strategy B: fault not-present pages of [va, va+size) in. The
// r89 kd-caught lesson: a DIRECT supervisor touch of a demand-zero user
// page spins forever on this vhv (KiPageFault loop, SEH unreachable, and
// the EFLAGS.AC bracket did not save it - the r88 touch worked only by
// heap-layout luck). The r86-proven pattern instead: MmProbeAndLockPages
// (UserMode, IoReadAccess) performs the fault-in through Mm itself and
// hard-fails cleanly on unresolvable pages.
void FaultInPages(HANDLE pid, u64 va, u32 size)
{
    PEPROCESS proc = nullptr;
    if (!NT_SUCCESS(PsLookupProcessByProcessId(pid, &proc)) || !proc)
        return;
    ReadVmApcState apc = {};
    KeStackAttachProcess(proc, &apc);
    PMDL mdl = IoAllocateMdl((void*)(ULONG_PTR)va, size, FALSE, FALSE,
                             nullptr);
    if (mdl)
    {
        // r89 (kd-caught, bugcheck 0x76 PROCESS_HAS_LOCKED_PAGES): unlock
        // ONLY a successfully locked MDL - MmUnlockPages on a probe-failed
        // MDL corrupts PFN accounting and the process dies at exit.
        bool locked = false;
        __try
        {
            MmProbeAndLockPages(mdl, UserMode, IoReadAccess);
            locked = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // NOACCESS/guard/freed: not lockable - walk decides residency
        }
        if (locked)
        {
            __try
            {
                MmUnlockPages(mdl);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                // locked pages are always unlockable; belt for the impossible
            }
        }
        IoFreeMdl(mdl);
    }
    KeUnstackDetachProcess(&apc);
    ObDereferenceObject(proc); // wdm.h macro
}

} // namespace

u32 MemReadVm(SVMB_CR3_READVM* p, u32 outCap)
{
    if (!p || outCap < sizeof(*p))
        return 0;
    const u32 size = p->Size;
    if (size == 0 || size > SVMB_READVM_MAX || outCap - sizeof(*p) < size)
        return 0;
    // user-VA contract: reads expose target USER memory only. A plain
    // IsKernelVa pair is NOT enough - non-canonical vas (2^63..2^64-2^17)
    // pass both checks yet their PML4 index aliases the kernel half, so
    // the walk would happily read target kernel memory (r88 acceptance
    // P1-1). Range-check against the Windows user top instead; the first
    // check also subsumes u64 wrap (va is the full range's low end).
    constexpr u64 kUserVaMax = 0x00007FFFFFFEFFFFull;
    if (p->Va > kUserVaMax || p->Va + size > kUserVaMax)
    {
        SVMB_LOGW("readvm: non-user va %llx+%x rejected", p->Va, size);
        return 0;
    }
    if (p->InFlags & ~(SVMB_READVM_F_FAULTIN | SVMB_READVM_F_GUESTVIEW))
    {
        SVMB_LOGW("readvm: unknown in-flags %x rejected", p->InFlags);
        return 0;
    }

    u64 cr3 = 0;
    if (!Cr3SeedInstance()->Lookup(p->Pid, cr3) || !ReadVmEntryFrame(cr3))
    {
        SVMB_LOGW("readvm: pid %u not seeded (cr3=%llx)", p->Pid, cr3);
        return 0;
    }

    // RAM-map gate: without firmware ranges we cannot distinguish RAM from
    // MMIO, and probing MMIO through MmCopyMemory can have device side
    // effects. gNpt inits at DriverEntry, so this only refuses on a
    // pathologically broken machine.
    NptManager* npt = NptInstance();
    PhysRanges* ranges = npt ? &npt->Ranges() : nullptr;
    if (!ranges || ranges->Count() == 0)
    {
        SVMB_LOGE("readvm: no RAM map, refusing");
        return 0;
    }

    if (p->InFlags & SVMB_READVM_F_FAULTIN)
        FaultInPages((HANDLE)(ULONG_PTR)p->Pid, p->Va, size);

    // r89 GUESTVIEW: resolve once - a hooked page redirects to a stable
    // hidden copy while it is hooked. Safety attribution (acceptance P3-4):
    // concurrent Remove/Deinit is excluded by the IOCTL gate (MemReadVm and
    // hook Remove both hold gIoGate); the graveyard only bounds the
    // VM-exit path's lock-free reads, not this one.
    const bool guestView = (p->InFlags & SVMB_READVM_F_GUESTVIEW) != 0;
    NptHookManager* hooks = guestView ? HookInstance() : nullptr;

    u8* payload = reinterpret_cast<u8*>(p + 1);
    RtlZeroMemory(payload, size);
    u32 done = 0;
    u32 resolved = 0;
    u32 pages = 0;
    bool partial = false;
    while (done < size)
    {
        const u64 va = p->Va + done;
        const u32 chunk = static_cast<u32>(PAGE_SIZE - (va & 0xFFFull));
        const u32 len = size - done < chunk ? size - done : chunk;
        u64 gpa = 0;
        SIZE_T got = 0;
        if (WalkVa(ranges, cr3, va, gpa))
        {
            // r89: GUESTVIEW swaps in the hook's patched hidden copy - the
            // frame the guest actually observes through the NPT remap;
            // ground truth (no flag) reads the pristine original
            u64 src = gpa;
            if (hooks)
            {
                NptHook* h = hooks->LookupPage(gpa & ~0xFFFull);
                if (h)
                    src = h->HiddenPa;
            }
            if (ranges->IsRam(src)
                && PhysRead(src, payload + done, len, got))
            {
                done += len;
                resolved += len;
                ++pages;
                continue;
            }
        }
        // strategy A semantics (docs/HYPERVISOR_MEM_READ.md 3.3): the page
        // is not resident / not RAM - zero the chunk (MmCopyMemory may have
        // transferred a prefix already) and keep the VA<->payload layout
        if (got && got < len)
            RtlZeroMemory(payload + done, len); // partial transfer: discard
        partial = true;
        done += len;
    }

    p->OutResolved = resolved;
    // r96 audit R2: the target may have died mid-walk (its frames get
    // recycled and the walk returns plausible garbage - the r87 TOCTOU).
    // Re-verify the seed still maps pid -> the CR3 we walked; a mismatch
    // marks the payload stale so the caller can weigh it.
    u32 flags = partial ? SVMB_READVM_F_PARTIAL : 0;
    {
        u64 cr3Now = 0;
        if (!Cr3SeedInstance()->Lookup(p->Pid, cr3Now) || cr3Now != cr3)
            flags |= SVMB_READVM_F_STALE;
    }
    p->OutFlags = flags;
    InterlockedIncrement64(&g_readVmCalls);
    InterlockedAdd64(&g_readVmPages, pages);
    SVMB_LOGI("readvm: pid=%u va=%llx size=%u resolved=%u pages=%u "
              "partial=%u faultin=%u",
              p->Pid, p->Va, size, resolved, pages, partial ? 1u : 0u,
              (p->InFlags & SVMB_READVM_F_FAULTIN) ? 1u : 0u);
    return sizeof(*p) + size;
}

void MemReadVmStats(u32& callsOut, u32& pagesOut)
{
    callsOut = static_cast<u32>(g_readVmCalls);
    pagesOut = static_cast<u32>(g_readVmPages);
}

} // namespace svmb
