// svmb - r107 freeze flight recorder. The r105-era freezes (5+ events)
// all died with kd reporting no_debuggee at freeze time - the CPUs never
// returned to guest context, so the hang is inside svmb's own exit/flush/
// DPC paths and guest-side debuggers are useless. This ring records the
// last CRUMBS_MAX hops across those paths (lock-free, approximate,
// IRQL-any) so a frozen state can be diagnosed two ways:
//   live   : IOCTL_CRUMBS (0x82C) while the guest still runs
//   frozen : vmrun suspend -> .vmss -> offline scan for the magic
//            (crumbs decode order-independently via Seq; the highest-Seq
//            entries are the most recent hops)
// Test facility: compiled out of production builds entirely.
#ifndef SVMB_CRUMBS_H
#define SVMB_CRUMBS_H

#include "platform/base.h"

namespace svmb
{

constexpr LONG CRUMBS_MAGIC0 = 0x43564253; // 'SBVC'
constexpr LONG CRUMBS_MAGIC1 = 0x31303752; // r701 (r107 rev)
constexpr u32 CRUMBS_MAX = 128;            // power of two

// tag vocabulary (documented for the offline vmss decoder)
constexpr LONG CRUMB_CONSUMER_ENTER = 0x101;  // SenseConsume entered
constexpr LONG CRUMB_CONSUMER_HIT = 0x102;    // watched-entry recorded
constexpr LONG CRUMB_CONSUMER_RESOLVED = 0x103; // page reopened
constexpr LONG CRUMB_CONSUMER_DONE = 0x106;   // KeSetTimer done, returning true
constexpr LONG CRUMB_CONSUMER_NOISE = 0x110;  // noise path taken
constexpr LONG CRUMB_DPC_ENTER = 0x120;       // re-deny DPC entered
constexpr LONG CRUMB_DPC_UNDENIED = 0x121;    // pages re-denied
constexpr LONG CRUMB_DPC_PREKICK = 0x122;     // before TlbKick
constexpr LONG CRUMB_DPC_POSTKICK = 0x123;    // after TlbKick returned
constexpr LONG CRUMB_KICK_ENTER = 0x130;      // TlbKickFlushAllCores
constexpr LONG CRUMB_KICK_DONE = 0x131;       // all VMCB writes issued
constexpr LONG CRUMB_WIGGLE_TS = 0x140;       // wiggle to-scratch done
constexpr LONG CRUMB_WIGGLE_REST = 0x141;     // wiggle restore done
constexpr LONG CRUMB_RESOLVER_ENTER = 0x150;  // resolver workitem entered
constexpr LONG CRUMB_RESOLVER_EXIT = 0x151;   // resolver workitem leaving
constexpr LONG CRUMB_NPF_ENTER = 0x104;      // HandleNpfExit entered
constexpr LONG CRUMB_NPF_TAIL = 0x105;       // HandleNpfExit fully handled
constexpr LONG CRUMB_DISPATCH_DEFAULT = 0x170; // exit hit default policy
constexpr LONG CRUMB_KILL_ENTER = 0x160;      // kill DPC entered

struct Crumb
{
    volatile LONG Tag;
    volatile LONG Cpu;
    volatile LONG64 Tick; // KeQueryInterruptTime at the hop
    volatile LONG64 Seq;  // monotonic claim (order key for the decoder)
};

// raw layout is part of the offline vmss-decoder contract: Magic0,
// Magic1, W, Pad (16 bytes) then CRUMBS_MAX x 24-byte entries.
struct CrumbRing
{
    volatile LONG Magic0;
    volatile LONG Magic1;
    volatile LONG W;
    volatile LONG Pad;
    volatile LONG64 Dropped;
    Crumb E[CRUMBS_MAX];
};

// allocated at DriverEntry (test builds only); null in production
CrumbRing* CrumbInstance();

void CrumbInit();

// IRQL-any, lock-free, approximate: a torn/lost hop is acceptable (this
// is a flight recorder, not accounting)
void CrumbPost(LONG tag);

} // namespace svmb

#endif // SVMB_CRUMBS_H
