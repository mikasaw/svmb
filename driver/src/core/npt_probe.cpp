// svmb - M3 live-NPT probe: the first controlled walk of the NPF exit path
// and the view-switch publish protocol on live cores.
//
// Stage 0 (NPF slide cycle): hooks a driver-local function. The target page
// starts in the slide DATA state (original bytes, RW + NX), so the first
// guest fetch of the function #NPFs into HookSlideApply, which swaps the
// page to the EXEC state (patched hidden copy). Every probe call therefore
// runs the detour; after Remove the original body must run again. All four
// counters are asserted by the caller:
//   DetourHits     == Iterations  (slide served the hidden copy)
//   OrigHitsHooked == 0           (no stale-TLB leak of the original page)
//   OrigHitsAfter  == Iterations  (remove restored the original mapping)
//   ExitDelta      >= 2           (the two forced CPUID exits; NPF exits
//                                  counted on top)
//
// Stage 1 (view-switch cycle): builds a second identity-filled view and
// flips the active view while every core is live. TlbApplyViewSwitchAllCores
// writes each VMCB's NCr3 (CleanBits=0) - the first live exercise of the
// publish path; view contents are identical so the guest must not notice.
// The local core's VMCB NCr3 is read back after one forced exit as proof
// the publish landed.
#include "core/npt_probe.h"
#include "core/hypervisor.h"
#include "core/nested.h" // r171: H-view register/unregister
#include "mm/npt.h"
#include "modules/dbg_events.h"
#include "modules/npt_hook_mgr.h"
#include "platform/logger.h"
#include "platform/util.h"


namespace svmb
{

namespace
{
volatile LONG64 gProbeDetourHits = 0;
volatile LONG64 gProbeOrigHits = 0;

// ---- stage 3/4: real-kernel hook via the r35 callback layer ----
//
// InstallCallback emits a per-hook stub on the trampoline page: it saves the
// volatile GPRs, calls the managed callback with a SVMB_HOOK_FRAME, restores
// everything and jumps into the stolen prefix (passthrough) - or returns the
// callback's value to the caller (override, body never runs). No global
// relay state: several hooks can be live at once, each with its own callback.

volatile LONG64 g_svmb_hook_hits = 0;

// stage 3: observe + passthrough
u64 NTAPI SvmbNtHookCountCb(const SVMB_HOOK_FRAME*)
{
    InterlockedIncrement64(&g_svmb_hook_hits);
    return 0; // passthrough: the original NtCreateFile body runs
}

// stage 4: override - deny cmd.exe (body never runs), pass everything else.
// r8 = POBJECT_ATTRIBUTES at NtCreateFile entry; for syscall callers this
// may be a USER pointer (the body captures it after our callback), so every
// dereference is guarded - an unguarded AV here would bugcheck the box.
u64 NTAPI SvmbNtHookDenyCmdCb(const SVMB_HOOK_FRAME* f)
{
    InterlockedIncrement64(&g_svmb_hook_hits);
    POBJECT_ATTRIBUTES oa = (POBJECT_ATTRIBUTES)f->R8;
    if (!oa)
        return 0;
    __try
    {
        UNICODE_STRING* name = oa->ObjectName;
        if (!name)
            return 0;
        UNICODE_STRING target;
        RtlInitUnicodeString(&target, L"\\SystemRoot\\System32\\cmd.exe");
        if (RtlEqualUnicodeString(name, &target, TRUE))
            return (u64)STATUS_ACCESS_DENIED; // override: deny the open
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0; // bad/crossed pointer from a caller mid-setup: pass it on
    }
    return 0; // passthrough
}

// Probe target: MUST stay noinline, and its first >=12 bytes must decode
// without RIP-relative operands (StealLen rejects those - the trampoline
// copy cannot relocate them). The volatile chain up front forces stack
// traffic before any global reference, keeping the stolen prefix clean.
//
// It ALSO lives in its own PE section (.svmbpr0): the PE loader maps
// sections page-aligned, so the page this function occupies contains
// nothing else. r31 showed why that is load-bearing: the linker placed
// SvmbNpfProbeTarget on the same 4K page as the ntstrsafe formatter
// (RtlStringCbVPrintfA) that every SVMB_LOGx call runs through. Arming
// the hook NX'd that page, so the NPF handler's own diagnostic logging
// re-faulted on the hooked page (a recursive NPF from inside the exit
// path) - the slide thrashed, the spin breaker started skipping live
// instructions, and the whole log subsystem bricked into a freeze.
#pragma code_seg(".svmbpr0")
__declspec(noinline) u64 SvmbNpfProbeTarget(u64 a)
{
    volatile u64 t = a;
    t = t * 3 + 7;
    t ^= t >> 1;
    t += 5;
    InterlockedIncrement64(&gProbeOrigHits);
    return t;
}
#pragma code_seg()

__declspec(noinline) u64 SvmbNpfProbeDetour(u64 a)
{
    InterlockedIncrement64(&gProbeDetourHits);
    return a + 1;
}

// open a kernel file that certainly exists; returns the NtCreateFile status
NTSTATUS ProbeOpenFile(PCWSTR name, HANDLE* handleOut)
{
    UNICODE_STRING uname;
    RtlInitUnicodeString(&uname, name);
    OBJECT_ATTRIBUTES obj;
    InitializeObjectAttributes(&obj, &uname,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    IO_STATUS_BLOCK iosb = {};
    return ZwCreateFile(handleOut, FILE_READ_ATTRIBUTES, &obj, &iosb, nullptr,
                        0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        FILE_OPEN, 0, nullptr, 0);
}

NTSTATUS ProbeOpenSystemFile(HANDLE* handleOut)
{
    return ProbeOpenFile(L"\\SystemRoot\\System32\\cmd.exe", handleOut);
}

void ForceFlushExit()
{
    int c[4] = {};
    __cpuid(c, 0);
}
} // namespace

// ---- r179 stage 30: multi-core concurrent shadow judge ----
// T independent H worlds (own VMCB + nested NPT + L2 tables + mark frame)
// run L2 round trips CONCURRENTLY on distinct cores. The same L2 GPA
// (0x513000) maps to a DIFFERENT physical frame in each core's shadow -
// the per-core pool, accept, and compose paths must all hold their
// worlds apart. File-scope statics: a timed-out thread may outlive the
// ioctl stack.
namespace {
struct McCtx
{
    u32 core;
    u64 magic;
    LONG ok;            // 1 = every round's mark matched the own magic
    LONG rounds;        // completed L2 round trips
    u64 lastMark;
    KEVENT done;
    volatile LONG started;
};
McCtx s_mc[4];
KEVENT s_mcGo;

void McThreadFn(PVOID arg)
{
    McCtx* c = (McCtx*)arg;
    KeSetSystemAffinityThread((KAFFINITY)1 << c->core);
    InterlockedIncrement(&c->started);
    // barrier: race out with the siblings for genuine overlap
    KeWaitForSingleObject(&s_mcGo, Executive, KernelMode, FALSE, nullptr);

    void* pages[12] = {};
    for (int i = 0; i < 12; ++i)
        pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
    LONG okRounds = 0;
    if (!pages[11])
    {
        for (int i = 0; i < 11; ++i)
            if (pages[i])
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
        KeSetEvent(&c->done, IO_NO_INCREMENT, FALSE);
        return;
    }
    auto pageAt = [](void* raw) -> u64* {
        return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) & ~0xFFFull);
    };
    u64* vmcb = pageAt(pages[0]);
    u8* code = (u8*)pageAt(pages[1]);
    u64* stack = pageAt(pages[2]);
    u64* mark = pageAt(pages[3]);
    u64* l2t[4] = { pageAt(pages[4]), pageAt(pages[5]), pageAt(pages[6]),
                    pageAt(pages[7]) };
    u64* hn[4] = { pageAt(pages[8]), pageAt(pages[9]), pageAt(pages[10]),
                   pageAt(pages[11]) };
    const u64 CODE_VA = 0x500000, STACK_VA = 0x511000, MARK_VA = 0x513000;
    l2t[0][0] = MmGetPhysicalAddress(l2t[1]).QuadPart | 3;
    l2t[1][0] = MmGetPhysicalAddress(l2t[2]).QuadPart | 3;
    l2t[2][2] = MmGetPhysicalAddress(l2t[3]).QuadPart | 3;
    l2t[3][(CODE_VA >> 12) & 0x1FF] = CODE_VA | 3;
    l2t[3][(STACK_VA >> 12) & 0x1FF] = STACK_VA | 3;
    l2t[3][(MARK_VA >> 12) & 0x1FF] = MARK_VA | 3;
    hn[0][0] = MmGetPhysicalAddress(hn[1]).QuadPart | 3;
    hn[1][0] = MmGetPhysicalAddress(hn[2]).QuadPart | 3;
    hn[2][2] = MmGetPhysicalAddress(hn[3]).QuadPart | 3;
    hn[3][(CODE_VA >> 12) & 0x1FF] = MmGetPhysicalAddress(code).QuadPart | 7;
    hn[3][(STACK_VA >> 12) & 0x1FF] =
        MmGetPhysicalAddress(stack).QuadPart | 7;
    hn[3][(MARK_VA >> 12) & 0x1FF] =
        MmGetPhysicalAddress(mark).QuadPart | 7;
    // cpuid; mov qword [0x513000], magic (positive imm32: no sign
    // extension, the mark equals the magic verbatim); hlt
    u8 prog[15] = { 0x0F, 0xA2, 0x48, 0xC7, 0x04, 0x25, 0x00, 0x30, 0x51,
                    0x00, 0, 0, 0, 0, 0xF4 };
    *(u32*)(prog + 10) = (u32)c->magic;
    RtlCopyMemory(code, prog, sizeof(prog));
    mark[0] = 0;
    RtlZeroMemory(vmcb, PAGE_SIZE);
    // ic1 = +0x0C (0x00 is the CR-read word - the r181 misarm catch)
    *(u32*)((u8*)vmcb + 0x0C) = (1u << 18) | (1u << 24); // CPUID
    *(u32*)((u8*)vmcb + 0x90) = 1; // NP_ENABLE
    vmcb[0xB0 / 8] = MmGetPhysicalAddress(hn[0]).QuadPart;
    *(u64*)((u8*)vmcb + 0x570) = 2;      // rflags IF=0
    *(u64*)((u8*)vmcb + 0x578) = CODE_VA; // rip
    *(u64*)((u8*)vmcb + 0x5D8) = STACK_VA + 0xFF8; // rsp
    *(u64*)((u8*)vmcb + 0x550) = MmGetPhysicalAddress(l2t[0]).QuadPart;
    u64 vmcbPa = MmGetPhysicalAddress(vmcb).QuadPart;

    const LONG ROUNDS = 50;
    LONG completed = 0;
    LONG exc = 0;
    for (LONG r = 0; r < ROUNDS; ++r)
    {
        *(u64*)((u8*)vmcb + 0x578) = CODE_VA;
        *(u64*)((u8*)vmcb + 0x5D8) = STACK_VA + 0xFF8;
        mark[0] = 0;
        bool roundOk = false;
        for (u32 guard = 0; guard < 64; ++guard)
        {
            __try { __svm_vmrun(vmcbPa); exc = 0; }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                exc = (LONG)GetExceptionCode();
                break;
            }
            u64 ec = vmcb[0x70 / 8];
            u64 rip = *(u64*)((u8*)vmcb + 0x578);
            if (ec == 0x60)
                continue;
            if (ec == 0x72 && rip == CODE_VA)
            {
                *(u64*)((u8*)vmcb + 0x578) = rip + 2;
                continue;
            }
            if (ec == 0x78 && mark[0] == c->magic)
                roundOk = true;
            break;
        }
        if (roundOk && !exc)
            ++completed;
        else
            break;
    }
    c->rounds = completed;
    c->lastMark = mark[0];
    c->ok = (completed == ROUNDS) ? 1 : 0;
    // r178 teardown invalidation (each world dies with its window)
    _svmb_invlpga(0, 1);
    for (int i = 0; i < 12; ++i)
        FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
    KeSetEvent(&c->done, IO_NO_INCREMENT, FALSE);
}
} // namespace

extern "C" LONG svmbHookScanEnable(LONG on);

NTSTATUS RunNptProbe(SVMB_NPT_PROBE* p)
{
    if (!p)
        return STATUS_INVALID_PARAMETER;
    NptHookManager* hooks = HookInstance();
    Hypervisor* hv = Hypervisor::Instance();
    NptManager* npt = NptInstance();
    if (!hooks || !hv || !npt)
        return STATUS_DEVICE_NOT_READY;
    if (!hv->IsRunning() || !npt->NptEnabled() || !npt->Active())
        return STATUS_INVALID_DEVICE_STATE;

    u32 iters = p->Iterations ? p->Iterations : 16;
    if (iters > 4096)
        iters = 4096;

    // pin the calling thread: slide arming is a per-core TLB domain event;
    // a migration mid-probe would serve pre-slide translations on the new
    // core and break the counter determinism the caller asserts on
    PROCESSOR_NUMBER pn = {};
    KeGetCurrentProcessorNumberEx(&pn);
    GROUP_AFFINITY ga = {}, prev = {};
    ga.Group = pn.Group;
    ga.Mask = 1ull << pn.Number;
    KeSetSystemGroupAffinityThread(&ga, &prev);

    NTSTATUS status = STATUS_SUCCESS;

    if (p->Stage == 0)
    {
        gProbeDetourHits = 0;
        gProbeOrigHits = 0;
        u64 exitBefore = hv->TotalExitCount();

        status = hooks->Install((u64)&SvmbNpfProbeTarget,
                                (u64)&SvmbNpfProbeDetour,
                                NptHookManager::MODE_JMP);
        if (NT_SUCCESS(status))
        {
            ForceFlushExit(); // drop pre-install translations of the target

            for (u32 i = 0; i < iters; ++i)
                SvmbNpfProbeTarget(i);
            p->DetourHits = gProbeDetourHits;
            p->OrigHitsHooked = gProbeOrigHits;

            status = hooks->Remove((u64)&SvmbNpfProbeTarget);
            ForceFlushExit();

            gProbeOrigHits = 0;
            for (u32 i = 0; i < iters; ++i)
                SvmbNpfProbeTarget(i);
            p->OrigHitsAfter = gProbeOrigHits;

            p->ExitDelta = hv->TotalExitCount() - exitBefore;
            p->TargetVa = (u64)&SvmbNpfProbeTarget;
            SVMB_LOGI("npf probe: iters=%u detour=%llu origHooked=%llu "
                      "origAfter=%llu exitDelta=%llu",
                      iters, (u64)p->DetourHits, (u64)p->OrigHitsHooked,
                      (u64)p->OrigHitsAfter, (u64)p->ExitDelta);
        }
    }
    else if (p->Stage == 1)
    {
        u32 id = 0;
        status = npt->CreateView(id);
        if (NT_SUCCESS(status))
        {
            status = npt->FillIdentityView(*npt->View(id));
            if (NT_SUCCESS(status))
                status = npt->SetActiveView(id);
            if (NT_SUCCESS(status))
            {
                ForceFlushExit(); // this core re-enters with the new NCr3
                VcpuContext* v = hv->Vcpu(pn.Number);
                p->Pml4Pa = npt->Active()->Pml4Pa();
                p->VmcbNcr3 = v ? v->GuestVmcb->Ctrl.NCr3 : 0;
                SVMB_LOGI("view probe: switched to view %u pml4=%llx "
                          "vmcbNcr3=%llx", id, (u64)p->Pml4Pa,
                          (u64)p->VmcbNcr3);

                NTSTATUS stBack = npt->SetActiveView(0);
                ForceFlushExit();
                if (!NT_SUCCESS(stBack))
                    status = stBack;
            }
            NTSTATUS stD = npt->DestroyView(id);
            if (NT_SUCCESS(status) && !NT_SUCCESS(stD))
                status = stD;
        }
    }
    else if (p->Stage == 2)
    {
        // install + DUMP + remove: never executes the hooked page. Ground
        // truth for the patch/trampoline bytes without kd (r31 boot4: wild
        // jump to rip=0 during stage 0 - verify the bytes first).
        status = hooks->Install((u64)&SvmbNpfProbeTarget,
                                (u64)&SvmbNpfProbeDetour,
                                NptHookManager::MODE_JMP);
        if (NT_SUCCESS(status))
        {
            const NptHook* h = hooks->FindHook((u64)&SvmbNpfProbeTarget);
            if (h)
            {
                u64 off = (u64)&SvmbNpfProbeTarget & 0xFFFull;
                p->TargetVa = (u64)&SvmbNpfProbeTarget;
                p->PagePa = h->PagePa;
                p->HiddenPa = h->HiddenPa;
                p->TrampPa = h->TrampPa;
                p->StolenLen = h->StolenLen;
                p->Off = (u32)off;
                RtlCopyMemory(p->OrigBytes, (void*)&SvmbNpfProbeTarget, 32);
                RtlCopyMemory(p->HiddenBytes, (void*)(h->HiddenVa + off), 32);
                RtlCopyMemory(p->TrampBytes, (void*)h->TrampVa, 16);
                SVMB_LOGI("npf probe s2: target=%llx page=%llx hidden=%llx "
                          "tramp=%llx off=%llx stolen=%u",
                          (u64)p->TargetVa, (u64)p->PagePa, (u64)p->HiddenPa,
                          (u64)p->TrampPa, (u64)p->Off, h->StolenLen);
            }
            else
            {
                status = STATUS_NOT_FOUND;
            }
            NTSTATUS stR = hooks->Remove((u64)&SvmbNpfProbeTarget);
            if (NT_SUCCESS(status) && !NT_SUCCESS(stR))
                status = stR;
        }
    }
    else if (p->Stage == 3 || p->Stage == 4)
    {
        // r35 callback-layer exercise on a real kernel function:
        //   stage 3 - counting callback, passthrough: every open must
        //             SUCCEED (proves the callback stub's restore path)
        //   stage 4 - override callback: cmd.exe opens must FAIL with
        //             STATUS_ACCESS_DENIED (body never runs) while other
        //             files still open (passthrough path of the same stub)
        UNICODE_STRING fnName;
        RtlInitUnicodeString(&fnName, L"NtCreateFile");
        void* target = MmGetSystemRoutineAddress(&fnName);
        if (!target)
            status = STATUS_NOT_FOUND;

        u32 perCore = p->Iterations ? p->Iterations : 4;
        g_svmb_hook_hits = 0;
        u64 exitBefore = hv->TotalExitCount();

        u32 hookId = 0;
        SvmbHookCallback cb =
            (p->Stage == 3) ? SvmbNtHookCountCb : SvmbNtHookDenyCmdCb;
        if (NT_SUCCESS(status))
            status = hooks->InstallCallback((u64)target, hookId, cb);
        if (NT_SUCCESS(status))
        {
            const NptHook* h = hooks->FindHook((u64)target);
            if (h)
            {
                p->TrampPa = h->TrampPa;
                p->StolenLen = h->StolenLen;
                RtlCopyMemory(p->TrampBytes, (void*)h->TrampVa,
                              sizeof(p->TrampBytes));
            }
            else
            {
                status = STATUS_NOT_FOUND;
            }
            p->HookId = hookId;
        }
        if (NT_SUCCESS(status))
        {
            volatile LONG okCount = 0;
            volatile LONG deniedCount = 0;
            volatile LONG otherOk = 0;
            NTSTATUS lastStatus = STATUS_SUCCESS;
            RunOnEachCore([&](u32) -> NTSTATUS {
                for (u32 i = 0; i < perCore; ++i)
                {
                    HANDLE fh = nullptr;
                    NTSTATUS st = ProbeOpenSystemFile(&fh);
                    if (NT_SUCCESS(st))
                    {
                        ZwClose(fh);
                        InterlockedIncrement(&okCount);
                    }
                    else
                    {
                        lastStatus = st;
                        if (st == STATUS_ACCESS_DENIED)
                            InterlockedIncrement(&deniedCount);
                    }
                    if (p->Stage == 4)
                    {
                        // passthrough control: a DIFFERENT real file must
                        // still open through the same hook
                        HANDLE fh2 = nullptr;
                        NTSTATUS st2 = ProbeOpenFile(
                            L"\\SystemRoot\\System32\\notepad.exe", &fh2);
                        if (NT_SUCCESS(st2))
                        {
                            ZwClose(fh2);
                            InterlockedIncrement(&otherOk);
                        }
                    }
                }
                return STATUS_SUCCESS;
            });
            p->OpenOk = (u32)okCount;
            p->HookHits = g_svmb_hook_hits;
            p->LastStatus = (u32)lastStatus;
            p->OvDeniedOk = (u32)deniedCount;
            p->OvOtherOk = (u32)otherOk;
            p->TargetVa = (u64)target;

            // remove, then one sanity open through the unhooked original
            NTSTATUS stR = hooks->Remove((u64)target);
            if (!NT_SUCCESS(stR))
                status = stR;

            HANDLE fh = nullptr;
            NTSTATUS stPost = ProbeOpenSystemFile(&fh);
            if (NT_SUCCESS(stPost))
                ZwClose(fh);
            p->OrigHitsAfter = NT_SUCCESS(stPost) ? 1 : 0;
            p->ExitDelta = hv->TotalExitCount() - exitBefore;
            SVMB_LOGI("nthook s%u: target=%llx id=%u hits=%llu openOk=%lu "
                      "denied=%lu otherOk=%lu last=%08x postOpen=%08x "
                      "exitDelta=%llu",
                      p->Stage, (u64)p->TargetVa, hookId, (u64)p->HookHits,
                      p->OpenOk, p->OvDeniedOk, p->OvOtherOk, p->LastStatus,
                      (u32)stPost, (u64)p->ExitDelta);
        }
    }
    else if (p->Stage == 5)
    {
        // r37 M4-B: return THIS thread's CR3 as the guest sees it. With
        // cr3_monitor attached and a watch armed on this process, the
        // CR3-read exit handler spoofs the value (Cr3ResolveRead targeted
        // branch) - the ctl compares pre/post watch values against the xor
        // key to prove the spoof end-to-end.
        p->Cr3Value = __readcr3();
        status = STATUS_SUCCESS;
    }
    else if (p->Stage == 6)
    {
        // r37 M4-D: #UD face end-to-end. With the debugger module attached,
        // a kernel __ud2() exits to the VMM (event pushed to the dbg ring,
        // transparently reinjected at the same rip), then the guest's own
        // exception dispatch delivers it to this __except - proving the
        // guest's handling survives the interception.
        //
        // DELIBERATELY NOT int3: the guest's kernel debug is always enabled
        // (KDNET is our forensic channel) - an int3 in kernel mode with
        // debug enabled but no debugger connected waits for a debugger
        // connection FOREVER (r37 hang). #UD is not a debug exception: no
        // kd involvement, SEH catches it directly.
        DbgEventRing* ring = DbgRingInstance();
        u32 before = ring ? ring->Count() : 0;
        LONG r = 0;
        __try
        {
            __ud2();
            r = 1; // unreached: ud2 raises before this line
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            r = GetExceptionCode();
        }
        u32 after = ring ? ring->Count() : 0;
        // r40: in-band record of the ring counters - the r39 004a run showed
        // Push-executed (kd log) but after==0; unreproducible since arming
        // is NMI-lethal on this vhv, so the counters ride the kd-visible
        // stream for the next bare-metal armed test.
        SVMB_LOGW("nptprobe s6: exc=%08x ring before=%u after=%u lost=%u",
                  (u32)r, before, after, ring ? ring->LostCount() : 0);
        p->OvDeniedOk = before;
        p->OvOtherOk = after;
        p->Cr3Value = (u64)r;
        status = STATUS_SUCCESS;
    }
    else if (p->Stage == 7)
    {
        // r41 M4-E: DR-hiding face. Plain kernel DR reads - without DR
        // intercepts these return the real registers; with the debugger
        // module's DR face armed (DBG_CONFIG HideDr/SpoofDr7Zero) they
        // exit and resolve through the shadow/spoof (HandleDrRead).
        // out: Cr3Value=DR7, OvDeniedOk/OvOtherOk = DR0 lo/hi.
        u64 dr7 = _svmb_read_dr7();
        u64 dr0 = _svmb_read_dr0();
        p->Cr3Value = dr7;
        p->OvDeniedOk = (u32)dr0;
        p->OvOtherOk = (u32)(dr0 >> 32);
        SVMB_LOGW("nptprobe s7: dr7=%llx dr0=%llx",
                  (unsigned long long)dr7, (unsigned long long)dr0);
        status = STATUS_SUCCESS;
    }
    else if (p->Stage == 8)
    {
        // r42 M4-F: in-target CR3 spoof self-check. `seen` is what THIS
        // thread's __readcr3() returns (an intercepted read - possibly the
        // targeted xor spoof); `truth` is the real CR3 straight from the
        // local core's VMCB save area (plain memory, no exit). Watched:
        // seen == truth ^ key. Unwatched: seen == truth.
        u64 seen = __readcr3();
        Hypervisor* hv8 = Hypervisor::Instance();
        u64 truth = 0;
        if (hv8)
        {
            VcpuContext* vcpu = hv8->Vcpu(CurrentCpuIndex());
            if (vcpu && vcpu->GuestVmcb)
                truth = vcpu->GuestVmcb->Save.Cr3;
        }
        p->Cr3Value = seen; // out: guest-visible CR3
        p->VmcbNcr3 = truth; // out: VMCB truth
        SVMB_LOGW("nptprobe s8: seen=%llx truth=%llx",
                  (unsigned long long)seen, (unsigned long long)truth);
        status = STATUS_SUCCESS;
    }
    // r43 wedge bisection variants for stage 8 (wedges only with
    // cr3_monitor attached): 9 = __readcr3 only, 10 = Vcpu/VMCB deref only,
    // 11 = LOGW only. Each isolates one of stage-8's three operations.
    else if (p->Stage == 9)
    {
        p->Cr3Value = __readcr3();
        status = STATUS_SUCCESS;
    }
    else if (p->Stage == 10)
    {
        u64 truth = 0;
        Hypervisor* hv10 = Hypervisor::Instance();
        if (hv10)
        {
            VcpuContext* vcpu = hv10->Vcpu(CurrentCpuIndex());
            if (vcpu && vcpu->GuestVmcb)
                truth = vcpu->GuestVmcb->Save.Cr3;
        }
        p->VmcbNcr3 = truth;
        status = STATUS_SUCCESS;
    }
    else if (p->Stage == 11)
    {
        SVMB_LOGW("nptprobe s11: logw-only variant");
        status = STATUS_SUCCESS;
    }
    else if (p->Stage == 12)
    {
        // r44 N2: deterministic unresolvable-NPF construction. NX the probe
        // target's 4K page in the NPT view directly (no hook owns it), then
        // call the target - the fetch NPFs into the deny path:
        //   Debug:  spin-breaker (256 re-executes) then AdvanceRip - the
        //           target's prologue is skipped, the probe survives
        //   Release: fail-fast bugcheck 'SVMB'+1 (0x53564D43) - this stage
        //           never returns on a production build
        u64 va = (u64)&SvmbNpfProbeTarget;
        u64 pageVa = va & ~0xFFFull;
        PHYSICAL_ADDRESS pa = MmGetPhysicalAddress((void*)pageVa);
        u64 gpa4k = pa.QuadPart & ~0xFFFull;
        NptView* view = npt->Active();

        NTSTATUS st = view->MapRange(gpa4k, 0x1000, {true, true, true});
        if (!NT_SUCCESS(st))
        {
            p->TargetVa = st; // split failure
            status = st;
        }
        else
        {
            st = view->SetPerm4k(gpa4k, {true, false, true}); // W+NX+US
            // r45 fix: {true,true,false} was W+X+U0 (NptPerms is
            // {Write, Execute, User}) - it never armed NX; the U0 flip alone
            // fired DenyUser on this vhv, not the designed DenyExecute.
            if (!NT_SUCCESS(st))
            {
                p->TargetVa = st;
                status = st;
            }
            else
            {
                ForceFlushExit();
                u64 exitBefore = hv->TotalExitCount();
                SvmbNpfProbeTarget(0x5A5A); // fetch NPFs -> deny path
                p->ExitDelta = hv->TotalExitCount() - exitBefore;
                p->TargetVa = va;
                p->DetourHits = 1; // marker: probe survived (Debug policy)
                (void)view->SetPerm4k(gpa4k, {true, true, true}); // restore
                ForceFlushExit();
                SVMB_LOGW("nptprobe s12: deny exercised exitDelta=%llu",
                          (unsigned long long)p->ExitDelta);
                status = STATUS_SUCCESS;
            }
        }
    }
    else if (p->Stage == 13)
    {
        // r45 N2 rework. The r44 stage-12 NX'd the probe target's 4K code
        // page - which it shares with RunNptProbe and whatever the linker
        // packs next to it. The Debug deny policy (NPF_SPIN_BREAK
        // re-executes, then advance) then skipped one instruction of EVERY
        // thread crossing that page: lock releases and IRP completion went
        // missing and the guest froze before the probe could print. Stage-13
        // NXes an ISOLATED executable pool page holding a 1-byte RET stub,
        // so the only code that can hit the deny path is the stub itself:
        //   Debug:  return with exitDelta == 1 + NPF_SPIN_BREAK (257)
        //   Release: the first deny bugchecks 'SVMB'+1 - never returns
        u8* stub = (u8*)AllocExecPage(TAG_SVMB);
        if (!stub)
        {
            p->TargetVa = (u64)(ULONG)STATUS_INSUFFICIENT_RESOURCES;
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
        stub[0] = 0xC3; // RET

        PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(stub);
        u64 gpa4k = pa.QuadPart & ~0xFFFull;
        NptView* view = npt->Active();

        NTSTATUS st = view->MapRange(gpa4k, 0x1000, {true, true, true});
        if (NT_SUCCESS(st))
            st = view->SetPerm4k(gpa4k, {true, false, true}); // W+NX+US
            // r45 fix: {true,true,false} was W+X+U0 (NptPerms is
            // {Write, Execute, User}) - it never armed NX; the U0 flip alone
            // fired DenyUser on this vhv, not the designed DenyExecute.
        if (!NT_SUCCESS(st))
        {
            p->TargetVa = st; // split/perm failure
            status = st;
        }
        else
        {
            ForceFlushExit();
            u64 exitBefore = hv->TotalExitCount();
            auto fn = (u64(*)(u64))stub;
            p->DetourHits = fn(0x5A5A); // garbage arg; stub returns at once
            p->ExitDelta = hv->TotalExitCount() - exitBefore;
            p->TargetVa = (u64)stub;
            (void)view->SetPerm4k(gpa4k, {true, true, true}); // restore
            ForceFlushExit();
            SVMB_LOGW("nptprobe s13: isolated deny exercised exitDelta=%llu",
                      (unsigned long long)p->ExitDelta);
            status = STATUS_SUCCESS;
        }
        FreeExecPage(stub, TAG_SVMB);
        }
    }
    else if (p->Stage == 14)
    {
        // r46 Route-A sentinel primitive. A W-denied NPT leaf must raise a
        // write-NPF per arm; the r45 resolve-in-place deny policy then lets
        // the write land for real. Trip 1 exercises spin-break + resolve
        // (fingerprint 1 + NPF_SPIN_BREAK); trip 2 verifies RE-ARM after a
        // resolve - the global spin budget is already spent, so the second
        // storm resolves immediately (1 exit). Both read-backs prove the
        // writes committed. This is the exact permission primitive the r47
        // per-process sentinels (watched thread kernel-stack pages) use.
        u64* page = (u64*)AllocNonPaged(PAGE_SIZE, TAG_SVMB);
        if (!page)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            p->TargetVa = status;
        }
        else
        {
            PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(page);
            u64 gpa4k = pa.QuadPart & ~0xFFFull;
            NptView* view = npt->Active();

            NTSTATUS st = view->MapRange(gpa4k, 0x1000, {true, true, true});
            if (NT_SUCCESS(st))
                st = view->SetPerm4k(gpa4k, {false, true, true}); // deny W
            if (!NT_SUCCESS(st))
            {
                status = st;
                p->TargetVa = st;
            }
            else
            {
                ForceFlushExit();
                u64 exitBefore = hv->TotalExitCount();
                page[0] = 0x5A5A5A5A5A5A5A5Aull; // write-NPF -> DenyWrite
                p->ExitDelta = hv->TotalExitCount() - exitBefore;
                p->DetourHits = page[0]; // resolve committed the write?

                st = view->SetPerm4k(gpa4k, {false, true, true}); // re-arm
                ForceFlushExit();
                u64 before2 = hv->TotalExitCount();
                page[1] = 0xA5A5A5A5A5A5A5A5ull;
                p->OrigHitsHooked = hv->TotalExitCount() - before2;
                p->OrigHitsAfter = page[1];

                (void)view->SetPerm4k(gpa4k, {true, true, true}); // restore
                ForceFlushExit();
                p->TargetVa = (u64)page;
                p->PagePa = gpa4k;
                SVMB_LOGW("nptprobe s14: trip1=%llu b0=%llx trip2=%llu "
                          "b1=%llx",
                          (unsigned long long)p->ExitDelta,
                          (unsigned long long)p->DetourHits,
                          (unsigned long long)p->OrigHitsHooked,
                          (unsigned long long)p->OrigHitsAfter);
                status = NT_SUCCESS(st) ? STATUS_SUCCESS : st;
            }
            FreeNonPaged(page, TAG_SVMB, PAGE_SIZE);
        }
    }
    else if (p->Stage == 15)
    {
        // r53 per-core process view verification. The ctl watches ITSELF
        // (raw CR3 target + EnableProcessView), so this thread's core
        // publishes the ProcView NCr3 on the next exit - which the forced
        // CPUID provides. Read-back proves the per-core publish landed;
        // the caller repeats the probe after unwatch to see the base PML4.
        ForceFlushExit(); // one exit -> the policy publishes for this core
        VcpuContext* vcpu = hv->Vcpu(CurrentCpuIndex());
        p->VmcbNcr3 = vcpu ? vcpu->GuestVmcb->Ctrl.NCr3 : 0;
        p->Pml4Pa = npt->Active() ? npt->Active()->Pml4Pa() : 0; // base
        p->PagePa = Hypervisor::ViewProcPml4();                  // proc
        p->TargetVa = Hypervisor::ViewTargetCr3();
        SVMB_LOGW("nptprobe s15: ncr3=%llx base=%llx proc=%llx target=%llx",
                  (unsigned long long)p->VmcbNcr3,
                  (unsigned long long)p->Pml4Pa,
                  (unsigned long long)p->PagePa,
                  (unsigned long long)p->TargetVa);
        status = STATUS_SUCCESS;
    }
    else if (p->Stage == 16)
    {
        // r54 ProcView content differentiation - the point of the per-core
        // view. A temp process view swaps the probe buffer's GPA at a
        // hidden pool page; while THIS thread is the policy target, reads
        // through the swapped GPA serve the hidden content, and after the
        // policy reverts they serve the real page again. Proves the view
        // isolation carries real content semantics, not just a different
        // NCr3 number.
        constexpr UCHAR REAL_FILL = 0x11;
        constexpr UCHAR HIDDEN_FILL = 0x22;
        u64* buf = (u64*)AllocNonPaged(PAGE_SIZE, TAG_SVMB);
        u64* hide = (u64*)AllocNonPaged(PAGE_SIZE, TAG_SVMB);
        if (!buf || !hide)
        {
            if (buf) FreeNonPaged(buf, TAG_SVMB, PAGE_SIZE);
            if (hide) FreeNonPaged(hide, TAG_SVMB, PAGE_SIZE);
            status = STATUS_INSUFFICIENT_RESOURCES;
            p->TargetVa = status;
        }
        else
        {
            RtlFillMemory(buf, PAGE_SIZE, REAL_FILL);
            RtlFillMemory(hide, PAGE_SIZE, HIDDEN_FILL);
            PHYSICAL_ADDRESS bpa = MmGetPhysicalAddress(buf);
            u64 bgpa = bpa.QuadPart & ~0xFFFull;
            PHYSICAL_ADDRESS hpa = MmGetPhysicalAddress(hide);
            u64 hpfn = hpa.QuadPart >> 12;
            SVMB_LOGW("nptprobe s16: bgpa=%llx hpfn=%llx", bgpa, hpfn);

            NptManager* npt16 = NptInstance();
            u32 viewId = 0;
            NTSTATUS st = npt16 ? npt16->CreateView(viewId) :
                                STATUS_DEVICE_NOT_READY;
            NptView* procView = NT_SUCCESS(st) ? npt16->View(viewId) : nullptr;
            if (NT_SUCCESS(st) && procView)
                st = npt16->FillView(*procView);
            SVMB_LOGW("nptprobe s16: fill st=%08x", st);
            if (NT_SUCCESS(st) && procView)
                st = procView->MapRange(bgpa, 0x1000, {true, true, true});
            SVMB_LOGW("nptprobe s16: map st=%08x", st);
            if (NT_SUCCESS(st))
            {
                // r55: the split-state probe. NOT_SPLIT here means map's
                // boundary path did not clear PS; NOT_MAPPED means fill
                // never covered bgpa; success + pte means swap should pass.
                u64 pteDbg = 0;
                NTSTATUS stDbg = procView->GetPte4k(bgpa, pteDbg);
                SVMB_LOGW("nptprobe s16: pre-swap ptest=%08x pte=%llx",
                          stDbg, pteDbg);
            }
            if (NT_SUCCESS(st))
                st = procView->SwapPage4k(bgpa, hpfn, {true, true, true});
            SVMB_LOGW("nptprobe s16: swap st=%08x", st);
            if (!NT_SUCCESS(st))
            {
                SVMB_LOGW("nptprobe s16: setup fail st=%08x view=%u",
                          st, viewId);
                status = st;
                p->TargetVa = st;
                if (viewId) npt16->DestroyView(viewId);
            }
            else
            {
                // policy on: this thread's process is the target
                u64 ownCr3 = __readcr3();
                NptView* base = npt16->Active();
                Hypervisor::SetPerCoreView(ownCr3,
                                           procView->Pml4Pa(),
                                           base->Pml4Pa());
                ForceFlushExit(); // publish ProcView on this core
                u32 seenSwapped = *(volatile u32*)buf; // hidden page
                Hypervisor::SetPerCoreView(0, 0, base->Pml4Pa());
                ForceFlushExit(); // publish base back
                u32 seenReal = *(volatile u32*)buf; // real page

                p->DetourHits = seenSwapped; // hidden-view readback
                p->OrigHitsAfter = seenReal; // base-view readback
                p->PagePa = bgpa;
                p->TargetVa = (u64)buf;
                (void)npt16->DestroyView(viewId);
                FreeNonPaged(buf, TAG_SVMB, PAGE_SIZE);
                FreeNonPaged(hide, TAG_SVMB, PAGE_SIZE);
                status = STATUS_SUCCESS;
            }
        }
    }
    else if (p->Stage == 17)
    {
        // r57: production-path content differentiation. The viewtest-style
        // setup (self-watch + EnableProcessView) published the per-core
        // policy; swap the probe buffer's GPA to a hidden page INSIDE the
        // production ProcView (not a temp view like stage-16) and read
        // through both roots. While this thread is the policy target the
        // core runs the ProcView (hidden content); flipped to base it
        // sees the real page again.
        constexpr UCHAR REAL_FILL = 0x11;
        u64 procPml4 = Hypervisor::ViewProcPml4();
        u64 basePml4 = Hypervisor::ViewBasePml4();
        u64 targetCr3 = Hypervisor::ViewTargetCr3();
        NptManager* npt17 = NptInstance();
        NptView* procView =
            (npt17 && procPml4) ? npt17->ViewByPml4Pa(procPml4) : nullptr;
        SVMB_LOGW("nptprobe s17: procPml4=%llx basePml4=%llx target=%llx "
                  "view=%d", procPml4, basePml4, targetCr3,
                  procView ? 1 : 0);
        u64* buf = procView ? (u64*)AllocNonPaged(PAGE_SIZE, TAG_SVMB)
                            : nullptr;
        u64* hide = procView ? (u64*)AllocNonPaged(PAGE_SIZE, TAG_SVMB)
                             : nullptr;
        if (!buf || !hide)
        {
            if (buf) FreeNonPaged(buf, TAG_SVMB, PAGE_SIZE);
            if (hide) FreeNonPaged(hide, TAG_SVMB, PAGE_SIZE);
            status = STATUS_INVALID_PARAMETER;
            p->TargetVa = status;
        }
        else
        {
            RtlFillMemory(buf, PAGE_SIZE, REAL_FILL);
            RtlFillMemory(hide, PAGE_SIZE, 0x22);
            PHYSICAL_ADDRESS bpa = MmGetPhysicalAddress(buf);
            u64 bgpa = bpa.QuadPart & ~0xFFFull;
            PHYSICAL_ADDRESS hpa = MmGetPhysicalAddress(hide);
            u64 hpfn = hpa.QuadPart >> 12;

            NTSTATUS st = procView->MapRange(bgpa, 0x1000,
                                             {true, true, true});
            SVMB_LOGW("nptprobe s17: map st=%08x", st);
            if (NT_SUCCESS(st))
                st = procView->SwapPage4k(bgpa, hpfn, {true, true, true});
            SVMB_LOGW("nptprobe s17: swap st=%08x", st);
            if (NT_SUCCESS(st))
            {
                ForceFlushExit(); // publish ProcView on this core
                u32 seenHidden = *(volatile u32*)buf;
                Hypervisor::SetPerCoreView(0, 0, basePml4);
                ForceFlushExit(); // publish base back
                u32 seenReal = *(volatile u32*)buf;
                // restore the policy for this core, then undo the swap
                Hypervisor::SetPerCoreView(targetCr3, procPml4, basePml4);
                ForceFlushExit();
                (void)procView->SwapPage4k(bgpa, bgpa >> 12,
                                           {true, true, true});
                p->DetourHits = seenHidden;
                p->OrigHitsAfter = seenReal;
                p->PagePa = bgpa;
                SVMB_LOGW("nptprobe s17: hidden=%08x real=%08x",
                          seenHidden, seenReal);
                status = STATUS_SUCCESS;
            }
            else
            {
                status = st;
                p->TargetVa = st;
            }
            FreeNonPaged(buf, TAG_SVMB, PAGE_SIZE);
            FreeNonPaged(hide, TAG_SVMB, PAGE_SIZE);
        }
    }
    else if (p->Stage == 18)
    {
        // r165 L2 stage-0: nested-virtualization baseline. This IOCTL
        // thread runs at guest CPL0 - it plays "H", a hypervisor inside
        // svmb's guest. svmb arms ic2.VMRUN (anti-nesting, hypercall.cpp
        // HandleVmrunExit): the __svm_vmrun below must exit to the VMM
        // and come back as an injected #BP landing in this __except -
        // proving the rejection path is clean, the instruction is
        // skipped exactly once, and the guest survives. r37 law read in
        // reverse: kernel #BP only hangs when kernel debug is ON; this
        // bench's iron law keeps guest debug OFF during hv, so SEH
        // catches directly (checked before every run).
        // The leading probes archive TODAY's virtualization face as the
        // baseline the r166+ nested work reshapes: CPUID SVME visibility
        // (hideCpuid may mask it), EFER and VM_HSAVE_PA reads (both MSR
        // intercepts are armed - the values seen here are whatever the
        // protect handlers emulate).
        int regs[4] = {};
        __cpuidex(regs, 0x80000001, 0);
        u64 efer = 0;
        bool eferOk = false;
        __try { efer = __readmsr(0xC0000080); eferOk = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { eferOk = false; }
        u64 hsave = 0;
        bool hsaveOk = false;
        __try { hsave = __readmsr(0xC0010117); hsaveOk = true; }
        __except (EXCEPTION_EXECUTE_HANDLER) { hsaveOk = false; }
        // VMCB skeleton: 4K-aligned zeroed page. The intercept exit fires
        // before any VMCB validation/state load (APM: the VMRUN intercept
        // is checked before the VMCB is processed; QEMU raises the exit
        // before helper_vmrun). If the intercept were ever missing, this
        // zeroed state loads and the guest shuts down - itself a verdict.
        void* raw = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
        if (!raw)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            u64 va = ((u64)(ULONG_PTR)raw + 0xFFFull) & ~0xFFFull;
            RtlZeroMemory((void*)va, PAGE_SIZE);
            u64 vmcbPa = MmGetPhysicalAddress((void*)va).QuadPart;
            LONG r = 0;
            __try
            {
                __svm_vmrun(vmcbPa);
                r = 1; // unreached while the intercept is armed
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                r = GetExceptionCode();
            }
            p->Cr3Value = (u64)(u32)r;             // VMRUN verdict
            p->Pml4Pa = vmcbPa;                     // VMCB PA used
            p->HiddenPa = efer;                     // EFER raw @ CPL0
            p->HookHits = hsave;                    // HSAVE raw @ CPL0
            p->OvDeniedOk = (u32)!!(regs[2] & 0x4); // CPUID SVME bit
            p->OvOtherOk = (u32)((eferOk ? 1u : 0u) |
                                 (hsaveOk ? 2u : 0u));
            SVMB_LOGW("nptprobe s18: exc=%08x vmcbPa=%llx svme=%u "
                      "efer=%llx(%u) hsave=%llx(%u)",
                      (u32)r, (unsigned long long)vmcbPa,
                      (u32)!!(regs[2] & 0x4),
                      (unsigned long long)efer, (u32)eferOk,
                      (unsigned long long)hsave, (u32)hsaveOk);
            FreeNonPaged(raw, TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_SUCCESS;
        }
    }
    else if (p->Stage == 19)
    {
        // r166 L2 stage-1: the full nested round trip. This thread plays
        // H: build L2's world (4-level page tables mapping one code page
        // + one stack page, VMCB with CPUID intercepted), VMRUN, and the
        // verdict comes from H's VMCB after control RETURNS here:
        //   - no exception (svmb accepted + reflected instead of #BP)
        //   - H_vmcb.ExitCode == 0x72 (L2's cpuid intercept)
        //   - H_vmcb.Save.Rip == the cpuid VA (fault-style exit rip)
        //   - the L2 Rax marker survived the round trip in Save
        // v1 skeleton-inherit contract: svmb fills segs/CR0/CR4/EFER from
        // L1; H supplies Rip/Rsp/Rflags/Rax/Cr3 + intercept bitmaps only.
        // L2 code: cpuid (exit) then hlt (never reached).
        constexpr u64 L2_CODE_VA = 0x100000;
        constexpr u64 L2_STACK_TOP = 0x111000; // one page, grows down
        void* pages[8] = {};                   // vmcb, code, stack, pml4,
                                               // pdpt, pd, pt, spare
        bool ok = true;
        for (u32 i = 0; i < 7 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        if (!ok)
        {
            for (u32 i = 0; i < 7; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            u64* vmcb = pageAt(pages[0]);
            u64* code = pageAt(pages[1]);
            u64* stack = pageAt(pages[2]);
            u64* pml4 = pageAt(pages[3]);
            u64* pdpt = pageAt(pages[4]);
            u64* pd = pageAt(pages[5]);
            u64* pt = pageAt(pages[6]);
            u64 codePa = MmGetPhysicalAddress(code).QuadPart;
            u64 stackPa = MmGetPhysicalAddress(stack).QuadPart;
            u64 pml4Pa = MmGetPhysicalAddress(pml4).QuadPart;
            u64 pdptPa = MmGetPhysicalAddress(pdpt).QuadPart;
            u64 pdPa = MmGetPhysicalAddress(pd).QuadPart;
            u64 ptPa = MmGetPhysicalAddress(pt).QuadPart;

            // L2 identity-ish map: VA [0, 2MB) -> the specific frames we
            // own (P|RW, supervisor; NX off). Only the two VAs we use.
            pml4[0] = pdptPa | 0x3;
            pdpt[0] = pdPa | 0x3;
            pd[0] = ptPa | 0x3;
            pt[(L2_CODE_VA >> 12) & 0x1FF] = codePa | 0x3;
            pt[(L2_STACK_TOP >> 12) & 0x1FF] = stackPa | 0x3;

            // L2 program: cpuid (0F A2, exits via the intercept) + hlt
            static const u8 l2code[] = {0x0F, 0xA2, 0xF4};
            RtlCopyMemory(code, l2code, sizeof(l2code));

            VMCB* hv = (VMCB*)vmcb;
            RtlZeroMemory(hv, PAGE_SIZE);
            hv->Ctrl.InterceptOpcode1 = (1u << 18) | (1u << 24); // ic1 bit18 = CPUID
            hv->Save.Rip = L2_CODE_VA;
            hv->Save.Rsp = L2_STACK_TOP - 8;
            hv->Save.Rflags = 0x202; // IF | reserved1
            hv->Save.Rax = 0xA5A51122; // marker: must survive in Save
            hv->Save.Cr3 = pml4Pa;
            u64 hvPa = MmGetPhysicalAddress(hv).QuadPart;

            LONG r = 0;
            u64 canaryBefore = 0x5A5A00FF;
            __try
            {
                __svm_vmrun(hvPa);
                r = 0; // control returned: the nested round trip happened
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                r = GetExceptionCode(); // rejection/validation failure path
            }

            p->Cr3Value = (u64)(u32)r;          // 0 = returned (accept path)
            p->Pml4Pa = hvPa;                   // H's VMCB PA
            p->HiddenPa = hv->Ctrl.ExitCode;    // reflected exit code
            p->HookHits = hv->Save.Rip;         // reflected L2 rip
            p->PagePa = hv->Save.Rax;           // L2 marker survival
            p->OvDeniedOk = (u32)(canaryBefore == 0x5A5A00FF);
            p->OvOtherOk = (u32)(hv->Save.Cr3 == pml4Pa);
            SVMB_LOGW("nptprobe s19: exc=%08x exit=%llx L2rip=%llx "
                      "L2rax=%llx",
                      (u32)r, (unsigned long long)hv->Ctrl.ExitCode,
                      (unsigned long long)hv->Save.Rip,
                      (unsigned long long)hv->Save.Rax);
            for (u32 i = 0; i < 7; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_SUCCESS;
        }
    }
    else if (p->Stage == 20)
    {
        // r167 L2 stage-2: the reflection gauntlet. L2 runs a five-exit
        // program; H loops VMRUN -> reflect -> advance-L2 like a real
        // nested hypervisor, exercising repeated round trips (stash/restore
        // drift), exception reflection (#UD), and the full exit record
        // (#PF must carry ExitInfo2 == fault VA). svmb force-reflects all
        // L2 exceptions + INTR while nested (the r167 safety net - the
        // skeleton-inherited L1 IDTR must never dispatch L2 state), so a
        // 0x60 intr exit may appear any round: H retries without advance.
        //   rip=0x100000: cpuid  (2)  -> exit 0x72
        //   rip=0x100002: hlt     (1)  -> exit 0x78
        //   rip=0x100008: rdmsr   (2)  -> exit 0x7C (ecx=EFER, in svmb's
        //                                  protected MSRPM)
        //   rip=0x10000A: ud2     (2)  -> exit 0x46 (exception reflect)
        //   rip=0x10000C: mov rax,[0x300000] (10) -> exit 0x4E, a #PF on
        //                                  the unmapped VA - ExitInfo2
        //                                  must equal 0x300000
        //   rip=0x100016: mov qword [0x112000], 0x5A5AF00D (11, no exit)
        //   rip=0x100021: hlt terminal -> marker read back via the
        //                                  direct PA proves the L2 store
        //                                  executed in L2's own CR3 world
        constexpr u64 L2_CODE_VA = 0x100000;
        constexpr u64 L2_MARK_VA = 0x112000;
        void* pages[9] = {}; // vmcb, code, stack, pml4, pdpt, pd, pt, mark
        bool ok = true;
        for (u32 i = 0; i < 8 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        if (!ok)
        {
            for (u32 i = 0; i < 8; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            u64* vmcb = pageAt(pages[0]);
            u64* code = pageAt(pages[1]);
            u64* stack = pageAt(pages[2]);
            u64* pml4 = pageAt(pages[3]);
            u64* pdpt = pageAt(pages[4]);
            u64* pd = pageAt(pages[5]);
            u64* pt = pageAt(pages[6]);
            u64* mark = pageAt(pages[7]);
            u64 codePa = MmGetPhysicalAddress(code).QuadPart;
            u64 stackPa = MmGetPhysicalAddress(stack).QuadPart;
            u64 pml4Pa = MmGetPhysicalAddress(pml4).QuadPart;
            u64 pdptPa = MmGetPhysicalAddress(pdpt).QuadPart;
            u64 pdPa = MmGetPhysicalAddress(pd).QuadPart;
            u64 ptPa = MmGetPhysicalAddress(pt).QuadPart;
            u64 markPa = MmGetPhysicalAddress(mark).QuadPart;

            // L2 map: code/stack/marker pages only - VA 0x300000 stays
            // unmapped on purpose (the #PF leg)
            pml4[0] = pdptPa | 0x3;
            pdpt[0] = pdPa | 0x3;
            pd[0] = ptPa | 0x3;
            pt[(L2_CODE_VA >> 12) & 0x1FF] = codePa | 0x3;
            pt[0x111] = stackPa | 0x3;              // VA 0x111000
            pt[(L2_MARK_VA >> 12) & 0x1FF] = markPa | 0x3;

            static const u8 l2code[] = {
                0x0F, 0xA2,                                   // cpuid
                0xF4,                                         // hlt
                0xB9, 0x80, 0x00, 0x00, 0xC0,                 // mov ecx,EFER
                0x0F, 0x32,                                   // rdmsr
                0x0F, 0x0B,                                   // ud2
                0x48, 0xA1, 0x00, 0x00, 0x30, 0x00, 0x00, 0x00,
                0x00, 0x00,                                   // mov rax,[0x300000]
                0x48, 0xC7, 0x04, 0x25, 0x00, 0x20, 0x11, 0x00,
                0x0D, 0xF0, 0x5A, 0x5A,                       // mov [mark],imm
                0xF4,                                         // hlt terminal
            };
            RtlCopyMemory(code, l2code, sizeof(l2code));

            VMCB* hv = (VMCB*)vmcb;
            RtlZeroMemory(hv, PAGE_SIZE);
            // r186b: rdmsr leg (0x7C) must be H-ordered too - the intent
            // gate reflects only what H armed, and the rdmsr exit comes
            // from svmb's baseline bit, not from H's request
            hv->Ctrl.InterceptOpcode1 = (1u << 18) | (1u << 24) | (1u << 28);
            hv->Save.Rip = L2_CODE_VA;
            hv->Save.Rsp = 0x111FF8;                // stack page top - 8
            hv->Save.Rflags = 0x202;
            hv->Save.Rax = 0x13579BDF;
            hv->Save.Cr3 = pml4Pa;
            u64 hvPa = MmGetPhysicalAddress(hv).QuadPart;

            struct Leg { u64 code; u64 rip; u32 adv; };
            static const Leg legs[] = {
                {0x72, 0x100000, 2},
                {0x78, 0x100002, 1},
                {0x7C, 0x100008, 2},
                {0x46, 0x10000A, 2},
                {0x4E, 0x10000C, 10},
                {0x78, 0x100022, 0}, // terminal (mov imm is 12 bytes)
            };
            constexpr u32 NLEGS = sizeof(legs) / sizeof(legs[0]);

            LONG r = 0;
            u32 completed = 0, intrSeen = 0;
            u64 mismatchExit = 0, pfAddr = 0;
            for (u32 guard = 0; guard < 32 && completed < NLEGS; ++guard)
            {
                __try
                {
                    __svm_vmrun(hvPa);
                    r = 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    r = GetExceptionCode();
                    break;
                }
                u64 ec = hv->Ctrl.ExitCode;
                if (ec == 0x60) // reflected interrupt: retry, no advance
                {
                    ++intrSeen;
                    continue;
                }
                if (ec == 0x4E)
                    pfAddr = hv->Ctrl.ExitInfo2;
                if (ec != legs[completed].code ||
                    hv->Save.Rip != legs[completed].rip)
                {
                    mismatchExit = ec; // rip mismatch recorded via legs
                    if (ec == legs[completed].code)
                        mismatchExit = ec | 0x100000000ull; // rip flag
                    break;
                }
                ++completed;
                if (completed == NLEGS)
                    break; // terminal hlt reached
                hv->Save.Rip += legs[completed - 1].adv;
            }

            p->Cr3Value = (u64)(u32)r;      // 0 = every VMRUN returned
            p->Pml4Pa = hvPa;
            p->HiddenPa = mismatchExit;     // 0 = all legs matched
            p->HookHits = pfAddr;           // expect 0x300000
            p->PagePa = mark[0];            // expect 0x5A5AF00D
            p->OvDeniedOk = completed;      // expect 6
            p->OvOtherOk = intrSeen;        // informational
            SVMB_LOGW("nptprobe s20: exc=%08x legs=%u/%u mm=%llx "
                      "pf=%llx mark=%llx intr=%u",
                      (u32)r, completed, (u32)NLEGS,
                      (unsigned long long)mismatchExit,
                      (unsigned long long)pfAddr,
                      (unsigned long long)mark[0], intrSeen);
            for (u32 i = 0; i < 8; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_SUCCESS;
        }
    }
    else if (p->Stage == 21)
    {
        // r168 L2 stage-3: the EVENTINJ pipeline. H builds L2 a real IDT
        // (r168 contract: a nonzero Idtr overrides the skeleton), runs a
        // two-exit program, and after the first exit ARMS H_vmcb's
        // EVENTINJ with external interrupt vector 0x31 - svmb must
        // transfer it into the real VMCB (hardware-consumed semantics,
        // Valid cleared in H's copy), QEMU delivers it into L2 on
        // re-entry, L2's stub bumps a marker and iretq's back, and the
        // program runs to the terminal hlt. Proves: EVENTINJ passthrough
        // + IDTR override + injected-event delivery inside L2 + the
        // injected event NOT re-exiting through the forced exception
        // bitmap (the M1 #BP precedent, now at the L2 layer).
        //   rip=0x100000: cpuid (2)  -> 0x72; H advances + arms inject
        //   rip=0x100002: hlt    (1)  -> 0x78 [vector 0x31 -> stub+iretq]
        //   rip=0x100003: mov qword [0x113000], 0x5A5AF00D (12, no exit)
        //   rip=0x10000F: hlt terminal -> 0x78; mark1 set, mark2 == 1
        //   stub @0x100020: inc qword [0x114000]; iretq
        constexpr u64 L2_CODE_VA = 0x100000;
        constexpr u64 L2_IDT_VA = 0x112000;
        constexpr u64 L2_MARK1_VA = 0x113000;
        constexpr u64 L2_MARK2_VA = 0x114000;
        void* pages[12] = {}; // vmcb, code, stack, pml4, pdpt, pd, pt,
                              // idt, mark1, mark2
        bool ok = true;
        for (u32 i = 0; i < 10 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        if (!ok)
        {
            for (u32 i = 0; i < 10; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            u64* vmcb = pageAt(pages[0]);
            u64* code = pageAt(pages[1]);
            u64* stack = pageAt(pages[2]);
            u64* pml4 = pageAt(pages[3]);
            u64* pdpt = pageAt(pages[4]);
            u64* pd = pageAt(pages[5]);
            u64* pt = pageAt(pages[6]);
            u64* idt = pageAt(pages[7]);
            u64* mark1 = pageAt(pages[8]);
            u64* mark2 = pageAt(pages[9]);
            u64 codePa = MmGetPhysicalAddress(code).QuadPart;
            u64 stackPa = MmGetPhysicalAddress(stack).QuadPart;
            u64 pml4Pa = MmGetPhysicalAddress(pml4).QuadPart;
            u64 pdptPa = MmGetPhysicalAddress(pdpt).QuadPart;
            u64 pdPa = MmGetPhysicalAddress(pd).QuadPart;
            u64 ptPa = MmGetPhysicalAddress(pt).QuadPart;
            u64 idtPa = MmGetPhysicalAddress(idt).QuadPart;
            u64 mark1Pa = MmGetPhysicalAddress(mark1).QuadPart;
            u64 mark2Pa = MmGetPhysicalAddress(mark2).QuadPart;

            pml4[0] = pdptPa | 0x3;
            pdpt[0] = pdPa | 0x3;
            pd[0] = ptPa | 0x3;
            pt[0x100] = codePa | 0x3;   // VA 0x100000
            pt[0x111] = stackPa | 0x3;  // VA 0x111000
            pt[0x112] = idtPa | 0x3;    // VA 0x112000
            pt[0x113] = mark1Pa | 0x3;  // VA 0x113000
            pt[0x114] = mark2Pa | 0x3;  // VA 0x114000

            static const u8 l2code[] = {
                0x0F, 0xA2,                                   // cpuid
                0xF4,                                         // hlt
                0x48, 0xC7, 0x04, 0x25, 0x00, 0x30, 0x11, 0x00,
                0x0D, 0xF0, 0x5A, 0x5A,                       // mov [mark1]
                0xF4,                                         // hlt terminal
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                // stub @ +0x20: inc qword [rip+rel32]; iretq
                0x48, 0xFF, 0x05, 0, 0, 0, 0,
                0xCF,
            };
            u64 stubVa = L2_CODE_VA + 0x20;
            // rel32 for the inc: target - next-rip (7 bytes).
            // l2code is .rdata (const) - patching rel32 in place was the
            // r168 bugcheck 0xBE (write-to-readonly, guest reboot-loop):
            // stage the bytes in a stack buffer first
            u8 l2img[64] = {};
            RtlCopyMemory(l2img, l2code, sizeof(l2code));
            *(i32*)(l2img + 0x20 + 3) = (i32)(L2_MARK2_VA - (stubVa + 7));
            RtlCopyMemory(code, l2img, sizeof(l2code));

            // IDT gate for vector 0x31: 16-byte interrupt gate, the
            // skeleton's CS selector (same GDT), P=1 DPL=0 type=0xE.
            // x64 gate layout: +0 offLo +2 sel +4 IST +6 ATTR +8 offHi32
            u16 cs = _svmb_cs();
            u8* gate = (u8*)idt + 0x31 * 16;
            u16* g16 = (u16*)gate;
            *(u64*)(gate + 8) = 0;
            g16[0] = (u16)stubVa;               // +0: offset 15:0
            g16[1] = cs;                        // +2: selector
            g16[2] = 0x8E00;                    // +4: byte5 = P|DPL0|type E
                                                // (gate attr byte is +5 -
                                                // QEMU do_interrupt64 reads
                                                // (e2>>8)&0x1f; +6 cost this
                                                // round as #GP(intno*8+2))
            *(u32*)(gate + 8) = (u32)(stubVa >> 32); // +8: offset 63:32

            VMCB* hv = (VMCB*)vmcb;
            RtlZeroMemory(hv, PAGE_SIZE);
            hv->Ctrl.InterceptOpcode1 = (1u << 18) | (1u << 24); // ic1 bit18 = CPUID
            hv->Save.Rip = L2_CODE_VA;
            hv->Save.Rsp = 0x111FF8;
            hv->Save.Rflags = 0x202;
            hv->Save.Rax = 0x2468ACE0;
            hv->Save.Cr3 = pml4Pa;
            hv->Save.Idtr.Base = L2_IDT_VA;     // r168 override contract
            hv->Save.Idtr.Limit = 0x7FF;
            u64 hvPa = MmGetPhysicalAddress(hv).QuadPart;

            struct Leg { u64 code; u64 rip; u32 adv; };
            static const Leg legs[] = {
                {0x72, 0x100000, 2},
                {0x78, 0x100002, 1},
                {0x78, 0x10000F, 0}, // terminal
            };
            constexpr u32 NLEGS = 3;

            LONG r = 0;
            u32 completed = 0, intrSeen = 0;
            u64 mismatchExit = 0, mismatchRip = 0, mismatchInfo1 = 0;
            u64 mismatchInfo2 = 0;
            for (u32 guard = 0; guard < 32 && completed < NLEGS; ++guard)
            {
                __try
                {
                    __svm_vmrun(hvPa);
                    r = 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    r = GetExceptionCode();
                    break;
                }
                u64 ec = hv->Ctrl.ExitCode;
                if (ec == 0x60)
                {
                    ++intrSeen;
                    continue;
                }
                if (ec != legs[completed].code ||
                    hv->Save.Rip != legs[completed].rip)
                {
                    mismatchExit = ec;
                    if (ec == legs[completed].code)
                        mismatchExit = ec | 0x100000000ull;
                    mismatchRip = hv->Save.Rip;
                    mismatchInfo1 = hv->Ctrl.ExitInfo1;
                    mismatchInfo2 = hv->Ctrl.ExitInfo2;
                    break;
                }
                ++completed;
                if (completed == NLEGS)
                    break;
                hv->Save.Rip += legs[completed - 1].adv;
                if (completed == 2)
                {
                    // past the first hlt: arm the injection (svmb
                    // transfers it at the next accept)
                    EventInj inj = {};
                    inj.F.Vector = 0x31;
                    inj.F.Type = 4; // SOFT: QEMU type-0 INTR
                    // delivers IMMEDIATELY inside helper_vmrun with
                    // partially loaded state (r168: the delivery poked a
                    // stale Windows RSP -> #PF at a kernel VA); type-4
                    // rides cpu_loop_exit - delivered at the next
                    // instruction boundary with full L2 state
                    inj.F.Valid = 1;
                    hv->Ctrl.EventInj = inj;
                }
            }

            p->Cr3Value = (u64)(u32)r;
            p->Pml4Pa = hvPa;
            p->HiddenPa = mismatchExit;
            p->HookHits = mark2[0];             // expect 1 (stub ran)
            p->PagePa = mark1[0];               // expect 0x5A5AF00D
            p->OvDeniedOk = completed;          // expect 3
            p->OvOtherOk = intrSeen;
            p->TargetVa = hv->Ctrl.EventInj.F.Valid; // transfer verdict
            p->VmcbNcr3 = mismatchRip;   // r168 forensics
            p->LastStatus = (u32)mismatchInfo1;
            p->OpenOk = (u32)mismatchInfo2; // r168: CR2 of the delivery #PF
            SVMB_LOGW("nptprobe s21: exc=%08x legs=%u/%u mm=%llx mrip=%llx "
                      "mi1=%llx mi2=%llx mark1=%llx mark2=%llx injValid=%u "
                      "intr=%u",
                      (u32)r, completed, (u32)NLEGS,
                      (unsigned long long)mismatchExit,
                      (unsigned long long)mismatchRip,
                      (unsigned long long)mismatchInfo1,
                      (unsigned long long)mismatchInfo2,
                      (unsigned long long)mark1[0],
                      (unsigned long long)mark2[0],
                      (u32)hv->Ctrl.EventInj.F.Valid, intrSeen);
            for (u32 i = 0; i < 10; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_SUCCESS;
        }
    }
    else if (p->Stage == 23)
    {
        // r169 sustained-L2 judge (the exit-per-tick interrupt model).
        // L2 runs a 16384-iteration loop - long enough to span multiple
        // guest timer ticks, so pending interrupts exit 0x60 (the r167
        // forced INTR reflect) and H retries its VMRUN without advance.
        // Every retry is a full accept/reflect stash-restore cycle - the
        // drift stress the sustained model is really testing. Verdict:
        // the terminal hlt reflects with the marker store landed and the
        // retry count reported. No per-leg advance: a single terminal leg.
        constexpr u64 L2_CODE_VA = 0x100000;
        constexpr u64 L2_MARK_VA = 0x113000;
        void* pages[9] = {}; // vmcb, code, stack, pml4, pdpt, pd, pt, mark
        bool ok = true;
        for (u32 i = 0; i < 8 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        if (!ok)
        {
            for (u32 i = 0; i < 8; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            u64* vmcb = pageAt(pages[0]);
            u64* code = pageAt(pages[1]);
            u64* stack = pageAt(pages[2]);
            u64* pml4 = pageAt(pages[3]);
            u64* pdpt = pageAt(pages[4]);
            u64* pd = pageAt(pages[5]);
            u64* pt = pageAt(pages[6]);
            u64* mark = pageAt(pages[7]);
            u64 codePa = MmGetPhysicalAddress(code).QuadPart;
            u64 stackPa = MmGetPhysicalAddress(stack).QuadPart;
            u64 pml4Pa = MmGetPhysicalAddress(pml4).QuadPart;
            u64 pdptPa = MmGetPhysicalAddress(pdpt).QuadPart;
            u64 pdPa = MmGetPhysicalAddress(pd).QuadPart;
            u64 ptPa = MmGetPhysicalAddress(pt).QuadPart;
            u64 markPa = MmGetPhysicalAddress(mark).QuadPart;

            pml4[0] = pdptPa | 0x3;
            pdpt[0] = pdPa | 0x3;
            pd[0] = ptPa | 0x3;
            pt[0x100] = codePa | 0x3;   // VA 0x100000
            pt[0x111] = stackPa | 0x3;  // VA 0x111000
            pt[0x113] = markPa | 0x3;   // VA 0x113000
            RtlZeroMemory(mark, PAGE_SIZE); // AllocNonPaged does NOT zero

            // xor rax,rax; mov rcx,0x4000; loop{6x nop; dec rcx; jnz -11};
            // mov qword [0x113000],0xD15EA5ED; hlt terminal
            static const u8 l2code[] = {
                0x48, 0x31, 0xC0,
                0x48, 0xC7, 0xC1, 0x00, 0x40, 0x00, 0x00,
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                0x48, 0xFF, 0xC9,
                0x75, 0xF5,
                0x48, 0xC7, 0x04, 0x25, 0x00, 0x30, 0x11, 0x00,
                0xED, 0xA5, 0x5E, 0xD1,
                0xF4,
            };
            RtlCopyMemory(code, l2code, sizeof(l2code));

            VMCB* hv = (VMCB*)vmcb;
            RtlZeroMemory(hv, PAGE_SIZE);
            hv->Ctrl.InterceptOpcode1 = 1u << 24; // r186: terminal hlt must be ordered by H
            hv->Save.Rip = L2_CODE_VA;
            hv->Save.Rsp = 0x111FF8;
            hv->Save.Rflags = 0x202;
            hv->Save.Rax = 0x0DEFACED;
            hv->Save.Cr3 = pml4Pa;
            u64 hvPa = MmGetPhysicalAddress(hv).QuadPart;

            LONG r = 0;
            u32 retries = 0;
            u64 gotExit = 0, gotRip = 0;
            bool terminal = false;
            u64 t0 = __rdtsc();
            for (u32 guard = 0; guard < 4096; ++guard)
            {
                __try
                {
                    __svm_vmrun(hvPa);
                    r = 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    r = GetExceptionCode();
                    break;
                }
                u64 ec = hv->Ctrl.ExitCode;
                if (ec == 0x60) // reflected interrupt: retry, no advance
                {
                    ++retries;
                    continue;
                }
                gotExit = ec;
                gotRip = hv->Save.Rip;
                terminal = (ec == 0x78 && gotRip == L2_CODE_VA + 0x21);
                break;
            }
            u64 tscDelta = __rdtsc() - t0;

            p->Cr3Value = (u64)(u32)r;
            p->Pml4Pa = hvPa;
            p->HiddenPa = terminal ? 0 : gotExit; // 0 = clean terminal
            p->HookHits = mark[0];                // expect 0xD15EA5ED
            p->PagePa = retries;                  // retry stress count
            p->OvDeniedOk = terminal ? 1u : 0u;
            p->OvOtherOk = (u32)(tscDelta / 1000000ull);
            SVMB_LOGW("nptprobe s23: exc=%08x term=%u exit=%llx rip=%llx "
                      "mark=%llx retries=%u dt=%lluM",
                      (u32)r, (u32)terminal, (unsigned long long)gotExit,
                      (unsigned long long)gotRip,
                      (unsigned long long)mark[0], retries,
                      (unsigned long long)(tscDelta / 1000000ull));
            for (u32 i = 0; i < 8; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_SUCCESS;
        }
    }

    else if (p->Stage == 25)
    {
        // r172 shadow-v0 judge: L2 runs on a VIRTUAL GPA window (0x500000)
        // that H's NON-IDENTITY nested NPT maps onto the real frames -
        // three table sets in play (L2's CR3 VA->GPA identity, H's nested
        // NPT GPA->L1-PA remapped, svmb's shadow GPA->HPA composed on
        // NPF). Decoupling of the code page's physical location from the
        // GPA L2 sees is the indisputable composition proof. Program:
        // cpuid -> mov qword [0x513000], 0xC0DE600D -> hlt terminal.
        constexpr u64 L2_CODE_VA = 0x500000;
        constexpr u64 L2_STACK_VA = 0x511000;
        constexpr u64 L2_MARK_VA = 0x513000;
        void* pages[16] = {}; // vmcb, code, stack, mark, l2 pml4..pt (4),
                              // H npt pml4..pt (4)
        bool ok = true;
        for (u32 i = 0; i < 12 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        if (!ok)
        {
            for (u32 i = 0; i < 12; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            u64* vmcb = pageAt(pages[0]);
            u64* code = pageAt(pages[1]);
            u64* stack = pageAt(pages[2]);
            u64* mark = pageAt(pages[3]);
            u64* l2t[4] = { pageAt(pages[4]), pageAt(pages[5]),
                            pageAt(pages[6]), pageAt(pages[7]) };
            u64* hn[4] = { pageAt(pages[8]), pageAt(pages[9]),
                           pageAt(pages[10]), pageAt(pages[11]) };
            u64 codePa = MmGetPhysicalAddress(code).QuadPart;
            u64 stackPa = MmGetPhysicalAddress(stack).QuadPart;
            u64 markPa = MmGetPhysicalAddress(mark).QuadPart;
            u64 l2Pml4Pa = MmGetPhysicalAddress(l2t[0]).QuadPart;
            u64 l2PdptPa = MmGetPhysicalAddress(l2t[1]).QuadPart;
            u64 l2PdPa = MmGetPhysicalAddress(l2t[2]).QuadPart;
            u64 l2PtPa = MmGetPhysicalAddress(l2t[3]).QuadPart;
            u64 hnPml4Pa = MmGetPhysicalAddress(hn[0]).QuadPart;
            u64 hnPdptPa = MmGetPhysicalAddress(hn[1]).QuadPart;
            u64 hnPdPa = MmGetPhysicalAddress(hn[2]).QuadPart;
            u64 hnPtPa = MmGetPhysicalAddress(hn[3]).QuadPart;

            // L2's own CR3 tables: VA->GPA identity over the window
            l2t[0][0] = l2PdptPa | 0x3;
            l2t[1][0] = l2PdPa | 0x3;
            l2t[2][2] = l2PtPa | 0x3; // PD index for 0x400000-0x5FFFFF
            l2t[3][(L2_CODE_VA >> 12) & 0x1FF] = L2_CODE_VA | 0x3;
            l2t[3][(L2_STACK_VA >> 12) & 0x1FF] = L2_STACK_VA | 0x3;
            l2t[3][(L2_MARK_VA >> 12) & 0x1FF] = L2_MARK_VA | 0x3;

            // H's NON-IDENTITY nested NPT: window GPAs -> the REAL frames
            hn[0][0] = hnPdptPa | 0x3;
            hn[1][0] = hnPdPa | 0x3;
            hn[2][2] = hnPtPa | 0x3;
            hn[3][(L2_CODE_VA >> 12) & 0x1FF] = codePa | 0x7;
            hn[3][(L2_STACK_VA >> 12) & 0x1FF] = stackPa | 0x7;
            hn[3][(L2_MARK_VA >> 12) & 0x1FF] = markPa | 0x7;

            static const u8 l2code[] = {
                0x0F, 0xA2,                                   // cpuid
                0x48, 0xC7, 0x04, 0x25, 0x00, 0x30, 0x51, 0x00,
                0x0D, 0x60, 0xDE, 0xC0,                       // mov [mark]
                0xF4,                                         // hlt terminal
            };
            RtlCopyMemory(code, l2code, sizeof(l2code));
            RtlZeroMemory(mark, PAGE_SIZE);

            VMCB* hv = (VMCB*)vmcb;
            RtlZeroMemory(hv, PAGE_SIZE);
            hv->Ctrl.InterceptOpcode1 = (1u << 18) | (1u << 24); // ic1 bit18 = CPUID
            hv->Ctrl.Np.F.NpEnable = 1;             // the r172 NP session
            hv->Ctrl.NCr3 = hnPml4Pa;               // H's nested root
            hv->Save.Rip = L2_CODE_VA;
            hv->Save.Rsp = L2_STACK_VA + 0xFF8;
            // IF clear: with the skeleton L1 IDTR inherited, an interrupt
            // delivered into L2 is the r167 hazard; with IF=0 it is never
            // deliverable to L2 so the pending IRQ flows to L1 instead of
            // storming 0x60 exits (the r142-family undeliverable-IRR known
            // bench bug made the storm absolute - 256/256 retries)
            hv->Save.Rflags = 0x2;
            hv->Save.Rax = 0x7BADBEEF;
            hv->Save.Cr3 = l2Pml4Pa;
            u64 hvPa = MmGetPhysicalAddress(hv).QuadPart;

            LONG r = 0;
            u32 completed = 0, intrSeen = 0;
            u64 mismatchExit = 0, gotExit = 0, gotRip = 0;
            for (u32 guard = 0; guard < 256; ++guard)
            {
                __try
                {
                    __svm_vmrun(hvPa);
                    r = 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    r = GetExceptionCode();
                    break;
                }
                u64 ec = hv->Ctrl.ExitCode;
                if (ec == 0x60)
                {
                    ++intrSeen;
                    continue;
                }
                gotExit = ec;
                gotRip = hv->Save.Rip;
                if (ec == 0x72 && gotRip == L2_CODE_VA)
                {
                    ++completed;
                    if (completed == 1)
                    {
                        hv->Save.Rip += 2; // past the cpuid
                        continue;
                    }
                }
                mismatchExit = ec;
                break;
            }

            p->Cr3Value = (u64)(u32)r;
            p->Pml4Pa = hvPa;
            p->HiddenPa = gotExit;             // expect terminal 0x78
            p->HookHits = gotRip;              // expect 0x50000F
            p->PagePa = mark[0];               // expect 0xC0DE600D
            p->OvDeniedOk = completed;         // expect 1
            p->OvOtherOk = intrSeen;
            SVMB_LOGW("nptprobe s25: exc=%08x legs=%u exit=%llx rip=%llx "
                      "mark=%llx intr=%u",
                      (u32)r, completed, (unsigned long long)gotExit,
                      (unsigned long long)gotRip,
                      (unsigned long long)mark[0], intrSeen);
            // r178: H teardown invalidation (the r176 contract) - the
            // next test's pool allocations may REUSE this world's frames,
            // and a surviving window would serve stale composes (the s13
            // first-leg #GP: pool LIFO handed the new H-NPT the same root)
            _svmb_invlpga(0, 1);
            for (u32 i = 0; i < 12; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_SUCCESS;
        }
    }

    else if (p->Stage == 24)
    {
        // r171 H-view judge: register this thread's CR3 as H, prove the
        // virtual SVM view (EFER.SVME settable/readable, own HSAVE, the
        // CPUID SVM bit unmasked), unregister, and prove the stealth face
        // snapped back (SVME masked, CPUID hidden again). Real MSRs are
        // never touched - the system staying alive is part of the verdict.
        auto rdmsrS = [&](u32 m) -> u64 {
            __try { return __readmsr(m); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return 0xdead; }
        };
        u64 eferPre = rdmsrS(0xC0000080);
        u64 hsavePre = rdmsrS(0xC0010117);
        int cpuidPre[4] = {};
        __cpuidex(cpuidPre, 0x80000001, 0);

        u64 cr3 = __readcr3();
        NestedHViewRegister(cr3);
        u64 eferH0 = rdmsrS(0xC0000080);
        u64 hsaveH0 = rdmsrS(0xC0010117);
        __try
        {
            __writemsr(0xC0000080, eferH0 | (1ull << 12)); // SVME
            __writemsr(0xC0010117, 0x12345000ull);         // own HSAVE
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        u64 eferH1 = rdmsrS(0xC0000080);
        u64 hsaveH1 = rdmsrS(0xC0010117);
        int cpuidH[4] = {};
        __cpuidex(cpuidH, 0x80000001, 0);
        NestedHViewUnregister(cr3);
        u64 eferPost = rdmsrS(0xC0000080);
        u64 hsavePost = rdmsrS(0xC0010117);
        int cpuidPost[4] = {};
        __cpuidex(cpuidPost, 0x80000001, 0);

        p->Cr3Value = eferPre;                 // stealth: SVME masked
        p->Pml4Pa = hsavePre;                  // stealth: 0
        p->HiddenPa = eferH1;                  // H view: SVME set + alive
        p->HookHits = hsaveH1;                 // H view: 0x12345000
        p->PagePa = eferPost;                  // back to stealth masked
        p->OvDeniedOk = (u32)!!(cpuidH[2] & 0x4);    // H sees SVM bit
        p->OvOtherOk = (u32)!!(cpuidPre[2] & 0x4) |
                       ((u32)!!(cpuidPost[2] & 0x4) << 1); // expect 0
        SVMB_LOGW("nptprobe s24: pre(%llx/%llx) H(%llx/%llx) post(%llx "
                  "/%llx) cpuid H=%u prePost=%u",
                  (unsigned long long)eferPre,
                  (unsigned long long)hsavePre,
                  (unsigned long long)eferH1,
                  (unsigned long long)hsaveH1,
                  (unsigned long long)eferPost,
                  (unsigned long long)hsavePost,
                  (u32)!!(cpuidH[2] & 0x4), p->OvOtherOk);
        status = STATUS_SUCCESS;
    }

    else if (p->Stage == 27)
    {
        // r176 shadow-v1 invalidation judge: H runs L2 which reads a mark
        // through the composed shadow; H then REMAPS the mark GPA to a
        // second frame, executes INVLPGA (the nested invalidation
        // contract), and runs L2 again - the read must observe the NEW
        // mapping (a stale composed entry returns the first frame's
        // magic). L2 program: mov rax,[mark]; hlt.
        constexpr u64 L2_CODE_VA = 0x500000;
        constexpr u64 L2_STACK_VA = 0x511000;
        constexpr u64 L2_MARK_VA = 0x513000;
        constexpr u64 MAGIC1 = 0x1122334455667788ull;
        constexpr u64 MAGIC2 = 0x8877665544332211ull;
        void* pages[16] = {};
        bool ok = true;
        for (u32 i = 0; i < 13 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        if (!ok)
        {
            for (u32 i = 0; i < 13; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            u64* vmcb = pageAt(pages[0]);
            u64* code = pageAt(pages[1]);
            u64* stack = pageAt(pages[2]);
            u64* mark1 = pageAt(pages[3]);
            u64* mark2 = pageAt(pages[4]);
            u64* l2t[4] = { pageAt(pages[5]), pageAt(pages[6]),
                            pageAt(pages[7]), pageAt(pages[8]) };
            u64* hnPml4 = pageAt(pages[9]);
            u64* hnPdpt = pageAt(pages[10]);
            u64* hn = pageAt(pages[11]); // the H PT (single-level shortcut:
                                         // PML4->PDPT->PD0 built inline)
            u64 l2Pml4Pa = MmGetPhysicalAddress(l2t[0]).QuadPart;
            u64 l2PdptPa = MmGetPhysicalAddress(l2t[1]).QuadPart;
            u64 l2PdPa = MmGetPhysicalAddress(l2t[2]).QuadPart;
            u64 l2PtPa = MmGetPhysicalAddress(l2t[3]).QuadPart;
            u64 hnPml4Pa = MmGetPhysicalAddress(hnPml4).QuadPart;
            u64 hnPdptPa = MmGetPhysicalAddress(hnPdpt).QuadPart;
            u64 hnPtPa = MmGetPhysicalAddress(hn).QuadPart;

            l2t[0][0] = l2PdptPa | 0x3;
            l2t[1][0] = l2PdPa | 0x3;
            l2t[2][2] = l2PtPa | 0x3;
            l2t[3][(L2_CODE_VA >> 12) & 0x1FF] = L2_CODE_VA | 0x3;
            l2t[3][(L2_STACK_VA >> 12) & 0x1FF] = L2_STACK_VA | 0x3;
            l2t[3][(L2_MARK_VA >> 12) & 0x1FF] = L2_MARK_VA | 0x3;

            // H's nested NPT: PML4->PDPT->PD(slot links)->PT; the PD page
            // rides mark2's 2-page allocation slack
            u64* hnPd = (u64*)((ULONG_PTR)mark2 + PAGE_SIZE);
            u64 hnPdPa = MmGetPhysicalAddress(hnPd).QuadPart;
            hnPml4[0] = hnPdptPa | 0x3;
            hnPdpt[0] = hnPdPa | 0x3;
            hnPd[2] = hnPtPa | 0x3;
            hn[(L2_CODE_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(code).QuadPart | 0x7;
            hn[(L2_STACK_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(stack).QuadPart | 0x7;
            hn[(L2_MARK_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(mark1).QuadPart | 0x7;

            static const u8 l2code[] = {
                0x48, 0x8B, 0x04, 0x25, 0x00, 0x30, 0x51, 0x00, // mov rax,[mark]
                0xF4,                                               // hlt
            };
            RtlCopyMemory(code, l2code, sizeof(l2code));
            mark1[0] = MAGIC1;
            mark2[0] = MAGIC2;

            VMCB* hv = (VMCB*)vmcb;
            RtlZeroMemory(hv, PAGE_SIZE);
            hv->Ctrl.InterceptOpcode1 = 1u << 24; // r186: terminal hlt must be ordered by H
            hv->Ctrl.Np.F.NpEnable = 1;
            hv->Ctrl.NCr3 = hnPml4Pa;
            hv->Save.Rflags = 0x2; // IF clear (the r173 storm law)
            hv->Save.Cr3 = l2Pml4Pa;
            u64 hvPa = MmGetPhysicalAddress(hv).QuadPart;

            // round helper: run L2 to its terminal hlt, return Rax
            auto runL2 = [&](u64* gotRax, u64* gotExit) {
                hv->Save.Rip = L2_CODE_VA;
                hv->Save.Rsp = L2_STACK_VA + 0xFF8;
                hv->Save.Rax = 0;
                for (u32 guard = 0; guard < 256; ++guard)
                {
                    LONG r = 0;
                    __try { __svm_vmrun(hvPa); }
                    __except (EXCEPTION_EXECUTE_HANDLER)
                    {
                        *gotExit = (u64)0xFFFFFFFF00000000ull |
                                   (u64)GetExceptionCode();
                        return false;
                    }
                    (void)r;
                    u64 ec = hv->Ctrl.ExitCode;
                    if (ec == 0x60)
                        continue; // pending intr: retry (IF=0 flows to L1)
                    *gotExit = ec;
                    if (ec == 0x78) // hlt terminal
                    {
                        *gotRax = hv->Save.Rax;
                        return true;
                    }
                    return false; // anything else: bail with the exit
                }
                return false;
            };

            u64 r1 = 0, e1 = 0, r2 = 0, e2 = 0;
            bool leg1 = runL2(&r1, &e1);
            // H remaps the mark GPA to frame 2 + invalidates (the r176
            // contract); L2's next read must compose fresh
            hn[(L2_MARK_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(mark2).QuadPart | 0x7;
            _svmb_invlpga(L2_MARK_VA, 1); // exits to NestedInvlpgaExit
            bool leg2 = runL2(&r2, &e2);

            p->HookHits = r1;          // expect MAGIC1
            p->PagePa = r2;            // expect MAGIC2 (fresh compose)
            p->HiddenPa = e1 | (e2 << 32);
            p->OvDeniedOk = (u32)leg1 | ((u32)leg2 << 1); // expect 3
            SVMB_LOGW("nptprobe s27: r1=%llx r2=%llx e1=%llx e2=%llx "
                      "legs=%u",
                      (unsigned long long)r1, (unsigned long long)r2,
                      (unsigned long long)e1, (unsigned long long)e2,
                      (u32)leg1 * 1u + (u32)leg2 * 2u);
            status = STATUS_SUCCESS;
            // r178: teardown invalidation (see stage 25's note)
            _svmb_invlpga(0, 1);
            for (u32 i = 0; i < 13; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
        }
    }

    else if (p->Stage == 28)
    {
        // r176 shadow-v1 perms judge: H maps the L2 code page NX in its
        // nested NPT. L2's first fetch must NPF back to H with the
        // execute bit set (correct nesting semantics: an H-NX page
        // executing in L2 is H's fault to handle); H then clears NX,
        // invalidates, and L2 completes the marker program.
        constexpr u64 L2_CODE_VA = 0x500000;
        constexpr u64 L2_STACK_VA = 0x511000;
        constexpr u64 L2_MARK_VA = 0x513000;
        void* pages[16] = {};
        bool ok = true;
        for (u32 i = 0; i < 12 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        if (!ok)
        {
            for (u32 i = 0; i < 12; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            u64* vmcb = pageAt(pages[0]);
            u64* code = pageAt(pages[1]);
            u64* stack = pageAt(pages[2]);
            u64* mark = pageAt(pages[3]);
            u64* l2t[4] = { pageAt(pages[4]), pageAt(pages[5]),
                            pageAt(pages[6]), pageAt(pages[7]) };
            u64* hn[4] = { pageAt(pages[8]), pageAt(pages[9]),
                           pageAt(pages[10]), pageAt(pages[11]) };
            u64 l2Pml4Pa = MmGetPhysicalAddress(l2t[0]).QuadPart;
            u64 l2PdptPa = MmGetPhysicalAddress(l2t[1]).QuadPart;
            u64 l2PdPa = MmGetPhysicalAddress(l2t[2]).QuadPart;
            u64 l2PtPa = MmGetPhysicalAddress(l2t[3]).QuadPart;
            u64 hnPml4Pa = MmGetPhysicalAddress(hn[0]).QuadPart;
            u64 hnPdptPa = MmGetPhysicalAddress(hn[1]).QuadPart;
            u64 hnPdPa = MmGetPhysicalAddress(hn[2]).QuadPart;
            u64 hnPtPa = MmGetPhysicalAddress(hn[3]).QuadPart;

            l2t[0][0] = l2PdptPa | 0x3;
            l2t[1][0] = l2PdPa | 0x3;
            l2t[2][2] = l2PtPa | 0x3;
            l2t[3][(L2_CODE_VA >> 12) & 0x1FF] = L2_CODE_VA | 0x3;
            l2t[3][(L2_STACK_VA >> 12) & 0x1FF] = L2_STACK_VA | 0x3;
            l2t[3][(L2_MARK_VA >> 12) & 0x1FF] = L2_MARK_VA | 0x3;

            hn[0][0] = hnPdptPa | 0x3;
            hn[1][0] = hnPdPa | 0x3;
            hn[2][2] = hnPtPa | 0x3;
            // the code page starts NX (bit 63) - the perms under test
            hn[3][(L2_CODE_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(code).QuadPart | 0x7 | (1ull << 63);
            hn[3][(L2_STACK_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(stack).QuadPart | 0x7;
            hn[3][(L2_MARK_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(mark).QuadPart | 0x7;

            static const u8 l2code[] = {
                0x48, 0xC7, 0x04, 0x25, 0x00, 0x30, 0x51, 0x00,
                0x0D, 0x60, 0xDE, 0xC0,                       // mov [mark]
                0xF4,                                         // hlt
            };
            RtlCopyMemory(code, l2code, sizeof(l2code));
            RtlZeroMemory(mark, PAGE_SIZE);

            VMCB* hv = (VMCB*)vmcb;
            RtlZeroMemory(hv, PAGE_SIZE);
            hv->Ctrl.InterceptOpcode1 = 1u << 24; // r186: terminal hlt must be ordered by H
            hv->Ctrl.Np.F.NpEnable = 1;
            hv->Ctrl.NCr3 = hnPml4Pa;
            hv->Save.Rip = L2_CODE_VA;
            hv->Save.Rsp = L2_STACK_VA + 0xFF8;
            hv->Save.Rflags = 0x2;
            hv->Save.Cr3 = l2Pml4Pa;
            u64 hvPa = MmGetPhysicalAddress(hv).QuadPart;

            u64 denyExit = 0, denyInfo1 = 0, termExit = 0;
            u32 denied = 0;
            for (u32 guard = 0; guard < 256; ++guard)
            {
                __try { __svm_vmrun(hvPa); }
                __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                u64 ec = hv->Ctrl.ExitCode;
                if (ec == 0x60)
                    continue;
                if (ec == 0x400 && !denied)
                {
                    // the NX fetch NPF reflected to H
                    denyExit = ec;
                    denyInfo1 = hv->Ctrl.ExitInfo1;
                    ++denied; // expect exactly 1
                    // H handles: clear NX, invalidate, retry
                    hn[3][(L2_CODE_VA >> 12) & 0x1FF] &= ~(1ull << 63);
                    _svmb_invlpga(L2_CODE_VA, 1);
                    hv->Save.Rip = L2_CODE_VA;
                    continue;
                }
                if (ec == 0x78 && denied)
                {
                    termExit = ec;
                    break;
                }
                // anything else (hlt without a deny, a second NPF, ...)
                denyExit = ec ? ec : denyExit;
                break;
            }

            p->HiddenPa = denyExit;   // expect 0x78
            p->HookHits = denyInfo1;  // expect bit4 (NPF_EXECUTE) set
            p->PagePa = mark[0];      // expect 0xC0DE600D (fix-up worked)
            p->OvDeniedOk = denied;   // expect 1
            p->OvOtherOk = (u32)(termExit == 0x78 ? 1 : 0);
            SVMB_LOGW("nptprobe s28: denied=%u exit=%llx info1=%llx "
                      "mark=%llx term=%llx",
                      denied, (unsigned long long)denyExit,
                      (unsigned long long)denyInfo1,
                      (unsigned long long)mark[0],
                      (unsigned long long)termExit);
            status = STATUS_SUCCESS;
            // r178: teardown invalidation (see stage 25's note)
            _svmb_invlpga(0, 1);
            for (u32 i = 0; i < 12; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
        }
    }

    else if (p->Stage == 29)
    {
        // r176 shadow-v1 large-page judge: H maps the whole 2MB slot with
        // ONE PD-level large entry covering code+stack+mark (a physically
        // contiguous region). L2's accesses compose through the 512-fill
        // path; the marker proves the whole program ran.
        constexpr u64 L2_CODE_VA = 0x500000;
        constexpr u64 L2_STACK_VA = 0x511000;
        constexpr u64 L2_MARK_VA = 0x513000;
        void* pages[16] = {};
        // one CONTIGUOUS region for code+stack+mark so a single 2MB large
        // entry can cover them (retry until the 16KB block does not
        // straddle a 2MB boundary)
        void* contig = nullptr;
        PHYSICAL_ADDRESS contigPa = {};
        for (u32 attempt = 0; attempt < 8 && !contig; ++attempt)
        {
            void* cand = AllocContiguous(4 * PAGE_SIZE, TAG_SVMB);
            if (!cand)
                break;
            PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(cand);
            if ((pa.QuadPart & 0x1FFFFFull) + 4 * PAGE_SIZE <= 0x200000)
            {
                contig = cand;
                contigPa = pa;
            }
            else
                MmFreeContiguousMemory(cand);
        }
        bool ok = contig != nullptr;
        for (u32 i = 0; i < 12 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        if (!ok)
        {
            if (contig)
                FreeContiguous(contig, TAG_SVMB);
            for (u32 i = 0; i < 12; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            u64* vmcb = pageAt(pages[0]);
            u8* code = (u8*)contig;                       // offset 0
            u64* stack = (u64*)((u8*)contig + 0x1000);    // offset 4K
            u64* mark = (u64*)((u8*)contig + 0x2000);     // offset 8K
            u64* l2t[4] = { pageAt(pages[1]), pageAt(pages[2]),
                            pageAt(pages[3]), pageAt(pages[4]) };
            u64* hn[4] = { pageAt(pages[5]), pageAt(pages[6]),
                           pageAt(pages[7]), pageAt(pages[8]) };
            u64 l2Pml4Pa = MmGetPhysicalAddress(l2t[0]).QuadPart;
            u64 l2PdptPa = MmGetPhysicalAddress(l2t[1]).QuadPart;
            u64 l2PdPa = MmGetPhysicalAddress(l2t[2]).QuadPart;
            u64 l2PtPa = MmGetPhysicalAddress(l2t[3]).QuadPart;
            u64 hnPml4Pa = MmGetPhysicalAddress(hn[0]).QuadPart;
            u64 hnPdptPa = MmGetPhysicalAddress(hn[1]).QuadPart;

            l2t[0][0] = l2PdptPa | 0x3;
            l2t[1][0] = l2PdPa | 0x3;
            l2t[2][2] = l2PtPa | 0x3;
            l2t[3][(L2_CODE_VA >> 12) & 0x1FF] = L2_CODE_VA | 0x3;
            l2t[3][(L2_STACK_VA >> 12) & 0x1FF] = L2_STACK_VA | 0x3;
            l2t[3][(L2_MARK_VA >> 12) & 0x1FF] = L2_MARK_VA | 0x3;

            // H's nested NPT: ONE 2MB large entry at PD level covers the
            // whole slot. The large target is the 2MB block holding the
            // contiguous region; the L2 VAs' page offsets inside the slot
            // must equal the region's physical offsets inside the block.
            u64 blk = contigPa.QuadPart & ~0x1FFFFFull;
            u64 inBlk = contigPa.QuadPart & 0x1FFFFFull;
            // shift the L2 VAs so slot-offset == physical-offset
            u64 codeVa = L2_CODE_VA; // rebuild the three VAs offset-mapped
            codeVa = (L2_CODE_VA & ~0x1FFFFFull) | inBlk;
            u64 stackVa = codeVa + 0x1000;
            u64 markVa = codeVa + 0x2000;
            l2t[2][2] = l2PtPa | 0x3;
            l2t[3][(codeVa >> 12) & 0x1FF] = codeVa | 0x3;
            l2t[3][(stackVa >> 12) & 0x1FF] = stackVa | 0x3;
            l2t[3][(markVa >> 12) & 0x1FF] = markVa | 0x3;

            hn[0][0] = hnPdptPa | 0x3;
            hn[1][0] = MmGetPhysicalAddress(hn[2]).QuadPart | 0x3;
            // the large entry itself: PD slot for this 2MB (slot index =
            // codeVa >> 21, PD0 idx)
            hn[2][2] = blk | 0xE7; // P|RW|US|A|D|PS - the r176 feature

            static const u8 l2code[] = {
                0x48, 0xC7, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00,
                0x0D, 0x60, 0xDE, 0xC0,   // mov [markVA] (imm patched)
                0xF4,
            };
            RtlCopyMemory(code, l2code, sizeof(l2code));
            *(u32*)((u8*)code + 4) = (u32)markVa; // patch the disp32
            RtlZeroMemory(mark, PAGE_SIZE);

            VMCB* hv = (VMCB*)vmcb;
            RtlZeroMemory(hv, PAGE_SIZE);
            hv->Ctrl.InterceptOpcode1 = 1u << 24; // r186: terminal hlt must be ordered by H
            hv->Ctrl.Np.F.NpEnable = 1;
            hv->Ctrl.NCr3 = hnPml4Pa;
            hv->Save.Rip = codeVa;
            hv->Save.Rsp = stackVa + 0xFF8;
            hv->Save.Rflags = 0x2;
            hv->Save.Cr3 = l2Pml4Pa;
            u64 hvPa = MmGetPhysicalAddress(hv).QuadPart;

            u64 gotExit = 0, gotRip = 0;
            u32 done = 0;
            for (u32 guard = 0; guard < 256; ++guard)
            {
                __try { __svm_vmrun(hvPa); }
                __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                u64 ec = hv->Ctrl.ExitCode;
                if (ec == 0x60)
                    continue;
                gotExit = ec;
                gotRip = hv->Save.Rip;
                if (ec == 0x78)
                {
                    done = 1;
                    break;
                }
                break;
            }

            p->HiddenPa = gotExit;   // expect 0x78
            p->HookHits = gotRip;    // expect codeVa+0xD (mov+hlt)
            p->PagePa = mark[0];     // expect 0xC0DE600D
            p->OvDeniedOk = done;    // expect 1
            p->Cr3Value = codeVa;    // the offset-shifted code VA (runtime)
            SVMB_LOGW("nptprobe s29: exit=%llx rip=%llx mark=%llx blk=%llx",
                      (unsigned long long)gotExit,
                      (unsigned long long)gotRip,
                      (unsigned long long)mark[0],
                      (unsigned long long)blk);
            status = STATUS_SUCCESS;
            _svmb_invlpga(0, 1); // r178: teardown invalidation
            FreeContiguous(contig, TAG_SVMB);
            for (u32 i = 0; i < 12; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
        }
    }

    else if (p->Stage == 30)
    {
        // r179 multi-core shadow judge: 4 H worlds, 4 cores, one shared
        // start barrier, 50 concurrent L2 round trips each. The same L2
        // GPA must land on four DIFFERENT physical frames through four
        // concurrent per-core shadow sessions.
        constexpr u32 T = 4;
        constexpr LONG ROUNDS = 50;
        KeInitializeEvent(&s_mcGo, NotificationEvent, FALSE);
        u32 okMask = 0;
        LONG totalRounds = 0;
        for (u32 t = 0; t < T; ++t)
        {
            RtlZeroMemory(&s_mc[t], sizeof(s_mc[t]));
            s_mc[t].core = t;
            s_mc[t].magic = 0x5AC00000ull + (u64)t * 0x10000ull;
            KeInitializeEvent(&s_mc[t].done, NotificationEvent, FALSE);
            HANDLE th = nullptr;
            NTSTATUS st = PsCreateSystemThread(&th, GENERIC_ALL, nullptr,
                                               nullptr, nullptr, McThreadFn,
                                               &s_mc[t]);
            if (!NT_SUCCESS(st))
            {
                SVMB_LOGW("nptprobe s30: thread %u create fail %08x",
                          t, st);
                s_mc[t].ok = -1; // distinguishes spawn failure
                continue;
            }
            ZwClose(th);
        }
        // release the barrier once every thread is parked (or failed to
        // spawn); a short delay lets stragglers reach the wait
        LARGE_INTEGER drift = {};
        drift.QuadPart = -1000000; // 100ms
        KeDelayExecutionThread(KernelMode, FALSE, &drift);
        KeSetEvent(&s_mcGo, IO_NO_INCREMENT, FALSE);
        for (u32 t = 0; t < T; ++t)
        {
            if (s_mc[t].ok == -1)
                continue;
            LARGE_INTEGER to = {};
            to.QuadPart = -300000000; // 30s per thread
            NTSTATUS w = KeWaitForSingleObject(&s_mc[t].done, Executive,
                                               KernelMode, FALSE, &to);
            if (w == STATUS_TIMEOUT)
            {
                SVMB_LOGW("nptprobe s30: thread %u TIMED OUT", t);
                continue;
            }
            if (s_mc[t].ok == 1)
                okMask |= 1u << t;
            totalRounds += s_mc[t].rounds;
            SVMB_LOGW("nptprobe s30: core=%u rounds=%d mark=%llx ok=%d",
                      s_mc[t].core, s_mc[t].rounds,
                      (unsigned long long)s_mc[t].lastMark, s_mc[t].ok);
        }
        p->HookHits = okMask;          // expect 0xF
        p->OrigHitsAfter = (u64)totalRounds; // expect 4*50
        p->OvDeniedOk = T;
        status = STATUS_SUCCESS;
    }

    else if (p->Stage == 31)
    {
        // r182: real-H-shape judge (the reference hypervisor asm pattern): fill the
        // guest VMCB in software, __svm_vmload(guestPa) BEFORE vmrun, and
        // per exit __svm_vmsave(guestPa) + __svm_vmload(hostPa) + handle
        // + re-load + re-vmrun. The r168 #GP refusal killed this shape at
        // the first vmload; the r182 emulation must carry the whole loop.
        // The host VMCB is pre-captured with one __svm_vmsave (the reference hypervisor
        // captures its host context the same way).
        constexpr u64 L2_CODE_VA = 0x500000;
        constexpr u64 L2_STACK_VA = 0x511000;
        constexpr u64 L2_MARK_VA = 0x513000;
        void* pages[16] = {};
        bool ok = true;
        for (u32 i = 0; i < 13 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        if (!ok)
        {
            for (u32 i = 0; i < 13; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            VMCB* hv = (VMCB*)pageAt(pages[0]); // H's guest VMCB
            VMCB* hostv = (VMCB*)pageAt(pages[1]); // H's host VMCB
            u64* code = pageAt(pages[2]);
            u64* stack = pageAt(pages[3]);
            u64* mark = pageAt(pages[4]);
            u64* l2t[4] = { pageAt(pages[5]), pageAt(pages[6]),
                            pageAt(pages[7]), pageAt(pages[8]) };
            u64* hnPml4 = pageAt(pages[9]);
            u64* hnPdpt = pageAt(pages[10]);
            u64* hn = pageAt(pages[11]);      // H PT (hnPd rides mark slack)
            u64* hnPd = (u64*)((ULONG_PTR)mark + PAGE_SIZE);
            u64 hnPml4Pa = MmGetPhysicalAddress(hnPml4).QuadPart;
            u64 hnPdptPa = MmGetPhysicalAddress(hnPdpt).QuadPart;
            u64 hnPdPa = MmGetPhysicalAddress(hnPd).QuadPart;
            u64 hnPtPa = MmGetPhysicalAddress(hn).QuadPart;

            l2t[0][0] = MmGetPhysicalAddress(l2t[1]).QuadPart | 3;
            l2t[1][0] = MmGetPhysicalAddress(l2t[2]).QuadPart | 3;
            l2t[2][2] = MmGetPhysicalAddress(l2t[3]).QuadPart | 3;
            l2t[3][(L2_CODE_VA >> 12) & 0x1FF] = L2_CODE_VA | 3;
            l2t[3][(L2_STACK_VA >> 12) & 0x1FF] = L2_STACK_VA | 3;
            l2t[3][(L2_MARK_VA >> 12) & 0x1FF] = L2_MARK_VA | 3;
            hnPml4[0] = hnPdptPa | 3;
            hnPdpt[0] = hnPdPa | 3;
            hnPd[2] = hnPtPa | 3;
            hn[(L2_CODE_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(code).QuadPart | 7;
            hn[(L2_STACK_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(stack).QuadPart | 7;
            hn[(L2_MARK_VA >> 12) & 0x1FF] =
                MmGetPhysicalAddress(mark).QuadPart | 7;

            static const u8 l2code[] = {
                0x0F, 0xA2,
                0x48, 0xC7, 0x04, 0x25, 0x00, 0x30, 0x51, 0x00,
                0x0D, 0x60, 0xDE, 0xC0,
                0xF4,
            };
            RtlCopyMemory(code, l2code, sizeof(l2code));
            RtlZeroMemory(mark, PAGE_SIZE);

            RtlZeroMemory(hv, PAGE_SIZE);
            RtlZeroMemory(hostv, PAGE_SIZE);
            hv->Ctrl.InterceptOpcode1 = (1u << 18) | (1u << 24); // ic1 CPUID (+0x0C!)
            hv->Ctrl.Np.F.NpEnable = 1;
            hv->Ctrl.NCr3 = hnPml4Pa;
            hv->Save.Rip = L2_CODE_VA;
            hv->Save.Rsp = L2_STACK_VA + 0xFF8;
            hv->Save.Rflags = 0x2;
            hv->Save.Rax = 0x7BADBEEF;
            hv->Save.Cr3 = MmGetPhysicalAddress(l2t[0]).QuadPart;
            // EFER must be valid long-mode+SVME - the emulated vmload
            // copies it (the reference hypervisor fills its VMCB EFER for exactly this)
            __try { hv->Save.Efer = __readmsr(0xC0000080) | (1ull << 12); }
            __except (EXCEPTION_EXECUTE_HANDLER) { hv->Save.Efer = 0xD01; }
            u64 hvPa = MmGetPhysicalAddress(hv).QuadPart;
            u64 hostPa = MmGetPhysicalAddress(hostv).QuadPart;

            // the reference hypervisor shape: capture host context once (a vmsave), then
            // the loop: vmload guest -> vmrun -> [exit] vmsave guest ->
            // vmload host -> handle -> repeat
            u16 csProbe = 0;
            __try
            {
                __svm_vmsave(hostPa);        // capture host (L1) state
                csProbe = hostv->Save.Cs.Selector;
                // the guest VMCB needs VALID segments: the reference hypervisor's guest
                // VMCB carries captured Windows segments, a zeroed page
                // would load null CS through the emulated vmload and
                // wedge L2 at its first fetch
                hv->Save.Es = hostv->Save.Es;
                hv->Save.Cs = hostv->Save.Cs;
                hv->Save.Ss = hostv->Save.Ss;
                hv->Save.Ds = hostv->Save.Ds;
                hv->Save.Fs = hostv->Save.Fs;
                hv->Save.Gs = hostv->Save.Gs;
                hv->Save.Gdtr = hostv->Save.Gdtr;
                hv->Save.Ldtr = hostv->Save.Ldtr;
                hv->Save.Idtr = hostv->Save.Idtr;
                hv->Save.Tr = hostv->Save.Tr;
                hv->Save.Cpl = hostv->Save.Cpl;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}

            LONG r = 0;
            u32 legs = 0, vl = 0, vs = 0;
            u64 gotExit = 0, gotRip = 0;
            for (u32 guard = 0; guard < 256; ++guard)
            {
                __try
                {
                    __svm_vmload(hvPa);      // the reference hypervisor line 257 shape
                    ++vl;
                    __svm_vmrun(hvPa);
                    r = 0;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    r = GetExceptionCode();
                    break;
                }
                // exit path: the reference hypervisor lines 299/303 shape
                __try
                {
                    __svm_vmsave(hvPa);      // save "current" into guest
                    __svm_vmload(hostPa);    // restore its host context
                    ++vs;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    r = GetExceptionCode();
                    break;
                }
                u64 ec = hv->Ctrl.ExitCode;
                if (ec == 0x60)
                    continue;
                gotExit = ec;
                gotRip = hv->Save.Rip;
                if (ec == 0x72 && hv->Save.Rip == L2_CODE_VA)
                {
                    ++legs;
                    hv->Save.Rip += 2;
                    continue;
                }
                if (ec == 0x78)
                    break; // terminal hlt
                break;
            }

            // the guest VMCB's segments after the final vmsave must hold
            // the real (L1) state - the emulation copied it back
            u16 csAfter = hv->Save.Cs.Selector;
            p->Cr3Value = (u64)(u32)r;
            p->HiddenPa = gotExit;          // expect 0x78
            p->HookHits = gotRip;           // expect 0x50000E
            p->PagePa = mark[0];            // expect 0xFFFFFFFFC0DE600D
            p->OvDeniedOk = legs;           // expect 1
            p->OvOtherOk = (csAfter != 0 && csAfter == csProbe) ? 1 : 0;
            SVMB_LOGW("nptprobe s31: exc=%08x legs=%u vmload=%u vmsave=%u "
                      "exit=%llx rip=%llx mark=%llx cs=%x==%x:%u",
                      (u32)r, legs, vl, vs, (unsigned long long)gotExit,
                      (unsigned long long)gotRip,
                      (unsigned long long)mark[0], (u32)csAfter,
                      (u32)csProbe, p->OvOtherOk);
            _svmb_invlpga(0, 1); // r178 teardown invalidation
            for (u32 i = 0; i < 13; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_SUCCESS;
        }
    }

    else if (p->Stage == 32)
    {
        // r183: full-RAM hook-slot judge. H's nested NPT identity-maps
        // everything with 2M large pages EXCEPT one slot OUTSIDE the
        // 128MB window, which it splits into a PT with ONE remapped
        // entry (the reference hypervisor hook shape). Without materialization the
        // blanket serves identity and L2 reads the WRONG frame; with it,
        // L2 reads H's hook target. Leg 2: H remaps the hook + INVLPGA
        // (the r176/r183 contract) - the rescan must serve the NEW frame.
        // r183: the hook scan runs ONLY when explicitly enabled here
        // (Iterations bit12) - default OFF keeps every other face
        // bit-identical to r182 until the scan's two open defects root
        bool scanOn = (p->Iterations & 0x1000u) != 0;
        svmbHookScanEnable(scanOn ? 1 : 0);
        constexpr u64 MAGIC_ID = 0x1111111111111111ull;
        constexpr u64 MAGIC_H1 = 0xAAAA2222AAAA2222ull;
        constexpr u64 MAGIC_H2 = 0xBBBB3333BBBB3333ull;
        void* pages[16] = {};
        bool ok = true;
        for (u32 i = 0; i < 12 && ok; ++i)
        {
            pages[i] = AllocNonPaged(PAGE_SIZE * 2, TAG_SVMB);
            ok = pages[i] != nullptr;
        }
        VMCB* hv = nullptr;
        u64 hvPa = 0;
        u64 r1 = 0, r2 = 0;
        u32 legs = 0, slot = 0, inWindow = 0;
        if (!ok)
        {
            for (u32 i = 0; i < 12; ++i)
                if (pages[i])
                    FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_INSUFFICIENT_RESOURCES;
        }
        else
        {
            auto pageAt = [](void* raw) -> u64* {
                return (u64*)(((u64)(ULONG_PTR)raw + 0xFFFull) &
                              ~0xFFFull);
            };
            hv = (VMCB*)pageAt(pages[0]);
            u64* markId = pageAt(pages[1]);   // the identity-position frame
            u64* markH1 = pageAt(pages[2]);   // hook target 1
            u64* markH2 = pageAt(pages[3]);   // hook target 2
            u64* l2t[4] = { pageAt(pages[4]), pageAt(pages[5]),
                            pageAt(pages[6]), pageAt(pages[7]) };
            u64* hnPml4 = pageAt(pages[8]);
            u64* hnPdpt = pageAt(pages[9]);
            u64* hnPd = pageAt(pages[10]);    // PD page of the hooked slot
            u64* hnPt = pageAt(pages[11]);    // H's PT for the hooked slot
            u64 markIdPa = MmGetPhysicalAddress(markId).QuadPart;
            u64 markH1Pa = MmGetPhysicalAddress(markH1).QuadPart;
            u64 markH2Pa = MmGetPhysicalAddress(markH2).QuadPart;
            markId[0] = MAGIC_ID;
            markH1[0] = MAGIC_H1;
            markH2[0] = MAGIC_H2;
            // the hook GPA sits at the identity position of markIdPa;
            // its 2MB slot index (usually way outside the 128MB window)
            u64 hookGpa = markIdPa & ~0xFFFull;
            slot = (u32)(hookGpa >> 21);
            inWindow = slot < 64 ? 1 : 0;

            // L2 tables: code/stack/read VAs all live in the 0x400000
            // slot's PT (slot index 2 of L2's PD)
            constexpr u64 L2_CODE_VA = 0x500000;
            constexpr u64 L2_STACK_VA = 0x511000;
            constexpr u64 L2_READ_VA = 0x5F0000;
            u64 l2PtPa = MmGetPhysicalAddress(l2t[3]).QuadPart;
            l2t[0][0] = MmGetPhysicalAddress(l2t[1]).QuadPart | 3;
            l2t[1][0] = MmGetPhysicalAddress(l2t[2]).QuadPart | 3;
            l2t[2][2] = l2PtPa | 3;
            l2t[3][(L2_READ_VA >> 12) & 0x1FF] = hookGpa | 3; // VA->GPA

            // H's nested NPT: this 1GB region gets a real PD; identity
            // large everywhere EXCEPT the hooked slot (PD link -> H's
            // PT with one remapped entry, rest identity)
            u64 hnPtPa = MmGetPhysicalAddress(hnPt).QuadPart;
            u32 j = slot >> 9, k = slot & 0x1FF;
            hnPml4[0] = MmGetPhysicalAddress(hnPdpt).QuadPart | 3;
            hnPdpt[j] = MmGetPhysicalAddress(hnPd).QuadPart | 3;
            for (u32 i = 0; i < 512; ++i)
                hnPd[i] = (((u64)j << 30) | ((u64)i << 21)) | 0xE7;
            hnPd[k] = hnPtPa | 0x7; // the hooked slot: link to H's PT
            u64 slotBase = hookGpa & ~0x1FFFFFull;
            // code + stack ride markH2's frame at identity positions
            // (same 2MB slot is the simplest geometry)
            u64 codeGpa = markH2Pa & ~0xFFFull;
            u64 stackGpa = codeGpa + 0x900;
            for (u32 i = 0; i < 512; ++i)
                hnPt[i] = (slotBase + ((u64)i << 12)) | 0xE7; // identity
            hnPt[(hookGpa >> 12) & 0x1FF] = markH1Pa | 7; // THE HOOK
            // (code/stack ride the identity BLANKET wherever their frames
            // land - explicit per-slot entries here would be cross-slot
            // index collisions)

            // L2 program: mov rax,[0x5F0000]; hlt
            static const u8 l2code[] = {
                0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x5F, 0x00, // mov rax,[0x5F0000]
                0xF4,
            };
            u8* code = (u8*)((ULONG_PTR)markH2 + 0x800); // slack in the
            // 2-page alloc; lands at codeGpa+0x800? NO - markH2 pageAt
            // is the aligned page; +0x800 stays inside it -> identity
            // position codeGpa+0x800... keep code at the page start+0x800
            // and point Rip there (still page codeGpa)
            RtlCopyMemory(code, l2code, sizeof(l2code));
            l2t[3][(L2_CODE_VA >> 12) & 0x1FF] = codeGpa | 3;
            l2t[3][(L2_STACK_VA >> 12) & 0x1FF] = stackGpa | 3;

            RtlZeroMemory(hv, PAGE_SIZE);
            hv->Ctrl.InterceptOpcode1 = 1u << 24; // r186: terminal hlt must be ordered by H
            hv->Ctrl.Np.F.NpEnable = 1;
            hv->Ctrl.NCr3 = MmGetPhysicalAddress(hnPml4).QuadPart;
            hv->Save.Rflags = 0x2;
            hv->Save.Cr3 = MmGetPhysicalAddress(l2t[0]).QuadPart;
            hvPa = MmGetPhysicalAddress(hv).QuadPart;

            auto runL2 = [&](u64* gotRax) {
                hv->Save.Rip = L2_CODE_VA;   // == codeGpa page start+0x800
                hv->Save.Rsp = L2_STACK_VA + 0xFF8;
                hv->Save.Rax = 0;
                for (u32 guard = 0; guard < 256; ++guard)
                {
                    __try { __svm_vmrun(hvPa); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
                    u64 ec = hv->Ctrl.ExitCode;
                    if (ec == 0x60)
                        continue;
                    if (ec == 0x78)
                    {
                        *gotRax = hv->Save.Rax;
                        return true;
                    }
                    return false;
                }
                return false;
            };
            // the code actually sits at codeGpa+0x800: adjust Rip math
            hv->Save.Rip = 0; // set inside runL2 via constant; patch:
            // (constant L2_CODE_VA maps to codeGpa; the code bytes were
            // written at markH2+0x800 = codeGpa+0x800 -> Rip must be
            // L2_CODE_VA+0x800)
            u64 ripBase = L2_CODE_VA + 0x800;
            hv->Save.Rip = ripBase; // (runL2 resets each call below)
            legs = 0;
            u64 rt = 0;
            // inline first leg (Rip +0x800)
            hv->Save.Rip = ripBase;
            hv->Save.Rsp = L2_STACK_VA + 0xFF8;
            hv->Save.Rax = 0;
            for (u32 guard = 0; guard < 256; ++guard)
            {
                __try { __svm_vmrun(hvPa); }
                __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                u64 ec = hv->Ctrl.ExitCode;
                if (ec == 0x60)
                    continue;
                if (ec == 0x78) { r1 = hv->Save.Rax; legs = 1; }
                else { r2 = ((u64)ec << 32) | hv->Ctrl.ExitInfo2; }
                break;
            }
            // H remaps the hook to frame 2 + flushes (the r176/r183
            // contract): the rescan must serve MAGIC_H2
            hnPt[(hookGpa >> 12) & 0x1FF] = markH2Pa | 7; // hook -> frame2
            _svmb_invlpga(hookGpa, 1);
            hv->Save.Rip = ripBase;
            hv->Save.Rsp = L2_STACK_VA + 0xFF8;
            hv->Save.Rax = 0;
            for (u32 guard = 0; guard < 256; ++guard)
            {
                __try { __svm_vmrun(hvPa); }
                __except (EXCEPTION_EXECUTE_HANDLER) { break; }
                u64 ec = hv->Ctrl.ExitCode;
                if (ec == 0x60)
                    continue;
                if (ec == 0x78) { r2 = hv->Save.Rax; ++legs; }
                else { r1 = ((u64)ec << 32) | hv->Ctrl.ExitInfo2; }
                break;
            }
            p->HookHits = r1;       // expect MAGIC_H1 (hook materialized)
            p->PagePa = r2;         // expect MAGIC_H2 (rescan served)
            p->HiddenPa = ((u64)slot << 32) | legs; // legs expect 2
            p->OvDeniedOk = inWindow; // informational: slot vs window
            SVMB_LOGW("nptprobe s32: r1=%llx r2=%llx legs=%u slot=%u "
                      "inWin=%u",
                      (unsigned long long)r1, (unsigned long long)r2,
                      legs, slot, inWindow);
            _svmb_invlpga(0, 1); // r178 teardown invalidation
            svmbHookScanEnable(0); // gate back off
            for (u32 i = 0; i < 12; ++i)
                FreeNonPaged(pages[i], TAG_SVMB, PAGE_SIZE * 2);
            status = STATUS_SUCCESS;
        }
    }

    else
    {
        status = STATUS_INVALID_PARAMETER;
    }

    KeRevertToUserGroupAffinityThread(&prev);
    return status;
}

} // namespace svmb

