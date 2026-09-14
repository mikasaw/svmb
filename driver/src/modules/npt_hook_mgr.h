// svmb - NPT hidden-page hook manager (offline-capable half).
// Hooks redirect guest *execution* of a target page by swapping the NPT 4K
// entry to a patched hidden page; the original physical page is never
// modified. Activation requires NPT to be live in the VMCB (tier-3); install,
// removal and bookkeeping are fully testable offline.
//
// Concurrency: hash-table STRUCTURE is spinlock-protected; the IOCTL-layer
// gate serializes Install/Remove against each other. With NPT live, the
// VM-exit path also reads this table (HookSlideApply) OUTSIDE that gate: a
// node whose Remove raced an in-flight LookupPage stays alive in the
// graveyard (bounded, freed oldest-first beyond the cap), so stale exit-path
// reads are never a UAF. Stale-state consequence: an exec flip lands on the
// withdrawn-patch hidden copy (guest runs original bytes - benign), while a
// write flip racing the LAST hook's removal can leave the original page
// mapped NX with no hook left; the next fetch then #NPFs into DenyExecute
// and the guest spins with capped logs (see CODE_REVIEW §十-2). Live-NPT
// single-writer prerequisites: see npt.h activation protocol notes.
#ifndef SVMB_NPT_HOOK_MGR_H
#define SVMB_NPT_HOOK_MGR_H

#include "mm/insn_len.h"
#include "mm/npt.h"
#include "platform/util.h"
#include "svmb_protocol.h"

namespace svmb
{

// install rejected: the new hook's patch/steal footprint overlaps another
// hook on the same page (distinct from generic NOT_SUPPORTED so callers and
// tests can tell the rejection reasons apart)
constexpr NTSTATUS NPT_HOOK_STATUS_OVERLAP = (NTSTATUS)0xC0DE0003;

// trampoline-page layout (page allocated per hook in Install):
//   0x000  stolen prefix (original bytes, re-executed on passthrough)
//   stolen FF 25 disp32 - register-free tail jump to the resume slot
//   0x100  8B resume slot: targetVa+stolen (the body, behind the patch)
//   0x108  8B entry slot: trampoline entry VA (stub passthrough target)
//   0x180  per-hook callback stub (InstallCallback, ~106 bytes, ends < 0x200)
constexpr u32 TRAMP_RESUME_SLOT = 0x100;
constexpr u32 TRAMP_ENTRY_SLOT = 0x108;
constexpr u32 TRAMP_STUB_OFF = 0x180;

// volatile-GPR frame captured by the callback stub, passed to every
// SvmbHookCallback. Field order matches the stub's stack layout: the stub
// reserves [rsp+0x00..0x1F] as the call's shadow space, stores EntryRsp at
// [rsp+0x20], keeps an 8-byte alignment pad at [rsp+0x28], and the seven
// pushed volatile GPRs land at [rsp+0x30..0x60] (the frame base = rsp+0x20).
struct SVMB_HOOK_FRAME
{
    u64 EntryRsp; // original entry rsp; [EntryRsp] = caller return address
    u64 Pad;      // stub alignment padding (always 0 - not a register)
    u64 R11;
    u64 R10;
    u64 R9;
    u64 R8;
    u64 Rdx;
    u64 Rcx;
    u64 Rax;      // WARNING: already clobbered by the 12-byte patch
                  // (mov rax,detour) before the stub runs - kept for layout
                  // completeness, never meaningful data
    u64 RetAddr() const { return *(u64*)EntryRsp; }
};

// user callback contract:
// - runs at the original caller's IRQL in the caller's process context
//   (Nt* syscalls: PASSIVE_LEVEL); must be nonpaged, allocation-light and
//   must not call APIs that this driver hooks (re-entry through the SAME
//   hook on the same CPU is converted to passthrough by the managed layer;
//   re-entry through a DIFFERENT hook deadlocks/ recurses for real)
// - return 0        -> passthrough: the original function body executes
// - return nonzero  -> override: volatile regs restored, rax = the returned
//   value, `ret` to the original caller (the body never runs). The caller
//   sees it as the function's return value; OUT parameters stay untouched,
//   so override handlers must return a clean error status.
using SvmbHookCallback = u64 (NTAPI*)(const SVMB_HOOK_FRAME* frame);

// managed entry the stub actually calls: applies the per-(cpu,hook)
// reentrancy latch and dispatches to the registered user callback
u64 NTAPI NptHookManagedCb(u64 hookId, const SVMB_HOOK_FRAME* frame);

struct NptHook : HashNode
{
    NptHook* NextAll;
    u64 TargetVa;
    u64 DetourVa;
    u64 PagePa;    // 4K-aligned PA of the target page (slide/restore key)
    u64 OrigPfn;   // original page PFN (for restore)
    u64 HiddenPa;
    u64 HiddenVa;  // kernel VA of the hidden page (patch composition)
    u64 TrampVa;
    u64 TrampPa;
    u32 StolenLen;
    u8  Mode;       // NptHookManager::MODE_*
    // r31b flip-flood guard state (see HookSlideApply)
    u32 Flips;
    u64 LastFlipTsc;
    // callback layer (InstallCallback); HookId = NO_HOOK_ID for raw installs
    u32 HookId;
    SvmbHookCallback Cb;
};

class NptHookManager
{
public:
    static constexpr u8 MODE_JMP = 0; // hidden copy gains a 12-byte abs jump
    static constexpr u8 MODE_CC = 1;  // hidden copy gains 0xCC (needs #BP intercept to fire)
    static constexpr u32 MAX_HOOKS = 32;
    static constexpr u32 MAX_GRAVEYARD = 32; // parked removed hooks before
                                             // the oldest is freed for real
    static constexpr u32 NO_HOOK_ID = 0xFFFFFFFF;

    static constexpr u32 MAX_CALLBACK_CPUS = 256; // latch array ceiling (same
                                                  // bound as the logger)

    // slide states of a hooked page (NPT leaf perms):
    //   DATA  - original page, P|RW|NX: data reads/writes succeed, a fetch
    //           #NPFs (NX) into the slide
    //   EXEC  - hidden page, P|X, no RW: the patched code executes, a write
    //           #NPFs back into the slide. NPT has no read-disable: while
    //           EXEC-mapped, data reads observe the patched copy (accepted
    //           window - same tradeoff as a classic EPT slide).
    //
    // r32: User MUST be true in BOTH states. On this VMware vhv host any
    // nested leaf with U/S=0 faults UNCONDITIONALLY - even supervisor
    // (CPL=0) data reads/fetches - while U/S=1 leaves never do (identity
    // fill: 5min idle + 60k exits, zero NPFs). With User=false the armed
    // page denied its OWN first access, the slide thrashed, and the deny
    // spin-break derailed the faulting thread (r31 boots 1/3/4).
    static constexpr NptPerms SLIDE_DATA = {true, false, true};
    static constexpr NptPerms SLIDE_EXEC = {false, true, true};

    NTSTATUS Init(NptManager* mm); // fills the default view (PassiveLevel)
    void Deinit();                 // removes all hooks, frees everything

    NTSTATUS Install(u64 targetVa, u64 detourVa, u8 mode);

    // install with the generic callback layer: a per-hook stub is emitted on
    // the trampoline page (saves the volatile GPRs, calls the managed
    // callback with a SVMB_HOOK_FRAME, restores, then register-free jumps to
    // the stolen prefix, or returns the callback's value on override).
    // hookIdOut receives the assigned callback slot (0..MAX_HOOKS-1), which
    // also selects the stub's baked hook id. Several callbacks may be live
    // at once (each hook owns its trampoline page + stub).
    NTSTATUS InstallCallback(u64 targetVa, u32& hookIdOut,
                             SvmbHookCallback cb, u8 mode = MODE_JMP);
    NTSTATUS Remove(u64 targetVa);
    // fills out->Hooks up to the protocol capacity; returns total count
    u32 FillList(SVMB_NPT_HOOK_CTL* out) const;

    // hook record for a 4K-aligned GPA, null when the page is not hooked
    NptHook* LookupPage(u64 gpa4k) const;

    // hook record for a target VA, null when not installed (r31 stage-2
    // byte dump: the probe needs HiddenVa/TrampVa/StolenLen for forensics)
    const NptHook* FindHook(u64 targetVa) const
    {
        HashNode* n = Hooks_.Find(targetVa);
        return n ? (const NptHook*)n : nullptr;
    }

    // removed hooks parked for exit-path lifetime safety
    u32 GraveyardCount() const { return GraveyardCount_; }

    // r36 teardown safety: number of guest threads currently inside the
    // managed callback (stub crossings). Remove/Deinit drain this to zero
    // (bounded) before freeing trampoline/hidden pages, so a crossing never
    // lands on freed memory.
    static u32 InflightCrossings();
    // bounded (1s) wait at PASSIVE_LEVEL for in-flight crossings to drain;
    // if the count is still nonzero afterwards the caller's fail-fast guard
    // takes over (see DevUnload)
    static void DrainInflightCrossings();

    // multiple hooks per page: every hook on one page shares the hidden
    // copy and composes its patch into it. Patch/steal regions of hooks on
    // the same page must be pairwise disjoint (Install rejects overlaps),
    // so each hook's region can be restored independently on Remove. The
    // hidden page is freed when the page's last hook goes away.

    // true when v is the view the hooks mutate (Install/ArmSlide bind to
    // NptManager's default view) - HookSlideApply declines other views
    bool OwnsView(const NptView& v) const
    {
        return Mm_ && &Mm_->Default() == &v;
    }

    // arm every hook to the DATA state (called when NPT goes live; Install
    // arms its own page when NPT is already live)
    void ArmSlide(NptView& view);

private:
    NTSTATUS StealLen(u64 targetVa, u32 need, u32 pageRemain, u32& stolenOut) const;

    // Install split for the callback layer: PrepareHook does everything that
    // does not depend on the detour VA (validation, steal, hidden copy,
    // trampoline build) WITHOUT arming; ArmHook composes the patch (needs
    // the detour VA), arms the slide and links the node. InstallCallback
    // needs the trampoline VA to host its stub BEFORE arming, so the detour
    // (= trampVa+TRAMP_STUB_OFF) only exists between the two halves.
    struct HookBuild
    {
        u64 PagePa;
        u64 HiddenPa;
        void* HiddenVa;
        void* TrampVa;
        u64 TrampPa;
        u32 Stolen;
        u64 Off;
        bool NewPage;
    };
    NTSTATUS PrepareHook(u64 targetVa, u8 mode, HookBuild& out);
    NTSTATUS ArmHook(const HookBuild& b, u64 targetVa, u64 detourVa, u8 mode,
                     u32 hookId = NO_HOOK_ID, SvmbHookCallback cb = nullptr);
    // emits the stub; callTarget is baked as the stub's call destination
    // (always NptHookManagedCb - the stub passes (hookId, frame))
    static void EmitCallbackStub(u8* s, u64 hookId, void* callTarget);

    // hooks sharing a page iterate the flat list comparing PagePa (MAX_HOOKS
    // bound keeps this trivial)
    NptHook* FirstPageHook(u64 pagePa) const;
    u32 CountPageHooks(u64 pagePa) const;
    // true when some live or parked node still references the hidden copy
    bool HiddenShared(u64 hiddenVa) const;
    // h must already be off both lists; frees hidden (if unshared) + tramp
    void FreeHookStorage(NptHook* h);

    static u32 PatchLen(u8 mode) { return mode == MODE_JMP ? 12 : 1; }

    NptManager* Mm_ = nullptr;
    HashTable Hooks_;     // key = target VA (unique per hook; several hooks
                          // may share one page)
    NptHook* HookList_ = nullptr;
    NptHook* Graveyard_ = nullptr; // removed nodes kept alive for exit-path
                                   // readers (see concurrency note above)
    u32 GraveyardCount_ = 0;
    u32 Count_ = 0;
};

// NPF slide callback body (registered into mm/npf via NpfSetSlideHook by
// main.cpp): looks up the hook owning gpa4k and flips the page between the
// EXEC and DATA states. Returns false when the GPA is not hooked or the
// fault type is not slide business (user/spurious faults decline). Only the
// default view is hooked/armed (M3 constraint: hooks bind to the view
// Install mutated), so faults surfacing on any other active view decline.
bool HookSlideApply(NptHookManager& hm, NptView& view, u64 gpa4k, u64 info1);

// main.cpp: accessor to the global hook manager (the tier-1 pointers live
// in main.cpp's anonymous namespace, so module TUs cannot link gHooks
// directly). Callers must hold the gate - module Init/Stop qualify.
NptHookManager* SysAuditGlobalHooks();

} // namespace svmb

#endif // SVMB_NPT_HOOK_MGR_H