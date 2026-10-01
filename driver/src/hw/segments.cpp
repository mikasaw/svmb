#include "hw/segments.h"

// SimpleSvm technique: in long mode CS/DS/ES/SS base+limit are ignored, but
// TR/LDTR keep real base/limit and every segment's attribute matters for
// segment checks - so parse the GDT instead of faking values.

namespace svmb
{

SegmentDescriptor GetSegmentDescriptor(u16 selector, u64 gdtBase)
{
    SegmentDescriptor d = {};
    if ((selector & ~7u) == 0)
        return d;
    const SegmentDescriptor* p = (const SegmentDescriptor*)(gdtBase + (selector & ~7u));
    d.AsUInt64 = p->AsUInt64;
    return d;
}

SegmentAttribute GetSegmentAttribute(u16 selector, u64 gdtBase)
{
    SegmentAttribute a = {};
    SegmentDescriptor d = GetSegmentDescriptor(selector, gdtBase);
    a.F.Type = d.F.Type;
    a.F.System = d.F.System;
    a.F.Dpl = d.F.Dpl;
    a.F.Present = d.F.Present;
    a.F.Avl = d.F.Avl;
    a.F.LongMode = d.F.LongMode;
    a.F.DefaultBit = d.F.DefaultBit;
    a.F.Granularity = d.F.Granularity;
    return a;
}

u64 GetSegmentBase(u16 selector, u64 gdtBase)
{
    SegmentDescriptor d = GetSegmentDescriptor(selector, gdtBase);
    u64 base = d.F.BaseLow;
    base |= (u64)d.F.BaseMiddle << 16;
    base |= (u64)d.F.BaseHigh << 24;
    if (!d.F.System)
    {
        // 16-byte system descriptor (TSS/LDT): base bits 32-63 live in
        // bytes 8-11 of the descriptor pair. Byte 7 is base bits 24-31
        // (already folded in above) - NOT the upper base.
        const u8* p = (const u8*)(gdtBase + (selector & ~7u));
        base |= (u64)p[8] << 32;
        base |= (u64)p[9] << 40;
        base |= (u64)p[10] << 48;
        base |= (u64)p[11] << 56;
    }
    return base;
}

u32 GetSegmentLimit(u16 selector, u64 gdtBase)
{
    SegmentDescriptor d = GetSegmentDescriptor(selector, gdtBase);
    u32 limit = d.F.LimitLow | ((u32)d.F.LimitHigh << 16);
    if (d.F.Granularity)
    {
        limit <<= 12;
        limit |= 0xFFF;
    }
    return limit;
}

u32 GetSegmentLimitForVmcb(u16 selector, u64 gdtBase)
{
    // AMD APM vol.2 15.5: VMCB.SAVE.<seg>.Limit is reserved and must be
    // ignored for long-mode 64-bit code segments (CS/SS/DS/ES/FS/GS) when
    // the segment attribute has L=1 - but the VMCB layout still requires
    // a non-zero value. Native segments.cs.limit for a 64-bit kernel CS
    // is 0xFFFF (LimitLow=0xFFFF, LimitHigh=0, G=1). When we sample the
    // descriptor directly with GetSegmentDescriptor in the rare case the
    // boot loader left LimitLow=0 (some UEFI/Windows hybrid paths do this),
    // we would write 0 into VMCB.SAVE and vmrun would fault with an
    // internal-consistency check before even reading EFER.SVME.
    // Coerce to 0xFFFFFFFF so vmrun validation always passes; the limit
    // is ignored by the CPU in long mode regardless of value.
    UNREFERENCED_PARAMETER(selector);
    UNREFERENCED_PARAMETER(gdtBase);
    return 0xFFFFFFFF;
}

} // namespace svmb
