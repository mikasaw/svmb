// svmbctl - R3 control tool for the svmb hypervisor framework
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <intrin.h>

extern "C" {
#include "../shared/svmb_protocol.h"
}

typedef unsigned long u32_; // protocol svmb_u32 alias (kernel uses 'long')

static HANDLE gDevice = INVALID_HANDLE_VALUE;

static bool OpenDrv()
{
    gDevice = CreateFileW(L"\\\\.\\svmb", GENERIC_READ | GENERIC_WRITE,
                          0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (gDevice == INVALID_HANDLE_VALUE)
    {
        printf("[!] cannot open \\\\.\\svmb (err %lu)\n", GetLastError());
        return false;
    }
    return true;
}

static BOOL Call(DWORD code, void* in, DWORD inSz, void* out, DWORD outSz, DWORD* ret = nullptr)
{
    DWORD bytes = 0;
    // Bounded wait: HV_CONTROL stop runs RunOnEachCore and can deadlock if a
    // core's devirtualize handoff fails - an unbounded DeviceIoControl would
    // hang svmbctl forever and wedge the whole guest-op channel with it.
    // 30s is generous (a healthy stop is sub-second); on timeout we report
    // and let the caller/user retry or reboot instead of freezing.
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent)
        return FALSE;
    BOOL ok = DeviceIoControl(gDevice, code, in, inSz, out, outSz,
                              ret ? ret : &bytes, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
    {
        DWORD wait = WaitForSingleObject(ov.hEvent, 30000);
        if (wait == WAIT_OBJECT_0)
        {
            ok = GetOverlappedResult(gDevice, &ov, &bytes, FALSE);
        }
        else
        {
            CancelIoEx(gDevice, &ov);
            CloseHandle(ov.hEvent);
            SetLastError(ERROR_DRIVER_FAILED_SLEEP);
            return FALSE;
        }
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static int CmdInfo()
{
    SVMB_INFO info = {};
    if (!Call(SVMB_IOCTL_GET_INFO, nullptr, 0, &info, sizeof(info)))
    {
        printf("[!] GET_INFO failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("magic        : %08X (%s)\n", info.Magic, info.Magic == SVMB_PROTO_MAGIC ? "ok" : "BAD");
    printf("proto        : %u\n", info.ProtoVersion);
    printf("driver       : %u.%u\n", info.DriverVersion >> 16, info.DriverVersion & 0xFFFF);
    printf("hv state     : %s\n",
           info.VmxHvState == SvmbHvStateRunning ? "RUNNING" :
           info.VmxHvState == SvmbHvStateStarting ? "STARTING" :
           info.VmxHvState == SvmbHvStateStopping ? "STOPPING" : "OFF");
    printf("cores        : %u virtualized / %u total\n", info.CoresVirtualized, info.CoreCount);
    printf("phys addr w  : %u bits\n", info.PaWidthBits);
    printf("vmexits      : %llu\n", (unsigned long long)info.ExitCounter);
    // regression check for the CPUID hide: while virtualized the SVM feature
    // bit (0x80000001.ECX[2]) must be masked; when OFF the real bit shows
    {
        int out[4] = {};
        __cpuid(out, 0x80000001);
        bool svm = ((unsigned)out[2] >> 2) & 1;
        printf("svm cpuid bit: %s", svm ? "visible" : "hidden");
        if (info.VmxHvState == SvmbHvStateRunning)
            printf(" (%s)", svm ? "LEAK - hide broken" : "ok, hidden while running");
        printf("\n");
    }
    return 0;
}

// r45: SVM exit-reason mnemonics (mirrors driver hw/vmcb.h vmexit namespace)
static const char* ExitSlotName(unsigned s)
{
    static char buf[24];
    if (s < SVMB_XP_DENSE)
    {
        if (s < 0x10)  { sprintf_s(buf, "cr%u.read", s); return buf; }
        if (s < 0x20)  { sprintf_s(buf, "cr%u.write", s - 0x10); return buf; }
        if (s < 0x30)  { sprintf_s(buf, "dr%u.read", s - 0x20); return buf; }
        if (s < 0x40)  { sprintf_s(buf, "dr%u.write", s - 0x30); return buf; }
        if (s < 0x60)  { sprintf_s(buf, "exc%u", s - 0x40); return buf; }
        switch (s)
        {
        case 0x60: return "intr";    case 0x61: return "nmi";
        case 0x62: return "smi";     case 0x63: return "init";
        case 0x64: return "vintr";   case 0x65: return "cr0sel.w";
        case 0x66: return "idtr.r";  case 0x67: return "gdtr.r";
        case 0x68: return "ldtr.r";  case 0x69: return "tr.r";
        case 0x6A: return "idtr.w";  case 0x6B: return "gdtr.w";
        case 0x6C: return "ldtr.w";  case 0x6D: return "tr.w";
        case 0x6E: return "rdtsc";   case 0x6F: return "rdpmc";
        case 0x70: return "pushf";   case 0x71: return "popf";
        case 0x72: return "cpuid";   case 0x73: return "rsm";
        case 0x74: return "iret";    case 0x75: return "intn";
        case 0x76: return "invd";    case 0x77: return "pause";
        case 0x78: return "hlt";     case 0x79: return "invlpg";
        case 0x7A: return "invlpga"; case 0x7B: return "ioio";
        case 0x7C: return "msr";     case 0x7D: return "tasksw";
        case 0x7E: return "ferr";    case 0x7F: return "shutdown";
        }
        sprintf_s(buf, "code %x", s);
        return buf;
    }
    if (s == SVMB_XP_NPF)   return "NPF";
    if (s == SVMB_XP_OTHER) return "other";
    sprintf_s(buf, "slot %x", s);
    return buf;
}

static int CmdExitProf()
{
    SVMB_EXITPROF ep = {};
    if (!Call(SVMB_IOCTL_EXITPROF, nullptr, 0, &ep, sizeof(ep)))
    {
        printf("[!] EXITPROF failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("exitprof: total=%llu cpus=%u\n",
           (unsigned long long)ep.Total, ep.CpuCount);
    unsigned idx[SVMB_XP_SLOTS];
    for (unsigned i = 0; i < SVMB_XP_SLOTS; ++i)
        idx[i] = i;
    for (unsigned i = 1; i < SVMB_XP_SLOTS; ++i) // insertion sort, count desc
    {
        unsigned v = idx[i];
        unsigned j = i;
        while (j > 0 && ep.Hist[idx[j - 1]] < ep.Hist[v])
        {
            idx[j] = idx[j - 1];
            --j;
        }
        idx[j] = v;
    }
    for (unsigned i = 0; i < SVMB_XP_SLOTS && i < 24; ++i)
    {
        if (!ep.Hist[idx[i]])
            break;
        printf("  %-10s %12llu  %llu%%\n", ExitSlotName(idx[i]),
               (unsigned long long)ep.Hist[idx[i]],
               (unsigned long long)(ep.Hist[idx[i]] * 100 /
                                    (ep.Total ? ep.Total : 1)));
    }
    return 0;
}

static int CmdHv(DWORD action, DWORD repeat)
{
    SVMB_HV_CONTROL c = {};
    c.Action = action;
    c.RepeatCount = repeat;
    if (!Call(SVMB_IOCTL_HV_CONTROL, &c, sizeof(c), nullptr, 0))
    {
        DWORD err = GetLastError();
        printf("[!] HV_CONTROL(%lu) failed (err %lu)\n", action, err);
        if (action == 1 && err == ERROR_DRIVER_FAILED_SLEEP)
            printf("[!] stop timed out after 30s: a core likely failed to "
                   "devirtualize (HC_EXIT_VMM handoff). The system is NOT "
                   "crashed; other IOCTLs may still work. Retry stop or reboot.\n");
        return 1;
    }
    printf("[+] HV_CONTROL(%lu) ok\n", action);
    return 0;
}

static int CmdLog()
{
    SVMB_LOG_ENTRY entries[32] = {};
    DWORD bytes = 0;
    while (Call(SVMB_IOCTL_LOG_READ, nullptr, 0, entries, sizeof(entries), &bytes) && bytes)
    {
        unsigned n = bytes / sizeof(SVMB_LOG_ENTRY);
        for (unsigned i = 0; i < n; ++i)
            printf("[%u] %s\n", entries[i].Core, entries[i].Text);
        if (n < 32)
            break;
    }
    return 0;
}

static int CmdStress(DWORD iters)
{
    SVMB_STRESS s = {};
    s.Kind = 1;
    s.Iterations = iters;
    if (!Call(SVMB_IOCTL_STRESS, &s, sizeof(s), &s, sizeof(s)))
    {
        printf("[!] STRESS failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("[+] stress done: %llu hypercall exits\n", (unsigned long long)s.Status);
    return 0;
}


static int CmdSelfTest()
{
    SVMB_SELFTEST_RESULT res = {};
    if (!Call(SVMB_IOCTL_SELFTEST, nullptr, 0, &res, sizeof(res)))
    {
        printf("[!] SELFTEST failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("selftest: %u checks, %u failed", res.Total, res.Failed);
    if (res.Failed)
        printf(" first=[%s]", res.LastFail);
    printf("\n");
    return res.Failed == 0 ? 0 : 1;
}

static int CmdNpt(const wchar_t* action, const wchar_t* a1, const wchar_t* a2)
{
    SVMB_NPT_HOOK_CTL c = {};
    if (!_wcsicmp(action, L"add"))
    {
        if (!a1 || !a2)
        {
            printf("[!] npt add <targetVa> <detourVa> (hex)\n");
            return 1;
        }
        c.Action = 0;
        c.TargetVa = wcstoull(a1, nullptr, 16);
        c.DetourVa = wcstoull(a2, nullptr, 16);
    }
    else if (!_wcsicmp(action, L"remove"))
    {
        if (!a1)
        {
            printf("[!] npt remove <targetVa> (hex)\n");
            return 1;
        }
        c.Action = 1;
        c.TargetVa = wcstoull(a1, nullptr, 16);
    }
    else if (!_wcsicmp(action, L"list"))
    {
        c.Action = 2;
    }
    else
    {
        printf("[!] npt action: add|remove|list\n");
        return 1;
    }

    if (!Call(SVMB_IOCTL_NPT_HOOK, &c, sizeof(c), &c, sizeof(c)))
    {
        printf("[!] NPT_HOOK %ls failed (err %lu)\n", action, GetLastError());
        return 1;
    }
    if (c.Action == 2)
    {
        printf("hooks (%lu):\n", (unsigned long)c.Count);
        for (u32_ i = 0; i < c.Count && i < 32; ++i)
            printf("  target=%llx detour=%llx\n",
                   (unsigned long long)c.Hooks[i].TargetVa,
                   (unsigned long long)c.Hooks[i].DetourVa);
    }
    else
        printf("[+] npt %ls ok\n", action);
    return 0;
}

static int CmdCr3Stats()
{
    SVMB_CR3_STATS st = {};
    if (!Call(SVMB_IOCTL_CR3_STATS, nullptr, 0, &st, sizeof(st)))
    {
        printf("[!] CR3_STATS failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("cr3 seed: monitored=%lu\n", (unsigned long)st.MonitoredCount);
    printf("sentinel trips: %lu armed pages: %lu diag=%08x",
           (unsigned long)st.SentinelTrips, (unsigned long)st.SentinelArmed, (unsigned long)st.SentinelDiag);
    printf("read exits: %lu\n", (unsigned long)st.ReadExitCount);
    printf("sentinel reuse: %lu npf heals: %lu region trips: %lu kicks: %lu\n",
           (unsigned long)st.SentinelReuse, (unsigned long)st.NpfHeals,
           (unsigned long)st.RegionTrips, (unsigned long)st.WiggleKicks);
    printf("kill: attempts=%lu denylisted=%lu dropped=%lu\n",
           (unsigned long)st.KillAttempts, (unsigned long)st.KillDenylisted,
           (unsigned long)st.KillDropped);
    printf("suspend: attempts=%lu denylisted=%lu dropped=%lu active=%lu\n",
           (unsigned long)st.SuspAttempts, (unsigned long)st.SuspDenylisted,
           (unsigned long)st.SuspDropped, (unsigned long)st.SuspActive);
    printf("readvm calls: %lu pages: %lu\n",
           (unsigned long)st.ReadVmCalls, (unsigned long)st.ReadVmPages);
    return 0;
}

// cr3 watch <name> [xorkeyhex] [wm] | cr3 unwatch
// r46: EnableWriteMonitor defaults OFF - arming the CR3-WRITE intercept is
// the whole-system context-switch tax that death-spirals this vhv (r43).
// The Route-A sentinel (NPT page protection) replaces it; "wm" is the
// explicit legacy opt-in.
static int CmdCr3Poison()
{
    DWORD n = 0;
    if (!Call(SVMB_IOCTL_CR3_POISON, nullptr, 0, &n, sizeof(n)))
    {
        printf("[!] CR3_POISON failed (err %lu)\n", GetLastError());
        return 1;
    }
    if (n)
        printf("[+] poisoned %lu armed slot (watermark -> deadbeef); the "
               "re-arm DPC should tear it down within ~100ms\n", n);
    else
        printf("[!] no armed slot to poison\n");
    return n ? 0 : 1;
}

// r92: raw guest-view CPUID (user-mode __cpuidex) - the stealth
// verification probe. Pure observation, no device, no IOCTL.
static int CmdCpuId(const wchar_t* leafW)
{
    unsigned long leaf = wcstoul(leafW, nullptr, 16);
    int out[4] = {};
    __cpuidex(out, (int)leaf, 0);
    printf("cpuid leaf=%08lx ecx=0: EAX=%08x EBX=%08x ECX=%08x EDX=%08x\n",
           leaf, (unsigned)out[0], (unsigned)out[1], (unsigned)out[2],
           (unsigned)out[3]);
    if (leaf == 0x40000000)
    {
        char sig[13] = {};
        memcpy(sig, &out[1], 4);
        memcpy(sig + 4, &out[2], 4);
        memcpy(sig + 8, &out[3], 4);
        printf("  hypervisor: max-leaf=%08x signature=\"%s\"\n",
               (unsigned)out[0], sig);
    }
    if (leaf == 1)
        printf("  hypervisor-present (ECX[31]) = %u\n",
               ((unsigned)out[2] >> 31) & 1);
    return 0;
}

// r99 syscall audit: drain the sysaudit module ring and print entries.
static int CmdSysAudit()
{
    static const char* const kFn[] = {"copy", "ntcf", "lost",
                                      "ntrd", "ntwr", "ntop"};
    if (!OpenDrv())
        return 1;
    SVMB_SYSCALL_AUDIT* a = (SVMB_SYSCALL_AUDIT*)calloc(1, sizeof(*a));
    if (!a)
        return 1;
    unsigned total = 0, rounds = 0;
    for (;;)
    {
        DWORD bytes = 0;
        if (!Call(SVMB_IOCTL_SYSCALL_AUDIT, nullptr, 0, a, sizeof(*a),
                  &bytes)
            || bytes < sizeof(*a))
        {
            printf("[!] sysaudit failed (err %lu)\n", GetLastError());
            free(a);
            return 1;
        }
        if (rounds == 0)
            printf("sysaudit: armed=%u resolved=%u stage=%u total=%llu "
                   "lost=%llu noise=%llu target=%llx targetRd=%llx\n",
                   a->Armed, a->Resolved, a->FailStage,
                   (unsigned long long)a->Total,
                   (unsigned long long)a->Lost,
                   (unsigned long long)a->Noise,
                   (unsigned long long)a->Target,
                   (unsigned long long)a->TargetRd);
        for (u32_ i = 0; i < a->OutCount; ++i)
        {
            const SVMB_SYSAUD_ENTRY* e = &a->Entries[i];
            const char* fn = (e->FnId < 6) ? kFn[e->FnId] : "?";
            printf("  [%u] fn=%s pid=%lu (%s) src=%llx:%llx dst=%llx:%llx "
                   "size=%llu\n",
                   total + i + 1, fn, (unsigned long)e->Pid, e->Image,
                   (unsigned long long)e->SrcProcess,
                   (unsigned long long)e->SrcAddress,
                   (unsigned long long)e->DstProcess,
                   (unsigned long long)e->DstAddress,
                   (unsigned long long)e->Size);
        }
        total += a->OutCount;
        ++rounds;
        if (a->OutCount < SVMB_SYSAUD_MAX_ENTRIES)
            break;
    }
    printf("sysaudit: %u entries drained\n", total);
    free(a);
    return 0;
}

// r101 kernel-VA qword dump: the SSDT table / candidate-region inspector
// (kd has been offline since r68). Args: hex VA, optional qword count
// (default 16, max 96).
static int CmdKdump(const wchar_t* vaStr, const wchar_t* cntStr)
{
    SVMB_KDUMP k = {};
    k.Va = _wcstoui64(vaStr, nullptr, 16);
    k.Qwords = cntStr ? (u32_)wcstoul(cntStr, nullptr, 0) : 16;
    if (!k.Va)
    {
        printf("usage: kdump <va-hex> [qwords]\n");
        return 1;
    }
    DWORD bytes = 0;
    if (!Call(SVMB_IOCTL_KDUMP, &k, sizeof(k), &k, sizeof(k), &bytes)
        || bytes < sizeof(k))
    {
        printf("[!] kdump failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("kdump: va=%llx qwords=%u ok=%u\n", (unsigned long long)k.Va,
           (unsigned)k.Qwords, (unsigned)k.FailIdx);
    const u32_ n = k.Qwords > SVMB_KDUMP_MAX_QWORDS ? SVMB_KDUMP_MAX_QWORDS
                                                    : k.Qwords;
    for (u32_ i = 0; i < n; i += 2)
    {
        printf("  +%03x %016llx %016llx\n", (unsigned)(i * 8),
               (unsigned long long)k.Q[i],
               (i + 1 < n) ? (unsigned long long)k.Q[i + 1] : 0ull);
    }
    return 0;
}

// r107 freeze flight recorder readout: raw breadcrumb ring (order by
// Seq; the highest Seq rows are the most recent hops).
static int CmdCrumbs()
{
    if (!OpenDrv())
        return 1;
    SVMB_CRUMBS* c = (SVMB_CRUMBS*)calloc(1, sizeof(*c));
    if (!c)
        return 1;
    DWORD bytes = 0;
    if (!Call(SVMB_IOCTL_CRUMBS, nullptr, 0, c, sizeof(*c), &bytes)
        || bytes < sizeof(*c))
    {
        printf("[!] crumbs failed (err %lu)\n", GetLastError());
        free(c);
        return 1;
    }
    printf("crumbs: magic=%08x/%08x w=%u count=%u dropped=%llu\n",
           (unsigned)c->Magic0, (unsigned)c->Magic1, (unsigned)c->W,
           (unsigned)c->OutCount, (unsigned long long)c->Dropped);
    for (u32_ i = 0; i < c->OutCount && i < SVMB_CRUMBS_MAX; ++i)
    {
        const SVMB_CRUMB* e = &c->Entries[i];
        printf("  seq=%llu tick=%llu cpu=%lu tag=%06x\n",
               (unsigned long long)e->Seq, (unsigned long long)e->Tick,
               (unsigned long)e->Cpu, (unsigned)e->Tag);
    }
    free(c);
    return 0;
}

// r102 behavioral analytics snapshot: per-caller cross-process syscall
// rates (1s windows, edge-triggered alerts - telemetry only).
static int CmdSysAuditBehavior()
{
    if (!OpenDrv())
        return 1;
    SVMB_SYSAUD_BEHAVIOR* b =
        (SVMB_SYSAUD_BEHAVIOR*)calloc(1, sizeof(*b));
    if (!b)
        return 1;
    DWORD bytes = 0;
    if (!Call(SVMB_IOCTL_SYSAUD_BEHAVIOR, nullptr, 0, b, sizeof(*b),
              &bytes)
        || bytes < sizeof(*b))
    {
        printf("[!] behavior failed (err %lu)\n", GetLastError());
        free(b);
        return 1;
    }
    printf("behavior: callers=%u alerts=%llu dropped=%llu tablefull=%llu "
           "(window=%ums rd>=%u wr>=%u op>=%u)\n",
           b->OutCount, (unsigned long long)b->Alerts,
           (unsigned long long)b->Dropped,
           (unsigned long long)b->TableFull, (unsigned)b->WindowMs,
           (unsigned)b->RdThresh, (unsigned)b->WrThresh,
           (unsigned)b->OpThresh);
    for (u32_ i = 0;
         i < b->OutCount && i < SVMB_SYSAUD_BEHAVE_SLOTS; ++i)
    {
        const SVMB_SYSAUD_CALLER* c = &b->Callers[i];
        printf("  pid=%lu (%s)%s rd=%llu/%llu (peak %llu) wr=%llu/%llu "
               "(peak %llu) op=%llu/%llu\n",
               (unsigned long)c->Pid, c->Image,
               c->Alerted ? " ALERT" : "",
               (unsigned long long)c->CurrRd,
               (unsigned long long)c->Rd,
               (unsigned long long)c->PeakRd,
               (unsigned long long)c->CurrWr,
               (unsigned long long)c->Wr,
               (unsigned long long)c->PeakWr,
               (unsigned long long)c->CurrOp,
               (unsigned long long)c->Op);
    }
    // r103: the "who reads whom" rows (post-resolution convergence)
    printf("behavior: targets=%u resolved=%llu resfail=%llu resdropped=%llu "
           "ntcf-excluded=%llu targetsfull=%llu\n",
           b->TargetCount, (unsigned long long)b->Resolved,
           (unsigned long long)b->ResFail,
           (unsigned long long)b->ResDropped,
           (unsigned long long)b->ExcludedNtcf,
           (unsigned long long)b->TargetsFull);
    for (u32_ i = 0;
         i < b->TargetCount && i < SVMB_SYSAUD_TARGET_SLOTS; ++i)
    {
        const SVMB_SYSAUD_TARGET* t = &b->Targets[i];
        printf("  target: %lu -> %lu rd=%llu wr=%llu op=%llu\n",
               (unsigned long)t->CallerPid, (unsigned long)t->TargetPid,
               (unsigned long long)t->Rd, (unsigned long long)t->Wr,
               (unsigned long long)t->Op);
    }
    free(b);
    return 0;
}

// r96 R1 verification: issue CR3_READVM with a RESERVED (never committed)
// output buffer. The DevCtrl probe must refuse it cleanly with
// ERROR_INVALID_USER_BUFFER (1784) BEFORE the walk; pre-fix this pointed
// the completion copy at unmapped user memory (kernel AV / spin territory
// on this vhv). Deliberately uses a raw DeviceIoControl: the refusal path
// never takes the IOCTL gate, so a clean result returns immediately - and
// if the fix is broken the hang IS the finding.
static int CmdReadVmDeadBuf()
{
    if (!OpenDrv())
        return 1;
    SVMB_CR3_READVM in = {};
    in.Pid = (u32_)GetCurrentProcessId();
    in.Va = (unsigned long long)(ULONG_PTR)&gDevice;
    in.Size = 32;
    void* dead = VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
    if (!dead)
    {
        printf("[!] reserve failed (err %lu)", GetLastError());
        return 1;
    }
    DWORD bytes = 0;
    BOOL ok = DeviceIoControl(gDevice, SVMB_IOCTL_CR3_READVM, &in,
                              sizeof(in), dead, 0x10000, &bytes, nullptr);
    printf("readvm-deadbuf: %s (err %lu)\n",
           ok ? "UNEXPECTED SUCCESS" : "refused", GetLastError());
    VirtualFree(dead, 0, MEM_RELEASE);
    return ok ? 1 : 0;
}

// r92: raw guest-view TSC - the TscProbe verification leg pairs the
// user-mode RDTSC (offset-affected if the vhv honors TscOffset) with the
// driver's L1-context reading (immune to it); their difference IS the
// applied offset. Driver readback rides the shared gDevice + the bounded-
// wait Call() wrapper (a raw DeviceIoControl could block forever behind a
// held IOCTL gate - the r91 frozen-writer scenario - and wedge the single
// guest-op channel with it).
static int CmdRdtsc()
{
    LARGE_INTEGER qpc = {};
    unsigned long long t = __rdtsc();
    QueryPerformanceCounter(&qpc);
    printf("rdtsc=%llu (0x%llx) qpc=%lld\n", t, t, qpc.QuadPart);
    unsigned long long drv = 0, user = __rdtsc();
    DWORD bytes = 0;
    if (Call(SVMB_IOCTL_RDTSC, nullptr, 0, &drv, sizeof(drv), &bytes)
        && bytes == sizeof(drv))
    {
        long long diff = (long long)(user - drv);
        printf("drvrdtsc=%llu (0x%llx) user-drv=%lld (0x%llx)\n", drv, drv,
               diff, (unsigned long long)diff);
    }
    else
        printf("[!] drvrdtsc unavailable (err %lu)\n", GetLastError());
    return 0;
}

static int CmdCr3Protect(const wchar_t* pidW, const wchar_t* baseW,
                         const wchar_t* sizeW, const wchar_t* policyW)
{
    SVMB_CR3_PROT p = {};
    p.Action = SVMB_PROT_ACTION_REGISTER;
    p.Pid = (svmb_u32)wcstoul(pidW, nullptr, 0);
    p.Base = _wcstoui64(baseW, nullptr, 16);
    p.Size = _wcstoui64(sizeW, nullptr, 16);
    if (policyW && !_wcsicmp(policyW, L"kill"))
        p.Policy = SVMB_PROT_POLICY_KILL; // r89 plan C: bypass = death
    else if (policyW && !_wcsicmp(policyW, L"suspend"))
        p.Policy = SVMB_PROT_POLICY_SUSPEND; // r91 plan C-: bypass = freeze
    if (!p.Pid || !p.Base || !p.Size)
    {
        printf("[!] usage: cr3 protect <pid> <base-hex> <size-hex> "
               "[kill|suspend]\n");
        return 1;
    }
    if (!Call(SVMB_IOCTL_CR3_PROT, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] CR3_PROT register failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("[+] protected pid=%lu base=%llx size=%llx id=%lu oldProt=%08x"
           "%s\n",
           (unsigned long)p.Pid, (unsigned long long)p.Base,
           (unsigned long long)p.Size, (unsigned long)p.OutId,
           (unsigned long)p.OutOrigProtect,
           p.Policy == SVMB_PROT_POLICY_KILL      ? " [KILL]"
           : p.Policy == SVMB_PROT_POLICY_SUSPEND ? " [SUSPEND]"
                                                  : "");
    return 0;
}

static int CmdCr3Unprotect(const wchar_t* pidW, const wchar_t* baseW)
{
    SVMB_CR3_PROT p = {};
    p.Action = SVMB_PROT_ACTION_UNPROTECT;
    p.Pid = (svmb_u32)wcstoul(pidW, nullptr, 0);
    p.Base = baseW ? _wcstoui64(baseW, nullptr, 16) : 0;
    if (!Call(SVMB_IOCTL_CR3_PROT, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] CR3_PROT unprotect failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("[%s] unprotected %lu region(s) for pid=%lu\n",
           p.OutId ? "+" : "!", (unsigned long)p.OutId,
           (unsigned long)p.Pid);
    return p.OutId ? 0 : 1;
}

// r78: unprotect by the id that REGISTER returned (multi-target workflow)
static int CmdCr3UnprotectId(const wchar_t* idW)
{
    SVMB_CR3_PROT p = {};
    p.Action = SVMB_PROT_ACTION_UNPROTECT;
    p.InUnprotectId = (svmb_u32)wcstoul(idW, nullptr, 0);
    if (!p.InUnprotectId)
    {
        printf("[!] usage: cr3 unprotectid <region-id>\n");
        return 1;
    }
    if (!Call(SVMB_IOCTL_CR3_PROT, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] CR3_PROT unprotect failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("[%s] unprotected %lu region(s) for id=%lu\n",
           p.OutId ? "+" : "!", (unsigned long)p.OutId,
           (unsigned long)p.InUnprotectId);
    return p.OutId ? 0 : 1;
}

static int CmdCr3Regions()
{
    SVMB_CR3_PROT p = {};
    p.Action = SVMB_PROT_ACTION_LIST;
    if (!Call(SVMB_IOCTL_CR3_PROT, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] CR3_PROT list failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("protected regions: %lu\n", (unsigned long)p.OutCount);
    for (ULONG i = 0; i < SVMB_PROT_MAX_ENTRIES; ++i)
    {
        if (!p.Entries[i].Base)
            break;
        // r91 P2-3: echo the LIVE policy (protect/holdpage only echo the
        // request - this is where a KILL/SUSPEND registration is verified)
        printf("  [%lu] id=%lu pid=%lu base=%llx size=%llx policy=%s\n",
               (unsigned long)i, (unsigned long)p.Entries[i].Id,
               (unsigned long)p.Entries[i].Pid,
               (unsigned long long)p.Entries[i].Base,
               (unsigned long long)p.Entries[i].Size,
               p.Entries[i].Policy == SVMB_PROT_POLICY_KILL      ? "KILL"
               : p.Entries[i].Policy == SVMB_PROT_POLICY_SUSPEND ? "SUSPEND"
                                                                 : "alert");
    }
    return 0;
}

// ---- r87 hypervisor-level memory read (CR3_READVM) --------------------

// known-content buffer the driver can read back through its own page-table
// walk; .rdata, touched on every use so it is always resident
static const char g_readvmMarker[32] = "SVMB-R87-MEMREAD-MARKER-0123";

// cr3 marker: print this process's pid + marker VA (cross-process source)
static int CmdCr3Marker()
{
    volatile const char* p = g_readvmMarker;
    char touched = 0;
    for (size_t i = 0; i < sizeof(g_readvmMarker); ++i)
        touched = (char)(touched + p[i]);
    (void)touched;
    printf("marker pid=%lu va=%llx bytes=%s\n",
           (unsigned long)GetCurrentProcessId(),
           (unsigned long long)(uintptr_t)g_readvmMarker, g_readvmMarker);
    return 0;
}

// cr3 readvm <pid> <va-hex> <size> [out.bin] [faultin]
static int CmdCr3ReadVm(const wchar_t* pidW, const wchar_t* vaW,
                        const wchar_t* sizeW, const wchar_t* fileW,
                        const wchar_t* flagW)
{
    svmb_u32 pid = (svmb_u32)wcstoul(pidW, nullptr, 0);
    svmb_u64 va = _wcstoui64(vaW, nullptr, 16);
    svmb_u32 size = (svmb_u32)wcstoul(sizeW, nullptr, 10); // base 10: a
                                                           // leading 0 must
                                                           // not go octal
    // accept `faultin` / `guestview` keywords in either optional slot
    // (r88 lesson: a keyword in the file slot was written to disk as a
    // 16-byte "payload" instead of enabling the mode)
    const wchar_t* file = fileW;
    svmb_u32 inFlags = 0;
    const wchar_t* kws[2] = {fileW, flagW};
    for (const wchar_t* kw : kws)
    {
        if (!kw)
            continue;
        if (!_wcsicmp(kw, L"faultin"))
            inFlags |= SVMB_READVM_F_FAULTIN;
        else if (!_wcsicmp(kw, L"guestview"))
            inFlags |= SVMB_READVM_F_GUESTVIEW;
        else
            file = kw;
    }
    if (!pid || !va || !size || size > SVMB_READVM_MAX)
    {
        printf("[!] usage: cr3 readvm <pid> <va-hex> <size<=65536> "
               "[out.bin] [faultin] [guestview]\n");
        return 1;
    }
    // header + payload in one buffered buffer
    SVMB_CR3_READVM* rv =
        (SVMB_CR3_READVM*)HeapAlloc(GetProcessHeap(), 0,
                                    sizeof(*rv) + SVMB_READVM_MAX);
    if (!rv)
    {
        printf("[!] oom\n");
        return 1;
    }
    // r89 LAW: memset the WHOLE allocation in user mode (kernel-mode first
    // touch of a demand-zero page wedges this vhv - see readvme2e note)
    memset(rv, 0, sizeof(*rv) + SVMB_READVM_MAX);
    rv->Pid = pid;
    rv->Va = va;
    rv->Size = size;
    rv->InFlags = inFlags;
    if (!Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + size,
              rv, sizeof(*rv) + size))
    {
        printf("[!] CR3_READVM failed (err %lu)\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, rv);
        return 1;
    }
    printf("[+] readvm pid=%lu va=%llx size=%u resolved=%u flags=%x%s%s\n",
           (unsigned long)rv->Pid, (unsigned long long)rv->Va,
           (unsigned)rv->Size, (unsigned)rv->OutResolved,
           (unsigned)rv->OutFlags,
           (rv->OutFlags & SVMB_READVM_F_PARTIAL) ? " (PARTIAL)" : "",
           inFlags ? (inFlags & SVMB_READVM_F_GUESTVIEW
                          ? " [guestview]"
                          : " [faultin]")
                    : "");
    const unsigned char* payload = (const unsigned char*)(rv + 1);
    const u32_ dump = size < 256 ? size : 256;
    for (u32_ i = 0; i < dump; i += 16)
    {
        printf("  %04x:", (unsigned)i);
        for (u32_ j = 0; j < 16 && i + j < dump; ++j)
            printf(" %02x", payload[i + j]);
        printf("\n");
    }
    if (file)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, file, L"wb") != 0 || !f)
        {
            printf("[!] cannot open %ls for write\n", file);
            HeapFree(GetProcessHeap(), 0, rv);
            return 1;
        }
        fwrite(payload, 1, size, f);
        fclose(f);
        printf("[+] payload -> %ls\n", file);
    }
    HeapFree(GetProcessHeap(), 0, rv);
    return 0;
}

// cr3 readvm-self: the driver walks THIS process's page tables and reads
// the marker back - must byte-match local memory (PASS/FAIL exit code)
static int CmdCr3ReadVmSelf()
{
    volatile const char* p = g_readvmMarker;
    char touched = 0;
    for (size_t i = 0; i < sizeof(g_readvmMarker); ++i)
        touched = (char)(touched + p[i]);
    (void)touched;

    svmb_u32 pid = GetCurrentProcessId();
    svmb_u64 va = (uintptr_t)g_readvmMarker;
    SVMB_CR3_READVM* rv =
        (SVMB_CR3_READVM*)HeapAlloc(GetProcessHeap(), 0,
                                    sizeof(*rv) + SVMB_READVM_MAX);
    if (!rv)
    {
        printf("[!] oom\n");
        return 1;
    }
    // r89 LAW: whole-allocation user-mode memset (see readvme2e note)
    memset(rv, 0, sizeof(*rv) + SVMB_READVM_MAX);
    rv->Pid = pid;
    rv->Va = va;
    rv->Size = sizeof(g_readvmMarker);
    if (!Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + rv->Size,
              rv, sizeof(*rv) + rv->Size))
    {
        printf("[!] CR3_READVM failed (err %lu)\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, rv);
        return 1;
    }
    int cmp = memcmp((const void*)(rv + 1), g_readvmMarker,
                     sizeof(g_readvmMarker));
    printf("[%s] readvm-self pid=%lu va=%llx resolved=%u flags=%x "
           "cmp=%s\n",
           (rv->OutResolved == rv->Size && !rv->OutFlags && !cmp) ? "+"
                                                                  : "!",
           (unsigned long)pid, (unsigned long long)va,
           (unsigned)rv->OutResolved, (unsigned)rv->OutFlags,
           !cmp ? "MATCH" : "DIFF");
    HeapFree(GetProcessHeap(), 0, rv);
    return (rv->OutResolved == rv->Size && !rv->OutFlags && !cmp) ? 0 : 1;
}

// cr3 readvm-hole: read a reserved-but-never-committed range - the walk
// must fail cleanly (PARTIAL, resolved=0), never bugcheck
static int CmdCr3ReadVmHole()
{
    PVOID hole = VirtualAlloc(nullptr, 0x100000, MEM_RESERVE, PAGE_NOACCESS);
    if (!hole)
    {
        printf("[!] VirtualAlloc(MEM_RESERVE) failed (err %lu)\n",
               GetLastError());
        return 1;
    }
    // header + payload in one buffer (the driver rejects a header-only
    // output buffer when Size > payload capacity - by design)
#pragma pack(push, 1)
    struct
    {
        SVMB_CR3_READVM h;
        unsigned char payload[32];
    } buf = {};
#pragma pack(pop)
    buf.h.Pid = GetCurrentProcessId();
    buf.h.Va = (uintptr_t)hole;
    buf.h.Size = sizeof(buf.payload);
    BOOL ok = Call(SVMB_IOCTL_CR3_READVM, &buf, sizeof(buf), &buf,
                   sizeof(buf));
    printf("[%s] readvm-hole va=%llx ok=%d resolved=%u flags=%x\n",
           (ok && buf.h.OutResolved == 0
            && (buf.h.OutFlags & SVMB_READVM_F_PARTIAL)) ? "+"
                                                         : "!",
           (unsigned long long)buf.h.Va, (int)ok,
           (unsigned)buf.h.OutResolved, (unsigned)buf.h.OutFlags);
    VirtualFree(hole, 0, MEM_RELEASE);
    return (ok && buf.h.OutResolved == 0
            && (buf.h.OutFlags & SVMB_READVM_F_PARTIAL)) ? 0 : 1;
}

// r89 standalone probe: write the magic into an existing region (kernel MDL
// write, bypasses L1) - the trip generator for the kill-policy E2E. The
// write runs attached to the OWNER, so the trip's CR3 attributes to the
// owner process: with Policy=KILL the driver terminates the owner.
// r91 plan C-: list / release suspended writers. UNGATED on the driver
// side by design - a suspended writer holds the gate inside its frozen
// IOCTL, so these two must work from outside the freeze.
static int CmdCr3Suspended()
{
    SVMB_SUSP s = {};
    s.Op = SVMB_SUSP_OP_LIST;
    if (!Call(SVMB_IOCTL_SUSP_LIST, &s, sizeof(s), &s, sizeof(s)))
    {
        printf("[!] SUSP list failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("suspend available: %s\n", s.SuspAvailable ? "yes" : "NO");
    printf("suspended writers: %lu\n", (unsigned long)s.OutCount);
    for (ULONG i = 0; i < SVMB_SUSP_MAX_ENTRIES; ++i)
    {
        if (!s.Entries[i].Pid)
            continue;
        const unsigned long rid = (unsigned long)s.Entries[i].RegionId;
        printf("  pid=%lu region=%lu%s tick=%llu image=%s\n",
               (unsigned long)s.Entries[i].Pid, rid,
               rid == 0xFFFFFFFFul ? " (SYSALERT)" : "",
               (unsigned long long)s.Entries[i].Tick,
               s.Entries[i].Image);
    }
    return 0;
}

static int CmdCr3Resume(const wchar_t* pidW)
{
    SVMB_SUSP s = {};
    if (pidW && _wcsicmp(pidW, L"all") != 0)
    {
        s.Op = SVMB_SUSP_OP_RESUME_PID;
        s.Pid = (svmb_u32)wcstoul(pidW, nullptr, 0);
        if (!s.Pid)
        {
            printf("[!] usage: cr3 resume <pid|all>\n");
            return 1;
        }
    }
    else
        s.Op = SVMB_SUSP_OP_RESUME_ALL;
    if (!Call(SVMB_IOCTL_SUSP_RESUME, &s, sizeof(s), &s, sizeof(s)))
    {
        printf("[!] SUSP resume failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("[%s] resumed %lu writer(s)\n",
           s.OutCount ? "+" : "!", (unsigned long)s.OutCount);
    return s.OutCount ? 0 : 1;
}

static int CmdCr3Probe(const wchar_t* pidW, const wchar_t* baseW,
                       const wchar_t* sizeW)
{
    SVMB_CR3_PROBE p = {};
    p.Pid = (svmb_u32)wcstoul(pidW, nullptr, 0);
    p.Base = _wcstoui64(baseW, nullptr, 16);
    p.Size = sizeW ? (svmb_u32)wcstoul(sizeW, nullptr, 0) : 8;
    if (!p.Pid || !p.Base || !p.Size || p.Size > 0x1000)
    {
        printf("[!] usage: cr3 probe <pid> <base-hex> [size<=4096]\n");
        return 1;
    }
    if (!Call(SVMB_IOCTL_CR3_PROBE, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] probe failed (err %lu, written=%lu)\n", GetLastError(),
               (unsigned long)p.OutWritten);
        return 1;
    }
    printf("[+] probe wrote %lu bytes, magic=%llx\n",
           (unsigned long)p.OutWritten,
           (unsigned long long)p.OutMagic);
    return 0;
}

// r89 bisect switch: 1 = all legs, 0 = L1 only (the wedge bisect)
#define READVME2E_LEGS_L2 1   // bisect: faultin leg
#define READVME2E_LEGS_REST 1 // bisect: L3-L7

// r88 E2E: strategy A vs B on a committed-but-untouched range (A: clean
// PARTIAL resolved=0; B: fault-in spans two pages -> resolved=full), plus
// the hardening rejections (kernel va, range wrap, bad in-flags) and a
// NOACCESS page under fault-in (SEH path must ride it out, never crash).
static int CmdCr3ReadVmE2E()
{
    setvbuf(stdout, nullptr, _IONBF, 0); // unbuffered: a wedge shows its leg
    const svmb_u32 pid = GetCurrentProcessId();
    SVMB_CR3_READVM* rv =
        (SVMB_CR3_READVM*)HeapAlloc(GetProcessHeap(), 0,
                                    sizeof(*rv) + SVMB_READVM_MAX);
    if (!rv)
    {
        printf("[!] oom\n");
        return 1;
    }
    // r89 LAW: memset the WHOLE buffer in USER mode before the IOCTL. A
    // kernel-mode first touch of a demand-zero page of this buffer spins
    // forever on this vhv (the buffered-IOCTL completion copy runs in a
    // special kernel APC; kd: KiPageFault loop, SEH cannot intercept).
    memset(rv, 0, sizeof(*rv) + SVMB_READVM_MAX);
    bool pass = true;

    // L1: committed, never touched -> strategy A sees it as not-present
    BYTE* pg = (BYTE*)VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_READWRITE);
    if (!pg)
    {
        printf("[!] VirtualAlloc failed (err %lu)\n", GetLastError());
        HeapFree(GetProcessHeap(), 0, rv);
        return 1;
    }
    memset(rv, 0, sizeof(*rv) + 32);
    rv->Pid = pid;
    rv->Va = (uintptr_t)pg;
    rv->Size = 32;
    bool ok = Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + 32, rv,
                   sizeof(*rv) + 32) != FALSE;
    bool l1 = ok && rv->OutResolved == 0
              && (rv->OutFlags & SVMB_READVM_F_PARTIAL);
    printf("[%s] L1 strategy-A untouched: resolved=%u flags=%x\n",
           l1 ? "+" : "!", (unsigned)rv->OutResolved,
           (unsigned)rv->OutFlags);
    pass = pass && l1;

#if READVME2E_LEGS_L2
    // L2: fault-in across two untouched pages -> resolved=full (bytes are
    // the committed zeros the local view sees once faulted)
    memset(rv, 0, sizeof(*rv) + 0x1008);
    rv->Pid = pid;
    rv->Va = (uintptr_t)pg;
    rv->Size = 0x1008;
    rv->InFlags = SVMB_READVM_F_FAULTIN;
    ok = Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + 0x1008, rv,
              sizeof(*rv) + 0x1008) != FALSE;
    volatile BYTE* vpg = pg;
    BYTE touched = 0;
    touched = (BYTE)(touched + vpg[0] + vpg[0x1000]);
    bool l2 = ok && rv->OutResolved == 0x1008 && !rv->OutFlags;
    printf("[%s] L2 faultin two pages: resolved=%u flags=%x (touched=%02x)\n",
           l2 ? "+" : "!", (unsigned)rv->OutResolved,
           (unsigned)rv->OutFlags, (unsigned)touched);
    pass = pass && l2;
#endif // READVME2E_LEGS_L2

#if READVME2E_LEGS_REST
    // L3: kernel va must be refused outright
    memset(rv, 0, sizeof(*rv) + 16);
    rv->Pid = pid;
    rv->Va = 0xFFFF800000000000ull;
    rv->Size = 16;
    ok = Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + 16, rv,
              sizeof(*rv) + 16) != FALSE;
    printf("[%s] L3 kernel-va rejected: ok=%d\n", !ok ? "+" : "!", (int)ok);
    pass = pass && !ok;

    // L3b: NON-canonical va must be refused too - it passes an IsKernelVa
    // probe but its PML4 index aliases the kernel half (r88 P1-1 regression
    // leg; the driver range-checks against the user top instead)
    memset(rv, 0, sizeof(*rv) + 16);
    rv->Pid = pid;
    rv->Va = 0x8000000000000000ull;
    rv->Size = 16;
    ok = Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + 16, rv,
              sizeof(*rv) + 16) != FALSE;
    printf("[%s] L3b noncanon-va rejected: ok=%d\n", !ok ? "+" : "!",
           (int)ok);
    pass = pass && !ok;

    // L4: u64 range past the user top must be refused
    memset(rv, 0, sizeof(*rv) + 16);
    rv->Pid = pid;
    rv->Va = 0x00007FFFFFFEFFF0ull; // +16 crosses 0x7FFFFFFEFFFF
    rv->Size = 16;
    ok = Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + 16, rv,
              sizeof(*rv) + 16) != FALSE;
    printf("[%s] L4 past-user-top rejected: ok=%d\n", !ok ? "+" : "!",
           (int)ok);
    pass = pass && !ok;

    // L5: unknown in-flags must be refused
    memset(rv, 0, sizeof(*rv) + 16);
    rv->Pid = pid;
    rv->Va = (uintptr_t)pg;
    rv->Size = 16;
    rv->InFlags = 0xFFFE;
    ok = Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + 16, rv,
              sizeof(*rv) + 16) != FALSE;
    printf("[%s] L5 bad-flags rejected: ok=%d\n", !ok ? "+" : "!", (int)ok);
    pass = pass && !ok;

    // L6: NOACCESS page under fault-in - the SEH guard must swallow the
    // touch fault; the IOCTL itself still completes (residency varies).
    // r89: touch FIRST (user mode) so the page is resident before the
    // NOACCESS flip - a demand-zero kernel probe spins on this vhv (the
    // r89 wedge), a protection AV on a resident page is clean.
    BYTE* na = (BYTE*)VirtualAlloc(nullptr, 0x1000,
                                   MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool l6 = false;
    DWORD oldProt = 0;
    if (na)
        *(volatile BYTE*)na = 0x77;
    if (na && VirtualProtect(na, 0x1000, PAGE_NOACCESS, &oldProt))
    {
        memset(rv, 0, sizeof(*rv) + 32);
        rv->Pid = pid;
        rv->Va = (uintptr_t)na;
        rv->Size = 32;
        rv->InFlags = SVMB_READVM_F_FAULTIN;
        ok = Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + 32, rv,
                  sizeof(*rv) + 32) != FALSE;
        l6 = ok; // survived = pass; resolved value is PTE-state dependent
        printf("[%s] L6 noaccess+faultin survived: ok=%d resolved=%u "
               "flags=%x\n",
               l6 ? "+" : "!", (int)ok, (unsigned)rv->OutResolved,
               (unsigned)rv->OutFlags);
    }
    else
        printf("[!] L6 setup failed (err %lu)\n", GetLastError());
    pass = pass && l6;
    if (na)
        VirtualFree(na, 0, MEM_RELEASE);
    VirtualFree(pg, 0, MEM_RELEASE);

    // L7: GUESTVIEW on an UNHOOKED user page must be a byte-exact no-op
    // (today only kernel pages carry NPT hooks, so this pins the
    // integration path; a hooked-page view test needs a user-page hook -
    // not installable until then, documented in r89)
    {
#pragma pack(push, 1)
        struct
        {
            SVMB_CR3_READVM h;
            unsigned char payload[32];
        } gv = {}, gt = {};
#pragma pack(pop)
        gv.h.Pid = gt.h.Pid = pid;
        gv.h.Va = gt.h.Va = (uintptr_t)g_readvmMarker;
        gv.h.Size = gt.h.Size = sizeof(gv.payload);
        gv.h.InFlags = SVMB_READVM_F_GUESTVIEW;
        bool okG = Call(SVMB_IOCTL_CR3_READVM, &gv, sizeof(gv), &gv,
                        sizeof(gv)) != FALSE;
        bool okT = Call(SVMB_IOCTL_CR3_READVM, &gt, sizeof(gt), &gt,
                        sizeof(gt)) != FALSE;
        bool same = !memcmp(gv.payload, gt.payload, sizeof(gv.payload));
        bool l7 = okG && okT && same && gt.h.OutResolved == sizeof(gt.payload);
        printf("[%s] L7 guestview-unhooked noop: ok=%d,%d equal=%d\n",
               l7 ? "+" : "!", (int)okG, (int)okT, (int)same);
        pass = pass && l7;
    }
#endif // READVME2E_LEGS_REST

    HeapFree(GetProcessHeap(), 0, rv);
    printf(pass ? "[+] readvme2e PASS\n" : "[-] readvme2e FAIL\n");
    return pass ? 0 : 1;
}

// r75 E2E blocking proof inside this process: alloc -> write OK -> protect
// (L1 readonly via the driver) -> write must fault; read still OK;
// unprotect -> write OK again.
static int CmdCr3BlockSelf()
{
    DWORD pid = GetCurrentProcessId();
    PVOID p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (!p)
    {
        printf("[!] VirtualAlloc failed (err %lu)\n", GetLastError());
        return 1;
    }
    *(volatile BYTE*)p = 0x41;
    printf("[*] pre-protect write OK val=%02x\n", *(volatile BYTE*)p);

    SVMB_CR3_PROT q = {};
    q.Action = SVMB_PROT_ACTION_REGISTER;
    q.Pid = pid;
    q.Base = (svmb_u64)(ULONG_PTR)p;
    q.Size = 0x1000;
    if (!Call(SVMB_IOCTL_CR3_PROT, &q, sizeof(q), &q, sizeof(q)))
    {
        printf("[!] protect failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("[+] protected id=%lu oldProt=%08x\n", (unsigned long)q.OutId,
           (unsigned long)q.OutOrigProtect);

    volatile BYTE r = *(volatile BYTE*)p;
    printf("[*] post-protect read OK val=%02x\n", r);

    bool blocked = false;
    ULONG code = 0;
    __try
    {
        *(volatile BYTE*)p = 0x42;
    }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        blocked = true;
    }
    printf(blocked ? "[+] WRITE BLOCKED (exception %08lx)\n"
                   : "[!] WRITE NOT BLOCKED (no fault)\n",
           code);

    SVMB_CR3_PROT u = {};
    u.Action = SVMB_PROT_ACTION_UNPROTECT;
    u.Pid = pid;
    u.Base = (svmb_u64)(ULONG_PTR)p;
    if (!Call(SVMB_IOCTL_CR3_PROT, &u, sizeof(u), &u, sizeof(u)))
    {
        printf("[!] unprotect failed (err %lu)\n", GetLastError());
        return 1;
    }
    __try
    {
        *(volatile BYTE*)p = 0x43;
        printf("[*] post-unprotect write OK val=%02x\n", *(volatile BYTE*)p);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[!] post-unprotect still blocked\n");
        return 1;
    }
    VirtualFree(p, 0, MEM_RELEASE);
    printf(blocked ? "[+] blockself PASS\n" : "[-] blockself FAIL\n");
    return blocked ? 0 : 1;
}

// r76 E2E L2-bypass proof, self-contained: alloc -> write OK -> protect ->
// kernel probe (driver writes the magic through a kernel MDL mapping,
// bypassing the L1 readonly PTE) -> magic visible through the normal VA +
// RegionTrips stat must increment (L2 sensed the bypass) + a direct write
// must STILL fault (probe bypassed L1, not disabled it) -> unprotect ->
// write OK again.
static int CmdCr3ProbeE2E(const wchar_t* policyW)
{
    setvbuf(stdout, nullptr, _IONBF, 0); // land prints even if killed
    // r89/r91: `kill`/`suspend` register the region with the matching
    // response policy - the probe write below then triggers the pipeline,
    // which kills (r89) or freezes (r91) THIS process. Expected with
    // kill: the process dies around "bypass write LANDED" (no verdict
    // printed); with suspend: the process FREEZES there until an external
    // `cr3 resume` - the outer script drives list/resume and checks the
    // verdict after release.
    const bool kill = policyW && !_wcsicmp(policyW, L"kill");
    const bool suspend = policyW && !_wcsicmp(policyW, L"suspend");
    // probe write below then triggers the plan-C workitem, which terminates
    // THIS process (the probe runs attached to the owner, so the trip's CR3
    // attributes here). Expected when run with kill: this process dies
    // around the "bypass write LANDED" step and never prints a verdict -
    // the outer script verifies via "KILL policy writer" + stats instead.
    DWORD pid = GetCurrentProcessId();
    PVOID p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (!p)
    {
        printf("[!] VirtualAlloc failed (err %lu)\n", GetLastError());
        return 1;
    }
    *(volatile BYTE*)p = 0x41;
    printf("[*] pre-protect write OK val=%02x\n", *(volatile BYTE*)p);

    SVMB_CR3_PROT q = {};
    q.Action = SVMB_PROT_ACTION_REGISTER;
    q.Pid = pid;
    q.Base = (svmb_u64)(ULONG_PTR)p;
    q.Size = 0x1000;
    if (kill)
        q.Policy = SVMB_PROT_POLICY_KILL;
    else if (suspend)
        q.Policy = SVMB_PROT_POLICY_SUSPEND;
    if (!Call(SVMB_IOCTL_CR3_PROT, &q, sizeof(q), &q, sizeof(q)))
    {
        printf("[!] protect failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("[+] protected id=%lu oldProt=%08x\n", (unsigned long)q.OutId,
           (unsigned long)q.OutOrigProtect);

    SVMB_CR3_STATS s0 = {};
    if (Call(SVMB_IOCTL_CR3_STATS, nullptr, 0, &s0, sizeof(s0)))
        printf("[*] region trips before: %lu\n",
               (unsigned long)s0.RegionTrips);

    // r87 read leg: the hypervisor-level read of the SAME protected page
    // must return the true content and raise NO sensing trip (reads never
    // NPF) - the trust-root asymmetry between read and write paths.
    {
#pragma pack(push, 1)
        struct
        {
            SVMB_CR3_READVM h;
            unsigned char payload[32];
        } rv = {};
#pragma pack(pop)
        rv.h.Pid = pid;
        rv.h.Va = (svmb_u64)(ULONG_PTR)p;
        rv.h.Size = sizeof(rv.payload);
        bool readOk = Call(SVMB_IOCTL_CR3_READVM, &rv, sizeof(rv), &rv,
                           sizeof(rv)) != FALSE;
        SVMB_CR3_STATS sr = {};
        Call(SVMB_IOCTL_CR3_STATS, nullptr, 0, &sr, sizeof(sr));
        bool silent = (sr.RegionTrips == s0.RegionTrips);
        bool content = readOk && rv.h.OutResolved == sizeof(rv.payload)
                       && !rv.h.OutFlags && rv.payload[0] == 0x41;
        printf(content && silent
                   ? "[+] readvm over protected page: content OK, trips "
                     "silent\n"
                   : "[!] readvm leg: content=%d tripsDelta=%d "
                     "(resolved=%u flags=%x)\n",
               (int)content, (int)(sr.RegionTrips - s0.RegionTrips),
               (unsigned)rv.h.OutResolved, (unsigned)rv.h.OutFlags);
        if (!(content && silent))
        {
            VirtualFree(p, 0, MEM_RELEASE);
            return 1;
        }
    }

    SVMB_CR3_PROBE b = {};
    b.Pid = pid;
    b.Base = (svmb_u64)(ULONG_PTR)p;
    b.Size = 8;
    if (!Call(SVMB_IOCTL_CR3_PROBE, &b, sizeof(b), &b, sizeof(b)) ||
        !b.OutWritten)
    {
        printf("[!] probe failed (err %lu, written=%lu)\n", GetLastError(),
               (unsigned long)b.OutWritten);
        VirtualFree(p, 0, MEM_RELEASE);
        return 1;
    }

    bool landed = (*(volatile svmb_u64*)p == SVMB_PROT_PROBE_MAGIC);
    printf(landed ? "[+] bypass write LANDED (magic %llx)\n"
                  : "[!] bypass write did NOT land (val=%llx)\n",
           (unsigned long long)*(volatile svmb_u64*)p);

    SVMB_CR3_STATS s1 = {};
    bool sensed = false;
    if (Call(SVMB_IOCTL_CR3_STATS, nullptr, 0, &s1, sizeof(s1)))
    {
        sensed = s1.RegionTrips > s0.RegionTrips;
        printf(sensed ? "[+] L2 SENSED the bypass (region trips %lu -> %lu)\n"
                      : "[!] L2 did NOT sense (region trips %lu -> %lu)\n",
               (unsigned long)s0.RegionTrips, (unsigned long)s1.RegionTrips);
    }

    bool blocked = false;
    ULONG code = 0;
    __try
    {
        *(volatile BYTE*)p = 0x42;
    }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
    {
        blocked = true;
    }
    printf(blocked ? "[+] L1 STILL BLOCKS direct writes (exception %08lx)\n"
                   : "[!] L1 LOST the block (direct write succeeded)\n",
           code);

    SVMB_CR3_PROT u = {};
    u.Action = SVMB_PROT_ACTION_UNPROTECT;
    u.Pid = pid;
    u.Base = (svmb_u64)(ULONG_PTR)p;
    if (!Call(SVMB_IOCTL_CR3_PROT, &u, sizeof(u), &u, sizeof(u)))
    {
        printf("[!] unprotect failed (err %lu)\n", GetLastError());
        VirtualFree(p, 0, MEM_RELEASE);
        return 1;
    }
    __try
    {
        *(volatile BYTE*)p = 0x43;
        printf("[*] post-unprotect write OK val=%02x\n", *(volatile BYTE*)p);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        printf("[!] post-unprotect still blocked\n");
        VirtualFree(p, 0, MEM_RELEASE);
        return 1;
    }
    VirtualFree(p, 0, MEM_RELEASE);
    int rc = (landed && sensed && blocked) ? 0 : 1;
    printf(rc ? "[-] probee2e FAIL\n" : "[+] probee2e PASS\n");
    return rc;
}

// r78 multi-target demo: protect a page of THIS process, print the region
// id, then hold the page protected for <seconds> (or until killed - the
// driver's death sweep releases regions of dying owners). A clean exit
// unprotects first. r87: optional outfile receives "pid base" right after
// the protect lands, so a concurrent ctl can readvm the protected page
// while this process is still alive. r89: trailing `kill` registers the
// region with SVMB_PROT_POLICY_KILL - a bypass trip terminates THIS
// process (the sacrificial victim for the kill-policy E2E).
static int CmdCr3HoldPage(const wchar_t* secW, const wchar_t* outW,
                          const wchar_t* policyW)
{
    ULONG seconds = secW ? wcstoul(secW, nullptr, 0) : 30;
    if (!seconds || seconds > 3600)
    {
        printf("[!] usage: cr3 holdpage <seconds 1..3600> [outfile] [kill]\n");
        return 1;
    }
    DWORD pid = GetCurrentProcessId();
    PVOID p = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
    if (!p)
    {
        printf("[!] VirtualAlloc failed (err %lu)\n", GetLastError());
        return 1;
    }
    *(volatile BYTE*)p = 0x41; // fault it in - not-present pages are rejected
    SVMB_CR3_PROT q = {};
    q.Action = SVMB_PROT_ACTION_REGISTER;
    q.Pid = pid;
    q.Base = (svmb_u64)(ULONG_PTR)p;
    q.Size = 0x1000;
    if (policyW && !_wcsicmp(policyW, L"kill"))
        q.Policy = SVMB_PROT_POLICY_KILL;
    else if (policyW && !_wcsicmp(policyW, L"suspend"))
        q.Policy = SVMB_PROT_POLICY_SUSPEND;
    if (!Call(SVMB_IOCTL_CR3_PROT, &q, sizeof(q), &q, sizeof(q)))
    {
        printf("[!] protect failed (err %lu)\n", GetLastError());
        VirtualFree(p, 0, MEM_RELEASE);
        return 1;
    }
    printf("[*] holdpage pid=%lu base=%llx id=%lu held %lus%s "
           "(kill me to exercise the death sweep)\n",
           (unsigned long)pid, (unsigned long long)q.Base,
           (unsigned long)q.OutId, seconds,
           q.Policy == SVMB_PROT_POLICY_KILL      ? " [KILL]"
           : q.Policy == SVMB_PROT_POLICY_SUSPEND ? " [SUSPEND]"
                                                  : "");
    fflush(stdout);
    if (outW)
    {
        FILE* hf = nullptr;
        if (_wfopen_s(&hf, outW, L"w") == 0 && hf)
        {
            fprintf(hf, "pid=%lu base=%llx\n", (unsigned long)pid,
                    (unsigned long long)q.Base);
            fclose(hf);
        }
        else
            printf("[!] cannot write %ls\n", outW);
    }
    Sleep(seconds * 1000);
    SVMB_CR3_PROT u = {};
    u.Action = SVMB_PROT_ACTION_UNPROTECT;
    u.Pid = pid;
    u.Base = (svmb_u64)(ULONG_PTR)p;
    if (!Call(SVMB_IOCTL_CR3_PROT, &u, sizeof(u), &u, sizeof(u)))
    {
        VirtualFree(p, 0, MEM_RELEASE);
        return 1;
    }
    printf("[*] holdpage exit (unprotected)\n");
    VirtualFree(p, 0, MEM_RELEASE);
    return 0;
}

static int CmdCr3Watch(const wchar_t* nameW, const wchar_t* keyW,
                       const wchar_t* wmArg)
{
    SVMB_CR3_CONFIG cfg = {};
    if (wmArg && !_wcsicmp(wmArg, L"wm"))
    {
        cfg.EnableWriteMonitor = 1;
        printf("[!] write monitor armed: CR3-WRITE intercept is the r43 "
               "death-spiral hazard on this vhv\n");
    }
    else if (wmArg && !_wcsicmp(wmArg, L"pv"))
    {
        // r57: carry the per-core process view on a name watch so the
        // sentinel (dual-view armed) sees in-target writes
        cfg.EnableProcessView = 1;
    }
    if (keyW)
    {
        wchar_t* end = nullptr;
        cfg.XorKey = wcstoull(keyW, &end, 16);
        if (!end || end == keyW)
        {
            printf("[!] bad xor key (want hex, e.g. 0xDEADC0DE)\n");
            return 1;
        }
        // r37: do NOT set EnableReadSpoof here. EnableReadSpoof is the
        // GLOBAL spoof - every CR3 read in the system would return
        // real^key, corrupting whatever the kernel does with the value
        // (live-locked the guest during M4-C). The targeted path in
        // Cr3ResolveRead (real == TargetCr3 -> real^key) needs only the
        // key and the armed target, both of which this config provides.
        cfg.EnableReadSpoof = 0;
    }
    // narrow to the ascii basename the seed table stores (15 chars max) -
    // strip any path the caller prefixed
    const wchar_t* base = nameW;
    for (const wchar_t* p = nameW; *p; ++p)
        if (*p == L'\\' || *p == L'/')
            base = p + 1;
    for (u32_ i = 0; i < 15 && base[i]; ++i)
        cfg.TargetImage[i] = (char)base[i];
    if (!cfg.TargetImage[0])
    {
        printf("[!] empty image name\n");
        return 1;
    }
    if (!Call(SVMB_IOCTL_CR3_CONFIG, &cfg, sizeof(cfg), nullptr, 0))
    {
        printf("[!] CR3_CONFIG failed (err %lu)\n", GetLastError());
        return 1;
    }
    wprintf(L"cr3 watch: %s armed%s (key=%llx)\n", base,
            keyW ? L", read spoof on" : L", monitor only",
            (unsigned long long)cfg.XorKey);
    return 0;
}

static int CmdCr3Unwatch()
{
    SVMB_CR3_CONFIG cfg = {};
    if (!Call(SVMB_IOCTL_CR3_CONFIG, &cfg, sizeof(cfg), nullptr, 0))
    {
        printf("[!] CR3_CONFIG failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("cr3 watch: cleared\n");
    return 0;
}

// r42 M4-F stage 8: in-target spoof self-check. This NEW svmbctl process
// was created AFTER `cr3 watch svmbctl.exe <key>` armed, so the seed
// callback marked it watched - its own __readcr3() must return
// truth ^ key (truth = real CR3 read from the VMCB by the probe itself).
// With no key: this process is not watched, seen must equal truth.
static int CmdCr3Self(const wchar_t* keyW)
{
    unsigned long long key = 0;
    if (keyW)
    {
        wchar_t* end = nullptr;
        key = wcstoull(keyW, &end, 16);
        if (!end || end == keyW)
        {
            printf("[!] bad xor key (want hex)\n");
            return 1;
        }
    }

    SVMB_NPT_PROBE p = {};
    p.Stage = 8;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] NPT_PROBE s8 failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("cr3self: seen=%llx truth=%llx key=%llx\n",
           (unsigned long long)p.Cr3Value,
           (unsigned long long)p.VmcbNcr3, key);
    if (p.VmcbNcr3 == 0)
    {
        printf("[!] cr3self: no VMCB truth (hypervisor not running?)\n");
        return 1;
    }
    unsigned long long expect = key ? (p.VmcbNcr3 ^ key) : p.VmcbNcr3;
    if (p.Cr3Value == expect)
    {
        printf("%s cr3self: %s\n", "[+]",
               key ? "watched - in-target read is spoofed (seen == truth^key)"
                   : "unwatched - read is real (seen == truth)");
        return 0;
    }
    printf("[!] cr3self: seen != expect (%llx)\n",
           (unsigned long long)expect);
    return 1;
}

static const char* DbgEvtName(u32_ type)
{
    switch (type)
    {
    case SvmbDbgEvtBreakpoint: return "bp";
    case SvmbDbgEvtSingleStep: return "step";
    case SvmbDbgEvtDebugReg: return "dr";
    case SvmbDbgEvtInvalidOpcode: return "ud";
    case SvmbDbgEvtProcessWatch: return "watch";
    case SvmbDbgEvtViewActivate: return "view";
    case SvmbDbgEvtSentinelTrip: return "sentinel";
    default: return "?";
    }
}

static int CmdDbgEvents()
{
    SVMB_DBG_EVENT_BUFFER buf = {};
    if (!Call(SVMB_IOCTL_DBG_EVENTS, nullptr, 0, &buf, sizeof(buf)))
    {
        printf("[!] DBG_EVENTS failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("dbg events: %lu (lost %lu)\n", (unsigned long)buf.Count,
           (unsigned long)buf.LostCount);
    for (u32_ i = 0; i < buf.Count; ++i)
        printf("  %-5s core=%lu rip=%llx cr3=%llx extra=%llx\n",
               DbgEvtName(buf.Events[i].Type),
               (unsigned long)buf.Events[i].Core,
               (unsigned long long)buf.Events[i].Rip,
               (unsigned long long)buf.Events[i].Cr3,
               (unsigned long long)buf.Events[i].Extra);
    return 0;
}

static int CmdSerial(const wchar_t* mode)
{
    SVMB_HV_CONTROL c = {};
    c.Action = 3; // serial mirror toggle
    if (!_wcsicmp(mode, L"on"))
        c.RepeatCount = 1;
    else if (!_wcsicmp(mode, L"off"))
        c.RepeatCount = 0;
    else
    {
        printf("[!] serial mode: on|off\n");
        return 1;
    }
    if (!Call(SVMB_IOCTL_HV_CONTROL, &c, sizeof(c), nullptr, 0))
    {
        printf("[!] serial %ls failed (err %lu)\n", mode, GetLastError());
        return 1;
    }
    printf("[+] serial mirror %ls (COM1 output enabled=%lu)\n", mode, c.RepeatCount);
    return 0;
}

static int CmdMod(const wchar_t* action, const char* name)
{
    SVMB_MODULE_CTL c = {};
    if (!_wcsicmp(action, L"attach"))
        c.Action = 0;
    else if (!_wcsicmp(action, L"detach"))
        c.Action = 1;
    else if (!_wcsicmp(action, L"list"))
        c.Action = 2;
    else
    {
        printf("[!] mod action: attach|detach|list\n");
        return 1;
    }

    if (c.Action == 2)
    {
        SVMB_MODULE_LIST list = {};
        if (!Call(SVMB_IOCTL_MODULE_CTL, &c, sizeof(c), &list, sizeof(list)))
        {
            printf("[!] mod list failed (err %lu)\n", GetLastError());
            return 1;
        }
        printf("modules (%u):\n", list.Count);
        for (u32_ i = 0; i < list.Count; ++i)
            printf("  [x] %s\n", list.Modules[i].Name);
        return 0;
    }

    if (name)
        strncpy_s(c.Name, name, _TRUNCATE);
    if (!Call(SVMB_IOCTL_MODULE_CTL, &c, sizeof(c), &c, sizeof(c)))
    {
        printf("[!] mod %ls failed (err %lu)\n", action, GetLastError());
        return 1;
    }
    printf("[+] mod %ls ok\n", action);
    return 0;
}

static void CpuidLeaf(u32_ leaf, u32_ out[4])
{
    int r[4] = {};
    __cpuid(r, (int)leaf);
    out[0] = (u32_)r[0];
    out[1] = (u32_)r[1];
    out[2] = (u32_)r[2];
    out[3] = (u32_)r[3];
}

static int CmdDemo()
{
    // 1) probe before: hypervisor not required to answer
    u32_ before[4];
    CpuidLeaf(0x40000100, before);
    printf("cpuid 0x40000100 before: %08X %08X %08X %08X\n", before[0], before[1], before[2], before[3]);

    // 2) attach the demo module
    if (CmdMod(L"attach", "demo_cpuid"))
        return 1;

    // 3) probe after: module answers with 'SVMB' signature
    u32_ after[4];
    CpuidLeaf(0x40000100, after);
    printf("cpuid 0x40000100 after : %08X %08X %08X %08X\n", after[0], after[1], after[2], after[3]);
    bool ok = after[0] == ('B' << 24 | 'M' << 16 | 'V' << 8 | 'S');
    printf("%s demo module exit handler %s\n", ok ? "[+]" : "[!]",
           ok ? "is live" : "NOT working");

    // 4) detach and verify it goes away
    CmdMod(L"detach", "demo_cpuid");
    u32_ gone[4];
    CpuidLeaf(0x40000100, gone);
    printf("cpuid 0x40000100 detach: %08X %08X %08X %08X\n", gone[0], gone[1], gone[2], gone[3]);
    return ok ? 0 : 1;
}

static int CmdNptProbe(DWORD stage, DWORD iters)
{
    SVMB_NPT_PROBE p = {};
    p.Stage = stage;
    p.Iterations = iters;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] NPT_PROBE failed (err %lu)\n", GetLastError());
        return 1;
    }
    if (stage == 12)
    {
        // r44 N2: deny-path exercise. Debug: survived + exit fingerprint
        // (~257: the NPF plus the spin-breaker re-executes). Release: this
        // line is never reached - the deny bugchecks 'SVMB'+1.
        printf("npf deny probe: survived, exitDelta=%llu target=%llx\n",
               (unsigned long long)p.ExitDelta,
               (unsigned long long)p.TargetVa);
        return 0;
    }
    if (stage == 16)
    {
        // r54: ProcView content differentiation. While the policy target is
        // current, the swapped GPA serves the hidden page; after revert the
        // real page is back. PASS = both readbacks exact.
        unsigned long long hid = 0x22222222u, rel = 0x11111111u;
        printf("view content: hidden-read=%08llx (expect 22222222) real-read=%08llx (expect 11111111)\n",
               (unsigned long long)p.DetourHits,
               (unsigned long long)p.OrigHitsAfter);
        bool ok = p.DetourHits == hid && p.OrigHitsAfter == rel;
        printf("%s view content swap %s\n", ok ? "[+]" : "[!]",
               ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    if (stage == 13)
    {
        // r45 N2 rework: isolated executable stub page (1-byte RET) NX'd in
        // the NPT view. Debug PASS = survived with exitDelta == 257
        // (1 fetch NPF + NPF_SPIN_BREAK re-executes, then advance skips the
        // stub). Release never returns: first deny bugchecks 'SVMB'+1.
        unsigned long long expected = 257; // 1 + NPF_SPIN_BREAK
        printf("npf deny isolated: exitDelta=%llu (expect %llu)\n",
               (unsigned long long)p.ExitDelta, expected);
        bool ok = p.ExitDelta == expected;
        printf("%s N2 isolated deny %s\n", ok ? "[+]" : "[!]",
               ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    if (stage == 14)
    {
        // r46 sentinel primitive: W-denied leaf, two trips.
        // PASS = trip1 257 (spin-break + resolve), byte0 committed,
        //        trip2 1 (re-arm resolves immediately), byte1 committed.
        unsigned long long e1 = 257; // 1 + NPF_SPIN_BREAK
        printf("sentinel probe: trip1=%llu (expect %llu) b0=%llx "
               "trip2=%llu (expect 1) b1=%llx\n",
               (unsigned long long)p.ExitDelta, e1,
               (unsigned long long)p.DetourHits,
               (unsigned long long)p.OrigHitsHooked,
               (unsigned long long)p.OrigHitsAfter);
        bool ok = p.ExitDelta == e1 && p.DetourHits == 0x5A5A5A5A5A5A5A5Aull &&
                  p.OrigHitsHooked == 1 &&
                  p.OrigHitsAfter == 0xA5A5A5A5A5A5A5A5ull;
        printf("%s sentinel primitive %s\n", ok ? "[+]" : "[!]",
               ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    if (stage == 0)
    {
        printf("npf probe: detour=%llu origWhileHooked=%llu origAfter=%llu "
               "exitDelta=%llu target=%llx\n",
               (unsigned long long)p.DetourHits,
               (unsigned long long)p.OrigHitsHooked,
               (unsigned long long)p.OrigHitsAfter,
               (unsigned long long)p.ExitDelta,
               (unsigned long long)p.TargetVa);
        bool ok = p.DetourHits == iters && p.OrigHitsHooked == 0 &&
                  p.OrigHitsAfter == iters && p.ExitDelta >= 2;
        printf("%s NPF slide %s\n", ok ? "[+]" : "[!]",
               ok ? "PASS" : "FAIL");
        return ok ? 0 : 1;
    }
    printf("view probe: pml4=%llx vmcbNcr3=%llx\n",
           (unsigned long long)p.Pml4Pa, (unsigned long long)p.VmcbNcr3);
    bool ok = p.Pml4Pa != 0 && p.Pml4Pa == p.VmcbNcr3;
    printf("%s view switch %s\n", ok ? "[+]" : "[!]", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static int CmdNpfDump()
{
    SVMB_NPT_PROBE p = {};
    p.Stage = 2;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] NPT_PROBE s2 failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("target=%llx page=%llx hidden=%llx tramp=%llx off=%lx stolen=%lu\n\n",
           (unsigned long long)p.TargetVa, (unsigned long long)p.PagePa,
           (unsigned long long)p.HiddenPa, (unsigned long long)p.TrampPa,
           (unsigned long)p.Off, (unsigned long)p.StolenLen);
    printf("orig   @target    :");
    for (int i = 0; i < 32; ++i) printf(" %02x", p.OrigBytes[i]);
    printf("\nhidden @target    :");
    for (int i = 0; i < 32; ++i) printf(" %02x", p.HiddenBytes[i]);
    printf("\ntramp  @tramp+0   :");
    for (int i = 0; i < 16; ++i) printf(" %02x", p.TrampBytes[i]);
    printf("\n");
    // expectation: hidden[0..1]=48 B8, hidden[2..9]=detour VA (unknown here,
    // printed by the driver log), hidden[10..11]=FF E0, then original bytes
    // from hidden[12..]; tramp = stolen original bytes then FF 25 <disp32>
    // (r34 register-free tail jump, resume VA in the slot at tramp+0x100)
    bool ok = p.HiddenBytes[0] == 0x48 && p.HiddenBytes[1] == 0xB8 &&
              p.HiddenBytes[10] == 0xFF && p.HiddenBytes[11] == 0xE0 &&
              p.StolenLen >= 12 && p.StolenLen <= 32 &&
              memcmp(p.OrigBytes + 12, p.HiddenBytes + 12, 32 - 12) == 0;
    bool tok = p.StolenLen <= 14 &&
               p.TrampBytes[14] == 0xFF && p.TrampBytes[15] == 0x25;
    printf("%s patch bytes %s; %s trampoline tail bytes %s\n",
           ok ? "[+]" : "[!]", ok ? "ok" : "BAD",
           tok ? "[+]" : "[!]", tok ? "ok" : "BAD/UNVERIFIABLE");
    return ok ? 0 : 1;
}

static int CmdNtHook(DWORD perCore, DWORD stage)
{
    SVMB_INFO info = {};
    DWORD bytes = 0;
    if (!Call(SVMB_IOCTL_GET_INFO, nullptr, 0, &info, sizeof(info), &bytes))
    {
        printf("[!] info failed before nthook\n");
        return 1;
    }
    SVMB_NPT_PROBE p = {};
    p.Stage = stage;
    p.Iterations = perCore;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] NPT_PROBE s%lu failed (err %lu)\n", stage, GetLastError());
        return 1;
    }
    u32_ cores = info.CoreCount ? info.CoreCount : 1;
    u32_ n = perCore ? perCore : 4;
    u32_ expect = cores * n;
    printf("nthook s%lu: target=%llx tramp=%llx id=%lu hits=%llu "
           "openOk=%lu/%lu denied=%lu otherOk=%lu last=%08x postOpen=%llu\n",
           stage, (unsigned long long)p.TargetVa,
           (unsigned long long)p.TrampPa, (unsigned long)p.HookId,
           (unsigned long long)p.HookHits, (unsigned long)p.OpenOk,
           (unsigned long)expect, (unsigned long)p.OvDeniedOk,
           (unsigned long)p.OvOtherOk, (unsigned long)p.LastStatus,
           (unsigned long long)p.OrigHitsAfter);
    bool ok;
    if (stage == 4)
    {
        // override contract: every cmd.exe open denied via the callback
        // (body skipped), every notepad.exe open passed through, and the
        // callback fired for all of them
        ok = p.OvDeniedOk == expect && p.OvOtherOk == expect &&
             p.HookHits >= 2 * expect && p.OrigHitsAfter == 1;
    }
    else
    {
        // passthrough contract: every open succeeded through the callback
        ok = p.OpenOk == expect && p.HookHits >= p.OpenOk &&
             p.LastStatus == 0 && p.OrigHitsAfter == 1;
    }
    printf("%s NtCreateFile hook %s %s\n", ok ? "[+]" : "[!]",
           stage == 4 ? "override" : "passthrough", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// r37 M4-B: end-to-end CR3 read spoof proof against THIS process.
// watch svmbctl.exe (seeded at create) -> the probe's CR3 read exits get
// the targeted spoof (real^key) -> unwatch -> reads go real again.
static int CmdCr3SpoofTest(const wchar_t* keyW)
{
    wchar_t* end = nullptr;
    unsigned long long key = wcstoull(keyW, &end, 16);
    if (!end || end == keyW)
    {
        printf("[!] bad xor key (want hex, e.g. 0xDEADC0DE)\n");
        return 1;
    }

    SVMB_CR3_CONFIG cfg = {};
    // r43: the write monitor arms the CR3-WRITE intercept - the whole-
    // system context-switch tax that death-spirals this vhv (r42/r43).
    // The read spoof needs only the read side; keep writes opt-in.
    cfg.EnableWriteMonitor = 0;
    cfg.XorKey = key;

    SVMB_NPT_PROBE pre = {};
    pre.Stage = 5;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &pre, sizeof(pre), &pre, sizeof(pre)))
    {
        printf("[!] cr3probe pre failed (err %lu)\n", GetLastError());
        return 1;
    }

    // r37: target by RAW CR3 (captured pre-watch). The image-name path
    // depends on Cr3Seed, whose Ex process-notify registration has been
    // failing c0000022 since r30 (test-signed driver) - the value-match
    // branch in Cr3ResolveRead works seed-less and is deterministic here.
    cfg.TargetCr3 = pre.Cr3Value;

    if (!Call(SVMB_IOCTL_CR3_CONFIG, &cfg, sizeof(cfg), nullptr, 0))
    {
        printf("[!] CR3_CONFIG watch failed (err %lu)\n", GetLastError());
        return 1;
    }

    SVMB_NPT_PROBE dur = {};
    dur.Stage = 5;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &dur, sizeof(dur), &dur, sizeof(dur)))
    {
        printf("[!] cr3probe during-watch failed (err %lu)\n", GetLastError());
        return 1;
    }

    SVMB_CR3_CONFIG off = {};
    if (!Call(SVMB_IOCTL_CR3_CONFIG, &off, sizeof(off), nullptr, 0))
    {
        printf("[!] CR3_CONFIG unwatch failed (err %lu)\n", GetLastError());
        return 1;
    }

    SVMB_NPT_PROBE post = {};
    post.Stage = 5;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &post, sizeof(post), &post, sizeof(post)))
    {
        printf("[!] cr3probe post failed (err %lu)\n", GetLastError());
        return 1;
    }

    unsigned long long expect = pre.Cr3Value ^ key;
    printf("cr3spoof: real=%llx spoofed=%llx expect=%llx post=%llx\n",
           (unsigned long long)pre.Cr3Value, (unsigned long long)dur.Cr3Value,
           (unsigned long long)expect, (unsigned long long)post.Cr3Value);
    bool ok = dur.Cr3Value == expect && post.Cr3Value == pre.Cr3Value;
    printf("%s CR3 read spoof %s\n", ok ? "[+]" : "[!]", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// r37 M4-E: apply debugger module config and/or arm one MTF single step
static int CmdDbgConfig(DWORD hide, DWORD spoof, DWORD arm)
{
    SVMB_DBG_CONFIG c = {};
    c.HideDr = hide;
    c.SpoofDr7Zero = spoof;
    c.ArmSingleStep = arm;
    if (!Call(SVMB_IOCTL_DBG_CONFIG, &c, sizeof(c), nullptr, 0))
    {
        printf("[!] DBG_CONFIG failed (err %lu)\n", GetLastError());
        return 1;
    }
    printf("[+] dbg config: hideDr=%lu spoofDr7=%lu armStep=%lu\n", hide,
           spoof, arm);
    return 0;
}

// r37 M4-D: kernel int3 through the debugger module's #BP face - the exit
// is reported to the ring and reinjected; this thread's own __except must
// still catch it as 0x80000003 and the ring must have grown.
static int CmdDbgTest()
{
    SVMB_NPT_PROBE p = {};
    p.Stage = 6;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] NPT_PROBE s6 failed (err %lu)\n", GetLastError());
        return 1;
    }
    // r40: GetExceptionCode sign-extends into the u64 field (0xFFFFFF...);
    // compare the low dword or every verdict reads FAIL (r37-r39 did).
    unsigned int exc32 = (unsigned int)p.Cr3Value;
    printf("dbgtest: exc=%llx ring %lu -> %lu\n",
           (unsigned long long)p.Cr3Value, (unsigned long)p.OvDeniedOk,
           (unsigned long)p.OvOtherOk);
    // r37: the probe uses __ud2 (not int3 - see the driver-side comment):
    // STATUS_ILLEGAL_INSTRUCTION expected, and the dbg ring must have grown
    // r40 verdict split: with interception armed (DBG_CONFIG EnableUd), the
    // full chain must light up (ring grows). With opt-in OFF (the default -
    // this VMware vhv NMI-aborts armed exception intercepts, r39), the
    // native catch is the CORRECT outcome, not a failure.
    if (exc32 == 0xC000001Du && p.OvOtherOk > p.OvDeniedOk)
    {
        printf("[+] debugger #UD face PASS (exit + ring + reinject + SEH all live)\n");
        return 0;
    }
    if (exc32 == 0xC000001Du && p.OvOtherOk == p.OvDeniedOk)
    {
        printf("[=] #UD caught natively - interception not armed (opt-in default;\n"
               "    arming it on this vhv dies by NMI abort 0x80, r39). SKIPPED.\n");
        return 0;
    }
    printf("[!] debugger #UD face FAIL (exc=%llx)\n",
           (unsigned long long)p.Cr3Value);
    return 1;
}

// r41 M4-E: DR-hiding end-to-end. Plain kernel DR reads before/while the
// debugger module's DR face is armed (dbg config 1 1): real DR7 must come
// back before (kernel default has bit 10 set = 0x400), the spoofed/shadow
// values while armed (DR7 reads 0), and the real value again after.
static int CmdDbgDrTest()
{
    SVMB_NPT_PROBE pre = {};
    pre.Stage = 7;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &pre, sizeof(pre), &pre, sizeof(pre)))
    {
        printf("[!] NPT_PROBE s7 pre failed (err %lu)\n", GetLastError());
        return 1;
    }
    if (CmdDbgConfig(1, 1, 0))
        return 1;

    SVMB_NPT_PROBE dur = {};
    dur.Stage = 7;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &dur, sizeof(dur), &dur, sizeof(dur)))
    {
        printf("[!] NPT_PROBE s7 during failed (err %lu)\n", GetLastError());
        (void)CmdDbgConfig(0, 0, 0);
        return 1;
    }
    (void)CmdDbgConfig(0, 0, 0);

    SVMB_NPT_PROBE post = {};
    post.Stage = 7;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &post, sizeof(post), &post, sizeof(post)))
    {
        printf("[!] NPT_PROBE s7 post failed (err %lu)\n", GetLastError());
        return 1;
    }

    printf("dbgdr: dr7 real=%llx hidden=%llx post=%llx dr0 hidden=%08x:%08x\n",
           (unsigned long long)pre.Cr3Value,
           (unsigned long long)dur.Cr3Value,
           (unsigned long long)post.Cr3Value,
           (unsigned)dur.OvOtherOk, (unsigned)dur.OvDeniedOk);
    // The face under test: while armed, DR reads resolve to the shadow/spoof
    // (DR7 reads 0, DR0 reads 0). The real values pre/post are whatever the
    // platform returns - on this VMware vhv an unarmed native DR read comes
    // back as emulated garbage (r41), so equality with `real` is not
    // asserted here (bare metal: expect 0x400 / 0).
    bool ok = (u32_)dur.Cr3Value == 0 && dur.OvDeniedOk == 0 &&
              dur.OvOtherOk == 0;
    printf("%s debugger DR face %s\n", ok ? "[+]" : "[!]",
           ok ? "PASS (hidden reads zeroed while armed)"
              : "FAIL (armed reads not hidden)");
    return ok ? 0 : 1;
}

// r53: per-core process view end-to-end test. Watch THIS process by raw
// CR3 with the process view enabled; the running core must publish the
// ProcView NCr3 (== view PML4) while the target is current, and revert to
// the base PML4 after the config clears.
static int CmdCr3ViewTest(const wchar_t* keyW)
{
    wchar_t* end = nullptr;
    unsigned long long key = wcstoull(keyW, &end, 16);
    if (!end || end == keyW)
    {
        printf("[!] bad xor key (want hex, e.g. 0xDEADC0DE)\n");
        return 1;
    }

    SVMB_NPT_PROBE pre = {};
    pre.Stage = 5;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &pre, sizeof(pre), &pre, sizeof(pre)))
    {
        printf("[!] viewtest pre failed (err %lu)\n", GetLastError());
        return 1;
    }

    SVMB_CR3_CONFIG cfg = {};
    cfg.TargetCr3 = pre.Cr3Value;   // watch THIS process by raw CR3
    cfg.XorKey = key;
    cfg.EnableProcessView = 1;
    if (!Call(SVMB_IOCTL_CR3_CONFIG, &cfg, sizeof(cfg), nullptr, 0))
    {
        printf("[!] viewtest config failed (err %lu)\n", GetLastError());
        return 1;
    }

    SVMB_NPT_PROBE in = {};
    in.Stage = 15;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &in, sizeof(in), &in, sizeof(in)))
    {
        printf("[!] viewtest in-probe failed (err %lu)\n", GetLastError());
        return 1;
    }

    SVMB_CR3_CONFIG off = {};
    if (!Call(SVMB_IOCTL_CR3_CONFIG, &off, sizeof(off), nullptr, 0))
    {
        printf("[!] viewtest unwatch failed (err %lu)\n", GetLastError());
        return 1;
    }

    SVMB_NPT_PROBE post = {};
    post.Stage = 15;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &post, sizeof(post), &post, sizeof(post)))
    {
        printf("[!] viewtest post-probe failed (err %lu)\n", GetLastError());
        return 1;
    }

    printf("viewtest: target=%llx procPml4=%llx basePml4=%llx\n",
           (unsigned long long)in.TargetVa,
           (unsigned long long)in.PagePa,
           (unsigned long long)in.Pml4Pa);
    printf("  in-target NCr3=%llx (expect procPml4)\n",
           (unsigned long long)in.VmcbNcr3);
    printf("  post      NCr3=%llx (expect basePml4)\n",
           (unsigned long long)post.VmcbNcr3);
    bool ok = in.TargetVa == pre.Cr3Value &&
              in.PagePa != 0 && in.VmcbNcr3 == in.PagePa &&
              post.VmcbNcr3 == post.Pml4Pa;
    printf("%s per-core view %s\n", ok ? "[+]" : "[!]",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

// r57: production-path content policy. Self-watch + EnableProcessView
// publishes the per-core ProcView for THIS process; the driver-side
// stage-17 swaps the probe buffer's GPA to a hidden page inside that
// production view and reads through both roots. PASS = in-target reads
// the hidden page, base reads the real page.
static int CmdCr3PolicyTest()
{
    SVMB_NPT_PROBE pre = {};
    pre.Stage = 5;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &pre, sizeof(pre), &pre, sizeof(pre)))
    {
        printf("[!] policytest pre failed (err %lu)\n", GetLastError());
        return 1;
    }

    SVMB_CR3_CONFIG cfg = {};
    cfg.TargetCr3 = pre.Cr3Value;
    cfg.XorKey = 0xDEADC0DE;
    cfg.EnableProcessView = 1;
    if (!Call(SVMB_IOCTL_CR3_CONFIG, &cfg, sizeof(cfg), nullptr, 0))
    {
        printf("[!] policytest config failed (err %lu)\n", GetLastError());
        return 1;
    }

    SVMB_NPT_PROBE in = {};
    in.Stage = 17;
    if (!Call(SVMB_IOCTL_NPT_PROBE, &in, sizeof(in), &in, sizeof(in)))
    {
        printf("[!] policytest probe failed (err %lu)\n", GetLastError());
        SVMB_CR3_CONFIG off = {};
        (void)Call(SVMB_IOCTL_CR3_CONFIG, &off, sizeof(off), nullptr, 0);
        return 1;
    }

    SVMB_CR3_CONFIG off = {};
    (void)Call(SVMB_IOCTL_CR3_CONFIG, &off, sizeof(off), nullptr, 0);

    unsigned long long hid = 0x22222222u, rel = 0x11111111u;
    printf("policy content: hidden-read=%08llx (expect %08llx) "
           "real-read=%08llx (expect %08llx)\n",
           (unsigned long long)in.DetourHits, hid,
           (unsigned long long)in.OrigHitsAfter, rel);
    bool ok = in.DetourHits == hid && in.OrigHitsAfter == rel;
    printf("%s policy view %s\n", ok ? "[+]" : "[!]",
           ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

static void Usage()
{
    printf("svmbctl - svmb hypervisor control\n"
           "  info                 query driver/hypervisor state\n"
           "  start                enter virtualization on all cores\n"
           "  stop                 leave virtualization\n"
           "  cycle <n>            enter/exit n times (stress)\n"
           "  storm <n>            n guest->VMM hypercall exits per core\n"
           "  mod attach <name>    attach a module\n"
           "  mod detach <name>    detach a module\n"
           "  mod list             list attached modules\n"
           "  serial on|off        COM1 log mirror (crash forensics)\n"
           "  demo                 demo_cpuid end-to-end test\n"
           "  selftest             run offline self-tests (no start needed)\n"
           "  npt add <t> <d>      install NPT hook (hex VAs)\n"
           "  npt remove <t>       remove NPT hook\n"
           "  npt list             list installed hooks\n"
           "  cpuid <leaf-hex>     r92: guest-view CPUID (stealth probe:\n"
           "                       leaf 1 ECX[31] + 0x40000000 signature)\n"
           "  rdtsc                r92: guest-view raw TSC (TscProbe leg\n"
           "                       pairs this with kd rdmsr 0x10)\n"
           "  npfprobe <n>         M3 live probe: NPF slide cycle (n calls)\n"
           "  npfdump              M3 probe: install+dump patch bytes, no exec\n"
           "  nthook [n]           hook real NtCreateFile, n opens/core (def 4)\n"
           "  nthookov [n]         same hook with an override callback (deny\n"
           "                       cmd.exe, pass notepad.exe)\n"
           "  viewprobe            M3 live probe: view-switch publish cycle\n"
           "  cr3 stats            process CR3 seed table stats\n"
           "  cr3 poison           r71 test hook: corrupt one armed sentinel\n"
           "                       watermark (simulates frame reuse)\n"
           "  cr3 protect <pid> <base> <size> [kill]\n"
           "                       r75: protect a region (L1 guest-MM block\n"
           "                       + L2 NPT sensing); kill = r89 plan C\n"
           "                       (bypass trip terminates the writer)\n"
           "  cr3 unprotect <pid> [base]\n"
           "                       remove protection(s)\n"
           "  cr3 unprotectid <id>  r78: remove one protection by region id\n"
           "  cr3 holdpage <sec> [outfile] [kill|suspend]\n"
           "                       r78: protect a page of THIS process and\n"
           "                       hold it (kill me = death sweep demo;\n"
           "                       kill/suspend = r89/r91 response policy)\n"
           "  cr3 regions          list protected regions\n"
           "  cr3 blockself        r75 E2E: protect a page of THIS process\n"
           "                       and prove the write faults\n"
           "  cr3 probee2e [kill|suspend]\n"
           "                       r76 E2E: kernel MDL write bypasses the\n"
           "                       L1 block, NPT sensing must alert; with\n"
           "                       kill/suspend = self-victim response demo\n"
           "  cr3 probe <pid> <base-hex> [size]\n"
           "                       r89: standalone bypass write into an\n"
           "                       existing region (kill-policy trigger)\n"
           "  cr3 suspended        r91: list suspended writers (plan C-)\n"
           "  cr3 resume <pid|all> r91: release suspended writer(s)\n"
           "  cr3 marker           r87: print this process's pid + known-\n"
           "                       content marker VA (readvm source)\n"
           "  cr3 readvm <pid> <va-hex> <size> [out.bin] [faultin] [guestview]\n"
           "                       r87/r88: hypervisor-level read of another\n"
           "                       process's memory (page-table walk +\n"
           "                       physical reads, invisible to the guest);\n"
           "                       faultin = r88 strategy B (attach+touch,\n"
           "                       completes not-present pages, has trace);\n"
           "                       guestview = r89: read the hook-patched\n"
           "                       hidden copy on NPT-hooked pages\n"
           "  sysaudit             r99: drain the syscall audit ring\n"
           "                       (mod attach sysaudit to arm)\n"
           "  behavior             r102: per-caller cross-process syscall\n"
           "                       rates + burst alerts (sysaudit armed)\n"
           "  crumbs               r107: dump the freeze flight recorder\n"
           "  kdump <va-hex> [n]   r101: dump n qwords of kernel VA space\n"
           "                       (SSDT table / candidate inspector)\n"
           "  cr3 readvm-deadbuf   r96 E2E: RESERVED+NOACCESS out buffer\n"
           "                       -> clean refusal (probe leg, no gate)\n"
           "  cr3 readvm-self      r87 E2E: driver reads THIS process's\n"
           "                       marker through the walk, must MATCH\n"
           "  cr3 readvm-hole      r87 E2E: read reserved-only VA -> clean\n"
           "                       PARTIAL, never a crash\n"
           "  cr3 readvme2e        r88 E2E: strategy A vs B on untouched\n"
           "                       pages + kernel-va/wrap/flags rejection\n"
           "                       + NOACCESS fault-in (SEH path)\n"
           "  cr3 watch <exe> [key] watch image (arms now + on create); key = xor spoof key\n"
           "  cr3 unwatch          clear watch/spoof config\n"
           "  dbg events           drain debugger event ring\n"
           "  log                  drain kernel log ring\n");
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        Usage();
        return 0;
    }

    if (!OpenDrv())
        return 1;
    int rc = 0;

    if (!_wcsicmp(argv[1], L"info"))
        rc = CmdInfo();
    else if (!_wcsicmp(argv[1], L"cpuid") && argc >= 3)
        rc = CmdCpuId(argv[2]);
    else if (!_wcsicmp(argv[1], L"rdtsc"))
        rc = CmdRdtsc();
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 3 &&
             !_wcsicmp(argv[2], L"readvm-deadbuf"))
        rc = CmdReadVmDeadBuf();
    else if (!_wcsicmp(argv[1], L"sysaudit"))
        rc = CmdSysAudit();
    else if (!_wcsicmp(argv[1], L"behavior"))
        rc = CmdSysAuditBehavior();
    else if (!_wcsicmp(argv[1], L"crumbs"))
        rc = CmdCrumbs();
    else if (!_wcsicmp(argv[1], L"kdump") && argc >= 3)
        rc = CmdKdump(argv[2], argc >= 4 ? argv[3] : nullptr);
    else if (!_wcsicmp(argv[1], L"exitprof"))
        rc = CmdExitProf();
    else if (!_wcsicmp(argv[1], L"start"))
        rc = CmdHv(0, 0);
    else if (!_wcsicmp(argv[1], L"stop"))
        rc = CmdHv(1, 0);
    else if (!_wcsicmp(argv[1], L"cycle") && argc >= 3)
        rc = CmdHv(2, (DWORD)_wtoi(argv[2]));
    else if (!_wcsicmp(argv[1], L"storm") && argc >= 3)
        rc = CmdStress((DWORD)_wtoi(argv[2]));
    else if (!_wcsicmp(argv[1], L"mod") && argc >= 3)
    {
        char name[16] = {};
        if (argc >= 4)
            WideCharToMultiByte(CP_ACP, 0, argv[3], -1, name, (int)sizeof(name), nullptr, nullptr);
        rc = CmdMod(argv[2], argc >= 4 ? name : nullptr);
    }
    else if (!_wcsicmp(argv[1], L"demo"))
        rc = CmdDemo();
    else if (!_wcsicmp(argv[1], L"selftest"))
        rc = CmdSelfTest();
    else if (!_wcsicmp(argv[1], L"npt") && argc >= 3)
        rc = CmdNpt(argv[2], argc >= 4 ? argv[3] : nullptr,
                    argc >= 5 ? argv[4] : nullptr);
    else if (!_wcsicmp(argv[1], L"npfprobe") && argc >= 3)
    {
        DWORD n = (DWORD)_wtoi(argv[2]);
        rc = CmdNptProbe(0, n ? n : 16);
    }
    else if (!_wcsicmp(argv[1], L"npfdump"))
        rc = CmdNpfDump();
    else if (!_wcsicmp(argv[1], L"probestage") && argc >= 3)
        rc = CmdNptProbe((DWORD)_wtoi(argv[2]), 0);
    else if (!_wcsicmp(argv[1], L"nthook"))
    {
        DWORD n = argc >= 3 ? (DWORD)_wtoi(argv[2]) : 0;
        rc = CmdNtHook(n, 3);
    }
    else if (!_wcsicmp(argv[1], L"nthookov"))
    {
        DWORD n = argc >= 3 ? (DWORD)_wtoi(argv[2]) : 0;
        rc = CmdNtHook(n, 4);
    }
    else if (!_wcsicmp(argv[1], L"viewprobe"))
        rc = CmdNptProbe(1, 0);
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 3 &&
             !_wcsicmp(argv[2], L"stats"))
        rc = CmdCr3Stats();
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"poison"))
        rc = CmdCr3Poison();
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 6 &&
             !_wcsicmp(argv[2], L"protect"))
        rc = CmdCr3Protect(argv[3], argv[4], argv[5],
                           argc >= 7 ? argv[6] : nullptr);
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 4 &&
             !_wcsicmp(argv[2], L"unprotect"))
        rc = CmdCr3Unprotect(argv[3], argc >= 5 ? argv[4] : nullptr);
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"regions"))
        rc = CmdCr3Regions();
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"marker"))
        rc = CmdCr3Marker();
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 6 &&
             !_wcsicmp(argv[2], L"readvm"))
        rc = CmdCr3ReadVm(argv[3], argv[4], argv[5],
                          argc >= 7 ? argv[6] : nullptr,
                          argc >= 8 ? argv[7] : nullptr);
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"readvm-self"))
        rc = CmdCr3ReadVmSelf();
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"readvm-hole"))
        rc = CmdCr3ReadVmHole();
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"readvme2e"))
        rc = CmdCr3ReadVmE2E();
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 5 &&
             !_wcsicmp(argv[2], L"probe"))
        rc = CmdCr3Probe(argv[3], argv[4], argc >= 6 ? argv[5] : nullptr);
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"suspended"))
        rc = CmdCr3Suspended();
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 4 &&
             !_wcsicmp(argv[2], L"resume"))
        rc = CmdCr3Resume(argv[3]);
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"blockself"))
        rc = CmdCr3BlockSelf();
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"probee2e"))
        rc = CmdCr3ProbeE2E(argc >= 4 ? argv[3] : nullptr);
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 4 &&
             !_wcsicmp(argv[2], L"holdpage"))
        rc = CmdCr3HoldPage(argv[3], argc >= 5 ? argv[4] : nullptr,
                            argc >= 6 ? argv[5] : nullptr);
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 4 &&
             !_wcsicmp(argv[2], L"unprotectid"))
        rc = CmdCr3UnprotectId(argv[3]);
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 4 &&
             !_wcsicmp(argv[2], L"watch"))
        rc = CmdCr3Watch(argv[3], argc >= 5 ? argv[4] : nullptr,
                         argc >= 6 ? argv[5] : nullptr);
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 4 &&
             !_wcsicmp(argv[2], L"viewtest"))
        rc = CmdCr3ViewTest(argv[3]);
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 3 &&
             !_wcsicmp(argv[2], L"policytest"))
        rc = CmdCr3PolicyTest();
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 3 &&
             !_wcsicmp(argv[2], L"unwatch"))
        rc = CmdCr3Unwatch();
    else if (!_wcsicmp(argv[1], L"cr3self") && argc >= 3)
        rc = CmdCr3Self(argv[2]);
    else if (!_wcsicmp(argv[1], L"cr3self"))
        rc = CmdCr3Self(nullptr);
    else if (!_wcsicmp(argv[1], L"cr3spoof") && argc >= 3)
        rc = CmdCr3SpoofTest(argv[2]);
    else if (!_wcsicmp(argv[1], L"dbg") && argc >= 3 &&
             !_wcsicmp(argv[2], L"events"))
        rc = CmdDbgEvents();
    else if (!_wcsicmp(argv[1], L"dbg") && argc >= 5 &&
             !_wcsicmp(argv[2], L"config"))
        rc = CmdDbgConfig((DWORD)_wtoi(argv[3]), (DWORD)_wtoi(argv[4]),
                          argc >= 6 ? (DWORD)_wtoi(argv[5]) : 0);
    else if (!_wcsicmp(argv[1], L"dbg") && argc >= 3 &&
             !_wcsicmp(argv[2], L"step"))
        rc = CmdDbgConfig(0, 0, 1);
    else if (!_wcsicmp(argv[1], L"dbgtest"))
        rc = CmdDbgTest();
    else if (!_wcsicmp(argv[1], L"dbgdrtest"))
        rc = CmdDbgDrTest();
    else if (!_wcsicmp(argv[1], L"serial") && argc >= 3)
        rc = CmdSerial(argv[2]);
    else if (!_wcsicmp(argv[1], L"log"))
        rc = CmdLog();
    else
        Usage();

    CloseHandle(gDevice);
    return rc;
}
