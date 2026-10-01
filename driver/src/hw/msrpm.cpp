#include "hw/msrpm.h"
#include "platform/util.h"
#include "platform/logger.h"

namespace svmb
{

// MSRPM layout (AMD APM): 2 bits per MSR (bit0 = read intercept, bit1 = write
// intercept). Three ranges:
//   msr 0x00000000-0x00001FFF -> byte offset 0x0000
//   msr 0xC0000000-0xC0001FFF -> byte offset 0x0800
//   msr 0xC0010000-0xC0011FFF -> byte offset 0x1000
// Bit offset within the byte: bit (msr & 3) * 2 + (read ? 0 : 1)
bool Msrpm::MsrToBitOffset(u32 msr, u32& byteOff, u8& bitOff)
{
    u32 base;
    if (msr <= 0x00001FFF)
        base = 0x0000;
    else if (msr >= 0xC0000000 && msr <= 0xC0001FFF)
        base = 0x0800;
    else if (msr >= 0xC0010000 && msr <= 0xC0011FFF)
        base = 0x1000;
    else
        return false;

    u32 msrIdx = msr & 0x1FFF;      // index inside the 8192-msr range
    byteOff = base + msrIdx / 4;    // 4 msrs per byte
    bitOff = (u8)((msrIdx % 4) * 2);
    return true;
}

NTSTATUS Msrpm::Init()
{
    for (u32 i = 0; i < 3; ++i)
    {
        Va_[i] = AllocContiguous(SIZE, TAG_MSRPM);
        if (!Va_[i])
        {
            while (i)
            {
                --i;
                FreeContiguous(Va_[i], TAG_MSRPM);
                Va_[i] = nullptr;
                Pas_[i] = 0;
            }
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(Va_[i], SIZE); // all zeros = no interception
        Pas_[i] = MmGetPhysicalAddress(Va_[i]).QuadPart;
    }
    ActiveIdx_ = 0;
    ShadowIdx_ = 1;
    SpareIdx_ = 2;
    Target_ = Va_[ActiveIdx_];
    return STATUS_SUCCESS;
}

void Msrpm::Deinit()
{
    // 3-buffer rotation (Active/Shadow/Spare) supports live-rotation while
    // a VMRUN is in flight (round 11 NPT merge). Each buffer is freed
    // explicitly; the OS reclaims the physical page when no process PTE
    // still references it.
    for (u32 i = 0; i < 3; ++i)
    {
        if (Va_[i])
            FreeContiguous(Va_[i], TAG_MSRPM);
        Va_[i] = nullptr;
        Pas_[i] = 0;
    }
    ActiveIdx_ = 0;
    ShadowIdx_ = 1;
    SpareIdx_ = 2;
    Target_ = nullptr;
}

void Msrpm::BeginBuild()
{
    if (!Va_[ShadowIdx_])
        return;
    // the page being cleared retired two commits ago (retire@K -> spare@K+1
    // -> shadow@K+2) - see the staleness bound in msrpm.h
    RtlZeroMemory(Va_[ShadowIdx_], SIZE); // fresh shadow: rebuild from zero
    Target_ = Va_[ShadowIdx_];
}

u64 Msrpm::CommitBuild()
{
    if (!Va_[ShadowIdx_])
        return PhysicalAddress();
    // role rotation: the shadow becomes active, the retiring active becomes
    // the spare, and the spare (retired a full cycle ago) becomes the next
    // build target. The new active PA is published into every VMCB by the
    // caller (sampled at VMRUN).
    u32 retiring = ActiveIdx_;
    ActiveIdx_ = ShadowIdx_;
    ShadowIdx_ = SpareIdx_;
    SpareIdx_ = retiring;
    Target_ = Va_[ActiveIdx_];
    return Pas_[ActiveIdx_];
}

bool Msrpm::GetIntercept(u32 msr, bool& read, bool& write) const
{
    u32 byteOff;
    u8 bitOff;
    if (!MsrToBitOffset(msr, byteOff, bitOff) || !Va_[ActiveIdx_])
        return false;
    u8 b = ((volatile u8*)Va_[ActiveIdx_])[byteOff];
    read = (b & (1u << bitOff)) != 0;
    write = (b & (1u << (bitOff + 1))) != 0;
    return true;
}

bool Msrpm::SetIntercept(u32 msr, bool read, bool write, bool set)
{
    u32 byteOff;
    u8 bitOff;
    if (!MsrToBitOffset(msr, byteOff, bitOff) || !Target_)
        return false;

    volatile u8* p = (volatile u8*)Target_ + byteOff;
    u8 mask = 0;
    if (read)
        mask |= (u8)(1u << bitOff);
    if (write)
        mask |= (u8)(1u << (bitOff + 1));
    if (set)
        InterlockedOr8((volatile char*)p, (char)mask);
    else
        InterlockedAnd8((volatile char*)p, (char)~mask);
    return true;
}

NTSTATUS Iopm::Init()
{
    KeInitializeSpinLock(&Lock_);
    NTSTATUS st = Ports_.Init(64, TAG_IOPM);
    if (!NT_SUCCESS(st))
        return st;
    Va_ = AllocContiguous(SIZE, TAG_IOPM);
    if (!Va_)
    {
        Ports_.Deinit();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(Va_, SIZE); // all zeros = all IO allowed
    Pa_ = MmGetPhysicalAddress(Va_).QuadPart;
    return STATUS_SUCCESS;
}

void Iopm::Deinit()
{
    // ref-counted PortRefs list must be drained before freeing the IOPM
    // page (round 8 M4): the bucket array holds pointers into the side list
    // and would dangle if released first.
    PortRefs* n = List_;
    while (n)
    {
        PortRefs* next = n->NextAll;
        FreeNonPaged(n, TAG_IOPM);
        n = next;
    }
    List_ = nullptr;
    Ports_.Deinit();
    if (Va_)
        FreeContiguous(Va_, TAG_IOPM);
    Va_ = nullptr;
    Pa_ = 0;
}

Iopm::PortRefs* Iopm::FindRefs(u32 port) const
{
    return (PortRefs*)Ports_.Find(port);
}

NTSTATUS Iopm::RequirePort(u32 port, bool read, bool write)
{
    if (!read && !write)
        return STATUS_INVALID_PARAMETER;
    if (port >= 65536 || !Va_)
        return STATUS_INVALID_PARAMETER;

    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    PortRefs* refs = FindRefs(port);
    if (!refs)
    {
        refs = (PortRefs*)AllocNonPaged(sizeof(*refs), TAG_IOPM);
        if (!refs)
        {
            KeReleaseSpinLock(&Lock_, old);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(refs, sizeof(*refs));
        refs->Key = port;
        refs->NextAll = List_;
        List_ = refs;
        Ports_.Insert(refs);
    }
    u8* readMap = (u8*)Va_;
    u8* writeMap = readMap + 8192;
    if (read)
    {
        if (refs->R++ == 0)
            InterlockedOr8((volatile char*)(readMap + port / 8),
                           (char)(1u << (port % 8)));
    }
    if (write)
    {
        if (refs->W++ == 0)
            InterlockedOr8((volatile char*)(writeMap + port / 8),
                           (char)(1u << (port % 8)));
    }
    KeReleaseSpinLock(&Lock_, old);
    return STATUS_SUCCESS;
}

NTSTATUS Iopm::ReleasePort(u32 port, bool read, bool write)
{
    if (!read && !write)
        return STATUS_INVALID_PARAMETER;
    if (port >= 65536 || !Va_)
        return STATUS_INVALID_PARAMETER;

    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    PortRefs* refs = FindRefs(port);
    // validate BEFORE mutating: every requested direction must have a live
    // reference; over-releasing changes nothing (mirrors
    // InterceptManager::Release and keeps mixed-direction calls lossless)
    if (!refs || (read && refs->R == 0) || (write && refs->W == 0))
    {
        KeReleaseSpinLock(&Lock_, old);
        return STATUS_NOT_FOUND;
    }
    u8* readMap = (u8*)Va_;
    u8* writeMap = readMap + 8192;
    if (read && --refs->R == 0)
        InterlockedAnd8((volatile char*)(readMap + port / 8),
                        (char)~(1u << (port % 8)));
    if (write && --refs->W == 0)
        InterlockedAnd8((volatile char*)(writeMap + port / 8),
                        (char)~(1u << (port % 8)));
    if (refs->R == 0 && refs->W == 0)
    {
        // both directions fully released - drop the tracking node (hash +
        // side list; no exit path reads nodes, the IOPM bits are the state)
        PortRefs** pp = &List_;
        while (*pp && *pp != refs)
            pp = &(*pp)->NextAll;
        if (*pp)
            *pp = refs->NextAll;
        Ports_.Remove(port);
        FreeNonPaged(refs, TAG_IOPM);
    }
    KeReleaseSpinLock(&Lock_, old);
    return STATUS_SUCCESS;
}

bool Iopm::GetPortIntercept(u32 port, bool& read, bool& write) const
{
    if (port >= 65536 || !Va_)
        return false;
    u8* readMap = (u8*)Va_;
    u8* writeMap = readMap + 8192;
    read = (readMap[port / 8] & (1u << (port % 8))) != 0;
    write = (writeMap[port / 8] & (1u << (port % 8))) != 0;
    return true;
}

} // namespace svmb
