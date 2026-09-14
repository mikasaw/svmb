// svmb driver entry: device + IOCTL surface over the hypervisor core
#include "core/hypervisor.h"
#include "core/npt_probe.h"
#include "mm/offline_tests.h"
#include "mm/npf.h"
#include "modules/npt_hook_mgr.h"
#include "modules/sysaudit.h"
#include "mm/tlb.h"
#include "modules/cr3_seed.h"
#include "modules/dbg_events.h"
#include "modules/cr3_monitor.h"
#include "modules/mem_read.h"
#include "modules/debugger.h"
#include "platform/logger.h"
#include "platform/saferead.h"
#include "core/crumbs.h"
#include "platform/util.h"
#include "svmb_protocol.h"
#include <wdmsec.h>

using namespace svmb;

void* operator new(size_t size, POOL_TYPE pool, ULONG tag);

namespace
{

PDEVICE_OBJECT gDevice = nullptr;
// r89: Parameters\ReadVmEnable gate (deployment hardening - the test
// platform defaults the capability ON)
static volatile LONG g_readVmEnable = 1;
UNICODE_STRING gDosName;

// serializes every IOCTL against each other AND against DriverUnload. All
// control paths (start/stop/cycle/module/stress/info) touch the Hypervisor
// singleton, whose teardown frees the per-core contexts exit handlers on
// other threads would still dereference. LogRead also takes it so the log
// pool cannot vanish mid-drain. (STRESS holds it for the whole storm - that
// is fine, waiters just block until it finishes.)
// PASSIVE-level sleep lock (ntddk-only): flag + auto-reset event.
// FAST_MUTEX would raise to APC_LEVEL, but SELFTEST/Unload call
// PASSIVE-only APIs (MmGetPhysicalMemoryRanges,
// MmAllocateContiguousMemory) inside this critical section.
volatile LONG gIoGate = 0; // 0 = free, 1 = held
// r91 acceptance P1-1: pid holding the gate (0 = none). A plan-C- victim
// freezes INSIDE DevCtrlLocked holding it; if the operator taskkills the
// frozen process, IoReleaseGate would never run and every gated IOCTL
// (including unload) would wedge forever. The death notify calls
// IoGateOwnerDied, which force-releases - the gate is a mutex, a dead
// holder is by definition the sole holder.
static volatile LONG gGateOwnerPid = 0;
KEVENT gIoGateFree;
// tier-1 offline subsystems (driver-load lifetime, independent of the
// hypervisor start/stop cycle). Pool-backed pointers, NOT global objects:
// non-trivial destructors on globals register atexit, which does not exist
// in kernel mode.
svmb::NptManager* gNpt = nullptr;

svmb::NptHookManager* gHooks = nullptr;
svmb::Cr3Seed* gCr3Seed = nullptr;
svmb::DbgEventRing* gDbgRing = nullptr;

// device class GUID for IoCreateDeviceSecure (arbitrary, unique to svmb)
const GUID SVMB_DEVCLASS = {0x2f4e6a1c, 0x9d3b, 0x4c58,
                            {0xa1, 0x7e, 0x63, 0xd2, 0x08, 0xb5, 0xf4, 0x9a}};

// CreateFile/CloseHandle require MJ_CREATE/MJ_CLOSE to succeed, otherwise
// the I/O manager completes them with STATUS_INVALID_DEVICE_REQUEST (Win32 err 1)
NTSTATUS DevCreateClose(PDEVICE_OBJECT, PIRP irp)
{
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = 0;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS DevCtrlLocked(PIRP irp)
{
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;
    void* buf = irp->AssociatedIrp.SystemBuffer;
    ULONG inLen = io->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = io->Parameters.DeviceIoControl.OutputBufferLength;

    Hypervisor* hv = Hypervisor::Instance();

    switch (io->Parameters.DeviceIoControl.IoControlCode)
    {
    case SVMB_IOCTL_RDTSC:
    {
        // r92 TSC probe readback: L1-context tick counter, immune to the
        // guest VMCB's TscOffset (see hypervisor.h) - the raw-TSC side of
        // the honor measurement against user-mode RDTSC.
        if (outLen < sizeof(u64))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        *(u64*)buf = __rdtsc();
        info = sizeof(u64);
        status = STATUS_SUCCESS;
        break;
    }
    case SVMB_IOCTL_GET_INFO:
    {
        if (outLen < sizeof(SVMB_INFO))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_INFO* inf = (SVMB_INFO*)buf;
        RtlZeroMemory(inf, sizeof(*inf));
        inf->Magic = SVMB_PROTO_MAGIC;
        inf->ProtoVersion = SVMB_PROTO_VERSION;
        inf->DriverVersion = SVMB_DRIVER_VERSION;
        inf->VmxHvState = hv && hv->IsRunning() ? SvmbHvStateRunning : SvmbHvStateOff;
        inf->CoreCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
        inf->CoresVirtualized = 0;
        if (hv)
        {
            for (u32 i = 0; i < hv->CoreCount(); ++i)
                if (VcpuContext* v = hv->Vcpu(i))
                    if (v->Info.State == (LONG)VcpuState::Guest)
                        ++inf->CoresVirtualized;
            inf->ExitCounter = hv->TotalExitCount();
        }
        inf->PaWidthBits = QueryPhysAddrWidth();
        inf->ModuleCount = hv ? hv->Modules().AttachedCount() : 0;
        info = sizeof(*inf);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_EXITPROF:
    {
        // r45: per-cause exit census. Read-only snapshot; sums the per-core
        // histograms (each core writes only its own slots, so no locking).
        if (outLen < sizeof(SVMB_EXITPROF))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_EXITPROF* ep = (SVMB_EXITPROF*)buf;
        RtlZeroMemory(ep, sizeof(*ep));
        ep->Version = SVMB_PROTO_VERSION;
        ep->CpuCount = hv ? hv->CoreCount() : 0;
        if (hv)
        {
            for (u32 i = 0; i < hv->CoreCount(); ++i)
            {
                if (VcpuContext* v = hv->Vcpu(i))
                {
                    for (u32 s = 0; s < SVMB_XP_SLOTS; ++s)
                        ep->Hist[s] += v->Info.ExitHist[s];
                }
            }
            u64 total = 0;
            for (u32 s = 0; s < SVMB_XP_SLOTS; ++s)
                total += ep->Hist[s];
            ep->Total = total;
        }
        info = sizeof(*ep);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_HV_CONTROL:
    {
        if (inLen < sizeof(SVMB_HV_CONTROL))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_HV_CONTROL* c = (SVMB_HV_CONTROL*)buf;
        SVMB_LOGI("ioctl HV_CONTROL action=%u repeat=%u", c->Action, c->RepeatCount);
#ifdef SVMB_PRODUCTION
        // r85 audit P2-6: IOCTL start is the round-27 known-fatal entry
        // context (load-time entry is the only supported mode), and the
        // cycle loop leaks vCPU pages per round by design
        if (c->Action == 0 || c->Action == 2)
        {
            SVMB_LOGE("HV_CONTROL action=%u refused in production", c->Action);
            status = STATUS_NOT_SUPPORTED;
            break;
        }
#endif
        if (c->Action == 0) // start
        {
            if (hv && hv->IsRunning())
                status = STATUS_ALREADY_REGISTERED;
            else
            {
                Hypervisor* h = new (NonPagedPoolNx, TAG_SVMB) Hypervisor();
                if (!h)
                    status = STATUS_INSUFFICIENT_RESOURCES;
                else
                {
                    // Start() rolls itself back internally on failure, so the
                    // object can be freed right away
                    status = h->Start();
                    if (!NT_SUCCESS(status))
                        delete h;
                }
            }
        }
        else if (c->Action == 1) // stop
        {
            if (!hv)
                status = STATUS_NOT_FOUND;
            else
            {
                status = hv->Stop();
                // Stop() refuses (and keeps everything allocated) when a core
                // could not be devirtualized - deleting here would be UAF
                if (NT_SUCCESS(status))
                    delete hv;
                else
                    SVMB_LOGE("HV_CONTROL stop failed (%08x), hypervisor kept for retry",
                              status);
            }
        }
        else if (c->Action == 2) // enter/exit stress loop
        {
            if (!hv || !hv->IsRunning())
            {
                status = STATUS_NOT_FOUND;
            }
            else
            {
                // snapshot attached modules: Stop() detaches them all (their
                // resources die with the hypervisor), so re-attach after the
                // final Start() to keep `cycle` a pure enter/exit stress
                SVMB_MODULE_LIST before = {};
                hv->Modules().Describe(before.Modules, SVMB_MAX_MODULES);
                u32 attached = before.Count < SVMB_MAX_MODULES ? before.Count : SVMB_MAX_MODULES;
                char names[SVMB_MAX_MODULES][16] = {};
                for (u32 m = 0; m < attached; ++m)
                    RtlCopyMemory(names[m], before.Modules[m].Name,
                                   sizeof(names[m]) - 1);

                // r85 audit P2-5: each cycle round leaks vCPU pages by design -
                // unbounded repeat would drain NonPaged pool
                for (u32 r = 0; r < c->RepeatCount && r < 16 && NT_SUCCESS(status); ++r)
                {
                    status = hv->Stop();
                    if (!NT_SUCCESS(status))
                        break;
                    status = hv->Start();
                }

                if (NT_SUCCESS(status))
                {
                    for (u32 m = 0; m < attached; ++m)
                    {
                        NTSTATUS st = hv->Modules().Attach(names[m]);
                        if (!NT_SUCCESS(st))
                            SVMB_LOGW("cycle: re-attach %s failed: %08x", names[m], st);
                    }
                }
            }
        }
        else if (c->Action == 3) // serial log mirror on/off (live switch)
        {
            LogSetSerial(c->RepeatCount != 0);
            status = STATUS_SUCCESS;
        }
        break;
    }

    case SVMB_IOCTL_LOG_READ:
    {
        if (outLen < sizeof(SVMB_LOG_ENTRY))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        u32 count = outLen / sizeof(SVMB_LOG_ENTRY);
        u32 lost = 0;
        info = (ULONG_PTR)LogDrain((SVMB_LOG_ENTRY*)buf, count, &lost) * sizeof(SVMB_LOG_ENTRY);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_MODULE_CTL:
    {
        if (inLen < sizeof(SVMB_MODULE_CTL))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_MODULE_CTL* c = (SVMB_MODULE_CTL*)buf;
        c->Name[sizeof(c->Name) - 1] = '\0';
        SVMB_LOGI("ioctl MODULE_CTL action=%u name=%s", c->Action, c->Name);
        if (!hv)
        {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        if (c->Action == 0)
            status = hv->Modules().Attach(c->Name);
        else if (c->Action == 1)
            status = hv->Modules().Detach(c->Name);
        else if (c->Action == 2)
        {
            if (outLen < sizeof(SVMB_MODULE_LIST))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            SVMB_MODULE_LIST* list = (SVMB_MODULE_LIST*)buf;
            RtlZeroMemory(list, sizeof(*list));
            list->Count = hv->Modules().Describe(list->Modules, SVMB_MAX_MODULES);
            info = sizeof(*list);
            status = STATUS_SUCCESS;
        }
        else
            status = STATUS_INVALID_PARAMETER;
        break;
    }

    case SVMB_IOCTL_SELFTEST:
    {
        if (outLen < sizeof(SVMB_SELFTEST_RESULT))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_SELFTEST_RESULT* res = (SVMB_SELFTEST_RESULT*)buf;
        RtlZeroMemory(res, sizeof(*res));
        RunOfflineSelfTests(res);
        info = sizeof(*res);
        // always SUCCESS: the caller reads res.Failed / LastFail (an
        // error status would hide the details behind err %lu)
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_CR3_CONFIG:
    {
        // config lives in the cr3_monitor module (image-name arming, spoof
        // policy) - applied even while the module is detached
        if (inLen < sizeof(SVMB_CR3_CONFIG))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        Cr3MonitorConfigure((SVMB_CR3_CONFIG*)buf);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_CR3_STATS:
    {
        if (outLen < sizeof(SVMB_CR3_STATS))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_CR3_STATS* st = (SVMB_CR3_STATS*)buf;
        RtlZeroMemory(st, sizeof(*st));
        Cr3MonitorStats(st); // exit counters + seed table size
        info = sizeof(*st);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_CR3_POISON:
    {
        // r71 test hook: simulate frame reuse by corrupting one armed
        // sentinel watermark - fires the kill-switch without a kd session
        u32* n = (u32*)buf;
        if (outLen < sizeof(*n))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        *n = Cr3MonitorPoison();
        info = sizeof(*n);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_CR3_PROT:
    {
        // r75 protected regions: L1 blocking (guest MM) + L2 NPT sensing
        if (outLen < sizeof(SVMB_CR3_PROT) || inLen < sizeof(SVMB_CR3_PROT))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        u32 n = Cr3RegionAction((SVMB_CR3_PROT*)buf);
        if (!n && ((SVMB_CR3_PROT*)buf)->Action == SVMB_PROT_ACTION_LIST)
        {
            // list with zero live regions is still a success
            status = STATUS_SUCCESS;
        }
        else
        {
            status = n ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        }
        info = sizeof(SVMB_CR3_PROT);
        break;
    }

    case SVMB_IOCTL_CR3_PROBE:
    {
        // r76 L2-bypass test probe: kernel MDL write into a protected
        // region - must trip the NPT sensing (RegionTrips) while the L1
        // guest-MM block stays intact
        if (outLen < sizeof(SVMB_CR3_PROBE) || inLen < sizeof(SVMB_CR3_PROBE))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        u32 n = Cr3RegionProbe((SVMB_CR3_PROBE*)buf);
        status = n ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        info = sizeof(SVMB_CR3_PROBE);
        break;
    }

    case SVMB_IOCTL_CR3_READVM:
    {
        // r87 hypervisor-level process memory read: payload follows the
        // header in the same buffered buffer. NOT in the production-refused
        // list - this is a capability, not a test weapon, and the device
        // SDDL (admin-only) is its gate (docs/SECURITY_AUDIT.md).
        // r89: Parameters\ReadVmEnable=0 refuses here (STATUS_NOT_SUPPORTED)
        if (!InterlockedCompareExchange(&g_readVmEnable, 0, 0))
        {
            SVMB_LOGW("ioctl %x refused - ReadVmEnable=0",
                      io->Parameters.DeviceIoControl.IoControlCode);
            status = STATUS_NOT_SUPPORTED;
            break;
        }
        if (inLen < sizeof(SVMB_CR3_READVM)
            || outLen < sizeof(SVMB_CR3_READVM))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_CR3_READVM* rv = (SVMB_CR3_READVM*)buf;
        if (rv->Size == 0 || rv->Size > SVMB_READVM_MAX
            || outLen - sizeof(*rv) < rv->Size)
        {
            status = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        u32 n = MemReadVm(rv, outLen);
        // r89 (kd-caught): the completion copy / ANY kernel-mode touch of a
        // demand-zero user page spins forever on this vhv (KiPageFault loop
        // - MiUserFault never resolves, SEH cannot intercept, r83-law
        // sibling). The caller (svmbctl) memsets its buffer in USER mode
        // before the IOCTL. r96: DevCtrl probes the output buffer before
        // the walk, so non-writable buffers are refused (STATUS_INVALID_
        // USER_BUFFER) before we ever get here - the caller-cooperation
        // precondition is gone.
        status = n ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        info = n;
        break;
    }

    case SVMB_IOCTL_SYSCALL_AUDIT:
    {
        // r99 sysaudit drain (consume, like LOG_READ). The ring lock is
        // independent of gIoGate - gated dispatch adds no deadlock risk.
        if (outLen < sizeof(SVMB_SYSCALL_AUDIT))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SysAuditDrain((SVMB_SYSCALL_AUDIT*)buf);
        info = sizeof(SVMB_SYSCALL_AUDIT);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_KDUMP:
    {
        // r101 diagnostic: safe-read a kernel-VA qword window (the SSDT
        // table / candidate-region inspector). In and out share the
        // buffered buffer; a failed read stops the walk at FailIdx.
        if (inLen < sizeof(SVMB_KDUMP) || outLen < sizeof(SVMB_KDUMP))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_KDUMP* k = (SVMB_KDUMP*)buf;
        const u32 n = (k->Qwords > SVMB_KDUMP_MAX_QWORDS)
                          ? SVMB_KDUMP_MAX_QWORDS
                          : k->Qwords;
        u32 i = 0;
        for (; i < n; ++i)
        {
            u64 v = 0;
            if (!svmb::SafRead64(k->Va + 8ull * i, &v))
                break;
            k->Q[i] = v;
        }
        k->FailIdx = i;
        for (u32 j = i; j < SVMB_KDUMP_MAX_QWORDS; ++j)
            k->Q[j] = SVMB_KDUMP_BAD;
        info = sizeof(SVMB_KDUMP);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_CRUMBS:
    {
        // r107 freeze flight recorder readout (raw ring mirror; the
        // decoder orders by Seq). Production builds have no ring -
        // OutCount=0 with the magic zeroed tells the caller as much.
        if (outLen < sizeof(SVMB_CRUMBS))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        static_assert(SVMB_CRUMBS_MAX == svmb::CRUMBS_MAX,
                      "crumbs ring size drift");
        SVMB_CRUMBS* c = (SVMB_CRUMBS*)buf;
        RtlZeroMemory(c, sizeof(*c));
        const svmb::CrumbRing* r = svmb::CrumbInstance();
        if (r)
        {
            c->Magic0 = (svmb_u32)r->Magic0;
            c->Magic1 = (svmb_u32)r->Magic1;
            c->W = (svmb_u32)r->W;
            c->Dropped = (svmb_u64)r->Dropped;
            u32 n = 0;
            for (u32 i = 0; i < SVMB_CRUMBS_MAX; ++i)
            {
                const svmb::Crumb* e = &r->E[i];
                if (e->Seq)
                {
                    c->Entries[n].Tag = (svmb_u32)e->Tag;
                    c->Entries[n].Cpu = (svmb_u32)e->Cpu;
                    c->Entries[n].Tick = (svmb_u64)e->Tick;
                    c->Entries[n].Seq = (svmb_u64)e->Seq;
                    ++n;
                }
            }
            c->OutCount = n;
        }
        info = sizeof(SVMB_CRUMBS);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_SYSAUD_BEHAVIOR:
    {
        // r102 behavioral snapshot (telemetry, not accounting - rows may
        // tear across a live window rollover). Lock-independent of
        // gIoGate like the sysaudit drain.
        if (outLen < sizeof(SVMB_SYSAUD_BEHAVIOR))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SysAuditBehaviorSnapshot((SVMB_SYSAUD_BEHAVIOR*)buf);
        info = sizeof(SVMB_SYSAUD_BEHAVIOR);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_DBG_CONFIG:
    {
        if (inLen < sizeof(SVMB_DBG_CONFIG))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_DBG_CONFIG* dbg = (SVMB_DBG_CONFIG*)buf;
        svmb::DebuggerConfigure(dbg);
        // r37 M4-E: arm an MTF single step from the config call - the step
        // fires on the calling thread's core at its next instruction and is
        // consumed by the debugger module as an event (never shown to guest)
        if (dbg->ArmSingleStep)
            svmb::DebuggerArmSingleStep();
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_DBG_EVENTS:
    {
        if (outLen < sizeof(SVMB_DBG_EVENT_BUFFER))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        if (!gDbgRing)
        {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        gDbgRing->Drain((SVMB_DBG_EVENT_BUFFER*)buf);
        info = sizeof(SVMB_DBG_EVENT_BUFFER);
        status = STATUS_SUCCESS;
        break;
    }

    case SVMB_IOCTL_NPT_HOOK:
    {
        if (inLen < sizeof(SVMB_NPT_HOOK_CTL))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_NPT_HOOK_CTL* c = (SVMB_NPT_HOOK_CTL*)buf;
        if (c->Action == 0) // add
        {
            status = gHooks->Install(c->TargetVa, c->DetourVa, 0 /* jmp mode */);
        }
        else if (c->Action == 1) // remove
        {
            status = gHooks->Remove(c->TargetVa);
        }
        else if (c->Action == 2) // list
        {
            if (outLen < sizeof(SVMB_NPT_HOOK_CTL))
            {
                status = STATUS_BUFFER_TOO_SMALL;
                break;
            }
            SVMB_NPT_HOOK_CTL* list = (SVMB_NPT_HOOK_CTL*)buf;
            RtlZeroMemory(list, sizeof(*list));
            list->Action = 2;
            gHooks->FillList(list);
            info = sizeof(*list);
            status = STATUS_SUCCESS;
        }
        else
        {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }

    case SVMB_IOCTL_NPT_PROBE:
    {
        // M3 live-NPT probe: stage 0 = NPF slide cycle, stage 1 = view
        // switch. Runs under the IO gate like every other control path.
        if (inLen < sizeof(SVMB_NPT_PROBE) || outLen < sizeof(SVMB_NPT_PROBE))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_NPT_PROBE* pr = (SVMB_NPT_PROBE*)buf;
        status = RunNptProbe(pr);
        if (NT_SUCCESS(status))
            info = sizeof(*pr);
        break;
    }

    case SVMB_IOCTL_STRESS:
    {
        if (inLen < sizeof(SVMB_STRESS) || outLen < sizeof(SVMB_STRESS))
        {
            status = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        SVMB_STRESS* s = (SVMB_STRESS*)buf;
        if (!hv || !hv->IsRunning())
        {
            status = STATUS_DEVICE_NOT_READY;
            break;
        }
        if (s->Kind == 1)
        {
            // exit storm: guest-side CPUID hypercalls, split across cores
            volatile LONG64 total = 0;
            u32 iters = s->Iterations;
            status = RunOnEachCore([iters, &total](u32) -> NTSTATUS {
                for (u32 i = 0; i < iters; ++i)
                {
                    int out[4] = {};
                    __cpuidex(out, HYPERCALL_CPUID_LEAF, HC_PROBE);
                    InterlockedAdd64(&total, out[0]);
                }
                return STATUS_SUCCESS;
            });
            s->Status = (u64)total;
            info = sizeof(*s);
        }
        else
            status = STATUS_INVALID_PARAMETER;
        break;
    }

    default:
        break;
    }

    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    return status;
}

// r91 plan C- ungated fast path: suspension table list/resume. No gate, no
// hypervisor state - a suspended writer holds the gate inside its in-flight
// IOCTL, so these MUST NOT take it (deadlock otherwise).
NTSTATUS DevCtrlSusp(PIRP irp, ULONG cc)
{
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONG info = 0;
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(irp);
    const ULONG inLen = io->Parameters.DeviceIoControl.InputBufferLength;
    const ULONG outLen = io->Parameters.DeviceIoControl.OutputBufferLength;
    void* buf = irp->AssociatedIrp.SystemBuffer;
    if (inLen < sizeof(SVMB_SUSP) || outLen < sizeof(SVMB_SUSP) || !buf)
    {
        status = STATUS_BUFFER_TOO_SMALL;
    }
    else
    {
        SVMB_SUSP* s = (SVMB_SUSP*)buf;
        u32 n = 0;
        if (cc == SVMB_IOCTL_SUSP_LIST)
            n = Cr3RegionSuspList(s);
        else
            n = Cr3RegionSuspResume(s);
        status = n ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
        info = sizeof(SVMB_SUSP);
    }
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

// PASSIVE-level sleep lock helpers (see the gIoGate comment)
void IoAcquireGate();
void IoReleaseGate();
NTSTATUS DevCtrl(PDEVICE_OBJECT dev, PIRP irp)
{
    const ULONG cc = IoGetCurrentIrpStackLocation(irp)
                         ->Parameters.DeviceIoControl.IoControlCode;
#ifdef SVMB_PRODUCTION
    // r96 audit S2: the production refusal now lives ABOVE the r91 ungated
    // fast path - DevCtrlSusp bypasses DevCtrlLocked entirely, so a filter
    // left there would not cover any future fast-path IOCTL.
    if (cc == SVMB_IOCTL_CR3_POISON || cc == SVMB_IOCTL_CR3_PROBE ||
        cc == SVMB_IOCTL_NPT_HOOK || cc == SVMB_IOCTL_NPT_PROBE ||
        cc == SVMB_IOCTL_STRESS || cc == SVMB_IOCTL_KDUMP ||
        cc == SVMB_IOCTL_CRUMBS)
    {
        SVMB_LOGE("ioctl %x refused - not available in production", cc);
        irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_NOT_SUPPORTED;
    }
#endif
    // r96 audit R1: READVM's completion copy (SystemBuffer -> user buffer)
    // happens after DevCtrlLocked returns; a demand-zero user page there
    // spins forever on this vhv (KiPageFault loop, r89 kd forensics) while
    // the thread holds gIoGate - wedging every gated IOCTL until reboot.
    // Locking the range through Mm BEFORE the walk faults it in cleanly
    // (or fails the IOCTL) - caller cooperation is no longer a wedge
    // precondition. r96 P3-3 precision: the unlock sits after
    // IoCompleteRequest per standard driver practice - the buffered copy
    // itself runs in the IopCompleteRequest APC AFTER MmUnlockPages, and
    // only re-faults under memory pressure / hostile concurrent teardown.
    PMDL outMdl = nullptr;
    if (cc == SVMB_IOCTL_CR3_READVM)
    {
        const ULONG outLen =
            IoGetCurrentIrpStackLocation(irp)
                ->Parameters.DeviceIoControl.OutputBufferLength;
        // r96 P3-4: outLen beyond the header with a NULL output pointer is
        // closable at zero cost - refuse it like any other probe failure
        if (outLen > sizeof(SVMB_CR3_READVM) && !irp->UserBuffer)
        {
            SVMB_LOGW("ioctl %x refused - null output buffer (r96 probe)",
                      cc);
            irp->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
            irp->IoStatus.Information = 0;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
            return STATUS_INVALID_USER_BUFFER;
        }
        if (outLen > sizeof(SVMB_CR3_READVM))
        {
            outMdl = IoAllocateMdl(irp->UserBuffer, outLen, FALSE, FALSE,
                                   nullptr);
            bool locked = false;
            if (outMdl)
            {
                __try
                {
                    MmProbeAndLockPages(outMdl, UserMode, IoWriteAccess);
                    locked = true;
                }
                __except (EXCEPTION_EXECUTE_HANDLER)
                {
                    locked = false;
                }
            }
            if (!locked)
            {
                if (outMdl)
                    IoFreeMdl(outMdl);
                // r96 P3-5: distinguish "cannot even build the MDL"
                // (resources) from the genuine probe refusal
                if (!outMdl)
                {
                    SVMB_LOGW("ioctl %x refused - MDL alloc failed "
                              "(r96 probe)",
                              cc);
                    irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
                    irp->IoStatus.Information = 0;
                    IoCompleteRequest(irp, IO_NO_INCREMENT);
                    return STATUS_INSUFFICIENT_RESOURCES;
                }
                SVMB_LOGW("ioctl %x refused - output buffer not writable "
                          "(r96 probe)",
                          cc);
                irp->IoStatus.Status = STATUS_INVALID_USER_BUFFER;
                irp->IoStatus.Information = 0;
                IoCompleteRequest(irp, IO_NO_INCREMENT);
                return STATUS_INVALID_USER_BUFFER;
            }
        }
    }
    // r91 plan C- fast paths run UNGATED: a process suspended by the
    // policy holds this gate inside its in-flight bypass IOCTL, and the
    // whole point of list/resume is to work from outside that freeze.
    // The suspension table has its own spin lock; no gated state touched.
    if (cc == SVMB_IOCTL_SUSP_LIST || cc == SVMB_IOCTL_SUSP_RESUME)
    {
        const NTSTATUS st = DevCtrlSusp(irp, cc);
        if (outMdl)
        {
            MmUnlockPages(outMdl);
            IoFreeMdl(outMdl);
        }
        return st;
    }
    // sleep-lock acquire/release keeps PASSIVE_LEVEL throughout - see gIoGate
    IoAcquireGate();
    NTSTATUS status = DevCtrlLocked(irp);
    IoReleaseGate();
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    if (outMdl)
    {
        MmUnlockPages(outMdl);
        IoFreeMdl(outMdl);
    }
    return status;
}

// PASSIVE-level sleep lock helpers (see the gIoGate comment)
void IoAcquireGate()
{
    for (;;)
    {
        if (InterlockedCompareExchange(&gIoGate, 1, 0) == 0)
        {
            InterlockedExchange(&gGateOwnerPid,
                                (LONG)(ULONG_PTR)PsGetCurrentProcessId());
            return;
        }
        KeWaitForSingleObject(&gIoGateFree, Executive, KernelMode, FALSE,
                              nullptr);
    }
}

void IoReleaseGate()
{
    InterlockedExchange(&gGateOwnerPid, 0);
    InterlockedExchange(&gIoGate, 0);
    KeSetEvent(&gIoGateFree, IO_NO_INCREMENT, FALSE); // wake one waiter
}



// ordered teardown of the tier-1 offline subsystems; every step is
// tolerant of its component never having been initialized
void OfflineTeardown()
{
    if (gCr3Seed)
    {
        gCr3Seed->Deinit(); // unregisters the process notify callback
        delete gCr3Seed;
        gCr3Seed = nullptr;
    }
    if (gHooks)
    {
        gHooks->Deinit(); // needs the NPT view alive
        delete gHooks;
        gHooks = nullptr;
    }
    if (gNpt)
    {
        gNpt->Deinit();
        delete gNpt;
        gNpt = nullptr;
    }
    if (gDbgRing)
    {
        gDbgRing->Deinit();
        delete gDbgRing;
        gDbgRing = nullptr;
    }
}

void DevUnload(PDRIVER_OBJECT driver)
{
    IoAcquireGate();
    if (Hypervisor* hv = Hypervisor::Instance())
    {
        // 2026-09-06 fix: never `delete hv` here. Stop() releases the bulk
        // (vcpu pages, MSRPM, dispatcher) but the Hypervisor object sits on
        // a thread-stack frame of the unload caller; freeing it during the
        // deletion path races with any still-draining IOCTLs that captured
        // the pointer, and the next PspProcessDelete on the svmbctl user
        // process then BSODs in MiDeleteFinalPageTables when it walks a
        // page table that referenced the freed pages. Leak the object; OS
        // reclaims at process teardown.
        NTSTATUS st = hv->Stop();

        // r36 unload-zombie guard: once this routine returns, the OS frees
        // the image. Stop() refuses (keeps Running_=true) when a core is
        // still inside SVM - proceeding would leave a live VMM + armed NPT
        // hooks executing FREED driver code (the r33/r34/r35 zombie: module
        // unlinked, SVME=1, half-mapped image, spin-looping threads). A
        // wedged core is unsaveable anyway - fail fast with a distinctive
        // bugcheck (MANUALLY_INITIATED_CRASH + 'SVMB' tag) instead.
        if (!NT_SUCCESS(st) || hv->IsRunning())
        {
            u64 exits = hv->TotalExitCount();
            bool running = hv->IsRunning();
            IoReleaseGate();
            KeBugCheckEx(MANUALLY_INITIATED_CRASH, 0x53564D42 /* 'SVMB' */,
                         (u64)st, running ? 1 : 0, exits);
        }
    }
    // r75: restore guest MM protections on any region still registered -
    // leaving user pages PAGE_READONLY after the driver dies would wedge
    // their owners on the next write.
    Cr3RegionCleanupAll();
    Cr3MonitorLoadDeinit();
    // r105 acceptance P1-4: drain the resolver FIRST - its workitem now
    // EXECUTES suspends, and a suspend drained after the kill-deinit's
    // final table sweep would freeze a process across unload (r91 law)
    svmb::SysAuditResDeinit();
    Cr3RegionKillDeinit(); // r89: drain an in-flight kill before the device
                           // object (and the work item with it) is deleted
    OfflineTeardown();
    LogDeinit();

    // 2026-09-06 fix: do not touch gDosName further. RtlInitUnicodeString
    // pointed it at the .rodata SVMB_DOS_NAME literal; on Windows 10 that
    // pointer is now backed by a pageable section that gets evicted
    // (or reclaimed) by the time DevUnload runs, so RtlFreeUnicodeString
    // + IoDeleteSymbolicLink walk stale bytes. DriverEntry also
    // already cleared gDosName at process teardown via the
    // delete-device callback. Leave the buffer pointer alone.
    //
    // We still try to drop the symbolic link if it is valid:
    if (gDevice)
    {
        UNICODE_STRING safe = {};
        RtlInitUnicodeString(&safe, SVMB_DOS_NAME);
        IoDeleteSymbolicLink(&safe);
    }
    if (gDevice)
    {
        IoDeleteDevice(gDevice);
        gDevice = nullptr;
    }
    IoReleaseGate();
    UNREFERENCED_PARAMETER(driver);
}

} // namespace

namespace svmb
{
// r99: module-facing access to the global hook manager (the tier-1
// pointers live in the anonymous namespace above, so module TUs cannot
// link gHooks directly). Callers must hold the gate - module Init/Stop
// qualify; the audit drain never touches hooks.
NptHookManager* SysAuditGlobalHooks()
{
    return gHooks;
}
} // namespace svmb

// called from the process-death notify (cr3_monitor) when the gate owner
// died while holding the gate - the only recovery for a plan-C- victim
// killed instead of resumed. Lives in namespace svmb: cr3_monitor's death
// notify is the caller.
void svmb::IoGateOwnerDied(u32 pid)
{
    if (InterlockedCompareExchange(&gIoGate, 0, 0) == 1
        && InterlockedCompareExchange(&gGateOwnerPid, 0, 0) == (LONG)pid)
    {
        SVMB_LOGE("gate: owner pid %u died holding the gate - force release",
                  pid);
        InterlockedExchange(&gGateOwnerPid, 0);
        InterlockedExchange(&gIoGate, 0);
        KeSetEvent(&gIoGateFree, IO_NO_INCREMENT, FALSE);
    }
}

// global-instance accessor consumed by core (ConfigureVmcb NPT consult,
// NPF handler registration)
svmb::NptManager* svmb::NptInstance()
{
    return gNpt;
}
svmb::NptHookManager* svmb::HookInstance()
{
    return gHooks;
}
svmb::DbgEventRing* svmb::DbgRingInstance()
{
    return gDbgRing;
}
svmb::Cr3Seed* svmb::Cr3SeedInstance()
{
    return gCr3Seed;
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING)
{
    // serial flight recorder first: every step below mirrors one short tag to
    // COM1 (VMware serial -> host file), which survives triple faults. The
    // last S-tag in that file is the exact statement that died.
    LogSerialTrace("S0:DriverEntry-enter");

    // First thing: liveness beacon, BEFORE anything else. If this doesn't
    // appear in WinDbg, KDNET is not actually attached (or the host KD is
    // disconnected). 2026-09-06 bring-up debugging.
    DbgPrint("[svmb][entry] ENTER driver=%p\n", (void*)driver);

    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = DevCtrl;
    driver->MajorFunction[IRP_MJ_CREATE] = DevCreateClose;
    driver->MajorFunction[IRP_MJ_CLOSE] = DevCreateClose;
    driver->DriverUnload = DevUnload;

    KeInitializeEvent(&gIoGateFree, SynchronizationEvent, FALSE);
    LogInit();
    LogSerialTrace("S1:LogInit-ok");
    DbgPrint("[svmb][entry] after-LogInit\n");
    LogUartProbe(); // serial channel self-diagnostic through the KDNET path
    DbgSetDebugFilterState(DPFLTR_IHVDRIVER_ID, DPFLTR_MASK, TRUE);
    LogSetSerial(false);
    DbgPrint("[svmb][entry] after-setfilter\n");
    LogSerialTrace("S2:setfilter-ok");

    // optional Parameters\ values under the service key - all optional, all
    // for diagnosing "vCPU shutdown on start" without rebuilding:
    //   SerialLog      DWORD 1   mirror every log line to COM1 (VMware serial
    //                            -> file = black-box recorder that survives the
    //                            death of the VM, unlike buffered DbgPrint)
    //   HideCpuidBits  DWORD 0   disable the CPUID SVM/NPT bit hiding (bisect)
    //   ProtectHsave   DWORD 0   disable VM_HSAVE_PA interception (bisect)
    //   NptEnable      DWORD 1   tier-3 gate: bring NPT live at start (M3
    //                            activation; do NOT set until the start
    //                            crash-line is resolved)
    //   AutoStart      DWORD 1   DEFAULT ON since r30: virtualization is
    //                            entered right from DriverEntry (reference
    //                            parity - its EnterVirtualization runs at
    //                            driver load). The IOCTL/ctl-start path is
    //                            the known-fatal enter context (round 27
    //                            A/B), so load-time entry is THE supported
    //                            mode. Set AutoStart=0 to opt out
    //                            (bisecting only - ctl start will then
    //                            hit the fatal path).
    ULONG nptEnable = 0; // consumed after offline subsystems init
    ULONG autoStart = 1; // default ON: load == virtualize (r30)
    ULONG tlbMode = 0;   // r58 A/B: 0 = flush-all armed (r54 semantics),
                         // 1 = never arm flush-all (reference semantics)
    ULONG guestAsid = 1; // r58 A/B: VMCB GuestAsid (the reference also uses 1)
    ULONG wiggleMode = 2; // r80: 0=off 1=back-to-back kick (r77) 2=real
                          // wiggle (scratch observed at next VMRUN, restore
                          // from the re-arm DPC)
#ifdef SVMB_PRODUCTION
    ULONG readVmEnable = 0; // r96 audit R3: the god-view read ships OFF in
                            // production - arm it with an explicit knob
#else
    ULONG readVmEnable = 1; // r89: Parameters\ReadVmEnable=0 refuses
                            // CR3_READVM (deployment hardening knob)
#endif
    ULONG tscProbe = 0;   // r92: TSC offset probe (vhv TscOffset-honor check)
    // r102: behavior-alert thresholds (sensed-burst alarms - see the
    // sampling note in modules/sysaudit.cpp; defaults = protocol consts)
    ULONG behaveRd = SVMB_SYSAUD_BEHAVE_RD_THS;
    ULONG behaveWr = SVMB_SYSAUD_BEHAVE_WR_THS;
    ULONG behaveOp = SVMB_SYSAUD_BEHAVE_OP_THS;
    ULONG behaveResp = 0; // r105: 0 = telemetry only (default)
    {
        RTL_QUERY_REGISTRY_TABLE table[15] = {};
        // Defaults chosen to mirror hypervisor.cpp's compiled-in baseline
        // (see CRASH_DEBUG_LOG round 5): protectHsave=0, hideBits=1. The
        // offline subsystem alloc (gNpt/gHooks/gCr3Seed/gDbgRing below)
        // is order-tolerant - if it fails we tear down what we already
        // allocated before returning.
        ULONG serialLog = 0, hideBits = 1, protectHsave = 0;
        table[0].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[0].Name = (PWSTR)L"SerialLog";
        table[0].EntryContext = &serialLog;
        table[0].DefaultType = REG_NONE;
        table[1].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[1].Name = (PWSTR)L"HideCpuidBits";
        table[1].EntryContext = &hideBits;
        table[1].DefaultType = REG_NONE;
        table[2].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[2].Name = (PWSTR)L"ProtectHsave";
        table[2].EntryContext = &protectHsave;
        table[2].DefaultType = REG_NONE;
        table[3].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[3].Name = (PWSTR)L"NptEnable";
        table[3].EntryContext = &nptEnable;
        table[3].DefaultType = REG_NONE;
        table[4].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[4].Name = (PWSTR)L"AutoStart";
        table[4].EntryContext = &autoStart;
        table[4].DefaultType = REG_NONE;
        table[5].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[5].Name = (PWSTR)L"TlbMode";
        table[5].EntryContext = &tlbMode;
        table[5].DefaultType = REG_NONE;
        table[6].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[6].Name = (PWSTR)L"GuestAsid";
        table[6].EntryContext = &guestAsid;
        table[6].DefaultType = REG_NONE;
        table[7].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[7].Name = (PWSTR)L"WiggleMode";
        table[7].EntryContext = &wiggleMode;
        table[7].DefaultType = REG_NONE;
        table[8].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[8].Name = (PWSTR)L"ReadVmEnable";
        table[8].EntryContext = &readVmEnable;
        table[8].DefaultType = REG_NONE;
        table[9].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[9].Name = (PWSTR)L"TscProbe";
        table[9].EntryContext = &tscProbe;
        table[9].DefaultType = REG_NONE;
        table[10].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[10].Name = (PWSTR)L"BehaveRdThs";
        table[10].EntryContext = &behaveRd;
        table[10].DefaultType = REG_NONE;
        table[11].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[11].Name = (PWSTR)L"BehaveWrThs";
        table[11].EntryContext = &behaveWr;
        table[11].DefaultType = REG_NONE;
        table[12].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[12].Name = (PWSTR)L"BehaveOpThs";
        table[12].EntryContext = &behaveOp;
        table[12].DefaultType = REG_NONE;
        table[13].Flags = RTL_QUERY_REGISTRY_DIRECT;
        table[13].Name = (PWSTR)L"BehaveResponse";
        table[13].EntryContext = &behaveResp;
        table[13].DefaultType = REG_NONE;
        // table[14] stays all-zero: RtlQueryRegistryValues needs the
        // NULL-Name terminator entry (that terminator is why [10] worked
        // with only 9 slots filled before)
        RtlQueryRegistryValues(RTL_REGISTRY_ABSOLUTE,
                               L"\\Registry\\Machine\\SYSTEM\\CurrentControlSet"
                               L"\\Services\\svmb\\Parameters",
                               table, nullptr, nullptr);
        LogSetSerial(serialLog != 0);
        GuestCpuidHideEnabled = hideBits ? 1 : 0;
        HsaveProtectionEnabled = protectHsave ? 1 : 0;
        Hypervisor::SetTlbKnobs(tlbMode, guestAsid);
        svmb::Cr3MonitorSetWiggleMode((u32)wiggleMode);
        g_readVmEnable = readVmEnable ? 1 : 0;
        TscProbeEnabled = tscProbe ? 1 : 0;
        svmb::SysAuditSetBehaveThresholds(behaveRd, behaveWr, behaveOp);
        svmb::SysAuditSetBehaveResponse(behaveResp);
        // log the EFFECTIVE thresholds (0 = knob absent -> protocol
        // default; logging the raw 0 would read as "alert at 0")
        SVMB_LOGI("config: serial=%u hideCpuid=%u protectHsave=%u nptEnable=%u "
                  "tlbMode=%u asid=%u wiggle=%u readvm=%u tscprobe=%u "
                  "behaveRd=%u behaveWr=%u behaveOp=%u behaveResp=%u",
                  serialLog != 0, hideBits != 0, protectHsave != 0,
                  nptEnable != 0, tlbMode, guestAsid, wiggleMode,
                  g_readVmEnable, TscProbeEnabled,
                  behaveRd ? behaveRd : SVMB_SYSAUD_BEHAVE_RD_THS,
                  behaveWr ? behaveWr : SVMB_SYSAUD_BEHAVE_WR_THS,
                  behaveOp ? behaveOp : SVMB_SYSAUD_BEHAVE_OP_THS,
                  behaveResp ? 1 : 0);
    }

    gNpt = new (NonPagedPoolNx, TAG_SVMB) svmb::NptManager();
    gHooks = new (NonPagedPoolNx, TAG_SVMB) svmb::NptHookManager();
    gCr3Seed = new (NonPagedPoolNx, TAG_SVMB) svmb::Cr3Seed();
    gDbgRing = new (NonPagedPoolNx, TAG_SVMB) svmb::DbgEventRing();
    if (!gNpt || !gHooks || !gCr3Seed || !gDbgRing)
    {
        SVMB_LOGE("offline subsystem alloc failed");
        OfflineTeardown();
        LogDeinit();
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    NTSTATUS status = gNpt->Init();
    if (!NT_SUCCESS(status))
    {
        SVMB_LOGE("npt init failed: %08x", status);
        LogDeinit();
        return status;
    }
    status = gCr3Seed->Init();
    if (!NT_SUCCESS(status))
        SVMB_LOGW("cr3 seed unavailable: %08x (continuing without)", status);
    status = gDbgRing->Init();
    if (!NT_SUCCESS(status))
        SVMB_LOGW("dbg ring init failed: %08x", status);
    // live-NPT publish path: view switches (and later NPF-driven changes)
    // reach every running VMCB through this callback; offline it no-ops.
    // When running, the publish completing is acknowledged with a view-
    // activate event so R3 can confirm the switch landed (cores sample the
    // new NCr3 at their next VMRUN).
    gNpt->SetApplyCallback([](u64 pml4Pa, void*) {
        TlbApplyViewSwitchAllCores(pml4Pa);
        Hypervisor* hv = Hypervisor::Instance();
        if (hv && hv->IsRunning() && gDbgRing)
        {
            SVMB_DBG_EVENT e = {};
            e.Type = SvmbDbgEvtViewActivate;
            e.Cr3 = pml4Pa;
            e.Extra = hv->CoreCount();
            gDbgRing->Push(e);
        }
    }, nullptr);
    // hook slide seam: hooked pages handle their own present faults (exec ->
    // hidden page, write -> original page) before the generic classifier
    NpfSetSlideHook([](void* ctx, NptView& view, u64 gpa4k, u64 info1) {
        return HookSlideApply(*(NptHookManager*)ctx, view, gpa4k, info1);
    }, gHooks);
    // r76: deny consumer (region sensing + orphan resolve) live at load -
    // protected regions are reachable via IOCTL without the module attach
    Cr3MonitorLoadInit();
    status = gHooks->Init(gNpt);
    if (!NT_SUCCESS(status))
    {
        SVMB_LOGE("npt hook mgr init failed: %08x", status);
        // gCr3Seed may not be registered yet - Deinit is order-tolerant
        OfflineTeardown();
        LogDeinit();
        return status;
    }

    // tier-3 gate: NPT goes live only when explicitly switched on via
    // Parameters/NptEnable (default OFF - keep start crash-line resolved
    // before flipping this on; once stable, set NptEnable=1 in registry
    // and reload the driver to enter the NPT path).
    if (nptEnable)
    {
        // round-12: identity-map the whole [0, RamEnd_) PA window as 2M RWX
        // large pages BEFORE enabling NPT. Without this fill, guest walks to
        // MMIO physical addresses (APIC, IO-APIC, HPET, ...) trap into the
        // NPF lazy-map path which absorbs the write into a stand-in RAM
        // frame, so interrupt delivery never reaches the device and the OS
        // scheduler wedges. The fill mirrors the reference hypervisor's
        // NPT fill which identity-maps the full 1 TB.
        NTSTATUS stFill = gNpt->FillIdentityWholePa();
        if (!NT_SUCCESS(stFill))
        {
            SVMB_LOGW("npt identity-fill failed: %08x (continuing - NPF will lazy-map)", stFill);
        }
        // r77: scratch identity view for the shadow kick (NCr3 wiggle) -
        // trip resolves are invisible to the vhv shadow until the NCr3
        // value changes (r69 law); ShadowKick wiggles through this view
        u32 wig = 0;
        if (NT_SUCCESS(gNpt->CreateView(wig)) && wig && gNpt->View(wig))
        {
            NTSTATUS stWig = gNpt->FillIdentityView(*gNpt->View(wig));
            if (NT_SUCCESS(stWig))
                gNpt->SetWiggleView(wig);
            else
                SVMB_LOGW("wiggle view fill failed: %08x (wiggle off)", stWig);
        }
        NTSTATUS stEn = gNpt->SetActiveView(0); // default view must be active
        if (NT_SUCCESS(stEn))
            stEn = gNpt->Enable();
        if (!NT_SUCCESS(stEn))
        {
            SVMB_LOGW("npt enable failed: %08x", stEn);
        }
        else
        {
            // arm installed hooks: hooked pages start NX so the first guest
            // fetch #NPFs into the slide (Install skipped arming - NPT was
            // still offline when it ran)
            gHooks->ArmSlide(gNpt->Default());
            SVMB_LOGI("NPT enabled (ncr3=%llx)", gNpt->Active()->Pml4Pa());
        }
    }

    UNICODE_STRING devName = RTL_CONSTANT_STRING(SVMB_DEVICE_NAME);
    RtlInitUnicodeString(&gDosName, SVMB_DOS_NAME);

    // secure descriptor: system + administrators only - the IOCTL surface can
    // start/stop virtualization machine-wide, every user must not reach it
    status = IoCreateDeviceSecure(driver, 0, &devName, FILE_DEVICE_UNKNOWN, 0, FALSE,
                                  &SDDL_DEVOBJ_SYS_ALL_ADM_ALL, &SVMB_DEVCLASS, &gDevice);
    if (!NT_SUCCESS(status))
    {
        OfflineTeardown();
        LogDeinit();
        return status;
    }
    LogSerialTrace("S4:DevCreate-ok");
    gDevice->Flags |= DO_BUFFERED_IO;
    gDevice->Flags &= ~DO_DEVICE_INITIALIZING;
    // r89 plan C: the kill work item is device-tied; from here on the
    // trip->DPC->workitem pipeline is armed
    Cr3RegionKillInit(gDevice);
    svmb::SysAuditResInit(gDevice); // r103 target-handle resolver
    svmb::CrumbInit(); // r107 freeze flight recorder (test builds)

    status = IoCreateSymbolicLink(&gDosName, &devName);
    if (!NT_SUCCESS(status))
    {
        LogSerialTrace("S5FAIL:SymLink");
        Cr3RegionKillDeinit(); // r89 hygiene: the work item is device-tied
        IoDeleteDevice(gDevice);
        gDevice = nullptr;
        OfflineTeardown();
        LogDeinit();
        return status;
    }
    LogSerialTrace("S5:SymLink-ok");

    // round-27 exp1: Parameters\AutoStart=1 enters virtualization directly
    // from DriverEntry - the load-time flow (EnterVirtualization at driver
    // load). Differences vs the ctl-start path being tested: the enter
    // sequence + first crossings run in the System-process load thread
    // (core 0-ish) instead of a user IOCTL thread, and they happen before
    // the service even reports RUNNING. Failure keeps the driver loaded
    // (no DriverEntry rollback) so forensics stay available.
    if (autoStart)
    {
        SVMB_LOGI("exp1: AutoStart - entering virtualization at DriverEntry");
        Hypervisor* h = new (NonPagedPoolNx, TAG_SVMB) Hypervisor();
        NTSTATUS stStart = h ? h->Start() : STATUS_INSUFFICIENT_RESOURCES;
        if (!NT_SUCCESS(stStart))
        {
            if (h)
                delete h;
            SVMB_LOGE("exp1: AutoStart Start failed: %08x (driver stays loaded)", stStart);
        }
    }

    SVMB_LOGI("svmb driver loaded (proto v%u)", SVMB_PROTO_VERSION);
    LogSerialTrace("S6:DriverEntry-exit");
    return STATUS_SUCCESS;
}

// sized placement new/delete for driver-internal objects.
// NOTE: everything allocated with this new is freed with TAG_SVMB.
// Legacy pool API for down-level Windows 10 compatibility (see util.cpp).
#pragma warning(disable : 4996) // ExAllocatePoolWithTag deprecation is deliberate
void* operator new(size_t size, POOL_TYPE pool, ULONG tag)
{
    UNREFERENCED_PARAMETER(tag);
    return ExAllocatePoolWithTag(pool == PagedPool ? PagedPool : NonPagedPoolNx,
                                 size, TAG_SVMB);
}
void operator delete(void* p, size_t)
{
    if (p)
        ExFreePoolWithTag(p, TAG_SVMB);
}
