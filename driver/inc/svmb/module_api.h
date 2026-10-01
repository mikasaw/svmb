// svmb - PUBLIC MODULE API. This is the only header a module author includes.
// A module is a set of handler callbacks registered through the SvmbApi
// function table; the core never references module code, and modules never
// touch core internals. See docs/MODULE_GUIDE.md for a walkthrough.
// NOTE: deliberately does NOT include core/hypervisor.h (circular dependency);
// everything a module needs is reachable from exit_dispatcher.h/hypercall.h.
#ifndef SVMB_MODULE_API_H
#define SVMB_MODULE_API_H

#include "core/exit_dispatcher.h"
#include "core/hypercall.h"
#include "modules/npt_hook_mgr.h"
#include "svmb_protocol.h"

namespace svmb
{

// function table handed to a module at attach time. `api->Token` identifies
// this module instance in every call (used for handler/require bookkeeping).
struct SvmbApi
{
    u32 Size;        // sizeof(SvmbApi), version guard
    ModuleToken Token;

    // ---- exit handlers ----
    // higher priority runs first; returning true stops the chain
    NTSTATUS (*RegisterExitHandler)(const SvmbApi* api, u64 exitCode,
                                    ExitHandlerFn fn, void* userData, u32 priority);

    // ---- interception requirements (refcounted, auto-released on detach) ----
    NTSTATUS (*RequireCr)(const SvmbApi* api, u32 cr, bool read, bool write);
    NTSTATUS (*ReleaseCr)(const SvmbApi* api, u32 cr, bool read, bool write);
    NTSTATUS (*RequireDr)(const SvmbApi* api, u32 dr, bool read, bool write);
    NTSTATUS (*ReleaseDr)(const SvmbApi* api, u32 dr, bool read, bool write);
    NTSTATUS (*RequireException)(const SvmbApi* api, u32 vector);
    NTSTATUS (*ReleaseException)(const SvmbApi* api, u32 vector);
    NTSTATUS (*RequireOpcode)(const SvmbApi* api, u64 exitReason);
    NTSTATUS (*ReleaseOpcode)(const SvmbApi* api, u64 exitReason);
    NTSTATUS (*RequireMsr)(const SvmbApi* api, u32 msr, bool read, bool write);
    NTSTATUS (*ReleaseMsr)(const SvmbApi* api, u32 msr, bool read, bool write);
    NTSTATUS (*RequireMtf)(const SvmbApi* api);
    NTSTATUS (*ReleaseMtf)(const SvmbApi* api);

    // IRQL NOTE for all of the above: an exit (and thus your handler) fires at
    // whatever IRQL the guest was running when it touched the intercepted
    // resource - including inside interrupts. If you intercept something the
    // kernel touches at DISPATCH_LEVEL+ (hot MSRs like GS_BASE, CR access on
    // context switch), the handler must be lock-free and non-pageable: no
    // spinlock contention-dependent code paths, no logging on the hot path.

    // ---- event injection into the guest ----
    void (*InjectException)(GuestContext* ctx, u32 vector);
    void (*InjectEvent)(GuestContext* ctx, u32 vector, u32 type,
                        bool errorCodeValid, u32 errorCode);
    void (*ReinjectExitEvent)(GuestContext* ctx);

    // ---- hypercalls (guest calls these with vmmcall rax=nr) ----
    NTSTATUS (*RegisterHypercall)(const SvmbApi* api, u32 nr, HypercallFn fn, void* userData);

    // ---- logging (goes to the kernel ring buffer + DbgPrint for warn/error) ----
    void (*LogError)(const char* fmt, ...);
    void (*LogWarn)(const char* fmt, ...);
    void (*LogInfo)(const char* fmt, ...);

    // ---- NPT execution hooks (r99) ----
    // passive hook with a managed callback (see NptHookManager::
    // InstallCallback): callback 0 = passthrough, nonzero = override with
    // that value as the function result. Install/Remove must run at
    // PASSIVE_LEVEL (module Init/Stop qualify - both hold the IOCTL gate).
    NTSTATUS (*InstallHookCb)(const SvmbApi* api, u64 targetVa,
                              u32& hookIdOut, SvmbHookCallback cb, u8 mode);
    NTSTATUS (*RemoveHook)(const SvmbApi* api, u64 targetVa);

    // ---- introspection ----
    u32 (*CoreCount)(const SvmbApi* api);
    u32 (*CurrentCore)(const SvmbApi* api);
    bool (*IsRunning)(const SvmbApi* api);
    VcpuContext* (*Vcpu)(const SvmbApi* api, u32 coreIdx); // advanced use
    u64 (*VmcbPa)(const SvmbApi* api, u32 coreIdx);
};

// module descriptor - define one per module TU and add it to the registry
// in src/core/module.cpp (see gModuleRegistry).
struct SvmbModule
{
    const char* Name;             // unique, used by svmbctl mod attach/detach
    NTSTATUS (*Init)(const SvmbApi* api);  // attach: register everything here
    void (*Stop)();               // detach: release module-owned resources
    u32 Priority;                 // attach ordering hint
};

} // namespace svmb

// convenience: defines the module descriptor symbol.
// extern keeps external linkage (const at namespace scope is internal by default)
#define SVMB_DEFINE_MODULE(varName, nameStr, priorityVal, initFn, stopFn) \
    extern const svmb::SvmbModule varName = { nameStr, initFn, stopFn, priorityVal };

#endif // SVMB_MODULE_API_H
