// svmb - physical memory map: firmware RAM ranges for NPT construction.
// Offline component: usable with the driver loaded and the hypervisor off.
#ifndef SVMB_PHYS_MEM_H
#define SVMB_PHYS_MEM_H

#include "platform/base.h"

namespace svmb
{

// firmware-reported physical RAM ranges (MMIO holes excluded)
class PhysRanges
{
public:
    PhysRanges() = default;
    ~PhysRanges() { Deinit(); }
    PhysRanges(const PhysRanges&) = delete;
    PhysRanges& operator=(const PhysRanges&) = delete;

    NTSTATUS Init();  // PASSIVE_LEVEL
    void Deinit();

    u32 Count() const { return Count_; }
    // highest RAM byte + 1 (0 = no ranges)
    u64 RamEnd() const { return RamEnd_; }
    // true when pa is covered by a RAM range
    bool IsRam(u64 pa) const;
    // i-th range (bounds-checked); false when i is out of range
    bool Range(u32 i, u64& baseOut, u64& lenOut) const;

private:
    struct MemRange
    {
        u64 Base;
        u64 Len;
    };
    MemRange* Ranges_ = nullptr;
    u32 Count_ = 0;
    u64 RamEnd_ = 0;
};

// zeroed 4K page for page-table use (pool allocs >= PAGE_SIZE are page
// aligned, which the hardware requires for the recorded PFN to be valid)
void* AllocNptPage(u64& paOut);
void  FreeNptPage(void* va);

} // namespace svmb

#endif // SVMB_PHYS_MEM_H
