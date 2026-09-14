// svmb - per-core virtual CPU context.
// One contiguous page-aligned allocation per logical core. The physical layout
// is locked to hw/asm/offsets.inc (VCPU_*), which the asm VMRUN loop relies on.
#ifndef SVMB_VCPU_H
#define SVMB_VCPU_H

#include "core/regs.h"
#include "hw/vmcb.h"
#include "svmb_protocol.h" // SVMB_XP_SLOTS (exit-cause histogram width)

namespace svmb
{

class Hypervisor;

enum class VcpuState : LONG
{
    Off = 0,       // never virtualized or devirtualized
    Entering = 1,  // enter sequence in progress
    Guest = 2,     // under SVM
    Leaving = 3,   // devirtualization in progress
};

// lives at VCPU_INFO (4KB space, keep small)
struct VcpuInfo
{
    volatile LONG State;          // VcpuState
    volatile LONG ExitTraceLeft;  // early-exit forensics budget (see SvmbVmExitEntry)
    volatile LONG PendingTlbKick; // r108: set cross-CPU by TlbKickFlushAll-
                                  // Cores, consumed by THIS core at its own
                                  // SvmbVmExitEntry (a VMCB must never be
                                  // written from another CPU - the r108
                                  // freeze class: lost CleanBits updates at
                                  // the VMRUN boundary)
    u32 CpuIndex;
    void* RawAlloc;               // unaligned base for ExFreePoolWithTag
    Hypervisor* Hv;
    volatile LONG64 ExitCount;
    u64 ActiveNCr3;               // 0 = NPT disabled
    u64 PublishedNcr3;            // r53: NCr3 this core last published for
                                  // the per-core process view (single writer)
    u64 ExitHist[SVMB_XP_SLOTS];  // r45: per-cause exit census; owning core
                                  // only writes it (no atomics needed), IOCTL
                                  // sums across cores. Lives in the Info page
                                  // so the pinned asm layout is untouched.
    u64 GuestVmcbPa;
    u64 HostVmcbPa;
    u64 VmmStackTop;
};

// round-27 exp3: GuestVmcb/HostVmcb/HSave moved OUT of the blob into their
// own physically-contiguous pages (svm-base reference pattern:
// MmAllocateContiguousMemory per structure). Info/Regs/VmmStack stay at the
// SAME absolute offsets the asm loop locked in (offsets.inc) - explicit pad
// pins them there (alignas alone would drift them to +0x1000).
struct VcpuContext
{
    VMCB* GuestVmcb;                     // +0x0000 (independent contiguous page)
    VMCB* HostVmcb;                      // +0x0008
    u8*   HSave;                         // +0x0010 (VM_HSAVE_PA target)
    void* GuestVmcbRaw;                  // +0x0018 (free bases; leak-on-stop)
    void* HostVmcbRaw;                   // +0x0020
    void* HSaveRaw;                      // +0x0028
    u8    Pad1[0x3000 - 0x30];
    VcpuInfo Info;                       // +0x3000
    u8    Pad2[0x1000 - sizeof(VcpuInfo)];
    GuestRegs Regs;                      // +0x4000
    u8    Pad3[0x1000 - sizeof(GuestRegs)];
    u8    VmmStack[0x6000];              // +0x5000
};

static_assert(offsetof(VcpuContext, GuestVmcb) == 0x0000);
static_assert(offsetof(VcpuContext, HostVmcb) == 0x0008);
static_assert(offsetof(VcpuContext, HSave) == 0x0010);
static_assert(offsetof(VcpuContext, Info) == 0x3000);
static_assert(offsetof(VcpuContext, Regs) == 0x4000);
static_assert(offsetof(VcpuContext, VmmStack) == 0x5000);
static_assert(sizeof(VcpuContext) == 0xB000); // 0x5000 + 0x6000 stack (unchanged blob size)
static_assert(offsetof(VcpuInfo, State) == 0);
static_assert(sizeof(VcpuInfo) <= 0x1000, "VcpuInfo must fit its page-sized slot (Pad2)");

// ---- asm entry points (hw/asm/svm_entry.asm) ----
extern "C" {
void _svmb_vmm_loop(void* vcpu, u64 guestVmcbPa, u64 hostVmcbPa, void* vmmStackTop);
void _svmb_save_or_load_regs(GuestRegs* regs);
u64   _svmb_hypercall(u64 nr, u64 a1, u64 a2, u64* out);

void _svmb_sgdt(u64* base, u16* limit);
void _svmb_sidt(u64* base, u16* limit);
void _svmb_sldt(u16* selector);
void _svmb_str(u16* selector);
u16   _svmb_cs();
u16   _svmb_ds();
u16   _svmb_es();
u16   _svmb_fs();
u16   _svmb_gs();
u16   _svmb_ss();

u64   _svmb_read_dr0();
u64   _svmb_read_dr1();
u64   _svmb_read_dr2();
u64   _svmb_read_dr3();
u64   _svmb_read_dr7();
void  _svmb_write_dr0(u64 v);
void  _svmb_write_dr1(u64 v);
void  _svmb_write_dr2(u64 v);
void  _svmb_write_dr3(u64 v);
void  _svmb_write_dr7(u64 v);

void  _svmb_invlpga(u64 gva, u32 asid);
}

} // namespace svmb

#endif // SVMB_VCPU_H
