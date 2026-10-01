// svmb - debugger event ring (M4 groundwork): fixed-capacity SVMB_DBG_EVENT
// slots with drop-on-full accounting; producers push from exit context in M4,
// R3 drains via IOCTL_DBG_EVENTS. Offline: drain/push are testable now.
#ifndef SVMB_DBG_EVENTS_H
#define SVMB_DBG_EVENTS_H

#include "platform/base.h"
#include "svmb_protocol.h"

namespace svmb
{

class DbgEventRing
{
public:
    NTSTATUS Init();
    void Deinit();

    // any IRQL; drops the event when the ring is full (LostCount++)
    void Push(const SVMB_DBG_EVENT& e);
    // copies up to the buffer capacity; returns drained count
    u32 Drain(SVMB_DBG_EVENT_BUFFER* out);

    u32 LostCount() const;
    u32 Count() const;

private:
    SVMB_DBG_EVENT Slots_[SVMB_DBG_MAX_EVENTS];
    KSPIN_LOCK Lock_;
    u32 Head_ = 0;  // next read index
    u32 Count_ = 0; // occupied slots
    u32 Lost_ = 0;
};

// global ring instance (defined in main.cpp) - the debugger module pushes
// events here; will migrate into the SvmbApi table as the module framework
// matures
DbgEventRing* DbgRingInstance();

} // namespace svmb

#endif // SVMB_DBG_EVENTS_H
