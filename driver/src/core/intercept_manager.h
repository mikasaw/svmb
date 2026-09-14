// svmb - refcounted interception requirement manager.
// Modules declare what they need (CR/DR access, exceptions, opcodes, MSRs);
// this manager folds requirements into every core's VMCB control fields and
// the MSRPM bitmap. Remote VMCB writes are safe: the CPU samples control
// fields only at VMRUN, and all touched fields are natively-sized stores.
#ifndef SVMB_INTERCEPT_MANAGER_H
#define SVMB_INTERCEPT_MANAGER_H

#include "core/vcpu.h"
#include "hw/msrpm.h"
#include "platform/base.h"

namespace svmb
{

class InterceptManager
{
public:
    struct Requirements
    {
        // refcounts [index], second dim {read, write}
        u32 Cr[16][2];
        u32 Dr[8][2];
        u32 Exception[32];
        u32 Opcode1[32];   // ic1 bits
        u32 Opcode2[32];   // ic2 bits 0-15 + CR write traps at 16-31
        u32 Mtf;           // monitor trap flag (CR0... actually ic1 HLT-free; MTF = DbgCtl)
    };

    NTSTATUS Init();
    void Deinit();

    // ---- requirement API (PASSIVE_LEVEL) ----
    NTSTATUS RequireCr(ModuleToken owner, u32 cr, bool read, bool write);
    NTSTATUS ReleaseCr(ModuleToken owner, u32 cr, bool read, bool write);
    NTSTATUS RequireDr(ModuleToken owner, u32 dr, bool read, bool write);
    NTSTATUS ReleaseDr(ModuleToken owner, u32 dr, bool read, bool write);
    NTSTATUS RequireException(ModuleToken owner, u32 vector);
    NTSTATUS ReleaseException(ModuleToken owner, u32 vector);
    NTSTATUS RequireOpcode(ModuleToken owner, u64 exitReason); // any vmexit:: reason
    NTSTATUS ReleaseOpcode(ModuleToken owner, u64 exitReason);
    NTSTATUS RequireMsr(ModuleToken owner, u32 msr, bool read, bool write);
    NTSTATUS ReleaseMsr(ModuleToken owner, u32 msr, bool read, bool write);
    NTSTATUS RequireMtf(ModuleToken owner);
    NTSTATUS ReleaseMtf(ModuleToken owner);

    // recompute VMCB control fields + MSRPM for one vcpu (called at enter)
    void ApplyToVcpu(VcpuContext* vcpu);
    // recompute + publish to all cores (PASSIVE)
    void ApplyAll();
    // revoke every requirement owned by `owner` (module detach safety net)
    u32 ReleaseOwner(ModuleToken owner);

    void SetMsrpm(Msrpm* msrpm) { Msrpm_ = msrpm; }
    const Requirements& Snapshot() const { return Req_; }

private:
    struct Entry
    {
        bool Used;
        ModuleToken Owner;
        u8 Kind;       // ReqKind
        u16 Index;     // cr/dr/vector
        u32 Exit;      // vmexit reason for Opcode kind
        u32 Msr;
        u8 Access;     // union of access bits ever requested (informational)
        u32 RefR;      // read refs (also the generic count for non-rw kinds)
        u32 RefW;      // write refs; an entry dies only when both hit zero
    };
    static constexpr u32 MAX_ENTRIES = 128;

    enum ReqKind : u8 { RK_CR, RK_DR, RK_EXC, RK_OPCODE, RK_MSR, RK_MTF };

    NTSTATUS AddRef(ModuleToken owner, ReqKind kind, u16 index, u32 exit, u32 msr, u8 access);
    NTSTATUS Release(ModuleToken owner, ReqKind kind, u16 index, u32 exit, u32 msr, u8 access);
    Entry* Find(ModuleToken owner, ReqKind kind, u16 index, u32 exit, u32 msr);
    void Fold(Requirements& req);

    Requirements Req_ = {};
    Entry Entries_[MAX_ENTRIES];
    KSPIN_LOCK Lock_;
    Msrpm* Msrpm_ = nullptr;
};

} // namespace svmb

#endif // SVMB_INTERCEPT_MANAGER_H
