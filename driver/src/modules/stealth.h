// svmb - r92 stealth module: CPUID evidence scrubbing (environment
// disguise). The scrub set and the transform are pure functions so the
// offline self-tests exercise the exact code the exit handler runs.
// Lifecycle is the switch: attaching the module enables the scrub,
// detaching restores the native answers (svmbctl mod attach/detach stealth).
#ifndef SVMB_STEALTH_H
#define SVMB_STEALTH_H

#include "platform/base.h"

namespace svmb
{

// true when `leaf` is one the stealth module owns (the handler answers it
// and stops the CPUID handler chain instead of falling through to core).
bool StealthScrubLeaf(u32 leaf);

// mutates emulated CPUID results in place for owned leaves. Deterministic,
// no side effects. Call only when StealthScrubLeaf(leaf) returned true.
void StealthApplyScrub(u32 leaf, u32 res[4]);

} // namespace svmb

#endif // SVMB_STEALTH_H
