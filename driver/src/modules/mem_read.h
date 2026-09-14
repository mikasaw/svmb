// svmb - hypervisor-level process memory read (r87, extended r88).
// Resolves the target CR3 from the seed table, walks the target's guest page
// tables with physical reads only, and copies [Va, Va+Size) into the IOCTL
// payload. No process attach, no handles, no guest kernel APIs - and
// because reads never NPF on the default RWX view, nothing is visible to
// the guest or to this driver's own protection layers (docs/HYPERVISOR_MEM_READ.md).
// r88 strategy B (SVMB_READVM_F_FAULTIN): optional attach+touch fault-in
// for not-present pages - completes the read, perturbs the working set.
#ifndef SVMB_MEM_READ_H
#define SVMB_MEM_READ_H

#include "platform/base.h"
#include "svmb_protocol.h"

namespace svmb
{

// IOCTL_CR3_READVM entry point (PASSIVE_LEVEL): `p` points at the buffered
// in/out buffer; the payload follows the struct. outCap is the output buffer
// size. Returns bytes written to the buffer (header + payload), 0 on
// rejection (unknown pid, bad size, kernel va, no RAM map).
u32 MemReadVm(SVMB_CR3_READVM* p, u32 outCap);

// counters surfaced through SVMB_CR3_STATS
void MemReadVmStats(u32& callsOut, u32& pagesOut);

// ---- pure x64 walk math (offline-testable core, r88) ------------------

// page-directory index of va at `level`: 0 = PML4, 1 = PDPT, 2 = PD, 3 = PT
u32 ReadVmVaIndex(u64 va, u32 level);
// physical frame of a page-directory entry (masks off flags)
u64 ReadVmEntryFrame(u64 entry);
// gpa inside a large page: PS entry + va offset; shift = 30 (1G) / 21 (2M)
u64 ReadVmLargeGpa(u64 entry, u64 va, u32 shift);
// gpa inside a 4K page: PTE frame + byte offset
u64 ReadVmPageGpa(u64 entry, u64 va);

} // namespace svmb

#endif // SVMB_MEM_READ_H
