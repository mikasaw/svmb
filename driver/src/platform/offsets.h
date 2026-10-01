// svmb - runtime-resolved system structure offsets.
// The driver previously hardcoded 1903 layout offsets (EPROCESS DTB 0x28,
// ThreadListHead 0x488, ETHREAD ThreadListEntry 0x6b8) - wrong on any other
// Windows build = silent wrong-memory reads. OffsetsResolve() derives all
// three from live invariants at load time instead; consumers must treat a
// failed resolve as "layout unknown - never touch EPROCESS/ETHREAD fields"
// and degrade their feature loudly.
#ifndef SVMB_OFFSETS_H
#define SVMB_OFFSETS_H

#include "platform/base.h"

struct _EPROCESS;

namespace svmb
{

struct SysOffsets
{
    u32 EprocDtb;         // EPROCESS.DirectoryTableBase
    u32 EprocThreadList;  // EPROCESS.ThreadListHead
    u32 EthreadListEntry; // ETHREAD.ThreadListEntry
};

// PASSIVE_LEVEL, once (idempotent). Derives:
//   EprocDtb         - the current process's directory base (__readcr3,
//                      low flags masked) appears exactly once in
//                      EPROCESS[0, 0x400)
//   EthreadListEntry - the CURRENT thread carries LIST_ENTRYs; the one
//                      whose Blink chain walks backwards into EPROCESS is
//                      the process's thread list (1903 does not export
//                      PsGetNextProcessThread, so "ask for the first
//                      thread" is not an option - r47)
//   EprocThreadList  - the head address found above minus the EPROCESS base
// Every speculative dereference is gated by MmIsAddressValid: reading
// freed NonPaged pool bugchecks 0x50 directly and is NOT catchable by
// SEH (the r83 kd catch proved it). The result is cross-checked against
// a small known-build table (1903 = the campaign's measured values) and
// any mismatch is logged loudly - the scan result always wins over the
// table (the table can be wrong, live invariants cannot).
NTSTATUS OffsetsResolve();

// valid only after OffsetsResolve returned success
const SysOffsets& Offsets();

// OS build number (RtlGetVersion), for logs and the known-build table
ULONG OffsetsBuild();

} // namespace svmb

#endif // SVMB_OFFSETS_H
