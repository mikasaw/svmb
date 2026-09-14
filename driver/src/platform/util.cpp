#include "platform/util.h"

// Allocation policy: legacy ExAllocatePoolWithTag only (no ExAllocatePool2),
// so the driver loads on every Windows 10 build (1507+) and Server 2016+.
// NonPagedPoolNx keeps routine allocations non-executable (Win8+).
// The deprecation warning is expected: down-level compatibility is deliberate.
#pragma warning(disable : 4996)

namespace svmb
{

void* AllocNonPaged(SIZE_T bytes, u32 tag)
{
    return ExAllocatePoolWithTag(NonPagedPoolNx, bytes, tag);
}

void FreeNonPaged(void* p, u32 tag, SIZE_T)
{
    UNREFERENCED_PARAMETER(tag);
    if (p)
        ExFreePoolWithTag(p, tag);
}

void* AllocContiguous(SIZE_T bytes, u32 tag)
{
    UNREFERENCED_PARAMETER(tag);
    PHYSICAL_ADDRESS highest = {};
    highest.QuadPart = MAXULONG64;
    PVOID p = MmAllocateContiguousMemory(bytes, highest);
    if (p)
        RtlZeroMemory(p, bytes);
    return p;
}

void FreeContiguous(void* p, u32 tag)
{
    UNREFERENCED_PARAMETER(tag);
    if (p)
        MmFreeContiguousMemory(p);
}

void* AllocExecPage(u32 tag)
{
    // executable non-paged pool (swap pages / trampolines need to be RX)
    PVOID p = ExAllocatePoolWithTag(NonPagedPoolExecute, PAGE_SIZE, tag);
    if (p)
        RtlZeroMemory(p, PAGE_SIZE);
    return p;
}

void FreeExecPage(void* p, u32 tag)
{
    if (p)
        ExFreePoolWithTag(p, tag);
}

u32 CurrentCpuIndex()
{
    PROCESSOR_NUMBER num = {};
    ULONG idx = KeGetCurrentProcessorNumberEx(&num);
    if (idx == INVALID_PROCESSOR_INDEX)
        return 0;
    return KeGetProcessorIndexFromNumber(&num);
}

u32 QueryPhysAddrWidth()
{
    int regs[4] = {};
    __cpuid(regs, 0x80000008);
    return (u32)(regs[0] & 0xFF);
}

NTSTATUS HashTable::Init(u32 bucketCount, u32 tag)
{
    Tag_ = tag;
    KeInitializeSpinLock(&Lock_);
    BucketCount_ = bucketCount;
    Buckets_ = (Bucket*)AllocNonPaged((SIZE_T)bucketCount * sizeof(Bucket), tag);
    if (!Buckets_)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(Buckets_, (SIZE_T)bucketCount * sizeof(Bucket));
    return STATUS_SUCCESS;
}

void HashTable::Deinit()
{
    if (Buckets_)
    {
        // frees the bucket ARRAY only - nodes are never dereferenced here,
        // so owners that keep their nodes alive elsewhere (e.g. a graveyard)
        // may Deinit with nodes still linked
        ExFreePoolWithTag(Buckets_, Tag_);
        Buckets_ = nullptr;
    }
    BucketCount_ = 0;
}

HashNode* HashTable::Find(u64 key) const
{
    KIRQL oldIrql;
    KeAcquireSpinLock(const_cast<KSPIN_LOCK*>(&Lock_), &oldIrql);
    HashNode* n = Buckets_[BucketIdx(key)].Head;
    while (n && n->Key != key)
        n = n->Next;
    KeReleaseSpinLock(const_cast<KSPIN_LOCK*>(&Lock_), oldIrql);
    return n;
}

void HashTable::Insert(HashNode* node)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&Lock_, &oldIrql);
    Bucket& b = Buckets_[BucketIdx(node->Key)];
    node->Next = b.Head;
    b.Head = node;
    KeReleaseSpinLock(&Lock_, oldIrql);
}

void HashTable::Remove(u64 key)
{
    KIRQL oldIrql;
    KeAcquireSpinLock(&Lock_, &oldIrql);
    Bucket& b = Buckets_[BucketIdx(key)];
    HashNode** pp = &b.Head;
    while (*pp)
    {
        if ((*pp)->Key == key)
        {
            *pp = (*pp)->Next;
            break;
        }
        pp = &(*pp)->Next;
    }
    KeReleaseSpinLock(&Lock_, oldIrql);
}

} // namespace svmb
