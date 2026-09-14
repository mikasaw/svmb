#include "mm/phys_mem.h"
#include "platform/util.h"
// legacy pool API for down-level compatibility (see util.cpp)
#pragma warning(disable : 4996)

namespace svmb
{

NTSTATUS PhysRanges::Init()
{
    if (Ranges_)
        return STATUS_ALREADY_REGISTERED;

    PPHYSICAL_MEMORY_RANGE fw = MmGetPhysicalMemoryRanges();
    if (!fw)
        return STATUS_INSUFFICIENT_RESOURCES;

    u32 n = 0;
    while (fw[n].BaseAddress.QuadPart != 0 || fw[n].NumberOfBytes.QuadPart != 0)
        ++n;
    if (n == 0)
    {
        ExFreePool(fw);
        return STATUS_UNSUCCESSFUL;
    }

    Ranges_ = (MemRange*)AllocNonPaged((SIZE_T)n * sizeof(MemRange), TAG_NPT);
    if (!Ranges_)
    {
        ExFreePool(fw);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (u32 i = 0; i < n; ++i)
    {
        Ranges_[i].Base = (u64)fw[i].BaseAddress.QuadPart;
        Ranges_[i].Len = (u64)fw[i].NumberOfBytes.QuadPart;
        u64 rangeEnd = (u64)fw[i].BaseAddress.QuadPart + (u64)fw[i].NumberOfBytes.QuadPart;
        if (rangeEnd > RamEnd_)
            RamEnd_ = rangeEnd;
    }
    Count_ = n;

    ExFreePool(fw);
    return STATUS_SUCCESS;
}

void PhysRanges::Deinit()
{
    if (Ranges_)
    {
        FreeNonPaged(Ranges_, TAG_NPT);
        Ranges_ = nullptr;
    }
    Count_ = 0;
    RamEnd_ = 0;
}

bool PhysRanges::IsRam(u64 pa) const
{
    for (u32 i = 0; i < Count_; ++i)
        if (pa >= Ranges_[i].Base && pa - Ranges_[i].Base < Ranges_[i].Len)
            return true;
    return false;
}

bool PhysRanges::Range(u32 i, u64& baseOut, u64& lenOut) const
{
    if (i >= Count_)
        return false;
    baseOut = Ranges_[i].Base;
    lenOut = Ranges_[i].Len;
    return true;
}

void* AllocNptPage(u64& paOut)
{
    void* va = ExAllocatePoolWithTag(NonPagedPoolNx, PAGE_SIZE, TAG_NPT);
    if (!va)
        return nullptr;
    RtlZeroMemory(va, PAGE_SIZE);
    paOut = MmGetPhysicalAddress(va).QuadPart;
    return va;
}

void FreeNptPage(void* va)
{
    if (va)
        ExFreePoolWithTag(va, TAG_NPT);
}

} // namespace svmb
