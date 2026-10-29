// svmb - r181 third-party-H virtualization-behavior recorder (vlog).
// Records the software-nesting control flow so an accepted third-party
// hypervisor driver's operations can be replayed and analyzed: session
// accepts (with its L2 entry state), every reflected L2 exit, the H's
// requested intercept sets, the injections it arms into its L2, its
// control edits between exits (nested-NCR3 switches, TSC-offset moves),
// and the shadow composes its nested NPT drives (its memory map intent).
//
// The recorder sits on the reflection path only - with no nested session
// alive it costs nothing. Fixed-size records in a spinlock ring, drained
// by IOCTL (SVMB_IOCTL_VLOG); analysis/decoding lives in svmbctl.
#ifndef SVMB_VLOG_H
#define SVMB_VLOG_H

#include "platform/base.h"

namespace svmb {

enum VlogKind : u8
{
    VLOG_ACCEPT  = 1, // a=hvmcbPa b=L2rip c=L2cr3 d=np|pendingL1<<1
    VLOG_REFLECT = 2, // a=exitCode b=info1 c=info2 d=L2rip
    VLOG_COMPOSE = 3, // a=gpa b=targetPa c=perms d=level(0=4k 1=2M 2=1G)
    VLOG_INTENT  = 4, // a=hIc1 b=hIc2 c=hExc - H's bitmap, on change only
    VLOG_HINJ    = 5, // a=eventInj word H armed for delivery into its L2
    VLOG_HCTRL   = 6, // a=field(1=nested NCr3 2=TscOffset) b=old c=new
};

bool VlogInit();
void VlogDeinit();

// raw single-record hook (spinlock + mask-gated)
void VlogRecord(u8 kind, u8 cpu, u64 a, u64 b, u64 c, u64 d);

// composed hooks; VlogAccept internally diffs H's control state against
// the per-core previous accept and emits INTENT/HINJ/HCTRL records
void VlogAccept(u32 cpu, u64 hvmcbPa, u64 rip, u64 cr3, u32 np,
                u32 pendingL1, u32 hIc1, u32 hIc2, u32 hExc, u64 hNcr3,
                u64 hTscOff, u64 hEventInj);
void VlogReflect(u32 cpu, u64 exitCode, u64 info1, u64 info2, u64 l2rip);
void VlogCompose(u32 cpu, u64 gpa, u64 pa, u64 perms, u32 level);

u32  VlogRead(void* out, u32 maxRecords, u32* droppedOut); // drain read
void VlogStats(u64 kindCount[6], u64 topExits[8][2]);
void VlogSetMask(u32 kindMask); // bit (kind-1); 0 bit = kind off
void VlogReset();

} // namespace svmb

#endif // SVMB_VLOG_H
