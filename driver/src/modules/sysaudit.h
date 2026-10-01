// svmb - r100 syscall audit: EXECUTE-SENSE on MmCopyVirtualMemory (the
// single kernel funnel for NtReadVirtualMemory / NtWriteVirtualMemory,
// i.e. every ReadProcessMemory / WriteProcessMemory). r99 verdict: an
// inline NPT hook on the function tripped PatchGuard 0x109 - this v2
// modifies ZERO guest bytes: the function's 4K page is armed X-deny in
// the NPT (hypervisor-private), the entry fetch trips one NPF, the
// consumer records caller (cr3->pid/image) + call args (still in the
// registers at entry-fetch) + resolves (page opens, the instruction
// re-executes) and a timer re-denies 250ms later (sentinel r68 pattern).
// PatchGuard-invisible by construction: there is nothing to hash.
#ifndef SVMB_SYSAUDIT_H
#define SVMB_SYSAUDIT_H

#include "platform/base.h"
#include "svmb_protocol.h"

namespace svmb
{

// drains up to SVMB_SYSAUD_MAX_ENTRIES plus the status block (main.cpp
// IOCTL glue). PASSIVE_LEVEL; callable with or without the module
// attached - counters persist until the next attach.
void SysAuditDrain(SVMB_SYSCALL_AUDIT* out);

// r102 behavioral snapshot (IOCTL_SYSAUD_BEHAVIOR glue): copies the
// caller-rate table + global counters. Approximate under concurrency.
void SysAuditBehaviorSnapshot(SVMB_SYSAUD_BEHAVIOR* out);

// r102: registry knob ingestion (Parameters\BehaveRdThs/BehaveWrThs/
// BehaveOpThs, read once at DriverEntry). 0 = keep the default.
void SysAuditSetBehaveThresholds(u32 rd, u32 wr, u32 op);

// r105: response knob ingestion (Parameters\BehaveResponse, read once
// at DriverEntry). 0 = telemetry only (default); 1 = suspend the
// alerting caller via the r91 reversible suspension infrastructure.
void SysAuditSetBehaveResponse(u32 v);

// r103: hand the target resolver its work item (DriverEntry-time, like
// Cr3RegionKillInit; pass the device object, nullptr disables). The
// resolver outlives attach cycles - its cache survives detach.
void SysAuditResInit(void* deviceObject);

// r103: unload hygiene (mirror Cr3RegionKillDeinit) - disarm the kick,
// wait bounded for an in-flight drain, free the work item
void SysAuditResDeinit();

// r103 (caller,target) cache-slot hash probe start (linear probe is
// the real mechanism; the hash degenerates under the slot mask)
u32 SysAuditTargetSlotFor(u32 callerPid, u32 targetPid);

// ---- pure slot math (offline-tested) --------------------------------------

// ring slot for a monotonic sequence number (power-of-two mask - the
// r97 signed-modulo landmine class)
u32 SysAuditSlotIndex(u32 seq);

// the drain accepts a slot when its Commit marker matches drain-pos+1
bool SysAuditSlotReady(u64 commit, LONG d);

// r101 runtime KiServiceTable law (18362/19H1, kdump-verified live): the
// on-disk initializer holds ABSOLUTE image RVAs, but the table is
// rewritten at boot into PACKED table-relative form - service =
// table + (s32(entry) >> 4), low nibble = stack-arg count
u64 SysAuditServiceVa(u64 tableVa, u32 entry);

// ---- r102 behavior math (offline-tested) ----------------------------------

// 1s analytics window index from an interrupt-time stamp (100ns units)
u64 SysAuditBehaveWindowOf(u64 tick);

// caller-hash slot probe start (Knuth multiplicative, power-of-two mask)
u32 SysAuditBehaveSlotFor(u32 pid);

// exact alert edge: true on the one increment that reaches the threshold
bool SysAuditBehaveEdge(u64 count, u64 threshold);

} // namespace svmb

#endif // SVMB_SYSAUDIT_H
