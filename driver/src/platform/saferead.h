// svmb - r102: shared __try-guarded kernel-VA readers (the r101 helpers
// KdumpSafeRead64 / KSafeRead32 consolidated). One prefilter (canonical
// kernel-high + alignment) + per-element exception guard. These happily
// read device MMIO - callers needing RAM-vs-MMIO discrimination must
// check the physical range themselves (PhysRanges::IsRam).
#ifndef SVMB_SAFEREAD_H
#define SVMB_SAFEREAD_H

#include "platform/base.h"

namespace svmb
{

inline bool SafRead32(u64 va, u32* out)
{
    if (va < 0xFFFF800000000000ull || (va & 3) || !out)
        return false;
    __try
    {
        *out = *(const volatile u32*)va;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

inline bool SafRead64(u64 va, u64* out)
{
    if (va < 0xFFFF800000000000ull || (va & 7) || !out)
        return false;
    __try
    {
        *out = *(const volatile u64*)va;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

} // namespace svmb

#endif // SVMB_SAFEREAD_H
