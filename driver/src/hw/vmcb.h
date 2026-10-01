// svmb - full VMCB layout (AMD APM Vol.2 ch.15) with compile-time offset checks.
// Field offsets are validated by static_assert below; the asm VMRUN loop relies
// on VMCB_EXIT_CODE_OFF only.
#ifndef SVMB_VMCB_H
#define SVMB_VMCB_H

#include "hw/svm_defs.h"

namespace svmb
{

union VIntrFields
{
    u64 Data;
    struct
    {
        u8  Vpr;
        u8  Bits0;
        u8  Bits1;
        u8  Bits2;
        u8  Vector;
        u8  Rsv[3];
    } F;
};

union EventInj
{
    u64 Data;
    struct
    {
        u64 Vector : 8;
        u64 Type : 3;
        u64 Ev : 1;        // error-code valid
        u64 Reserved : 19;
        u64 Valid : 1;
        u64 ErrorCode : 32;
    } F;
};

union NpCtrl  // offset 0x90 (NP_ENABLE)
{
    u64 Data;
    struct
    {
        u64 NpEnable : 1;
        u64 SepEnable : 1;
        u64 SepEspEnable : 1;
        u64 Gmet : 1;
        u64 SssCheck : 1;
        u64 VTe : 1;
        u64 VirtTransparent : 1;
        u64 Ovrc : 1;
        u64 InvlpgbAsUd : 1;
        u64 Reserved : 55;
    } F;
};

struct VmcbCleanBits { u32 Bits; u32 Reserved; };

struct SegmentReg
{
    u16 Selector;
    u16 Attribute;
    u32 Limit;
    u64 Base;
};
static_assert(sizeof(SegmentReg) == 16);

#pragma pack(push, 1)
struct VmcbControl
{
    u16 InterceptCrRead;      // 0x00
    u16 InterceptCrWrite;     // 0x02
    u16 InterceptDrRead;      // 0x04
    u16 InterceptDrWrite;     // 0x06
    u32 InterceptException;   // 0x08
    u32 InterceptOpcode1;     // 0x0C
    u32 InterceptOpcode2;     // 0x10
    u32 InterceptOpcode3;     // 0x14
    u8  Reserved0[0x24];      // 0x18
    u16 PauseFilterThreshold; // 0x3C
    u16 PauseFilterCount;     // 0x3E
    u64 IopmBasePa;           // 0x40
    u64 MsrpmBasePa;          // 0x48
    u64 TscOffset;            // 0x50
    u32 GuestAsid;            // 0x58
    u8  TlbControl;           // 0x5C
    u8  Reserved1[3];         // 0x5D
    VIntrFields VIntr;        // 0x60
    u64 GuestIntStatus;       // 0x68 (interrupt shadow + guest int status)
    u64 ExitCode;             // 0x70
    u64 ExitInfo1;            // 0x78
    u64 ExitInfo2;            // 0x80
    u64 ExitIntInfo;          // 0x88
    NpCtrl Np;                // 0x90
    u64 AvicApicBar;          // 0x98
    u64 GhcbPa;               // 0xA0
    EventInj EventInj;        // 0xA8
    u64 NCr3;                 // 0xB0
    u64 Reserved2;            // 0xB8 (LBR virt / vmsave-vmload virt)
    VmcbCleanBits CleanBits;  // 0xC0
    u64 NRip;                 // 0xC8
    u64 Reserved3[2];         // 0xD0, 0xD8
    u64 AvicBackingPage;      // 0xE0
    u64 Reserved4;            // 0xE8
    u64 AvicLogicalTable;     // 0xF0
    u64 AvicPhysicalTable;    // 0xF8
    u64 Reserved5;            // 0x100
    u64 Vmsa;                 // 0x108
    u64 VmgExitRax;           // 0x110
    u8  VmgExitCpl;           // 0x118
    u8  ReservedPad;          // 0x119 (BusThresholdCounter is 2-byte aligned per APM)
    u16 BusThresholdCounter;  // 0x11A
    u8  Reserved6[0x2C4];     // 0x11C .. 0x3DF
    u8  HostDefined[0x20];    // 0x3E0 .. 0x3FF
};
#pragma pack(pop)

struct VmcbSave
{
    SegmentReg Es;            // 0x400
    SegmentReg Cs;            // 0x410
    SegmentReg Ss;            // 0x420
    SegmentReg Ds;            // 0x430
    SegmentReg Fs;            // 0x440
    SegmentReg Gs;            // 0x450
    SegmentReg Gdtr;          // 0x460
    SegmentReg Ldtr;          // 0x470
    SegmentReg Idtr;          // 0x480
    SegmentReg Tr;            // 0x490
    u8  Reserved0[0x2A];      // 0x4A0
    u8  Cpl;                  // 0x4CA
    u8  Reserved0b[5];        // 0x4CB
    u64 Efer;                 // 0x4D0
    u8  Reserved2[0x70];      // 0x4D8
    u64 Cr4;                  // 0x548
    u64 Cr3;                  // 0x550
    u64 Cr0;                  // 0x558
    u64 Dr7;                  // 0x560
    u64 Dr6;                  // 0x568
    u64 Rflags;               // 0x570
    u64 Rip;                  // 0x578
    u8  Reserved3[0x58];      // 0x580
    u64 Rsp;                  // 0x5D8
    u64 SCet;                 // 0x5E0
    u64 Ssp;                  // 0x5E8
    u64 IsstAddr;             // 0x5F0
    u64 Rax;                  // 0x5F8
    u64 Star;                 // 0x600
    u64 Lstar;                // 0x608
    u64 Cstar;                // 0x610
    u64 Sfmask;               // 0x618
    u64 KernelGsBase;         // 0x620
    u64 SysenterCs;           // 0x628
    u64 SysenterEsp;          // 0x630
    u64 SysenterEip;          // 0x638
    u64 Cr2;                  // 0x640
    u8  Reserved4[0x20];      // 0x648
    u64 GPat;                 // 0x668
    u64 DbgCtl;               // 0x670
    u64 BrFrom;               // 0x678
    u64 BrTo;                 // 0x680
    u64 LastExcFrom;          // 0x688
    u64 LastExcTo;            // 0x690
    u64 DbgExtnCfg;           // 0x698
    u8  Reserved5[0x48];      // 0x6A0
    u64 SpecCtrl;             // 0x6E8
    u8  Reserved6[0x388];     // 0x6F0 .. 0xA78
    u8  PadTo4k[0x588];       // 0xA78 .. 0xFFF
};

struct VMCB
{
    VmcbControl Ctrl;
    VmcbSave    Save;
};
static_assert(sizeof(VMCB) == 0x1000, "VMCB must be exactly 4KB");
static_assert(offsetof(VMCB, Ctrl.ExitCode) == 0x70);
static_assert(offsetof(VMCB, Ctrl.ExitInfo1) == 0x78);
static_assert(offsetof(VMCB, Ctrl.ExitInfo2) == 0x80);
static_assert(offsetof(VMCB, Ctrl.ExitIntInfo) == 0x88);
static_assert(offsetof(VMCB, Ctrl.NCr3) == 0xB0);
static_assert(offsetof(VMCB, Ctrl.NRip) == 0xC8);
static_assert(offsetof(VMCB, Ctrl.EventInj) == 0xA8);
static_assert(offsetof(VMCB, Ctrl.MsrpmBasePa) == 0x48);
static_assert(offsetof(VMCB, Ctrl.IopmBasePa) == 0x40);
static_assert(offsetof(VMCB, Ctrl.HostDefined) == 0x3E0);
static_assert(offsetof(VMCB, Save.Efer) == 0x4D0);
static_assert(offsetof(VMCB, Save.Cr3) == 0x550);
static_assert(offsetof(VMCB, Save.Cr0) == 0x558);
static_assert(offsetof(VMCB, Save.Cr4) == 0x548);
static_assert(offsetof(VMCB, Save.Rip) == 0x578);
static_assert(offsetof(VMCB, Save.Rsp) == 0x5D8);
static_assert(offsetof(VMCB, Save.Rax) == 0x5F8);
static_assert(offsetof(VMCB, Save.Rflags) == 0x570);

// ---- intercept opcode bit positions ----
// interceptOpcode1 (0x0C)
namespace ic1
{
constexpr u32 INTR = 1u << 0;
constexpr u32 NMI = 1u << 1;
constexpr u32 SMI = 1u << 2;
constexpr u32 INIT = 1u << 3;
constexpr u32 VINTR = 1u << 4;
constexpr u32 CR0_WRITE_15 = 1u << 5;  // writes to CR0 bits other than CR0.TS/MP
constexpr u32 IDTR_READ = 1u << 6;
constexpr u32 GDTR_READ = 1u << 7;
constexpr u32 LDTR_READ = 1u << 8;
constexpr u32 TR_READ = 1u << 9;
constexpr u32 IDTR_WRITE = 1u << 10;
constexpr u32 GDTR_WRITE = 1u << 11;
constexpr u32 LDTR_WRITE = 1u << 12;
constexpr u32 TR_WRITE = 1u << 13;
constexpr u32 RDTSC = 1u << 14;
constexpr u32 RDPMC = 1u << 15;
constexpr u32 PUSHF = 1u << 16;
constexpr u32 POPF = 1u << 17;
constexpr u32 CPUID = 1u << 18;
constexpr u32 RSM = 1u << 19;
constexpr u32 IRET = 1u << 20;
constexpr u32 INTN = 1u << 21;
constexpr u32 INVD = 1u << 22;
constexpr u32 PAUSE = 1u << 23;
constexpr u32 HLT = 1u << 24;
constexpr u32 INVLPG = 1u << 25;
constexpr u32 INVLPGA = 1u << 26;
constexpr u32 IOIO = 1u << 27;
constexpr u32 MSR = 1u << 28;      // RDMSR + WRMSR
constexpr u32 TASK_SWITCH = 1u << 29;
constexpr u32 FERR_FREEZE = 1u << 30;
constexpr u32 SHUTDOWN = 1u << 31;
} // namespace ic1

// interceptOpcode2 (0x10)
namespace ic2
{
constexpr u32 VMRUN = 1u << 0;   // must always be set for anti-nesting
constexpr u32 VMMCALL = 1u << 1;
constexpr u32 VMLOAD = 1u << 2;
constexpr u32 VMSAVE = 1u << 3;
constexpr u32 STGI = 1u << 4;
constexpr u32 CLGI = 1u << 5;
constexpr u32 SKINIT = 1u << 6;
constexpr u32 RDTSCP = 1u << 7;
constexpr u32 ICEBP = 1u << 8;
constexpr u32 WBINVD = 1u << 9;
constexpr u32 MONITOR = 1u << 10;
constexpr u32 MWAIT = 1u << 11;
constexpr u32 MWAIT_ARMED = 1u << 12;
constexpr u32 XSETBV = 1u << 13;
constexpr u32 RDPRU = 1u << 14;
constexpr u32 EFER_WRITE = 1u << 15;
constexpr u32 CR_WRITE(u32 n) { return 1u << (16 + n); } // CR0..CR15 write traps
constexpr u32 ALL_CR_WRITES = 0xFFFF0000u;
} // namespace ic2

// ---- VM exit reasons ----
namespace vmexit
{
constexpr u64 CR_READ(u32 n)   { return 0x00 + n; }
constexpr u64 CR_WRITE(u32 n)  { return 0x10 + n; }
constexpr u64 DR_READ(u32 n)   { return 0x20 + n; }
constexpr u64 DR_WRITE(u32 n)  { return 0x30 + n; }
constexpr u64 EXCEPTION(u32 v) { return 0x40 + v; }
constexpr u64 INTR = 0x60;
constexpr u64 NMI = 0x61;
constexpr u64 SMI = 0x62;
constexpr u64 INIT = 0x63;
constexpr u64 VINTR = 0x64;
constexpr u64 CR0_SEL_WRITE = 0x65;
constexpr u64 IDTR_READ = 0x66;
constexpr u64 GDTR_READ = 0x67;
constexpr u64 LDTR_READ = 0x68;
constexpr u64 TR_READ = 0x69;
constexpr u64 IDTR_WRITE = 0x6A;
constexpr u64 GDTR_WRITE = 0x6B;
constexpr u64 LDTR_WRITE = 0x6C;
constexpr u64 TR_WRITE = 0x6D;
constexpr u64 RDTSC = 0x6E;
constexpr u64 RDPMC = 0x6F;
constexpr u64 PUSHF = 0x70;
constexpr u64 POPF = 0x71;
constexpr u64 CPUID = 0x72;
constexpr u64 RSM = 0x73;
constexpr u64 IRET = 0x74;
constexpr u64 INTN = 0x75;
constexpr u64 INVD = 0x76;
constexpr u64 PAUSE = 0x77;
constexpr u64 HLT = 0x78;
constexpr u64 INVLPG = 0x79;
constexpr u64 INVLPGA = 0x7A;
constexpr u64 IOIO = 0x7B;
constexpr u64 MSR = 0x7C;
constexpr u64 TASK_SWITCH = 0x7D;
constexpr u64 FERR_FREEZE = 0x7E;
constexpr u64 SHUTDOWN = 0x7F;
constexpr u64 VMRUN = 0x80;
constexpr u64 VMMCALL = 0x81;
constexpr u64 VMLOAD = 0x82;
constexpr u64 VMSAVE = 0x83;
constexpr u64 STGI = 0x84;
constexpr u64 CLGI = 0x85;
constexpr u64 SKINIT = 0x86;
constexpr u64 RDTSCP = 0x87;
constexpr u64 ICEBP = 0x88;
constexpr u64 WBINVD = 0x89;
constexpr u64 MONITOR = 0x8A;
constexpr u64 MWAIT = 0x8B;
constexpr u64 MWAIT_COND = 0x8C;
constexpr u64 XSETBV = 0x8D;
constexpr u64 EFER_WRITE_TRAP = 0x8F;
constexpr u64 CR_WRITE_TRAP(u32 n) { return 0x90 + n; }
constexpr u64 NPF = 0x400;
constexpr u64 AVIC_IPI = 0x401;
constexpr u64 AVIC_NOACCEL = 0x402;
constexpr u64 VMGEXIT = 0x403;
constexpr u64 INVALID = 0xFFFFFFFFFFFFFFFFull;
constexpr u64 BUSY = 0xFFFFFFFFFFFFFFFEull;
constexpr u64 ILLEGAL = 0xFFFFFFFFFFFFFFFDull;
} // namespace vmexit

} // namespace svmb

#endif // SVMB_VMCB_H
