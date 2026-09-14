// svmb - offline self-tests: exercised via IOCTL_SELFTEST with the driver
// loaded and the hypervisor NOT started. Every check is pure bookkeeping /
// data-structure work, so the whole suite runs without VMRUN.
#ifndef SVMB_OFFLINE_TESTS_H
#define SVMB_OFFLINE_TESTS_H

#include "platform/base.h"
#include "svmb_protocol.h"

namespace svmb
{

// runs every registered offline test; fills out with an aggregate result and
// logs each failure. PASSIVE_LEVEL only (allocates and touches page tables).
NTSTATUS RunOfflineSelfTests(SVMB_SELFTEST_RESULT* out);

} // namespace svmb

#endif // SVMB_OFFLINE_TESTS_H
