#include "modules/cr3_seed.h"
#include "platform/logger.h"
#include "platform/offsets.h"

namespace svmb
{

namespace
{
Cr3Seed* g_activeSeed = nullptr; // static-callback bridge (single instance)
volatile LONG s_dtbLogGate = 0;  // capped "offsets unresolved" logging
}

bool Cr3ImageMatch(const char* stored, const char* want)
{
    if (!want || !want[0])
        return false;
    for (u32 i = 0; i < 16; ++i)
    {
        char a = stored[i];
        char b = want[i];
        if (b == 0)
            return a == 0; // exact length: no prefix matches
        a = (a >= 'A' && a <= 'Z') ? char(a + 32) : a;
        b = (b >= 'A' && b <= 'Z') ? char(b + 32) : b;
        if (a != b)
            return false;
    }
    return false; // wanted name exceeds the stored capacity
}

NTSTATUS Cr3Seed::Init()
{
    if (g_activeSeed)
        return STATUS_ALREADY_REGISTERED;
    NTSTATUS status = Table_.Init(128, TAG_CR3);
    if (!NT_SUCCESS(status))
        return status;
    KeInitializeSpinLock(&ListLock_);

    status = PsSetCreateProcessNotifyRoutineEx(&Cr3Seed::ProcNotify, FALSE);
    if (!NT_SUCCESS(status))
    {
        Table_.Deinit();
        return status;
    }
    g_activeSeed = this;
    return STATUS_SUCCESS;
}

VOID Cr3Seed::ProcNotify(PEPROCESS process, HANDLE pid,
                         PPS_CREATE_NOTIFY_INFO info)
{
    Cr3Seed* self = g_activeSeed;
    if (!self)
        return;
    u32 key = (u32)(ULONG_PTR)pid;

    if (info) // creation
    {
        Cr3Node* node = (Cr3Node*)AllocNonPaged(sizeof(*node), TAG_CR3);
        if (!node)
            return; // best-effort: the process just is not monitored
        RtlZeroMemory(node, sizeof(*node));
        node->Key = key;
        node->Pid = key;
        // r83: DTB offset resolved at load (OffsetsResolve is idempotent)
        // - without a verified layout the node still lists the process
        // but carries cr3=0 (consumers treat 0 as unknown)
        node->Cr3 = 0;
        if (NT_SUCCESS(OffsetsResolve()))
            node->Cr3 = *(u64*)((u8*)process + Offsets().EprocDtb);
        else if (InterlockedCompareExchange(&s_dtbLogGate, 1, 0) == 0)
            SVMB_LOGE("seed: offsets unresolved - nodes carry cr3=0");
        if (info->ImageFileName && info->ImageFileName->Buffer)
        {
            // store the basename (after the last path separator) - the
            // notify info carries the full \Device\... path, and watch
            // configs name executables, not volumes
            u32 n = info->ImageFileName->Length / sizeof(WCHAR);
            u32 start = 0;
            for (u32 i = 0; i < n; ++i)
            {
                if (info->ImageFileName->Buffer[i] == L'\\')
                    start = i + 1;
            }
            u32 m = n - start;
            if (m > 15)
                m = 15;
            for (u32 i = 0; i < m; ++i)
            {
                WCHAR w = info->ImageFileName->Buffer[start + i];
                node->Image[i] = (w >= 0x20 && w < 0x7F) ? (char)w : '?';
            }
        }

        KIRQL old;
        KeAcquireSpinLock(&self->ListLock_, &old);
        node->NextAll = self->List_;
        self->List_ = node;
        ++self->Count_;
        KeReleaseSpinLock(&self->ListLock_, old);
        self->Table_.Insert(node);

        // consumer hook (PASSIVE, outside the lock - consumers may call
        // back into Lookup*); node memory is stable for the driver lifetime
        Cr3SeedNotifyFn fn = self->NotifyFn_;
        if (fn)
            fn(key, node->Cr3, node->Image, self->NotifyCtx_, process, true);
    }
    else // termination
    {
        HashNode* n = self->Table_.Find(key);
        if (!n)
            return;
        self->Table_.Remove(key);

        KIRQL old;
        KeAcquireSpinLock(&self->ListLock_, &old);
        Cr3Node** pp = &self->List_;
        while (*pp && *pp != (Cr3Node*)n)
            pp = &(*pp)->NextAll;
        if (*pp)
            *pp = (*pp)->NextAll;
        --self->Count_;
        // park in the graveyard instead of freeing: concurrent Lookup()s may
        // still hold the node pointer (freed together at Deinit)
        ((Cr3Node*)n)->NextAll = self->Graveyard_;
        self->Graveyard_ = (Cr3Node*)n;
        KeReleaseSpinLock(&self->ListLock_, old);

        // r47: termination notice - consumers holding NPT tripwires on this
        // process's pages must disarm them before the objects are freed
        // (a deny left on recycled pool memory is a live fault trap)
        Cr3SeedNotifyFn fn = self->NotifyFn_;
        if (fn)
            fn(key, 0, nullptr, self->NotifyCtx_, process, false);
    }
}

void Cr3Seed::Deinit()
{
    if (g_activeSeed != this)
        return;
    PsSetCreateProcessNotifyRoutineEx(&Cr3Seed::ProcNotify, TRUE);
    g_activeSeed = nullptr;
    NotifyFn_ = nullptr; // consumer slot dies with the table

    // callbacks drained by the unregister; the table is now stable
    Cr3Node* lists[2] = {List_, Graveyard_};
    for (Cr3Node* n : lists)
        while (n)
        {
            Cr3Node* next = n->NextAll;
            Table_.Remove(n->Key);
            FreeNonPaged(n, TAG_CR3);
            n = next;
        }
    List_ = nullptr;
    Graveyard_ = nullptr;
    Count_ = 0;
    Table_.Deinit();
}

bool Cr3Seed::Lookup(u32 pid, u64& cr3Out) const
{
    HashNode* n = Table_.Find(pid);
    if (!n)
        return false;
    cr3Out = ((Cr3Node*)n)->Cr3;
    return true;
}

bool Cr3Seed::LookupByImage(const char* image, u64& cr3Out, u32& pidOut) const
{
    if (!image || !image[0])
        return false;
    // lock-free walk over a list whose nodes are never freed while loaded
    for (Cr3Node* n = List_; n; n = n->NextAll)
    {
        if (Cr3ImageMatch(n->Image, image))
        {
            cr3Out = n->Cr3;
            pidOut = n->Pid;
            return true;
        }
    }
    return false;
}

// r89 plan C: cr3 -> pid reverse lookup for the kill policy. Same lock-free
// guarantee as Lookup/LookupByImage: nodes live forever (Graveyard_), so a
// walk racing a notify insert is safe - the node either is not linked yet
// (miss, alert-only) or fully published. Acceptance P2-2: the graveyard
// reuses NextAll, so this walk can step onto a DEAD node whose recycled
// DTB matches the writer - confirm against the live hash table before
// attributing a kill (a dead node's pid may belong to an innocent new
// process).
bool Cr3Seed::LookupPidByCr3(u64 cr3, u32& pidOut, char* imageOut) const
{
    for (Cr3Node* n = List_; n; n = n->NextAll)
    {
        if (n->Cr3 != cr3)
            continue;
        u64 confirm = 0;
        if (!Lookup(n->Pid, confirm) || confirm != cr3)
            continue; // graveyard residue: keep walking for the live node
        pidOut = n->Pid;
        if (imageOut)
        {
            RtlCopyMemory(imageOut, n->Image, 16);
            imageOut[15] = '\0';
        }
        return true;
    }
    return false;
}

} // namespace svmb
