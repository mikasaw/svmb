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
        Hypervisor* hv = Hypervisor::Instance();
        u64 truth = 0;
        if (hv)
        {
            VcpuContext* vcpu = hv->Vcpu(CurrentCpuIndex());
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
        Hypervisor* hv = Hypervisor::Instance();
        if (hv)
        {
            VcpuContext* vcpu = hv->Vcpu(CurrentCpuIndex());
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
            p->TargetVa = STATUS_INSUFFICIENT_RESOURCES;
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
        constexpr u32 REAL_FILL = 0x11111111;
        constexpr u32 HIDDEN_FILL = 0x22222222;
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
            RtlFillMemory(buf, PAGE_SIZE, 0x11);
            RtlFillMemory(hide, PAGE_SIZE, 0x22);
            PHYSICAL_ADDRESS bpa = MmGetPhysicalAddress(buf);
            u64 bgpa = bpa.QuadPart & ~0xFFFull;
            PHYSICAL_ADDRESS hpa = MmGetPhysicalAddress(hide);
            u64 hpfn = hpa.QuadPart >> 12;
            SVMB_LOGW("nptprobe s16: bgpa=%llx hpfn=%llx", bgpa, hpfn);

            NptManager* npt = NptInstance();
            u32 viewId = 0;
            NTSTATUS st = npt ? npt->CreateView(viewId) :
                                STATUS_DEVICE_NOT_READY;
            NptView* procView = NT_SUCCESS(st) ? npt->View(viewId) : nullptr;
            if (NT_SUCCESS(st) && procView)
                st = npt->FillView(*procView);
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
                if (viewId) npt->DestroyView(viewId);
            }
            else
            {
                // policy on: this thread's process is the target
                u64 ownCr3 = __readcr3();
                NptView* base = npt->Active();
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
                (void)npt->DestroyView(viewId);
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
        constexpr u32 REAL_FILL = 0x11111111;
        u64 procPml4 = Hypervisor::ViewProcPml4();
        u64 basePml4 = Hypervisor::ViewBasePml4();
        u64 targetCr3 = Hypervisor::ViewTargetCr3();
        NptManager* npt = NptInstance();
        NptView* procView =
            (npt && procPml4) ? npt->ViewByPml4Pa(procPml4) : nullptr;
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
            RtlFillMemory(buf, PAGE_SIZE, 0x11);
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
    else
    {
        status = STATUS_INVALID_PARAMETER;
    }

    KeRevertToUserGroupAffinityThread(&prev);
    return status;
}

} // namespace svmb
