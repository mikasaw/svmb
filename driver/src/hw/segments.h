// svmb - GDT segment descriptor parsing (SimpleSvm technique) for filling
// the guest VMCB save area before the first VMRUN.
#ifndef SVMB_SEGMENTS_H
#define SVMB_SEGMENTS_H

#include "platform/base.h"

namespace svmb
{

union SegmentDescriptor
{
    u64 AsUInt64;
    struct
    {
        u16 LimitLow;
        u16 BaseLow;
        u8 BaseMiddle;
        u8 Type : 4;
        u8 System : 1;
        u8 Dpl : 2;
        u8 Present : 1;
        u8 LimitHigh : 4;
        u8 Avl : 1;
        u8 LongMode : 1;
        u8 DefaultBit : 1;
        u8 Granularity : 1;
        u8 BaseHigh;
    } F;
};

union SegmentAttribute
{
    u16 AsUInt16;
    struct
    {
        u16 Type : 4;
        u16 System : 1;
        u16 Dpl : 2;
        u16 Present : 1;
        u16 Avl : 1;
        u16 LongMode : 1;
        u16 DefaultBit : 1;
        u16 Granularity : 1;
        u16 Reserved : 4;
    } F;
};

SegmentDescriptor GetSegmentDescriptor(u16 selector, u64 gdtBase);
SegmentAttribute  GetSegmentAttribute(u16 selector, u64 gdtBase);
u64               GetSegmentBase(u16 selector, u64 gdtBase);
u32               GetSegmentLimit(u16 selector, u64 gdtBase);
// Same as GetSegmentLimit but forces a non-zero value safe for VMCB.SAVE.<seg>
// fields in long mode (see segments.cpp for the AMD spec citation).
u32               GetSegmentLimitForVmcb(u16 selector, u64 gdtBase);

} // namespace svmb

#endif // SVMB_SEGMENTS_H
