// svmb - r107 freeze flight recorder (see crumbs.h). Test builds only:
// the ring and all CrumbPost sites are compiled out under SVMB_PRODUCTION.
#include "core/crumbs.h"
#include "platform/util.h"

#ifndef SVMB_PRODUCTION

namespace svmb
{

namespace
{
CrumbRing* g_Ring = nullptr;
}

CrumbRing* CrumbInstance()
{
    return g_Ring;
}

void CrumbInit()
{
    if (!g_Ring)
    {
        g_Ring = static_cast<CrumbRing*>(
            svmb::AllocNonPaged(sizeof(CrumbRing), 'BRCV'));
        if (g_Ring)
        {
            // AllocNonPaged is NOT zeroed - W/Dropped/Seq from garbage
            // pool memory made the sequence numbers nonsense (r107
            // first-live-readout bug)
            RtlZeroMemory(g_Ring, sizeof(CrumbRing));
            g_Ring->Magic0 = CRUMBS_MAGIC0;
            g_Ring->Magic1 = CRUMBS_MAGIC1;
        }
    }
}

void CrumbPost(LONG tag)
{
    CrumbRing* r = g_Ring;
    if (!r || r->Magic0 != CRUMBS_MAGIC0)
        return;
    const LONG seq = InterlockedIncrement(&r->W) - 1;
    Crumb* c = &r->E[(ULONG)seq & (CRUMBS_MAX - 1)];
    c->Tag = tag;
    c->Cpu = (LONG)KeGetCurrentProcessorNumber();
    c->Tick = (LONG64)KeQueryInterruptTime();
    c->Seq = (LONG64)seq + 1; // commit last (order key for the decoder)
}

} // namespace svmb

#else

namespace svmb
{
CrumbRing* CrumbInstance()
{
    return nullptr;
}
void CrumbInit()
{
}
void CrumbPost(LONG)
{
}
} // namespace svmb

#endif // SVMB_PRODUCTION
