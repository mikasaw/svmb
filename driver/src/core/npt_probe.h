// svmb - M3 live-NPT probe declarations.
// Deterministic exercises for the two M3 integration seams that offline
// tests cannot cover: the real NPF exit path (hook slide) and the live
// view-switch publish protocol. See core/npt_probe.cpp for the stage
// definitions; the caller-facing contract is SVMB_NPT_PROBE in
// svmb_protocol.h. Requires hypervisor running + NptEnable=1.
#ifndef SVMB_NPT_PROBE_H
#define SVMB_NPT_PROBE_H

#include "platform/base.h"
#include "svmb_protocol.h"

namespace svmb
{

class NptHookManager; // defined in modules/npt_hook_mgr.h (main.cpp owns the
                      // instance; accessor below avoids a core->modules cycle)

NptHookManager* HookInstance();
NTSTATUS RunNptProbe(SVMB_NPT_PROBE* p);

} // namespace svmb

#endif // SVMB_NPT_PROBE_H
