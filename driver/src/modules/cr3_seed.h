// svmb - process CR3 seed table (M4 groundwork).
// PsSetCreateProcessNotifyRoutineEx keeps a pid -> CR3 map alive from driver
// load; consumers (cr3 exit handlers) attach in M4. Works entirely offline.
#ifndef SVMB_CR3_SEED_H
#define SVMB_CR3_SEED_H

#include "platform/util.h"

namespace svmb
{

struct Cr3Node : HashNode
{
    Cr3Node* NextAll;
    u64 Cr3;
    u32 Pid;
    char Image[16];
};

// case-insensitive exact match of a stored image basename against the
// wanted name; the stored side is a fixed 16-byte NUL-terminated field, an
// empty/desired-longer-than-15 name never matches. Pure (offline-testable).
bool Cr3ImageMatch(const char* stored, const char* want);

// creation notify: invoked at PASSIVE_LEVEL (outside the table lock) for
// every process creation once its seed node is published. The image pointer
// refers to the node's stable storage (nodes are never freed while loaded).
// r47: also carries the PEPROCESS (valid for the callback duration; the
// sentinel uses it to locate thread objects) and the creation flag (false =
// termination, the sentinel must disarm its pages then).
typedef void (*Cr3SeedNotifyFn)(u32 pid, u64 cr3, const char* image,
                                void* ctx, PEPROCESS process, bool created);

class Cr3Seed
{
public:
    NTSTATUS Init();  // registers the process notify routine
    void Deinit();    // unregisters and drains the table

    u32 Count() const { return Count_; }
    // pid lookup; returns false when unknown. CR3 is captured at process
    // creation and never changes for a process, so the read is stable.
    bool Lookup(u32 pid, u64& cr3Out) const;
    // first process whose stored image basename matches (case-insensitive)
    bool LookupByImage(const char* image, u64& cr3Out, u32& pidOut) const;

    // r89 plan C: cr3 -> pid reverse lookup (kill-policy attribution);
    // imageOut (16 bytes, optional) receives the node's image basename.
    // Miss = CR3 unknown to the seed table (pre-load / System / spoofed).
    bool LookupPidByCr3(u64 cr3, u32& pidOut, char* imageOut) const;

    // single consumer slot (serialized by the IOCTL gate); fires on every
    // later process creation
    void SetNotifyCallback(Cr3SeedNotifyFn fn, void* ctx)
    {
        NotifyFn_ = fn;
        NotifyCtx_ = ctx;
    }

private:
    static VOID ProcNotify(PEPROCESS process, HANDLE pid,
                           PPS_CREATE_NOTIFY_INFO info);

    HashTable Table_;   // key = pid
    Cr3Node* List_ = nullptr;
    Cr3Node* Graveyard_ = nullptr; // terminated nodes, freed at Deinit only:
                                   // Lookup reads lock-free, so node memory
                                   // must stay valid for the driver lifetime
    KSPIN_LOCK ListLock_;
    Cr3SeedNotifyFn NotifyFn_ = nullptr;
    void* NotifyCtx_ = nullptr;
    u32 Count_ = 0;
};

// global instance accessor (defined in main.cpp)
Cr3Seed* Cr3SeedInstance();

} // namespace svmb

#endif // SVMB_CR3_SEED_H
