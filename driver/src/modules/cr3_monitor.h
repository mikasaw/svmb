// cr3_monitor module (M4 skeleton): CR3 write monitoring + optional read
// spoofing, backed by the Cr3Seed process table.
//
// Attach = CR3 read/write intercepts on. The write path fires on every
// context switch (hot) - the handler is deliberately minimal (counter +
// save-area emulation). Read spoofing policy:
//   EnableReadSpoof && XorKey   -> every intercepted read returns real^K
//   TargetCr3 set               -> reads observed while that CR3 is active
//                                  also return real^K (per-process spoof)
#ifndef SVMB_CR3_MONITOR_H
#define SVMB_CR3_MONITOR_H

#include "platform/base.h"
#include "svmb_protocol.h"

namespace svmb
{

struct GuestContext; // core/regs.h
class NptView;       // mm/npt.h

// IOCTL_CR3_CONFIG entry point (called from main.cpp)
void Cr3MonitorConfigure(const SVMB_CR3_CONFIG* cfg);
// r76: DriverEntry-time init - register the NPF deny consumer (region
// sensing + orphan resolve) and the region death notify for the driver
// lifetime, independent of the module attach state
void Cr3MonitorLoadInit();
// r80: wiggle mode knob (registry Parameters\WiggleMode):
// 0=off, 1=back-to-back kick (r77), 2=real wiggle with DPC restore
void Cr3MonitorSetWiggleMode(u32 mode);

// r100 execute-sense seam (syscall audit): a non-region deny consumer
// checked BEFORE the region machinery on every Deny* classification.
// Returning true consumes the fault (the consumer recorded/resolved/
// re-armed itself). gpa4k = faulting page, info1 = NPF error code,
// cr3/rip = the faulting context, g = full guest registers (the
// MmCopyVirtualMemory call args are still live at entry-fetch).
typedef bool (*Cr3SenseCb)(u64 gpa4k, u64 info1, u64 cr3, u64 rip,
                           GuestContext& g);
void Cr3MonitorSetSenseCb(Cr3SenseCb cb);

// the arm views (base + optional ProcView) that region/sense denies must
// be installed on - fills up to 2 pointers, returns the count
u32 Cr3MonitorArmViews(NptView* views[2]);
// driver-unload counterpart of Cr3MonitorLoadInit
void Cr3MonitorLoadDeinit();
// IOCTL_CR3_STATS entry point: fills the protocol struct
void Cr3MonitorStats(SVMB_CR3_STATS* st);
// IOCTL_CR3_POISON entry point (r71 test hook): corrupt one armed slot's
// watermark to simulate frame reuse - exercises the kill-switch (F1 via a
// foreign trip, F2 via the re-arm DPC) without a kd session. Returns the
// number of slots poisoned (0 when nothing is armed).
u32 Cr3MonitorPoison();

// IOCTL_CR3_PROT entry point (r75 protected regions): Action register /
// unprotect / list. L1 = ZwProtectVirtualMemory(PAGE_READONLY) on the
// region in the owning process (guest MM blocks writers); L2 = NPT W-deny
// on the same frames (trips = L1 bypass alerts). Returns 1/entry count on
// success, 0 on rejection.
u32 Cr3RegionAction(SVMB_CR3_PROT* p);
// driver-unload hygiene: restore guest MM protections for every region
void Cr3RegionCleanupAll();
// r76: unprotect every region owned by a dying process (process-exit
// notify). Without this a freed frame keeps its live W-deny into pool
// reuse - region slots have no watermark kill-switch, so innocent writers
// of the recycled frame would trip forever.
void Cr3RegionSweepPid(u32 pid);
// IOCTL_CR3_PROBE entry point (r76 test probe): write the magic through a
// kernel MDL mapping of [Base, Base+Size) in the owning process - bypasses
// the L1 readonly PTE, so a live region must alert via the NPT W-deny
// (RegionTrips). Returns bytes written (0 on rejection).
u32 Cr3RegionProbe(SVMB_CR3_PROBE* p);

// ---- r89 plan C: alert + kill policy (docs/L2_BLOCKING_DESIGN.md 3-C) --
// deny list for the kill policy (pure, offline-tested): system-critical
// images the policy must never terminate
bool Cr3RegionKillDenylisted(const char* image);

// r105: suspend a pid on a sysaudit behavior alert (opt-in via
// Parameters\BehaveResponse=1). Shares the r91 suspension table +
// resume-all with the kill path; S1 check-before-freeze + the r96-ticketed
// resume belt (insert-time full table undoes the freeze). Returns the pid
// on success, 0 on refusal (guards/missing primitive/table full).
u32 Cr3RegionSuspendPidSysalert(u32 pid, const char* image);
// init right after the control device exists (work item is device-tied);
// deinit before the device is deleted (bounded in-flight wait + resume-all)
void Cr3RegionKillInit(PDEVICE_OBJECT dev);
void Cr3RegionKillDeinit();
// r91 plan C- ungated IOCTL helpers (the suspended writer holds the gate
// inside its in-flight IOCTL - gated list/resume would deadlock)
u32 Cr3RegionSuspList(SVMB_SUSP* p);
u32 Cr3RegionSuspResume(SVMB_SUSP* p);
// r91 gate rescue (defined in main.cpp, called from the death notify):
// force-release the IOCTL gate when its holder died while holding it
void IoGateOwnerDied(u32 pid);

// Pure resolver for "mov reg, cr3" emulation: returns the value the guest
// observes for the real CR3 value `real`. With readSpoof, every read is
// xor-striked with xorKey; targetCr3 additionally spoofs reads observed
// while the monitored CR3 is active (both are no-ops when xorKey is 0).
u64 Cr3ResolveRead(u64 real, u64 xorKey, bool readSpoof, u64 targetCr3);

// pure view-switch decision for the CR3 write path: with process-view mode
// enabled and a target armed, a context switch INTO the target process
// activates its view, leaving it returns to the default view
enum class Cr3ViewSwitch
{
    None = 0,
    Enter, // newCr3 == target (switching into the watched process)
    Leave, // curCr3 == target && newCr3 != target (switching out)
};
Cr3ViewSwitch Cr3ViewSwitchDecision(u64 curCr3, u64 newCr3, u64 targetCr3,
                                    bool processViewEnabled);

} // namespace svmb

#endif // SVMB_CR3_MONITOR_H
