// svmb - module registry and lifecycle.
// Modules are listed in gModuleRegistry below; adding a module = write its TU
// + add one entry here. Attach allocates a token and calls Init with a bound
// SvmbApi; Detach stops the module and revokes every resource it registered.
#ifndef SVMB_MODULE_H
#define SVMB_MODULE_H

#include "svmb/module_api.h"

namespace svmb
{

class Hypervisor;

class ModuleManager
{
public:
    static constexpr u32 MAX_SLOTS = 16;

    // CONCURRENCY: Attach/Detach/DetachAll are NOT internally locked - they
    // are serialized by the IOCTL-layer fast mutex (driver/src/main.cpp),
    // which also guards the hypervisor lifecycle against DriverUnload.
    struct Slot
    {
        const SvmbModule* Module;
        ModuleToken Token;   // 0 = detached
    };

    NTSTATUS Init(Hypervisor* hv);
    void Deinit(); // detaches everything (hypervisor teardown path)

    // PASSIVE_LEVEL only
    NTSTATUS Attach(const char* name);
    NTSTATUS Detach(const char* name);
    void DetachAll();

    u32 AttachedCount() const;
    // fills up to maxEntries; returns total registered modules
    u32 Describe(SVMB_MODULE_INFO* out, u32 maxEntries);

    ModuleToken TokenOf(const char* name) const;

private:
    Slot Slots_[MAX_SLOTS];
    Hypervisor* Hv_ = nullptr;
    volatile LONG NextToken_ = 1;
    KSPIN_LOCK Lock_;

    const SvmbModule* FindInRegistry(const char* name) const;
    SvmbApi BuildApi(ModuleToken token);
};

} // namespace svmb

#endif // SVMB_MODULE_H
