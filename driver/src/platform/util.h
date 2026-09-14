// svmb - kernel utilities: allocation wrappers, per-core dispatch, containers
#ifndef SVMB_UTIL_H
#define SVMB_UTIL_H

#include "platform/base.h"

namespace svmb
{

// ---- allocation wrappers (never bugcheck on failure, return nullptr) ----
void* AllocNonPaged(SIZE_T bytes, u32 tag);
void  FreeNonPaged(void* p, u32 tag, SIZE_T bytes = 0);
// physically contiguous (MSRPM / IOPM hardware requirement)
void* AllocContiguous(SIZE_T bytes, u32 tag);
void  FreeContiguous(void* p, u32 tag);
// page-aligned non-paged + executable
void* AllocExecPage(u32 tag);
void  FreeExecPage(void* p, u32 tag);

// run fn(cpuIndex) on every core via thread group affinity (serial, PASSIVE)
template <typename Fn>
NTSTATUS RunOnEachCore(Fn&& fn)
{
    NTSTATUS status = STATUS_SUCCESS;
    ULONG count = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    for (ULONG i = 0; i < count; ++i)
    {
        PROCESSOR_NUMBER num = {};
        status = KeGetProcessorNumberFromIndex(i, &num);
        if (!NT_SUCCESS(status))
            break;
        GROUP_AFFINITY aff = {}, old = {};
        aff.Group = num.Group;
        aff.Mask = 1ull << num.Number;
        KeSetSystemGroupAffinityThread(&aff, &old);
        status = fn(i);
        KeRevertToUserGroupAffinityThread(&old);
        if (!NT_SUCCESS(status))
            break;
    }
    return status;
}

// index of the current processor (valid at any IRQL)
u32 CurrentCpuIndex();

// physical address width from CPUID.80000008.EAX[7:0]
u32 QueryPhysAddrWidth();

// ---- minimal intrusive hash map for NPT page records (pa -> record) ----
struct HashNode
{
    HashNode* Next;
    u64 Key;
};

// fixed-bucket hash table, external chaining, spinlock protected.
// nodes carry their key; value lives in the derived node type.
class HashTable
{
public:
    struct Bucket { HashNode* Head; };

    NTSTATUS Init(u32 bucketCount, u32 tag);
    void     Deinit();

    // returns node with matching key or nullptr
    HashNode* Find(u64 key) const;  // read-only lookup (lock const-cast)
    void      Insert(HashNode* node);
    void      Remove(u64 key);          // unlink, node not freed
    template <typename T> static T* Cast(HashNode* n) { return static_cast<T*>(n); }

private:
    u32 BucketIdx(u64 key) const { return (u32)((key >> 12) % BucketCount_); }
    Bucket* Buckets_ = nullptr;
    u32 BucketCount_ = 0;
    KSPIN_LOCK Lock_ = 0;
    u32 Tag_ = 0;
};

} // namespace svmb

#endif // SVMB_UTIL_H
