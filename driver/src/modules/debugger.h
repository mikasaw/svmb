// debugger module (M4): exception events (BP/DB/UD), DR shadowing/spoofing,
// MTF single-step primitive.
// Config lives in the module; IOCTL_DBG_CONFIG routes here. Config changes
// are stored even while detached and applied at attach time.
#ifndef SVMB_MODULE_DEBUGGER_H
#define SVMB_MODULE_DEBUGGER_H

#include "platform/base.h"
#include "svmb_protocol.h"

namespace svmb
{

// IOCTL_DBG_CONFIG entry point (called from main.cpp)
void DebuggerConfigure(const SVMB_DBG_CONFIG* cfg);

// MTF single-step primitive: arms the monitor-trap flag on every VMCB
// (require-processed through the InterceptManager); the next executed guest
// instruction raises a consumed #DB-class exit. Returns false when the
// hypervisor is not running or the module is not attached.
bool DebuggerArmSingleStep();

// per-CPU shadow copy of the guest-visible debug registers (used while
// HideDr is on: writes land here, reads come back from here)
struct DrShadow
{
    u64 Dr[4]; // DR0-3
    u64 Dr6;
    u64 Dr7;
};

// Pure resolver for "mov reg, drN" emulation: returns the value the guest
// observes. realDr/realDr7 are the hardware values, consulted only when
// hiding is off; passing them in keeps this free of hardware access (and
// offline-testable). dr must be 0-3, 6 or 7 - anything else reads as 0.
u64 DrResolveRead(u32 dr, const DrShadow& shadow, bool hide, bool spoofDr7,
                  u64 realDr, u64 realDr7);

} // namespace svmb

#endif // SVMB_MODULE_DEBUGGER_H
