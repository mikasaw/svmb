// svmb - guest register image shared with the asm VMRUN loop.
// Layout is locked to hw/asm/offsets.inc (GREG_*).
#ifndef SVMB_REGS_H
#define SVMB_REGS_H

#include "platform/base.h"

namespace svmb
{

struct M128
{
    u64 Lo;
    u64 Hi;
};

struct GuestRegs
{
    // 0x000 - 0x0FF: xmm0..xmm15
    M128 Xmm0;
    M128 Xmm1;
    M128 Xmm2;
    M128 Xmm3;
    M128 Xmm4;
    M128 Xmm5;
    M128 Xmm6;
    M128 Xmm7;
    M128 Xmm8;
    M128 Xmm9;
    M128 Xmm10;
    M128 Xmm11;
    M128 Xmm12;
    M128 Xmm13;
    M128 Xmm14;
    M128 Xmm15;
    // 0x100 - 0x170: r15..rbx, then rax
    u64 R15;      // 0x100
    u64 R14;      // 0x108
    u64 R13;      // 0x110
    u64 R12;      // 0x118
    u64 R11;      // 0x120
    u64 R10;      // 0x128
    u64 R9;       // 0x130
    u64 R8;       // 0x138
    u64 Rbp;      // 0x140
    u64 Rsi;      // 0x148
    u64 Rdi;      // 0x150
    u64 Rdx;      // 0x158
    u64 Rcx;      // 0x160
    u64 Rbx;      // 0x168
    u64 Rax;      // 0x170
    u64 RFlags;   // 0x178
    u64 Rip;      // 0x180
    u64 Rsp;      // 0x188
    u64 Extra1;   // 0x190 devirt flag / phase-1 resume rip (asm contract)
    u64 Extra2;   // 0x198 devirt resume rip (asm contract)
};

static_assert(offsetof(GuestRegs, Rax) == 0x170);
static_assert(offsetof(GuestRegs, RFlags) == 0x178);
static_assert(offsetof(GuestRegs, Rip) == 0x180);
static_assert(offsetof(GuestRegs, Rsp) == 0x188);
static_assert(offsetof(GuestRegs, Extra1) == 0x190);
static_assert(offsetof(GuestRegs, Extra2) == 0x198);
static_assert(sizeof(GuestRegs) == 0x1A0);
// GuestGpr walks r8..r15 upward from &R8 - lock the descending-declaration
// layout it relies on (no padding possible, but the field order itself is
// not otherwise pinned)
static_assert(offsetof(GuestRegs, R8) == 0x138);
static_assert(offsetof(GuestRegs, R15) == 0x100);

// GPR access by the AMD register encoding used in CR/DR exit EXITINFO1[3:0]
// (0=AX 1=CX 2=DX 3=BX 4=SP 5=BP 6=SI 7=DI 8-15=r8-r15)
inline u64& GuestGpr(GuestRegs* r, u32 idx)
{
    switch (idx & 0xF)
    {
    case 0: return r->Rax;
    case 1: return r->Rcx;
    case 2: return r->Rdx;
    case 3: return r->Rbx;
    case 4: return r->Rsp;
    case 5: return r->Rbp;
    case 6: return r->Rsi;
    case 7: return r->Rdi;
    default: return *(&r->R8 - ((idx & 0xF) - 8)); // r8..r15 contiguous
    }
}

} // namespace svmb

#endif // SVMB_REGS_H
