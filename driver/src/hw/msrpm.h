// svmb - MSR permission map (MSRPM): 8KB contiguous, refcounted MSR interception
// and IO permission map (IOPM): 16KB contiguous, refcounted port interception
#ifndef SVMB_MSRPM_H
#define SVMB_MSRPM_H

#include "platform/base.h"
#include "platform/util.h"

namespace svmb
{

class Msrpm
{
public:
    static constexpr SIZE_T SIZE = 2 * PAGE_SIZE; // hardware required size

    NTSTATUS Init();  // allocates THREE buffers: active + shadow + spare
    void     Deinit();

    // triple-buffered updates (closes the review finding P2-4 clear window):
    // BeginBuild clears and targets the shadow page; SetIntercept writes
    // there; CommitBuild rotates the roles (shadow -> active, active ->
    // spare, spare -> shadow) and returns the new active PA for publication
    // into every VMCB.
    //
    // Staleness bound: the page BeginBuild zeroes retired at commit K and is
    // only cleared at BeginBuild(K+2) - spare@K+1, shadow@K+2 - so a laggard
    // core holding a retired PA gets hit only after >=3 back-to-back builds
    // (the old two-page swap was >=2), and each commit's publication into
    // all VMCBs completes synchronously before the cycle ends (see
    // InterceptManager::ApplyAll). Closing even that window needs a
    // core-quiesce barrier (live-NPT prerequisites register).
    void BeginBuild();
    u64  CommitBuild();

    // direct writes target the page selected by BeginBuild/CommitBuild state
    // (the active page while no build is in progress)
    bool SetIntercept(u32 msr, bool read, bool write, bool set);
    bool GetIntercept(u32 msr, bool& read, bool& write) const;

    u64      PhysicalAddress() const { return Pas_[ActiveIdx_]; }

private:
    static bool MsrToBitOffset(u32 msr, u32& byteOff, u8& bitOff);
    void* Va_[3] = {};        // role-indexed pages
    u64   Pas_[3] = {};
    u32   ActiveIdx_ = 0;
    u32   ShadowIdx_ = 1;
    u32   SpareIdx_ = 2;
    void* Target_ = nullptr;  // where SetIntercept currently writes
};

class Iopm
{
public:
    static constexpr SIZE_T SIZE = 4 * PAGE_SIZE; // 8KB read map + 8KB write map

    NTSTATUS Init();
    void Deinit();
    u64 PhysicalAddress() const { return Pa_; }

    // refcounted port interception, mirroring the InterceptManager bit
    // semantics: RequirePort increments a per-direction count and sets the
    // IOPM bit on the 0->1 transition; ReleasePort decrements and clears
    // the bit on 1->0. Over-releasing a not-required direction returns
    // STATUS_NOT_FOUND instead of under-clearing a bit someone else needs.
    NTSTATUS RequirePort(u32 port, bool read, bool write);
    NTSTATUS ReleasePort(u32 port, bool read, bool write);

    // diagnostic read of the current IOPM bit state
    bool GetPortIntercept(u32 port, bool& read, bool& write) const;

private:
    struct PortRefs : HashNode
    {
        PortRefs* NextAll;
        u32 R;
        u32 W;
    };

    PortRefs* FindRefs(u32 port) const;

    void* Va_ = nullptr;
    u64 Pa_ = 0;
    HashTable Ports_;   // key = port number -> PortRefs
    PortRefs* List_ = nullptr; // all live nodes (Deinit walk)
    KSPIN_LOCK Lock_;   // require/release are read-modify-write sequences
};

} // namespace svmb

#endif // SVMB_MSRPM_H
