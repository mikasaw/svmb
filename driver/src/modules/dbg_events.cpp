#include "modules/dbg_events.h"

namespace svmb
{

NTSTATUS DbgEventRing::Init()
{
    KeInitializeSpinLock(&Lock_);
    RtlZeroMemory(Slots_, sizeof(Slots_));
    Head_ = 0;
    Count_ = 0;
    Lost_ = 0;
    return STATUS_SUCCESS;
}

void DbgEventRing::Deinit()
{
    Head_ = 0;
    Count_ = 0;
}

void DbgEventRing::Push(const SVMB_DBG_EVENT& e)
{
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    if (Count_ < SVMB_DBG_MAX_EVENTS)
    {
        u32 slot = (Head_ + Count_) % SVMB_DBG_MAX_EVENTS;
        Slots_[slot] = e;
        ++Count_;
    }
    else
    {
        ++Lost_; // drop-on-full: consumers must not see torn events
    }
    KeReleaseSpinLock(&Lock_, old);
}

u32 DbgEventRing::Drain(SVMB_DBG_EVENT_BUFFER* out)
{
    if (!out)
        return 0;
    KIRQL old;
    KeAcquireSpinLock(&Lock_, &old);
    u32 n = Count_ < SVMB_DBG_MAX_EVENTS ? Count_ : SVMB_DBG_MAX_EVENTS;
    for (u32 i = 0; i < n; ++i)
        out->Events[i] = Slots_[(Head_ + i) % SVMB_DBG_MAX_EVENTS];
    out->Count = n;
    out->LostCount = Lost_;
    Head_ = (Head_ + n) % SVMB_DBG_MAX_EVENTS;
    Count_ -= n;
    Lost_ = 0;
    KeReleaseSpinLock(&Lock_, old);
    return n;
}

u32 DbgEventRing::LostCount() const
{
    KIRQL old;
    KeAcquireSpinLock(const_cast<KSPIN_LOCK*>(&Lock_), &old);
    u32 lost = Lost_;
    KeReleaseSpinLock(const_cast<KSPIN_LOCK*>(&Lock_), old);
    return lost;
}

u32 DbgEventRing::Count() const
{
    KIRQL old;
    KeAcquireSpinLock(const_cast<KSPIN_LOCK*>(&Lock_), &old);
    u32 n = Count_;
    KeReleaseSpinLock(const_cast<KSPIN_LOCK*>(&Lock_), old);
    return n;
}

} // namespace svmb
