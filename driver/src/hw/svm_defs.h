// svmb - AMD SVM constants: MSRs, CPUID leaves, intercept bit positions, hypercalls
#ifndef SVMB_SVM_DEFS_H
#define SVMB_SVM_DEFS_H

#include "platform/base.h"

namespace svmb
{

// ---- MSRs ----
constexpr u32 MSR_EFER            = 0xC0000080;
constexpr u32 MSR_STAR            = 0xC0000081;
constexpr u32 MSR_LSTAR           = 0xC0000082;
constexpr u32 MSR_CSTAR           = 0xC0000083;
constexpr u32 MSR_SFMASK          = 0xC0000084;
constexpr u32 MSR_FS_BASE         = 0xC0000100;
constexpr u32 MSR_GS_BASE         = 0xC0000101;
constexpr u32 MSR_KERNEL_GS_BASE  = 0xC0000102;
constexpr u32 MSR_VM_CR           = 0xC0010114;
constexpr u32 MSR_VM_HSAVE_PA     = 0xC0010117;

// ---- EFER bits ----
constexpr u32 EFER_SVME  = 12;
constexpr u32 EFER_SCE   = 0;
constexpr u32 EFER_NXE   = 11;

// ---- VM_CR bits ----
constexpr u32 VM_CR_SVMDIS      = 4;
constexpr u32 VM_CR_SVMDIS_LOCK = 3;  // firmware lockdown (VMware L1 sets this)

// ---- CPUID leaves ----
constexpr u32 CPUID_EXT_FEATURES   = 0x80000001; // ECX[2] = SVM
constexpr u32 CPUID_SVM_FEATURES   = 0x8000000A; // EDX[0]=NPT, EDX[3]=NRIPS
constexpr u32 CPUID_ADDR_WIDTH     = 0x80000008;
constexpr u32 SVM_CPUID_BIT        = 2;  // in 0x80000001.ECX
constexpr u32 NPT_CPUID_BIT        = 0;  // in 0x8000000A.EDX
constexpr u32 NRIPS_CPUID_BIT      = 3;  // in 0x8000000A.EDX
constexpr u32 VMCB_CLB_CPUID_BIT   = 29; // in 0x8000000A.EDX

// ---- exception vectors ----
constexpr u32 EXC_DE = 0;
constexpr u32 EXC_DB = 1;
constexpr u32 EXC_BP = 3;
constexpr u32 EXC_UD = 6;
constexpr u32 EXC_GP = 13;
constexpr u32 EXC_PF = 14;

// ---- EFLAGS bits ----
constexpr u32 EFLAGS_TF = 8;
constexpr u32 EFLAGS_RF = 16;

// ---- event injection types (VMCB eventInj.type) ----
constexpr u32 EVT_INTR = 0;
constexpr u32 EVT_NMI = 1;
constexpr u32 EVT_EXCEPTION = 2;
constexpr u32 EVT_SOFTINT = 3;

// ---- hypercall channels ----
// primary: VMMCALL instruction (rax = nr, args rbx/rcx/rdx/rsi/rdi, result rax)
// fallback: CPUID leaf 0x400000FF (nr = rcx, args rbx/rdx, results rax/rbx/rdx)
constexpr u32 HYPERCALL_CPUID_LEAF = 0x400000FF;

// hypercall numbers (nr)
constexpr u32 HC_PROBE        = 0x0000; // rax <- 'SVMB' magic
constexpr u32 HC_VERSION      = 0x0001; // rax <- version
constexpr u32 HC_EXIT_VMM     = 0x0002; // devirtualize this core (kernel-mode only)
constexpr u32 HC_UPDATE_BARRIER = 0x0003; // internal: apply pending vmcb updates
constexpr u32 HC_MODULE_BASE  = 0x0100; // modules get (HC_MODULE_BASE | (token << 8) | idx)

constexpr u32 HYPERCALL_MAGIC = MAKE_TAG_('S', 'V', 'M', 'B');

// ---- exit info decode helpers ----
// CR/DR access exits: EXITINFO1[3:0] = general purpose register number
inline u32 ExitCrDrGpr(u64 exitInfo1) { return (u32)(exitInfo1 & 0xF); }

// NPF EXITINFO1 bits
constexpr u64 NPF_PRESENT  = 1ull << 0;
constexpr u64 NPF_WRITE    = 1ull << 1;
constexpr u64 NPF_EXECUTE  = 1ull << 4;
constexpr u64 NPF_USER     = 1ull << 2;

} // namespace svmb

#endif // SVMB_SVM_DEFS_H
