#include "core/intercept_manager.h"
#include "core/hypervisor.h"
#include "platform/logger.h"
#include "platform/util.h"

namespace svmb
{

NTSTATUS InterceptManager::Init()
{
    KeInitializeSpinLock(&Lock_);
    RtlZeroMemory(Entries_, sizeof(Entries_));
    RtlZeroMemory(&Req_, sizeof(Req_));
    return STATUS_SUCCESS;
}

void InterceptManager::Deinit()
{
    RtlZeroMemory(Entries_, sizeof(Entries_));
    RtlZeroMemory(&Req_, sizeof(Req_));
}

InterceptManager::Entry* InterceptManager::Find(ModuleToken owner, ReqKind kind,
                                                u16 index, u32 exit, u32 msr)
{
    for (u32 i = 0; i < MAX_ENTRIES; ++i)
    {
        Entry& e = Entries_[i];
        if (e.Used && e.Owner == owner && e.Kind == kind && e.Index == index &&
            e.Exit == exit && e.Msr == msr)
            return &e;
    }
    return nullptr;
}

NTSTATUS InterceptManager::AddRef(ModuleToken owner, ReqKind kind, u16 index,
                                  u32 exit, u32 msr, u8 access)
{
    if (!access)
        return STATUS_INVALID_PARAMETER;
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    Entry* e = Find(owner, kind, index, exit, msr);
    if (e)
    {
        if (access & 1) ++e->RefR;
        if (access & 2) ++e->RefW;
        e->Access |= access;
    }
    else
    {
        for (u32 i = 0; i < MAX_ENTRIES; ++i)
        {
            Entry& slot = Entries_[i];
            if (!slot.Used)
            {
                slot.Used = true;
                slot.Owner = owner;
                slot.Kind = kind;
                slot.Index = index;
                slot.Exit = exit;
                slot.Msr = msr;
                slot.Access = access;
                slot.RefR = (access & 1) ? 1 : 0;
                slot.RefW = (access & 2) ? 1 : 0;
                e = &slot;
                break;
            }
        }
        if (!e)
        {
            KeReleaseSpinLock(&Lock_, old);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    KeReleaseSpinLock(&Lock_, old);

    ApplyAll();
    return STATUS_SUCCESS;
}

NTSTATUS InterceptManager::Release(ModuleToken owner, ReqKind kind, u16 index,
                                   u32 exit, u32 msr, u8 access)
{
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    Entry* e = Find(owner, kind, index, exit, msr);
    // per-direction counts: releasing an access direction that has no live
    // reference is an error, not a silent no-op (keeps the books balanced)
    if (!e || ((access & 1) && !e->RefR) || ((access & 2) && !e->RefW))
    {
        KeReleaseSpinLock(&Lock_, old);
        return STATUS_NOT_FOUND;
    }
    if (access & 1) --e->RefR;
    if (access & 2) --e->RefW;
    if (!e->RefR && !e->RefW)
        e->Used = false;
    KeReleaseSpinLock(&Lock_, old);

    ApplyAll();
    return STATUS_SUCCESS;
}

NTSTATUS InterceptManager::RequireCr(ModuleToken o, u32 cr, bool r, bool w)
{
    return cr >= 16 ? STATUS_INVALID_PARAMETER : AddRef(o, RK_CR, (u16)cr, 0, 0, (u8)((r ? 1 : 0) | (w ? 2 : 0)));
}
NTSTATUS InterceptManager::ReleaseCr(ModuleToken o, u32 cr, bool r, bool w)
{
    return cr >= 16 ? STATUS_INVALID_PARAMETER : Release(o, RK_CR, (u16)cr, 0, 0, (u8)((r ? 1 : 0) | (w ? 2 : 0)));
}
NTSTATUS InterceptManager::RequireDr(ModuleToken o, u32 dr, bool r, bool w)
{
    return dr >= 8 ? STATUS_INVALID_PARAMETER : AddRef(o, RK_DR, (u16)dr, 0, 0, (u8)((r ? 1 : 0) | (w ? 2 : 0)));
}
NTSTATUS InterceptManager::ReleaseDr(ModuleToken o, u32 dr, bool r, bool w)
{
    return dr >= 8 ? STATUS_INVALID_PARAMETER : Release(o, RK_DR, (u16)dr, 0, 0, (u8)((r ? 1 : 0) | (w ? 2 : 0)));
}
NTSTATUS InterceptManager::RequireException(ModuleToken o, u32 v)
{
    return v >= 32 ? STATUS_INVALID_PARAMETER : AddRef(o, RK_EXC, (u16)v, 0, 0, 1);
}
NTSTATUS InterceptManager::ReleaseException(ModuleToken o, u32 v)
{
    return v >= 32 ? STATUS_INVALID_PARAMETER : Release(o, RK_EXC, (u16)v, 0, 0, 1);
}
namespace
{

// exit reasons the InterceptManager can actually map onto VMCB control bits.
// NPF/AVIC/VMGEXIT (0x400+) are not opcode-bit controlled, IOIO needs a wired
// IOPM (the framework has none yet - setting the bit without a valid
// IopmBasePa fails the VMRUN consistency check), and 0x28-0x2F/0x38-0x3F are
// not exit codes at all. Rejecting here keeps the Require*/Release* contract
// honest: STATUS_SUCCESS really means "the intercept is in effect".
bool IsMappableOpcodeExit(u64 exit)
{
    if (exit <= 0x1F)
        return true;                                    // CR0-15 read (0x00-0x0F) / write (0x10-0x1F)
    if (exit >= 0x20 && exit <= 0x27)
        return true;                                    // DR0-7 read
    if (exit >= 0x30 && exit <= 0x37)
        return true;                                    // DR0-7 write
    if (exit >= vmexit::EXCEPTION(0) && exit <= vmexit::EXCEPTION(31))
        return true;                                    // exceptions (0x40-0x5F)
    if (exit >= 0x60 && exit <= 0x8F)
        return exit != vmexit::IOIO;                    // ic1 bits + ic2 bits 0-15
    if (exit >= vmexit::CR_WRITE_TRAP(0) && exit <= vmexit::CR_WRITE_TRAP(15))
        return true;                                    // CR write traps -> ic2 bits 16-31
    return false;
}

} // namespace

NTSTATUS InterceptManager::RequireOpcode(ModuleToken o, u64 exitReason)
{
    if (!IsMappableOpcodeExit(exitReason))
    {
        SVMB_LOGW("RequireOpcode(%llx) rejected: not an opcode-bit exit", exitReason);
        return STATUS_NOT_IMPLEMENTED;
    }
    return AddRef(o, RK_OPCODE, 0, (u32)exitReason, 0, 1);
}
NTSTATUS InterceptManager::ReleaseOpcode(ModuleToken o, u64 exitReason)
{
    if (!IsMappableOpcodeExit(exitReason))
        return STATUS_NOT_IMPLEMENTED;
    return Release(o, RK_OPCODE, 0, (u32)exitReason, 0, 1);
}
NTSTATUS InterceptManager::RequireMsr(ModuleToken o, u32 msr, bool r, bool w)
{
    return AddRef(o, RK_MSR, 0, 0, msr, (u8)((r ? 1 : 0) | (w ? 2 : 0)));
}
NTSTATUS InterceptManager::ReleaseMsr(ModuleToken o, u32 msr, bool r, bool w)
{
    return Release(o, RK_MSR, 0, 0, msr, (u8)((r ? 1 : 0) | (w ? 2 : 0)));
}
NTSTATUS InterceptManager::RequireMtf(ModuleToken o) { return AddRef(o, RK_MTF, 0, 0, 0, 1); }
NTSTATUS InterceptManager::ReleaseMtf(ModuleToken o) { return Release(o, RK_MTF, 0, 0, 0, 1); }

u32 InterceptManager::ReleaseOwner(ModuleToken owner)
{
    u32 count = 0;
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    for (u32 i = 0; i < MAX_ENTRIES; ++i)
    {
        Entry& e = Entries_[i];
        if (e.Used && e.Owner == owner)
        {
            e.Used = false;
            ++count;
        }
    }
    KeReleaseSpinLock(&Lock_, old);
    if (count)
        ApplyAll();
    return count;
}

void InterceptManager::Fold(Requirements& req)
{
    RtlZeroMemory(&req, sizeof(req));
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    for (u32 i = 0; i < MAX_ENTRIES; ++i)
    {
        const Entry& e = Entries_[i];
        if (!e.Used)
            continue;
        switch (e.Kind)
        {
        case RK_CR:
            if (e.RefR) req.Cr[e.Index][0] += e.RefR;
            if (e.RefW) req.Cr[e.Index][1] += e.RefW;
            break;
        case RK_DR:
            if (e.RefR) req.Dr[e.Index][0] += e.RefR;
            if (e.RefW) req.Dr[e.Index][1] += e.RefW;
            break;
        case RK_EXC:
            req.Exception[e.Index] += e.RefR;
            break;
        case RK_OPCODE:
        {
            // map exit reason back to its bitmap bit
            u32 bit = e.Exit;
            if (bit >= vmexit::EXCEPTION(0) && bit <= vmexit::EXCEPTION(31))
                req.Exception[bit - vmexit::EXCEPTION(0)] += e.RefR;
            else if (bit <= 0x0F)
                req.Cr[bit][0] += e.RefR;
            else if (bit >= 0x10 && bit <= 0x1F)
                req.Cr[bit - 0x10][1] += e.RefR;
            else if (bit >= 0x20 && bit <= 0x27)
                req.Dr[bit - 0x20][0] += e.RefR;
            else if (bit >= 0x30 && bit <= 0x37)
                req.Dr[bit - 0x30][1] += e.RefR;
            else if (bit >= 0x60 && bit <= 0x7F)
                req.Opcode1[bit - 0x60] += e.RefR; // ic1 bit n <-> exit 0x60+n
            else if (bit >= 0x80 && bit <= 0x8F)
                req.Opcode2[bit - 0x80] += e.RefR; // ic2 bit n <-> exit 0x80+n
            else if (bit >= vmexit::CR_WRITE_TRAP(0) && bit <= vmexit::CR_WRITE_TRAP(15))
                req.Opcode2[16 + (bit - vmexit::CR_WRITE_TRAP(0))] += e.RefR; // ic2 bit 16+n
            break;
        }
        case RK_MSR:
            if (Msrpm_)
            {
                Msrpm_->SetIntercept(e.Msr, e.RefR != 0, e.RefW != 0, true);
            }
            break;
        case RK_MTF:
            req.Mtf += e.RefR;
            break;
        }
    }
    KeReleaseSpinLock(&Lock_, old);
}

void InterceptManager::ApplyToVcpu(VcpuContext* vcpu)
{
    Requirements req;
    Fold(req);

    // ---- fold core baseline (always-on) ----
    // CPUID + MSR (the reference educational hypervisor also sets only these two).
    //
    // ic2.VMRUN (anti-nesting) IS set: 2026-09-07 17:30 cross-test with
    // a reference hypervisor on the same VMware vhv VM showed their
    // driver sets VMRUN intercept and successfully runs vmrun. Our prior
    // belief that "VMware nested-VMRUN emulation triple-faults on VMRUN
    // intercept bit" (CRASH_DEBUG_LOG round 9, 2026-09-07) was wrong - the
    // triple-fault was caused by something else (turned out to be EFER.SVME
    // racing with an L1 msrpm intercept). VMRUN intercept is mandatory for
    // proper anti-nesting (reference-implementation comment: "vmrun拦截必须打开,
    // 否则vmrun会失败").
    //
    // round-13 align: drop ic2.VMMCALL from baseline. The reference does NOT
    // intercept VMMCALL; the hypercall channel there is via CPUID leaf
    // 0x400000ff. Our VMMCALL intercept was added for the same purpose but
    // the VMware vhv panic on "Invalid VMCB" fires before any VMMCALL ever
    // executes, so the baseline intercept is just excess baggage. Modules
    // that need VMMCALL can re-add it via RequireOpcode.
    // round-15 bisect: baseline CPUID + MSR intercepts REMOVED. Every
    // pre-death exit trace showed only CPUID (0x72) exits from the guest's
    // own Hvi/licensing probes, and the machine still hard-reset with them
    // serviced cleanly - so the CPUID/MSR exit path is the remaining delta
    // candidate against the reference (which runs stable with its own handlers).
    // With op1 baseline = 0 the hypervisor is CPUID/MSR-transparent; guest
    // sees VMware's real CPUID including the SVM bit. Hypercall detection
    // via the CPUID leaf fallback will not answer while this bisect is in
    // place (svmbctl info shows hv OFF - use ping/exittrace for liveness).
    // round-18: full reference-parity baseline RESTORED (CPUID + MSR), now
    // that the round-18 VMCB byte diff identified the segment LIMIT fields
    // (the reference zeroes them, svmb filled 4GB/real values) plus DR6/DR7 as
    // the remaining divergent state. Hypervisor.cpp FillGuestState now
    // zeroes limits + DR6/DR7 to match the reference exactly.
    u32 op1 = (1u << 18) | (1u << 28);  // CPUID + RDMSR/WRMSR (reference baseline)
    // ic2 bits per vmcb.h: bit 0 = VMRUN (anti-nesting; must be set, see
    // reference baseline + CRASH_DEBUG_LOG round 10).
    u32 op2 = ic2::VMRUN;
    // opcode1 requirements (bit n = exit reason 0x60+n)
    for (u32 i = 0; i < 32; ++i)
        if (req.Opcode1[i])
            op1 |= 1u << i;
    // opcode2: bits 0-15 are the ic2 instruction bits, 16-31 the CR0-15
    // write-trap shortcuts (exit reasons 0x90+n)
    for (u32 i = 0; i < 32; ++i)
        if (req.Opcode2[i])
            op2 |= 1u << i;

    u16 crRead = 0, crWrite = 0;
    for (u32 i = 0; i < 16; ++i)
    {
        if (req.Cr[i][0]) crRead |= (u16)(1u << i);
        if (req.Cr[i][1]) crWrite |= (u16)(1u << i);
    }
    u16 drRead = 0, drWrite = 0;
    for (u32 i = 0; i < 8; ++i)
    {
        if (req.Dr[i][0]) drRead |= (u16)(1u << i);
        if (req.Dr[i][1]) drWrite |= (u16)(1u << i);
    }
    u32 exc = 0;
    for (u32 i = 0; i < 32; ++i)
        if (req.Exception[i])
            exc |= 1u << i;

    vcpu->GuestVmcb->Ctrl.InterceptCrRead = crRead;
    vcpu->GuestVmcb->Ctrl.InterceptCrWrite = crWrite;
    vcpu->GuestVmcb->Ctrl.InterceptDrRead = drRead;
    vcpu->GuestVmcb->Ctrl.InterceptDrWrite = drWrite;
    vcpu->GuestVmcb->Ctrl.InterceptException = exc;
    vcpu->GuestVmcb->Ctrl.InterceptOpcode1 = op1;
    vcpu->GuestVmcb->Ctrl.InterceptOpcode2 = op2;

    // r38: one line per apply call so a module's Require* is observable
    // against what actually landed in the live VMCBs (the M4-D finding:
    // the debugger's exception intercepts were believed armed but the #UD
    // never exited)
    SVMB_LOGI("intercept apply: cpu=%u exc=%04x crR=%04x crW=%04x "
              "drR=%04x drW=%04x op1=%08x op2=%08x mtf=%u",
              vcpu->Info.CpuIndex, exc, crRead, crWrite, drRead, drWrite,
              op1, op2, (u32)req.Mtf);

    // MSRPM is global (shared across cores); its bits were set directly in Fold
    vcpu->GuestVmcb->Ctrl.MsrpmBasePa = Msrpm_ ? Msrpm_->PhysicalAddress() : 0;

    // single-step via MTF when required (DbgCtl.MTF lives in save area)
    if (req.Mtf)
        vcpu->GuestVmcb->Save.DbgCtl |= (1ull << 12); // bit 12 = MTF
    else
        vcpu->GuestVmcb->Save.DbgCtl &= ~(1ull << 12);

    // all control fields changed -> invalidate the clean bits cache
    vcpu->GuestVmcb->Ctrl.CleanBits.Bits = 0;
}

void InterceptManager::ApplyAll()
{
    // MSRPM double-buffered rebuild (closes review finding P2-4): bits are
    // built into the shadow page, then committed by swapping pages; the new
    // active PA is republished into every VMCB (sampled only at VMRUN).
    Hypervisor* hv = Hypervisor::Instance();
    if (Msrpm_)
        Msrpm_->BeginBuild();
    RtlZeroMemory(&Req_, sizeof(Req_));
    Fold(Req_); // recomputes Req_ AND sets MSRPM bits in the build page
    if (Msrpm_)
        Msrpm_->CommitBuild();

    if (!hv)
        return;
    hv->ForEachVcpu([this](VcpuContext* vcpu) {
        ApplyToVcpu(vcpu);
    });
}

} // namespace svmb
