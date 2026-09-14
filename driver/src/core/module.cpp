#include "core/module.h"
#include "core/hypervisor.h"
#include "platform/logger.h"
#include "platform/util.h"

// ---- module registry -------------------------------------------------------
// To add a module: implement its TU under src/modules/, then add one extern
// declaration + one entry here. That is the entire integration cost.
// main.cpp holds the tier-1 subsystem pointers inside an anonymous
// namespace - route through the exported accessor instead.
static svmb::NptHookManager* GlobalHooks()
{
    return svmb::SysAuditGlobalHooks();
}

namespace svmb
{
extern const SvmbModule ModuleDemoCpuid;   // modules/demo_cpuid.cpp
extern const SvmbModule ModuleDebugger;    // modules/debugger.cpp
extern const SvmbModule ModuleCr3Monitor;  // modules/cr3_monitor.cpp
extern const SvmbModule ModuleStealth;     // modules/stealth.cpp
// ModuleSysAudit (modules/sysaudit.cpp, r100): EXECUTE-SENSE on
// MmCopyVirtualMemory - zero guest bytes modified, PatchGuard-invisible
// by construction (the r99 inline-hook variant was PatchGuard-fatal and
// is unregistered; see tests/CRASH_DEBUG_LOG.md Round 99).
extern const SvmbModule ModuleSysAudit;    // modules/sysaudit.cpp

static const SvmbModule* const gModuleRegistry[] = {
    &ModuleDemoCpuid,
    &ModuleDebugger,
    &ModuleCr3Monitor,
    &ModuleStealth,
    &ModuleSysAudit,
};
}

namespace svmb
{

// ---- SvmbApi wrappers (route through the hypervisor singleton) ----

namespace
{

NTSTATUS Api_RegisterExit(const SvmbApi* api, u64 exitCode, ExitHandlerFn fn,
                          void* userData, u32 priority)
{
    return Hypervisor::Instance()->Dispatcher().Register(exitCode, fn, userData, api->Token, priority);
}

NTSTATUS Api_RequireCr(const SvmbApi* api, u32 cr, bool r, bool w)
{
    return Hypervisor::Instance()->Intercepts().RequireCr(api->Token, cr, r, w);
}
NTSTATUS Api_ReleaseCr(const SvmbApi* api, u32 cr, bool r, bool w)
{
    return Hypervisor::Instance()->Intercepts().ReleaseCr(api->Token, cr, r, w);
}
NTSTATUS Api_RequireDr(const SvmbApi* api, u32 dr, bool r, bool w)
{
    return Hypervisor::Instance()->Intercepts().RequireDr(api->Token, dr, r, w);
}
NTSTATUS Api_ReleaseDr(const SvmbApi* api, u32 dr, bool r, bool w)
{
    return Hypervisor::Instance()->Intercepts().ReleaseDr(api->Token, dr, r, w);
}
NTSTATUS Api_RequireException(const SvmbApi* api, u32 v)
{
    return Hypervisor::Instance()->Intercepts().RequireException(api->Token, v);
}
NTSTATUS Api_ReleaseException(const SvmbApi* api, u32 v)
{
    return Hypervisor::Instance()->Intercepts().ReleaseException(api->Token, v);
}
NTSTATUS Api_RequireOpcode(const SvmbApi* api, u64 exitReason)
{
    return Hypervisor::Instance()->Intercepts().RequireOpcode(api->Token, exitReason);
}
NTSTATUS Api_ReleaseOpcode(const SvmbApi* api, u64 exitReason)
{
    return Hypervisor::Instance()->Intercepts().ReleaseOpcode(api->Token, exitReason);
}
NTSTATUS Api_RequireMsr(const SvmbApi* api, u32 msr, bool r, bool w)
{
    return Hypervisor::Instance()->Intercepts().RequireMsr(api->Token, msr, r, w);
}
NTSTATUS Api_ReleaseMsr(const SvmbApi* api, u32 msr, bool r, bool w)
{
    return Hypervisor::Instance()->Intercepts().ReleaseMsr(api->Token, msr, r, w);
}
NTSTATUS Api_RequireMtf(const SvmbApi* api)
{
    return Hypervisor::Instance()->Intercepts().RequireMtf(api->Token);
}
NTSTATUS Api_ReleaseMtf(const SvmbApi* api)
{
    return Hypervisor::Instance()->Intercepts().ReleaseMtf(api->Token);
}

void Api_InjectException(GuestContext* ctx, u32 vector)
{
    InjectEvent(*ctx, vector, EVT_EXCEPTION, false, 0);
}
void Api_InjectEvent(GuestContext* ctx, u32 vector, u32 type, bool ecValid, u32 ec)
{
    InjectEvent(*ctx, vector, type, ecValid, ec);
}
void Api_ReinjectExitEvent(GuestContext* ctx)
{
    ReinfectExitEvent(*ctx);
}

NTSTATUS Api_RegisterHypercall(const SvmbApi* api, u32 nr, HypercallFn fn, void* ud)
{
    return Hypervisor::Instance()->Hypercalls().Register(nr, fn, ud, api->Token);
}

// r99: hook access for modules. gHooks lives in main.cpp, allocated at
// DriverEntry - before any module attach can run.
NTSTATUS Api_InstallHookCb(const SvmbApi* api, u64 targetVa, u32& hookIdOut,
                           SvmbHookCallback cb, u8 mode)
{
    NptHookManager* h = GlobalHooks();
    if (!h)
        return STATUS_DEVICE_NOT_READY;
    return h->InstallCallback(targetVa, hookIdOut, cb, mode);
}

NTSTATUS Api_RemoveHook(const SvmbApi* api, u64 targetVa)
{
    NptHookManager* h = GlobalHooks();
    if (!h)
        return STATUS_DEVICE_NOT_READY;
    return h->Remove(targetVa);
}

void Api_LogError(const char* fmt, ...) { va_list a; va_start(a, fmt); char b[192]; RtlStringCbVPrintfA(b, sizeof(b), fmt, a); va_end(a); LogWrite(LogLevel::Error, "%s", b); }
void Api_LogWarn(const char* fmt, ...) { va_list a; va_start(a, fmt); char b[192]; RtlStringCbVPrintfA(b, sizeof(b), fmt, a); va_end(a); LogWrite(LogLevel::Warn, "%s", b); }
void Api_LogInfo(const char* fmt, ...) { va_list a; va_start(a, fmt); char b[192]; RtlStringCbVPrintfA(b, sizeof(b), fmt, a); va_end(a); LogWrite(LogLevel::Info, "%s", b); }

u32 Api_CoreCount(const SvmbApi*) { return Hypervisor::Instance()->CoreCount(); }
u32 Api_CurrentCore(const SvmbApi*) { return CurrentCpuIndex(); }
bool Api_IsRunning(const SvmbApi*) { return Hypervisor::Instance()->IsRunning(); }
VcpuContext* Api_Vcpu(const SvmbApi*, u32 idx) { return Hypervisor::Instance()->Vcpu(idx); }
u64 Api_VmcbPa(const SvmbApi*, u32 idx)
{
    VcpuContext* v = Hypervisor::Instance()->Vcpu(idx);
    return v ? v->Info.GuestVmcbPa : 0;
}

} // namespace

// ---- ModuleManager ----

NTSTATUS ModuleManager::Init(Hypervisor* hv)
{
    Hv_ = hv;
    KeInitializeSpinLock(&Lock_);
    RtlZeroMemory(Slots_, sizeof(Slots_));
    return STATUS_SUCCESS;
}

SvmbApi ModuleManager::BuildApi(ModuleToken token)
{
    SvmbApi api = {};
    api.Size = sizeof(api);
    api.Token = token;
    api.RegisterExitHandler = Api_RegisterExit;
    api.RequireCr = Api_RequireCr;
    api.ReleaseCr = Api_ReleaseCr;
    api.RequireDr = Api_RequireDr;
    api.ReleaseDr = Api_ReleaseDr;
    api.RequireException = Api_RequireException;
    api.ReleaseException = Api_ReleaseException;
    api.RequireOpcode = Api_RequireOpcode;
    api.ReleaseOpcode = Api_ReleaseOpcode;
    api.RequireMsr = Api_RequireMsr;
    api.ReleaseMsr = Api_ReleaseMsr;
    api.RequireMtf = Api_RequireMtf;
    api.ReleaseMtf = Api_ReleaseMtf;
    api.InjectException = Api_InjectException;
    api.InjectEvent = Api_InjectEvent;
    api.ReinjectExitEvent = Api_ReinjectExitEvent;
    api.RegisterHypercall = Api_RegisterHypercall;
    api.InstallHookCb = Api_InstallHookCb;
    api.RemoveHook = Api_RemoveHook;
    api.LogError = Api_LogError;
    api.LogWarn = Api_LogWarn;
    api.LogInfo = Api_LogInfo;
    api.CoreCount = Api_CoreCount;
    api.CurrentCore = Api_CurrentCore;
    api.IsRunning = Api_IsRunning;
    api.Vcpu = Api_Vcpu;
    api.VmcbPa = Api_VmcbPa;
    return api;
}

const SvmbModule* ModuleManager::FindInRegistry(const char* name) const
{
    for (const SvmbModule* m : gModuleRegistry)
    {
        if (m && _strnicmp(m->Name, name, 15) == 0)
            return m;
    }
    return nullptr;
}

NTSTATUS ModuleManager::Attach(const char* name)
{
    if (!Hv_ || !Hv_->IsRunning())
        return STATUS_DEVICE_NOT_READY;

#ifdef SVMB_PRODUCTION
    // r96 audit ST1: the CPUID disguise is an adversarial capability -
    // hiding hypervisor presence from guest EDR/attestation does not ship
    // in the production build (r85 P1-1 philosophy). Production boxes run
    // attestable.
    if (name && _strnicmp(name, "stealth", 7) == 0)
    {
        SVMB_LOGE("module %s refused - not available in production", name);
        return STATUS_NOT_SUPPORTED;
    }
#endif

    const SvmbModule* m = FindInRegistry(name);
    if (!m)
        return STATUS_NOT_FOUND;

    for (u32 i = 0; i < MAX_SLOTS; ++i)
        if (Slots_[i].Token && Slots_[i].Module == m)
            return STATUS_ALREADY_REGISTERED;

    LONG slotIdx = -1;
    for (u32 i = 0; i < MAX_SLOTS; ++i)
        if (!Slots_[i].Token) { slotIdx = (LONG)i; break; }
    if (slotIdx < 0)
        return STATUS_INSUFFICIENT_RESOURCES;

    ModuleToken token = (ModuleToken)InterlockedIncrement(&NextToken_) - 1;
    SvmbApi api = BuildApi(token);

    Slots_[slotIdx].Module = m;
    Slots_[slotIdx].Token = token;
    NTSTATUS status = m->Init(&api);
    if (!NT_SUCCESS(status))
    {
        Slots_[slotIdx].Module = nullptr;
        Slots_[slotIdx].Token = 0;
        // r38: revoke everything the half-initialized module registered -
        // the old path cleared only the slot, leaking exit handlers and
        // intercept requirements under the abandoned token (a later
        // Attach's arming could then be silently undone by a stale
        // ReleaseOwner pass folding to exc=0)
        if (Hv_)
        {
            Hv_->Dispatcher().UnregisterOwner(token);
            Hv_->Intercepts().ReleaseOwner(token);
            Hv_->Hypercalls().UnregisterOwner(token);
        }
        SVMB_LOGE("module %s init failed: %08x", name, status);
        return status;
    }
    SVMB_LOGI("module %s attached (token %u)", name, token);
    return STATUS_SUCCESS;
}

NTSTATUS ModuleManager::Detach(const char* name)
{
    const SvmbModule* m = FindInRegistry(name);
    if (!m)
        return STATUS_NOT_FOUND;

    for (u32 i = 0; i < MAX_SLOTS; ++i)
    {
        if (Slots_[i].Token && Slots_[i].Module == m)
        {
            ModuleToken token = Slots_[i].Token;
            m->Stop();
            // revoke anything the module forgot to release
            if (Hv_)
            {
                Hv_->Dispatcher().UnregisterOwner(token);
                Hv_->Intercepts().ReleaseOwner(token);
                Hv_->Hypercalls().UnregisterOwner(token);
            }
            Slots_[i].Module = nullptr;
            Slots_[i].Token = 0;
            SVMB_LOGI("module %s detached", name);
            return STATUS_SUCCESS;
        }
    }
    return STATUS_NOT_FOUND;
}

void ModuleManager::DetachAll()
{
    for (u32 i = 0; i < MAX_SLOTS; ++i)
    {
        if (Slots_[i].Token && Slots_[i].Module)
        {
            ModuleToken token = Slots_[i].Token;
            Slots_[i].Module->Stop();
            if (Hv_)
            {
                Hv_->Dispatcher().UnregisterOwner(token);
                Hv_->Intercepts().ReleaseOwner(token);
                Hv_->Hypercalls().UnregisterOwner(token);
            }
            SVMB_LOGI("module %s detached (teardown)", Slots_[i].Module->Name);
            Slots_[i].Module = nullptr;
            Slots_[i].Token = 0;
        }
    }
}

void ModuleManager::Deinit()
{
    DetachAll();
    Hv_ = nullptr;
}

u32 ModuleManager::AttachedCount() const
{
    u32 n = 0;
    for (u32 i = 0; i < MAX_SLOTS; ++i)
        if (Slots_[i].Token)
            ++n;
    return n;
}

u32 ModuleManager::Describe(SVMB_MODULE_INFO* out, u32 maxEntries)
{
    u32 total = 0;
    for (u32 i = 0; i < MAX_SLOTS; ++i)
    {
        if (!Slots_[i].Token)
            continue;
        if (total < maxEntries)
        {
            SVMB_MODULE_INFO* info = &out[total];
            RtlZeroMemory(info, sizeof(*info));
            size_t nameLen = strlen(Slots_[i].Module->Name);
            if (nameLen > sizeof(info->Name) - 1)
                nameLen = sizeof(info->Name) - 1;
            RtlCopyMemory(info->Name, Slots_[i].Module->Name, nameLen);
            info->Attached = 1;
        }
        ++total;
    }
    return total;
}

ModuleToken ModuleManager::TokenOf(const char* name) const
{
    const SvmbModule* m = FindInRegistry(name);
    if (!m)
        return 0;
    for (u32 i = 0; i < MAX_SLOTS; ++i)
        if (Slots_[i].Token && Slots_[i].Module == m)
            return Slots_[i].Token;
    return 0;
}

} // namespace svmb
