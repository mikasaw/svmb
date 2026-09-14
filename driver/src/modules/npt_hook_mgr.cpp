#include "modules/npt_hook_mgr.h"
#include "hw/svm_defs.h"
#include "mm/tlb.h"
#include "platform/logger.h"

namespace svmb
{

namespace {
constexpr u64 PAGE_MASK = ~0xFFFull;

// withdraw a composed patch: copy the live original bytes back over the
// hook's patch region in the hidden copy
void UndoPatch(u64 pageVa, u64 off, u32 patchLen, void* hiddenVa)
{
    RtlCopyMemory((u8*)hiddenVa + off, (u8*)pageVa + off, patchLen);
}

// callback registry: slot -> user callback. Slots are assigned/cleared under
// the IOCTL gate (Install/Remove); reads happen in guest context WITHOUT it,
// but 8-byte aligned pointer stores are atomic on x64, so a reader sees
// either null (passthrough) or a valid nonpaged code pointer.
SvmbHookCallback g_cbTable[NptHookManager::MAX_HOOKS] = {};

// per-(cpu,hook) reentrancy latch: when a callback calls a hooked API and
// re-enters its OWN hook on the same CPU, the nested managed call declines
// (passthrough) instead of recursing. Entry through a DIFFERENT hook is the
// callback author's responsibility (documented in the header contract).
volatile LONG g_cbLatch[NptHookManager::MAX_HOOKS *
                        NptHookManager::MAX_CALLBACK_CPUS] = {};

// r36 teardown safety: guest threads currently inside the managed callback.
// Inc at callback entry, dec at exit; Remove/Deinit drain this (bounded)
// before freeing trampoline/hidden pages. The few stub instructions outside
// the callback (push/call/restore/jmp) are covered by the drain's delay
// budget - a crossing spends nanoseconds there and the disarmed page cannot
// start NEW crossings.
volatile LONG g_npthk_inflight = 0;
} // namespace

u32 NptHookManager::InflightCrossings()
{
    return (u32)g_npthk_inflight;
}

void NptHookManager::DrainInflightCrossings()
{
    // PASSIVE_LEVEL only (KeDelayExecutionThread waits); callers run under
    // the driver gate at PASSIVE (Remove via IOCTL, Deinit via DevUnload).
    for (int ms = 0; ms < 1000 && g_npthk_inflight != 0; ++ms)
    {
        LARGE_INTEGER delay;
        delay.QuadPart = -10000; // 1ms
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
}

u64 NTAPI NptHookManagedCb(u64 hookId, const SVMB_HOOK_FRAME* frame)
{
    if (hookId >= NptHookManager::MAX_HOOKS)
        return 0;
    u32 cpu = CurrentCpuIndex();
    if (cpu >= NptHookManager::MAX_CALLBACK_CPUS)
        return 0;
    volatile LONG* latch =
        &g_cbLatch[cpu * NptHookManager::MAX_HOOKS + hookId];
    if (InterlockedCompareExchange(latch, 1, 0) != 0)
        return 0; // recursion through this hook on this CPU: passthrough
    InterlockedIncrement(&g_npthk_inflight);
    u64 result = 0;
    SvmbHookCallback cb = g_cbTable[hookId];
    if (cb)
        result = cb(frame);
    InterlockedDecrement(&g_npthk_inflight);
    InterlockedExchange(latch, 0);
    return result;
}

NTSTATUS NptHookManager::Init(NptManager* mm)
{
    if (!mm)
        return STATUS_INVALID_PARAMETER;
    NTSTATUS status = Hooks_.Init(32, TAG_NPTHK);
    if (!NT_SUCCESS(status))
        return status;
    Mm_ = mm;
    status = mm->FillDefaultView();
    if (!NT_SUCCESS(status) && status != STATUS_ALREADY_REGISTERED)
    {
        Hooks_.Deinit();
        Mm_ = nullptr;
        return status;
    }
    return STATUS_SUCCESS;
}

void NptHookManager::Deinit()
{
    // r36: let in-flight stub crossings finish before any page is freed -
    // past this point the view still exists, so each page's mapping is
    // restored and its split reference dropped exactly once (when its last
    // live hook is popped)
    DrainInflightCrossings();
    while (HookList_)
    {
        NptHook* h = HookList_;
        HookList_ = h->NextAll;
        if (h->Cb)
            g_cbTable[h->HookId] = nullptr; // callbacks now passthrough
        Mm_->Default().SwapPage4k(h->PagePa, h->OrigPfn, {true, true, true});
        bool pageStillHooked = FirstPageHook(h->PagePa) != nullptr;
        if (!pageStillHooked)
            Mm_->Default().UnrefSplit(h->PagePa);
        FreeHookStorage(h); // h is off both lists; shared-hidden aware
    }
    while (Graveyard_)
    {
        NptHook* h = Graveyard_;
        Graveyard_ = h->NextAll;
        FreeHookStorage(h);
    }
    GraveyardCount_ = 0;
    HookList_ = nullptr;
    Hooks_.Deinit();
    Mm_ = nullptr;
    Count_ = 0;
}

NptHook* NptHookManager::FirstPageHook(u64 pagePa) const
{
    for (NptHook* h = HookList_; h; h = h->NextAll)
    {
        if (h->PagePa == pagePa)
            return h;
    }
    return nullptr;
}

u32 NptHookManager::CountPageHooks(u64 pagePa) const
{
    u32 count = 0;
    for (NptHook* h = HookList_; h; h = h->NextAll)
    {
        if (h->PagePa == pagePa)
            ++count;
    }
    return count;
}

NTSTATUS NptHookManager::StealLen(u64 targetVa, u32 need, u32 pageRemain,
                                  u32& stolenOut) const
{
    // walk instructions from the target until at least `need` bytes are
    // covered; RIP-relative instructions are rejected (the trampoline copy
    // would execute them at a different address) and so are control
    // transfers (call/jmp/jcc/loop/jrcxz/ret - r35: their displacement is
    // PC-relative, so re-executing the copy from the trampoline would jump
    // to the wrong target; `ret` in a prefix would return early)
    u32 stolen = 0;
    const u8* code = (const u8*)targetVa;
    while (stolen < need)
    {
        u32 ripFlags = 0;
        u32 ilen = InsnLen64(code + stolen, pageRemain - stolen, &ripFlags);
        if (ilen == 0)
            return STATUS_NOT_SUPPORTED;
        if (ripFlags & (INSN_F_RIPREL | INSN_F_CTLX))
            return STATUS_NOT_SUPPORTED;
        stolen += ilen;
    }
    stolenOut = stolen;
    return STATUS_SUCCESS;
}

NTSTATUS NptHookManager::Install(u64 targetVa, u64 detourVa, u8 mode)
{
    if (!IsKernelVa(detourVa))
        return STATUS_INVALID_PARAMETER;
    HookBuild b = {};
    NTSTATUS st = PrepareHook(targetVa, mode, b);
    if (!NT_SUCCESS(st))
        return st;
    return ArmHook(b, targetVa, detourVa, mode);
}

NTSTATUS NptHookManager::InstallCallback(u64 targetVa, u32& hookIdOut,
                                         SvmbHookCallback cb, u8 mode)
{
    hookIdOut = NO_HOOK_ID;
    if (!cb)
        return STATUS_INVALID_PARAMETER;

    // claim a callback slot; assignment happens under the IOCTL gate and the
    // stub is not armed until ArmHook, so guest-context readers can only see
    // null or the final valid pointer
    u32 id = MAX_HOOKS;
    for (u32 s = 0; s < MAX_HOOKS; ++s)
    {
        if (!g_cbTable[s])
        {
            g_cbTable[s] = cb;
            id = s;
            break;
        }
    }
    if (id == MAX_HOOKS)
        return STATUS_INSUFFICIENT_RESOURCES; // no free callback slot

    HookBuild b = {};
    NTSTATUS st = PrepareHook(targetVa, mode, b);
    if (!NT_SUCCESS(st))
    {
        g_cbTable[id] = nullptr;
        return st;
    }
    // the detour is the per-hook stub on this hook's own trampoline page;
    // it must exist BEFORE arming (the first fetch after the slide arms
    // jumps straight into it). The stub calls the MANAGED callback (latch +
    // registry dispatch), not the user callback directly - a direct call
    // would skip the reentrancy latch and the null-slot passthrough.
    EmitCallbackStub((u8*)b.TrampVa + TRAMP_STUB_OFF, id, NptHookManagedCb);
    st = ArmHook(b, targetVa, (u64)b.TrampVa + TRAMP_STUB_OFF, mode, id, cb);
    if (!NT_SUCCESS(st))
    {
        g_cbTable[id] = nullptr;
        return st;
    }
    hookIdOut = id;
    return STATUS_SUCCESS;
}

NTSTATUS NptHookManager::PrepareHook(u64 targetVa, u8 mode, HookBuild& out)
{
    if (!Mm_ || !IsKernelVa(targetVa))
        return STATUS_INVALID_PARAMETER;
    if (mode != MODE_JMP && mode != MODE_CC)
        return STATUS_INVALID_PARAMETER;
    if (Count_ >= MAX_HOOKS)
        return STATUS_INSUFFICIENT_RESOURCES;

    u64 pageVa = targetVa & PAGE_MASK;
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress((void*)pageVa);
    if (pa.QuadPart == 0)
        return STATUS_INVALID_PARAMETER; // target not backed (paged out)
    u64 pagePa = pa.QuadPart & PAGE_MASK;

    NptView& view = Mm_->Default();
    NTSTATUS st = view.Split2M(pagePa); // idempotent when already split
    if (!NT_SUCCESS(st))
        return st;

    u32 need = (mode == MODE_JMP) ? 12 : 1;
    u32 pageRemain = (u32)(0x1000 - (targetVa & ~PAGE_MASK));
    u32 stolen = 0;
    st = StealLen(targetVa, need, pageRemain, stolen);
    if (!NT_SUCCESS(st))
        return st;
    if (mode == MODE_JMP && stolen + 6 > TRAMP_RESUME_SLOT)
        return STATUS_NOT_SUPPORTED; // stolen prefix would run into the slot

    // hooks compose into ONE shared hidden copy per page: their
    // patch+steal footprints must be pairwise disjoint so each patch can
    // be applied and restored independently
    u64 off = targetVa & ~PAGE_MASK;
    u32 region = stolen > need ? stolen : need;
    for (NptHook* o = HookList_; o; o = o->NextAll)
    {
        if (o->PagePa != pagePa)
            continue;
        u64 off2 = o->TargetVa & ~PAGE_MASK;
        u32 oLen = o->StolenLen;
        u32 oPatch = PatchLen(o->Mode);
        if (oPatch > oLen)
            oLen = oPatch;
        if (off < off2 + oLen && off2 < off + region)
            return NPT_HOOK_STATUS_OVERLAP; // overlapping hook footprints
    }

    RtlZeroMemory(&out, sizeof(out));
    out.PagePa = pagePa;
    out.Off = off;
    out.Stolen = stolen;
    NptHook* head = FirstPageHook(pagePa);
    out.NewPage = (head == nullptr);
    if (out.NewPage)
    {
        // first hook on the page: full copy of the original
        out.HiddenVa = AllocNptPage(out.HiddenPa);
        if (!out.HiddenVa)
            return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyMemory(out.HiddenVa, (void*)pageVa, 0x1000);
    }
    else
    {
        out.HiddenPa = head->HiddenPa;
        out.HiddenVa = (void*)head->HiddenVa;
    }

    out.TrampVa = AllocExecPage(TAG_HOOK);
    if (!out.TrampVa)
    {
        if (out.NewPage)
            FreeNptPage(out.HiddenVa);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    out.TrampPa = MmGetPhysicalAddress(out.TrampVa).QuadPart;

    if (mode == MODE_JMP)
    {
        // trampoline: stolen bytes + register-free tail jump (r34: the tail
        // must not touch ANY register - the body behind the stolen prefix
        // stages rax (=0, left by e.g. NtCreateFile's xor eax,eax) into
        // locals). The resume VA lives in a slot on this page; the entry
        // slot mirrors the trampoline entry for the callback stub's
        // passthrough jump.
        RtlCopyMemory(out.TrampVa, (void*)targetVa, stolen);
        u8* back = (u8*)out.TrampVa + stolen;
        back[0] = 0xFF;
        back[1] = 0x25; // jmp qword ptr [rip+disp32]
        u32 disp = TRAMP_RESUME_SLOT - stolen - 6;
        RtlCopyMemory(back + 2, &disp, 4);
        u64 resume = targetVa + stolen;
        RtlCopyMemory((u8*)out.TrampVa + TRAMP_RESUME_SLOT, &resume, 8);
        u64 entry = (u64)out.TrampVa;
        RtlCopyMemory((u8*)out.TrampVa + TRAMP_ENTRY_SLOT, &entry, 8);
    }
    else
    {
        // CC mode has no return path by itself; the tier-3 #BP handler
        // re-executes the original instruction through this trampoline.
        // Clamp to the page: crossing into an unmapped VA would bugcheck.
        u32 copy = pageRemain < 16 ? pageRemain : 16;
        RtlCopyMemory(out.TrampVa, (void*)targetVa, copy);
    }
    return STATUS_SUCCESS;
}

NTSTATUS NptHookManager::ArmHook(const HookBuild& b, u64 targetVa,
                                 u64 detourVa, u8 mode, u32 hookId,
                                 SvmbHookCallback cb)
{
    NptView& view = Mm_->Default();

    // compose this hook's patch into the shared hidden copy (first statement
    // that depends on detourVa - everything before lives in PrepareHook)
    u8* patch = (u8*)b.HiddenVa + b.Off;
    if (mode == MODE_JMP)
    {
        patch[0] = 0x48;
        patch[1] = 0xB8; // mov rax, detourVa
        RtlCopyMemory(patch + 2, &detourVa, 8);
        patch[10] = 0xFF;
        patch[11] = 0xE0; // jmp rax
    }
    else
    {
        patch[0] = 0xCC; // int3 - fires once a #BP intercept exists (tier-3)
    }

    // two arming modes: with NPT live the page starts in the DATA state
    // (original bytes, RW + NX) so the first guest fetch #NPFs into the
    // slide; offline (NPT disabled) the hidden page is published directly -
    // legacy behavior, fully visible to data reads. Only the FIRST hook on
    // a page flips the mapping - later hooks just compose into the hidden
    // copy the page already points at.
    if (b.NewPage)
    {
        NTSTATUS st = Mm_->NptEnabled()
            ? view.SwapPage4k(b.PagePa, b.PagePa >> 12, SLIDE_DATA)
            : view.SwapPage4k(b.PagePa, b.HiddenPa >> 12, {true, true, true});
        if (!NT_SUCCESS(st))
        {
            FreeExecPage(b.TrampVa, TAG_HOOK);
            FreeNptPage(b.HiddenVa);
            return st;
        }
        view.RefSplit(b.PagePa); // page-level split reference
    }
    TlbInvlpgaLocal(b.PagePa);

    NptHook* h = (NptHook*)AllocNonPaged(sizeof(*h), TAG_NPTHK);
    if (!h)
    {
        if (b.NewPage)
        {
            view.SwapPage4k(b.PagePa, b.PagePa >> 12, {true, true, true});
            view.UnrefSplit(b.PagePa);
            FreeNptPage(b.HiddenVa);
        }
        else
        {
            UndoPatch(targetVa & PAGE_MASK, b.Off, PatchLen(mode),
                      b.HiddenVa);
        }
        FreeExecPage(b.TrampVa, TAG_HOOK);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(h, sizeof(*h));
    h->Key = targetVa; // unique per hook (several hooks may share a page)
    h->TargetVa = targetVa;
    h->DetourVa = detourVa;
    h->PagePa = b.PagePa;
    h->OrigPfn = b.PagePa >> 12;
    h->HiddenPa = b.HiddenPa;
    h->HiddenVa = (u64)b.HiddenVa;
    h->TrampVa = (u64)b.TrampVa;
    h->TrampPa = b.TrampPa;
    h->StolenLen = b.Stolen;
    h->Mode = mode;
    h->HookId = hookId;
    h->Cb = cb;
    h->NextAll = HookList_;
    HookList_ = h;
    Hooks_.Insert(h);
    ++Count_;

    SVMB_LOGI("npt hook: target=%llx detour=%llx hidden=%llx tramp=%llx "
              "stolen=%u mode=%u hooks-on-page=%u cb=%u",
              targetVa, detourVa, b.HiddenPa, (u64)b.TrampVa, b.Stolen, mode,
              CountPageHooks(b.PagePa), hookId);
    return STATUS_SUCCESS;
}

// Per-hook callback stub, emitted at trampVa+TRAMP_STUB_OFF by InstallCallback.
// Entered from the hidden-copy patch with the original caller's state
// (rsp = entry rsp, rsp%16==8; rax already clobbered by the patch). It:
//   1. pushes the volatile GPRs and builds a SVMB_HOOK_FRAME on the stack
//   2. cld + calls the managed callback (hookId, frame)
//   3. on null return: restores everything, register-free jumps to the
//      trampoline entry (stolen prefix) - r34's lesson, no touched register
//      survives into the body
//   4. on nonzero return: restores everything except rax, `ret`s to the
//      original caller with rax = the callback's value (override; the body
//      never runs)
// Non-volatile GPRs are never touched, so the caller's state is intact in
// both paths.
void NptHookManager::EmitCallbackStub(u8* s, u64 hookId, void* callTarget)
{
    const u32 off = TRAMP_STUB_OFF; // page offset of the stub (for rip-rel disp)
    u32 i = 0;
    auto b = [&](u8 v) { s[i++] = v; };
    auto imm64 = [&](u64 v) { RtlCopyMemory(s + i, &v, 8); i += 8; };
    auto disp32 = [&](u32 targetOff) {
        u32 d = targetOff - (off + i + 4);
        RtlCopyMemory(s + i, &d, 4);
        i += 4;
    };

    b(0x50);                                        // push rax
    b(0x51);                                        // push rcx
    b(0x52);                                        // push rdx
    b(0x41); b(0x50);                               // push r8
    b(0x41); b(0x51);                               // push r9
    b(0x41); b(0x52);                               // push r10
    b(0x41); b(0x53);                               // push r11   rsp=R-38h
    b(0x48); b(0x83); b(0xEC); b(0x30);             // sub rsp,30h (shadow 20h +
                                                    //  EntryRsp slot) rsp=R-68h
    b(0x48); b(0x8D); b(0x44); b(0x24); b(0x68);    // lea rax,[rsp+68h] (=R)
    b(0x48); b(0x89); b(0x44); b(0x24); b(0x20);    // mov [rsp+20h],rax (EntryRsp)
    b(0x48); b(0xB9); imm64(hookId);                // mov rcx, hookId
    b(0x48); b(0x8D); b(0x54); b(0x24); b(0x20);    // lea rdx,[rsp+20h] (frame)
    b(0x48); b(0xB8); imm64((u64)callTarget);       // mov rax, call target
    b(0xFC);                                        // cld (DF=0 for the call)
    b(0xFF); b(0xD0);                               // call rax
    b(0x48); b(0x85); b(0xC0);                      // test rax, rax
    u32 jnzPos = i;
    b(0x75); b(0x00);                               // jnz override (patched)
    // passthrough: restore, run the stolen prefix via the entry slot.
    // r35b: add rsp,30h undoes ONLY the sub (the 7 pops undo the pushes) -
    // the first cut added 0x68 here, leaving rsp = R+0x38 at the body and
    // trampling the caller's syscall trap frame (0x3B, guest RIP garbage).
    b(0x48); b(0x83); b(0xC4); b(0x30);             // add rsp,30h
    b(0x41); b(0x5B);                               // pop r11
    b(0x41); b(0x5A);                               // pop r10
    b(0x41); b(0x59);                               // pop r9
    b(0x41); b(0x58);                               // pop r8
    b(0x5A);                                        // pop rdx
    b(0x59);                                        // pop rcx
    b(0x58);                                        // pop rax (patch-clobbered)
    b(0xFF); b(0x25); disp32(TRAMP_ENTRY_SLOT);
    // ^ FF 25 disp32: jmp qword ptr [rip+...] -> TRAMP_ENTRY_SLOT
    s[jnzPos + 1] = (u8)(i - (jnzPos + 2));         // override target = here
    // override: restore everything except rax, ret with the callback value
    b(0x48); b(0x83); b(0xC4); b(0x30);             // add rsp,30h (r35b fix)
    b(0x41); b(0x5B);                               // pop r11
    b(0x41); b(0x5A);                               // pop r10
    b(0x41); b(0x59);                               // pop r9
    b(0x41); b(0x58);                               // pop r8
    b(0x5A);                                        // pop rdx
    b(0x59);                                        // pop rcx
    b(0x48); b(0x83); b(0xC4); b(0x08);             // add rsp,8 (discard rax)
    b(0xC3);                                        // ret
    // stub must stay clear of anything mapped after TRAMP_STUB_OFF
    if (i > 0x80)
        RtlFillMemory(s, i, 0xCC); // should be unreachable; fail loud offline
}

NTSTATUS NptHookManager::Remove(u64 targetVa)
{
    if (!Mm_ || !IsKernelVa(targetVa))
        return STATUS_INVALID_PARAMETER;

    HashNode* n = Hooks_.Find(targetVa); // key = target VA (unique per hook)
    if (!n)
        return STATUS_NOT_FOUND;
    NptHook* h = (NptHook*)n;
    if (h->Cb)
        g_cbTable[h->HookId] = nullptr; // nested callbacks now passthrough
    u64 pagePa = h->PagePa;
    u64 pageVa = h->TargetVa & PAGE_MASK;
    u64 off = h->TargetVa & ~PAGE_MASK;

    // page-level teardown only when this is the page's last hook; while
    // siblings remain, the view keeps pointing at the shared hidden copy
    bool last = CountPageHooks(pagePa) == 1;
    if (last)
    {
        NTSTATUS st = Mm_->Default().SwapPage4k(pagePa, h->OrigPfn,
                                                {true, true, true});
        if (!NT_SUCCESS(st))
            return st;
        Mm_->Default().UnrefSplit(pagePa);
    }

    // withdraw this hook's patch from the shared hidden copy (no-fail),
    // then unlink - past this point the removal always completes
    UndoPatch(pageVa, off, PatchLen(h->Mode), (void*)h->HiddenVa);

    NptHook** pp = &HookList_;
    while (*pp && *pp != h)
        pp = (NptHook**)&(*pp)->NextAll;
    if (*pp)
        *pp = h->NextAll;
    Hooks_.Remove(targetVa);

    if (last)
        TlbInvlpgaLocal(pagePa); // drop stale slide translations (no-op offline)

    // park instead of freeing: the VM-exit path may already hold this node
    // pointer from a LookupPage that raced the removal (the IOCTL gate does
    // not cover exit contexts). Graveyarded memory stays valid, so stale
    // reads are never a UAF. Stale-state consequences: an exec flip lands
    // on the withdrawn-patch hidden copy (guest runs original bytes -
    // benign), but a write flip racing the LAST hook's removal can leave
    // the original page mapped NX with no hook left - the next fetch then
    // #NPFs into DenyExecute and the guest spins with capped logs
    // (CODE_REVIEW §十-2), no memory-safety impact.
    h->NextAll = Graveyard_;
    Graveyard_ = h;
    ++GraveyardCount_;
    if (GraveyardCount_ > MAX_GRAVEYARD)
    {
        // bound the pool: free the OLDEST parked entry for real - the
        // exit-path race window reopens for that node only. r36: drain
        // first so a crossing inside this node's tramp/callback never
        // lands on freed memory.
        DrainInflightCrossings();
        NptHook** gp = &Graveyard_;
        while ((*gp)->NextAll)
            gp = &(*gp)->NextAll;
        NptHook* oldest = *gp;
        *gp = nullptr;
        FreeHookStorage(oldest);
        --GraveyardCount_;
    }
    --Count_;

    SVMB_LOGI("npt unhook: target=%llx remaining-on-page=%u graveyard=%u",
              targetVa, CountPageHooks(pagePa), GraveyardCount_);
    return STATUS_SUCCESS;
}

u32 NptHookManager::FillList(SVMB_NPT_HOOK_CTL* out) const
{
    u32 total = 0;
    NptHook* h = HookList_;
    while (h)
    {
        if (total < 32)
        {
            out->Hooks[total].TargetVa = h->TargetVa;
            out->Hooks[total].DetourVa = h->DetourVa;
        }
        ++total;
        h = h->NextAll;
    }
    out->Count = total;
    return total;
}

NptHook* NptHookManager::LookupPage(u64 gpa4k) const
{
    // flat scan: several hooks may share the page PA (hash keys are target
    // VAs now), and MAX_HOOKS keeps this trivial. Graveyarded (removed)
    // nodes are not on HookList_, so removal is observable immediately.
    u64 pagePa = gpa4k & PAGE_MASK;
    return FirstPageHook(pagePa);
}

bool NptHookManager::HiddenShared(u64 hiddenVa) const
{
    for (NptHook* h = HookList_; h; h = h->NextAll)
        if (h->HiddenVa == hiddenVa)
            return true;
    for (NptHook* h = Graveyard_; h; h = h->NextAll)
        if (h->HiddenVa == hiddenVa)
            return true;
    return false;
}

void NptHookManager::FreeHookStorage(NptHook* h)
{
    // hooks sharing a page reference ONE hidden copy - free it exactly once
    if (!HiddenShared(h->HiddenVa))
        FreeNptPage((void*)h->HiddenVa);
    FreeExecPage((void*)h->TrampVa, TAG_HOOK);
    FreeNonPaged(h, TAG_NPTHK);
}

void NptHookManager::ArmSlide(NptView& view)
{
    for (NptHook* h = HookList_; h; h = h->NextAll)
    {
        NTSTATUS st = view.SwapPage4k(h->PagePa, h->OrigPfn, SLIDE_DATA);
        if (!NT_SUCCESS(st))
        {
            SVMB_LOGE("npt hook arm failed gpa=%llx st=%08x", h->PagePa, st);
            continue;
        }
        TlbInvlpgaLocal(h->PagePa);
    }
}

bool HookSlideApply(NptHookManager& hm, NptView& view, u64 gpa4k, u64 info1)
{
    // r31 diagnostics: first slide invocations tell whether the hook was
    // found and whether the swap took on the software side
    constexpr u32 MAX_SLIDE_DIAG = 16;
    static volatile LONG s_slideDiag = 0;
    auto diag = [&]() -> bool {
        LONG n = InterlockedIncrement(&s_slideDiag);
        return n <= MAX_SLIDE_DIAG;
    };

    // hooks bind to the default view only - faults surfacing on any other
    // active view decline quietly (constraint documented in the header)
    if (!hm.OwnsView(view))
        return false;

    NptHook* h = hm.LookupPage(gpa4k);
    if (!h)
        return false; // not ours - let the classifier speak

    const bool exec = (info1 & NPF_EXECUTE) != 0;
    const bool write = (info1 & NPF_WRITE) != 0;
    if (!exec && !write)
        return false; // user/spurious faults are not slide business

    // r31b flip-flood guard: a healthy slide flips each page a handful of
    // times per second at most. The SAME page re-faulting immediately after
    // a successful swap means the CPU keeps serving a stale translation -
    // flipping again is a livelock (and the flipping thread usually holds
    // the IO gate, deadlocking every diagnostic IOCTL). Past the bound,
    // decline: the classifier's deny path takes over, its spin breaker
    // advances RIP, and the triggering probe FAILS but RETURNS with the
    // diagnostic ring intact. Counter self-resets after a quiet second.
    u64 tsc = __rdtsc();
    if (tsc - h->LastFlipTsc > 3000000000ull) // ~1s @3GHz
        h->Flips = 0;
    h->LastFlipTsc = tsc;
    if (++h->Flips > 64)
        return false;

    // EXEC state on a fetch fault (NX was armed), DATA state on a write
    // fault (the EXEC state maps without RW). Concurrent flips from two
    // cores converge: the PTE swap is an atomic 8-byte store and a lost
    // update just re-faults into the same decision.
    NptPerms perms = exec ? NptHookManager::SLIDE_EXEC : NptHookManager::SLIDE_DATA;
    u64 pfn = exec ? (h->HiddenPa >> 12) : h->OrigPfn;
    NTSTATUS st = view.SwapPage4k(h->PagePa, pfn, perms);
    if (!NT_SUCCESS(st))
    {
        // r31: capped - unbounded LOGE here was log-storm livelock fuel
        if (diag())
            SVMB_LOGE("npf slide: swap failed gpa=%llx pfn=%llx st=%08x",
                      h->PagePa, pfn, st);
        return false;
    }
    TlbInvlpgaLocal(h->PagePa);

    // r31 readback: if the PTE reads back as the state we just wrote but the
    // SAME fault re-fires, the CPU is serving a stale translation (flush
    // contract broken) - the PTE dump proves the software side is correct
    if (diag())
    {
        u64 pteNow = 0;
        NTSTATUS stR = view.GetPte4k(h->PagePa, pteNow);
        SVMB_LOGI("npf slide: %s flip gpa=%llx pte=%llx (rd st=%08x)",
                  exec ? "EXEC" : "DATA", h->PagePa, pteNow, stR);
    }
    return true; // faulting instruction re-executes against the new state
}

} // namespace svmb
