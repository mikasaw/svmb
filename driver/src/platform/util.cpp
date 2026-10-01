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
    if (!bucketCount)
        return STATUS_INVALID_PARAMETER;
    Tag_ = tag;
    KeInitializeSpinLock(&Lock_);
    Bucket* buckets = (Bucket*)AllocNonPaged(
        (SIZE_T)bucketCount * sizeof(Bucket), tag);
    if (!buckets)
        return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(buckets, (SIZE_T)bucketCount * sizeof(Bucket));
    Buckets_ = buckets;
    BucketCount_ = bucketCount;
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
    if (!Buckets_ || !BucketCount_)
        return nullptr;
    KIRQL oldIrql;
    KeAcquireSpinLock(const_cast<KSPIN_LOCK*>(&Lock_), &oldIrql);
    HashNode* n = Buckets_[BucketIdx(key)].Head;
    while (n && n->Key != key)
        n = n->Next;
    KeReleaseSpinLock(const_cast<KSPIN_LOCK*>(&Lock_), oldIrql);
    return n;
}

// lock-free variant - see util.h. The volatile bucket read plus the plain
// chain walk rely on x64 TSO: the inserting CPU writes node->Key/node
// payload before the (locked) head publication, so a reader that observes
// the new head also observes the fields; a reader that raced just ahead
// simply misses the node this round.
HashNode* HashTable::FindAppendOnly(u64 key) const
{
    if (!Buckets_ || !BucketCount_)
        return nullptr;
    HashNode* n =
        ((volatile Bucket*)Buckets_)[BucketIdx(key)].Head;
    while (n && n->Key != key)
        n = n->Next;
    return n;
}

void HashTable::Insert(HashNode* node)
{
    if (!node || !Buckets_ || !BucketCount_)
        return;
    KIRQL oldIrql;
    KeAcquireSpinLock(&Lock_, &oldIrql);
    Bucket& b = Buckets_[BucketIdx(node->Key)];
    node->Next = b.Head;
    b.Head = node;
    KeReleaseSpinLock(&Lock_, oldIrql);
}

void HashTable::Remove(u64 key)
{
    if (!Buckets_ || !BucketCount_)
        return;
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
