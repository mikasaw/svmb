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

static int CmdNpt(const wchar_t* action, const wchar_t* a1,
                  const wchar_t* a2, const wchar_t* a3)
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

// cr3 gve2e: GUESTVIEW hidden-copy offset E2E (CODE_REVIEW 14 A4). Hook the
// marker page (jmp-mode patch lands at the marker's page offset inside the
// hidden copy: 48 B8 <detourVa> FF E0), then byte-compare GUESTVIEW reads
// at zero offset, NON-zero offset (the A4 case - the old code dropped the
// in-page offset and read from the hidden page base), and a cross-page
// chunk against pristine walk reads. Debug-only in practice: production
// refuses NPT_HOOK, so Install fails there.
static int CmdCr3GveE2E()
{
    volatile const char* p = g_readvmMarker;
    char touched = 0;
    for (size_t i = 0; i < sizeof(g_readvmMarker); ++i)
        touched = (char)(touched + p[i]);
    (void)touched;

    const svmb_u64 markerVa = (uintptr_t)g_readvmMarker;
    const u32_ markerOff = (u32_)(markerVa & 0xFFFull);
    if (markerOff < 16 || markerOff > 0x1000 - 16)
    {
        printf("[!] gve2e skip: marker page offset %u leaves no patch room\n",
               (unsigned)markerOff);
        return 1;
    }

    // one readvm round trip; caller checks the returned bytes
    auto readVm = [&](svmb_u64 va, svmb_u32 size, svmb_u32 flags, BYTE* out,
                      svmb_u32& resolved, svmb_u32& outFlags) -> bool {
        SVMB_CR3_READVM* rv = (SVMB_CR3_READVM*)HeapAlloc(
            GetProcessHeap(), 0, sizeof(*rv) + size);
        if (!rv)
            return false;
        memset(rv, 0, sizeof(*rv) + size); // r89 LAW: user-mode full memset
        rv->Pid = GetCurrentProcessId();
        rv->Va = va;
        rv->Size = size;
        rv->InFlags = flags;
        bool ok = Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + size, rv,
                       sizeof(*rv) + size) != FALSE;
        resolved = rv->OutResolved;
        outFlags = rv->OutFlags;
        if (ok)
            memcpy(out, rv + 1, size);
        HeapFree(GetProcessHeap(), 0, rv);
        return ok;
    };

    int fail = 0;
    BYTE got[32] = {};
    BYTE want[32] = {};
    svmb_u32 resolved = 0, outFlags = 0;

    // pristine baselines before the hook exists
    BYTE pristineMarker[16] = {};
    if (!readVm(markerVa, 16, 0, pristineMarker, resolved, outFlags) ||
        resolved != 16 || outFlags != 0 ||
        memcmp(pristineMarker, g_readvmMarker, 16) != 0)
    {
        printf("[!] gve2e: pristine marker read mismatch (resolved=%u "
               "flags=%x)\n",
               resolved, outFlags);
        return 1;
    }
    BYTE pristinePage[16] = {};
    if (!readVm(markerVa - markerOff, 16, 0, pristinePage, resolved,
                outFlags) ||
        resolved != 16 || outFlags != 0)
    {
        printf("[!] gve2e: pristine page-start read failed\n");
        return 1;
    }

    // arm: jmp-mode hook on the marker; detour value is inert (never
    // executed) but must be stable bytes for the patch comparison
    const svmb_u64 detourVa = markerVa;
    SVMB_NPT_HOOK_CTL hc = {};
    hc.Action = 0;
    hc.TargetVa = markerVa;
    hc.DetourVa = detourVa;
    if (!Call(SVMB_IOCTL_NPT_HOOK, &hc, sizeof(hc), &hc, sizeof(hc)))
    {
        // err 87 = STATUS_INVALID_PARAMETER from the kernel-VA-only guard
        // (r88 P1-1) - hooks are kernel-page-only BY VALIDATION, so a
        // user-page GUESTVIEW E2E needs a test-only data-hook mode
        printf("[!] gve2e SKIP: npt add refused (err %lu) - hooks are "
               "kernel-VA-only; see CODE_REVIEW 14 A4\n",
               GetLastError());
        return 2;
    }

    // expected hidden bytes at the marker offset: 48 B8 <detourVa> FF E0
    BYTE patch[12] = {};
    patch[0] = 0x48;
    patch[1] = 0xB8;
    memcpy(patch + 2, &detourVa, 8);
    patch[10] = 0xFF;
    patch[11] = 0xE0;

    // L1 (A4 core): zero in-page offset
    if (!readVm(markerVa, 12, SVMB_READVM_F_GUESTVIEW, got, resolved,
                outFlags) ||
        resolved != 12 || outFlags != 0 ||
        memcmp(got, patch, 12) != 0)
    {
        printf("[!] gve2e FAIL: guestview @+0 (resolved=%u flags=%x)\n",
               resolved, outFlags);
        ++fail;
    }
    else
        printf("[+] gve2e guestview @+0 matches hidden patch\n");

    // L2 (the A4 bug shape): non-zero in-page offset
    if (!readVm(markerVa + 3, 8, SVMB_READVM_F_GUESTVIEW, got, resolved,
                outFlags) ||
        resolved != 8 || outFlags != 0 || memcmp(got, patch + 3, 8) != 0)
    {
        printf("[!] gve2e FAIL: guestview @+3 (resolved=%u flags=%x)\n",
               resolved, outFlags);
        ++fail;
    }
    else
        printf("[+] gve2e guestview @+3 matches hidden patch+3\n");

    // L3: cross-page chunk - second chunk must be hidden[0..16) (== the
    // original page start; the patch sits at markerOff >= 16)
    if (!readVm(markerVa - 4, 20, SVMB_READVM_F_GUESTVIEW, got, resolved,
                outFlags) ||
        resolved != 20 || outFlags != 0 ||
        memcmp(got + 4, pristinePage, 16) != 0)
    {
        printf("[!] gve2e FAIL: guestview cross-page tail (resolved=%u "
               "flags=%x)\n",
               resolved, outFlags);
        ++fail;
    }
    else
        printf("[+] gve2e guestview cross-page tail matches page start\n");

    // pristine reads stay pristine while the hook is live
    if (!readVm(markerVa, 16, 0, got, resolved, outFlags) || resolved != 16 ||
        outFlags != 0 || memcmp(got, pristineMarker, 16) != 0)
    {
        printf("[!] gve2e FAIL: pristine read drifted under hook\n");
        ++fail;
    }

    hc.Action = 1;
    hc.TargetVa = markerVa;
    if (!Call(SVMB_IOCTL_NPT_HOOK, &hc, sizeof(hc), &hc, sizeof(hc)))
        printf("[!] gve2e: npt remove failed (err %lu)\n", GetLastError());

    printf("[%s] gve2e %s\n", fail ? "!" : "+", fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
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

// intercepted CPUID each. External interrupts never exit this guest
// (r122b law - an IPI/DPC broadcast executes in guest context), so the
// intercepted CPUID is the only in-guest event that forces the exit
// each parked private frame's generation predicate waits for. The
// exit consumes the pending TLB kick (TLB_CTL_FLUSH_ALL on the next
// VMRUN), after which the stale GPA->PrivPa translation can never be
// consulted again and the driver's reap frees the frame.
static void OvlNudgeAllCores()
{
    ULONG_PTR processMask = 0, systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask,
                                &systemMask))
        return;
    DWORD_PTR old = SetThreadAffinityMask(GetCurrentThread(),
                                          (DWORD_PTR)processMask);
    if (!old)
        return;
    for (ULONG_PTR core = 1; core; core <<= 1)
    {
        if (!(systemMask & core) || !(processMask & core))
            continue;
        if (SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)core))
        {
            int regs[4];
            __cpuid(regs, 1);
        }
    }
    SetThreadAffinityMask(GetCurrentThread(), old);
}

// crypt enc|dec|list (r133 mem_crypt - dual-frame block encrypt):
//   crypt enc <pid> <va-hex> <size> <key-hex>   - target keeps plaintext,
//                                                everyone else sees cipher
//   crypt dec <pid> <va-hex>                    - restore plaintext+identity
//   crypt list
// enc runs the r133 two-phase dance: stage 1 (ARMED maps + gate snapshot),
// per-core CPUID nudge, seal retries until the exit-generation gate
// passes (~10s budget).
static void OvlNudgeAllCores(); // r130 reap nudge (defined below)

// nudge + seal retry loop (the r133 gate dance); 0 = sealed
static DWORD CryptSealStaged(svmb_u32 pid, svmb_u64 va)
{
    // seal gate: every core needs ONE exit past the post-map snapshot.
    // The nudge pins to each core in turn and takes one intercepted
    // CPUID (the only in-guest event that forces exits here - the r130
    // reap law); seal returns DEVICE_BUSY until the last core is past.
    for (int i = 0; i < 50; ++i) // ~10s budget at 200ms cadence
    {
        OvlNudgeAllCores();
        SVMB_CR3_CRYPT s = {};
        s.Action = 3;
        s.Pid = pid;
        s.BaseVa = va;
        if (Call(SVMB_IOCTL_CR3_CRYPT, &s, sizeof(s), &s, sizeof(s)))
            return 0;
        DWORD e = GetLastError();
        if (e != ERROR_BUSY)
            return e;
        Sleep(200);
    }
    return ERROR_BUSY;
}

// full two-phase enc: stage 1 (ARMED maps + gate snapshot), then the
// nudge+seal loop. 0 = sealed; otherwise the WIN32 error (block left
// ARMED - dec undoes it).
static DWORD CryptEncStaged(svmb_u32 pid, svmb_u64 va, svmb_u64 size,
                            svmb_u64 key, unsigned long* pagesOut)
{
    SVMB_CR3_CRYPT c = {};
    c.Action = 0;
    c.Pid = pid;
    c.BaseVa = va;
    c.Size = size;
    c.Key = key;
    if (!Call(SVMB_IOCTL_CR3_CRYPT, &c, sizeof(c), &c, sizeof(c)))
        return GetLastError();
    if (pagesOut)
        *pagesOut = (unsigned long)c.OutStatus;
    return CryptSealStaged(pid, va);
}

static int CmdCrypt(const wchar_t* sub, const wchar_t* a1, const wchar_t* a2,
                    const wchar_t* a3, const wchar_t* a4)
{
    if (!OpenDrv())
        return 1;
    SVMB_CR3_CRYPT c = {};
    if (!_wcsicmp(sub, L"enc"))
    {
        if (!a1 || !a2 || !a3 || !a4)
        {
            printf("[!] usage: crypt enc <pid> <va-hex> <size> <key-hex>\n");
            return 1;
        }
        c.Action = 0;
        c.Pid = (svmb_u32)wcstoul(a1, nullptr, 0);
        c.BaseVa = _wcstoui64(a2, nullptr, 16);
        c.Size = _wcstoui64(a3, nullptr, 0); // base 10: page counts are small
        c.Key = _wcstoui64(a4, nullptr, 16);
        if (!c.Pid || !c.BaseVa || !c.Size || !c.Key || (c.Size & 0xFFF))
        {
            printf("[!] enc args: pid/va/size/key nonzero, size 4K multiple"
                   " (<= %u pages)\n",
                   (unsigned)SVMB_CRYPT_MAX_PAGES);
            return 1;
        }
    }
    else if (!_wcsicmp(sub, L"dec"))
    {
        if (!a1 || !a2)
        {
            printf("[!] usage: crypt dec <pid> <va-hex>\n");
            return 1;
        }
        c.Action = 1;
        c.Pid = (svmb_u32)wcstoul(a1, nullptr, 0);
        c.BaseVa = _wcstoui64(a2, nullptr, 16);
        if (!c.Pid || !c.BaseVa)
        {
            printf("[!] dec args: pid/va nonzero\n");
            return 1;
        }
    }
    else if (!_wcsicmp(sub, L"list"))
    {
        c.Action = 2;
    }
    else
    {
        printf("[!] usage: crypt enc|dec|list ...\n");
        return 1;
    }

    if (!Call(SVMB_IOCTL_CR3_CRYPT, &c, sizeof(c), &c, sizeof(c)))
    {
        printf("[!] CR3_CRYPT %ls failed (err %lu)\n", sub, GetLastError());
        return 1;
    }
    if (c.Action == 0)
    {
        // two-phase enc: stage 1 above, now nudge + seal to completion
        unsigned long pages = (unsigned long)c.OutStatus;
        DWORD e = CryptSealStaged(c.Pid, c.BaseVa);
        if (e)
        {
            printf("[!] crypt enc seal failed (err %lu) - block left "
                   "ARMED (dec to undo)\n",
                   e);
            return 1;
        }
        printf("[+] crypt enc ok: pages=%lu (sealed)\n", pages);
        return 0;
    }
    if (c.Action == 2)
    {
        printf("[+] crypt blocks live: %lu\n", (unsigned long)c.OutStatus);
        for (svmb_u32 i = 0; i < c.Count && i < SVMB_CRYPT_MAX_BLOCKS; ++i)
            printf("  pid=%lu base=%llx size=%llx pages=%lu\n",
                   (unsigned long)c.Blocks[i].Pid,
                   (unsigned long long)c.Blocks[i].BaseVa,
                   (unsigned long long)c.Blocks[i].Size,
                   (unsigned long)c.Blocks[i].Pages);
    }
    else
    {
        printf("[+] crypt %ls ok: pages=%lu\n", sub,
               (unsigned long)c.OutStatus);
    }
    return 0;
}

// ---- r133 crypte2e machinery -------------------------------------------

static svmb_u8 CryptPatternByte(int i) { return (svmb_u8)(i * 31 + 7); }

static bool PatternMatches16(const svmb_u8* p)
{
    for (int i = 0; i < 16; ++i)
        if (p[i] != CryptPatternByte(i))
            return false;
    return true;
}

// readvm 16 bytes from (pid, va) - the operator's (original-frame) view.
// Sets *errOut from inside (before HeapFree) so callers print the real
// error (review P3-7: GetLastError across HeapFree is unreliable)
static bool CryptReadVm16(svmb_u32 pid, svmb_u64 va, svmb_u8 out[16],
                          DWORD* errOut = nullptr)
{
    SVMB_CR3_READVM* rv = (SVMB_CR3_READVM*)HeapAlloc(GetProcessHeap(), 0,
                                                      sizeof(*rv) + 16);
    if (!rv)
    {
        if (errOut)
            *errOut = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    memset(rv, 0, sizeof(*rv) + 16); // r89 law: user-mode touch first
    rv->Pid = pid;
    rv->Va = va;
    rv->Size = 16;
    BOOL ok = Call(SVMB_IOCTL_CR3_READVM, rv, sizeof(*rv) + 16,
                   rv, sizeof(*rv) + 16);
    if (errOut)
        *errOut = ok ? ERROR_SUCCESS : GetLastError();
    if (ok)
        memcpy(out, (const void*)(rv + 1), 16);
    HeapFree(GetProcessHeap(), 0, rv);
    return ok != FALSE;
}

struct TargetProc
{
    HANDLE hProc;
    HANDLE hOutRd; // our end of the child stdout pipe
    unsigned long pid;
    unsigned long long va;  // page 1
    unsigned long long va2; // page 2 (the multi-block regression shape)
    char buf[4096];
    DWORD bufLen;
};

static bool SpawnMcryptTarget(TargetProc& t, bool stdoutToFile = false)
{
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (stdoutToFile)
    {
        // hold mode: the child OUTLIVES this process - a pipe would break
        // at our exit. Redirect to a file the unload leg inspects later.
        wr = CreateFileW(L"mcrypt_hold_out.txt", GENERIC_WRITE,
                         FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
        if (wr == INVALID_HANDLE_VALUE)
        {
            printf("[!] hold-output file failed (err %lu)\n",
                   GetLastError());
            return false;
        }
    }
    else if (!CreatePipe(&rd, &wr, &sa, 0))
        return false;
    if (rd)
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    // argv[0]-first law: the command line must lead with the program name
    wchar_t cmd[64] = L"mcrypt_target.exe";
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        printf("[!] spawn mcrypt_target.exe failed (err %lu)\n",
               GetLastError());
        if (rd)
            CloseHandle(rd);
        CloseHandle(wr);
        return false;
    }
    CloseHandle(wr); // our write end must close or reads never see data
    CloseHandle(pi.hThread);
    memset(&t, 0, sizeof(t));
    t.hProc = pi.hProcess;
    t.pid = pi.dwProcessId;
    t.hOutRd = rd; // null in file mode (no pipe to read)
    return true;
}

// one \n-terminated line from the child pipe, poll-bounded by timeoutMs
static bool TargetGetLine(TargetProc& t, char* out, size_t outCap,
                          DWORD timeoutMs)
{
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;)
    {
        char* nl = (char*)memchr(t.buf, '\n', t.bufLen);
        if (nl)
        {
            size_t n = (size_t)(nl - t.buf);
            if (n >= outCap)
                n = outCap - 1;
            memcpy(out, t.buf, n);
            out[n] = 0;
            size_t rest = t.bufLen - (size_t)(nl - t.buf) - 1;
            memmove(t.buf, nl + 1, rest);
            t.bufLen = (DWORD)rest;
            return true;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(t.hOutRd, nullptr, 0, nullptr, &avail, nullptr)
            || !avail)
        {
            if (GetTickCount64() >= deadline)
                return false;
            Sleep(100);
            continue;
        }
        DWORD take = avail;
        if (take > sizeof(t.buf) - t.bufLen)
            take = (DWORD)(sizeof(t.buf) - t.bufLen);
        DWORD got = 0;
        if (!ReadFile(t.hOutRd, t.buf + t.bufLen, take, &got, nullptr)
            || !got)
            return false;
        t.bufLen += got;
    }
}

// file-mode handshake: poll the child's CLOSED handshake file
// (mcrypt_va.txt - written once at startup, no live handle to share)
// for "<pid> <va1> <va2>" (two pages = the r133 review P0-1
// multi-block regression shape)
static bool TargetHandshakeFromFile(unsigned long& pidOut,
                                    unsigned long long& va1Out,
                                    unsigned long long& va2Out,
                                    DWORD timeoutMs)
{
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, L"mcrypt_va.txt", L"rb") == 0 && f)
        {
            char buf[160];
            size_t got = fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            buf[got] = 0;
            unsigned long pid = 0;
            unsigned long long va1 = 0, va2 = 0;
            if (sscanf(buf, "%lu %llx %llx", &pid, &va1, &va2) == 3
                && pid && va1 && va2)
            {
                pidOut = pid;
                va1Out = va1;
                va2Out = va2;
                return true;
            }
        }
        Sleep(150);
    }
    return false;
}

// r165 L2 stage-0: nested-virtualization baseline. This process's IOCTL
// thread plays "H" (a hypervisor inside svmb's guest): the driver-side
// probe does CPUID/EFER/HSAVE reads (archiving today's virtualization
// face) and one __svm_vmrun against a zeroed VMCB skeleton. With ic2.VMRUN
// armed (anti-nesting) the VMRUN must exit, return as an injected #BP
// (0x80000003), and be caught by the probe's SEH - clean rejection,
// guest alive. PASS = exc==80000003; the serial must show the matching
// "vmrun exit: anti-nesting reject" line (r164 cross-check law).
// r166 stage 1 (`l2test 1`): the FULL round trip - H builds L2's world
// (page tables + code + stack + VMCB with CPUID intercepted), VMRUN,
// control RETURNS (no exception), and H's VMCB must hold the reflected
// exit: ExitCode==0x72, L2 rip at the cpuid, the L2 Rax marker intact.
// r177: the external-H registration face. Registers/unregisters the
// CALLING process's CR3 as an H view (EFER.SVME settable, own virtual
// HSAVE, CPUID SVM unmasked) - what an out-of-tree H driver's loader
// calls before handing control to its kernel side.
// r181: the third-party-H virtualization-behavior recorder face.
//   vlog stats        - aggregate analysis (kind counts, top reflected
//                       exits, dropped) - the "what does that hypervisor
//                       driver DO" reading
//   vlog dump [n]     - drain and decode up to n raw records
//   vlog mask <hex>   - kind gate (bit kind-1)
//   vlog reset        - clear the ring and stats
static void VlogDecodeExit(unsigned long long e, char* out, size_t n)
{
    if (e == 0x400) _snprintf(out, n, "NPF (nested paging fault)");
    else if (e == 0x78) _snprintf(out, n, "HLT");
    else if (e == 0x72) _snprintf(out, n, "CPUID");
    else if (e == 0x70) _snprintf(out, n, "EXCP base");
    else if (e == 0x60) _snprintf(out, n, "INTR");
    else if (e == 0x75) _snprintf(out, n, "SWINT/INTn");
    else if (e == 0x7A) _snprintf(out, n, "INVLPGA");
    else if (e == 0x81) _snprintf(out, n, "VMMCALL");
    else if (e >= 0x40 && e <= 0x5F)
        _snprintf(out, n, "#EXC vector %llu", e - 0x40);
    else if (e >= 0x80 && e <= 0x8F)
        _snprintf(out, n, "SVM-instr exit 0x%llx", e);
    else _snprintf(out, n, "exit 0x%llx", e);
}

static const char* Ic1BitName(unsigned bit)
{
    switch (bit)
    {
    case 0: return "INTR";
    case 4: return "VINTR";
    case 18: return "CPUID";
    case 20: return "IRET";
    case 21: return "INTN";
    case 24: return "HLT";
    case 25: return "INVLPG";
    case 26: return "INVLPGA";
    case 27: return "IOIO";
    case 28: return "MSR";
    case 29: return "TASK_SWITCH";
    default: return nullptr;
    }
}

static void VlogDecodeIntent(unsigned long long ic1, unsigned long long ic2,
                             unsigned long long exc)
{
    printf("  H intercepts ic1=%08llx ic2=%08llx exc=%08llx:", ic1, ic2, exc);
    for (unsigned b = 0; b < 30; ++b)
        if ((ic1 >> b) & 1)
        {
            const char* nm = Ic1BitName(b);
            if (nm) printf(" %s", nm);
            else printf(" ic1.%u", b);
        }
    for (unsigned b = 0; b < 6; ++b)
        if ((ic2 >> b) & 1)
            printf(" ic2.%u", b);
    if (exc)
        printf(" exc-bmp=%08llx (exceptions reflected)", exc);
    printf("\n");
}

static int CmdVLog(int argc, wchar_t** argv)
{
    SVMB_VLOG_CTL ctl = {};
    if (argc >= 3 && !_wcsicmp(argv[2], L"reset"))
        ctl.Op = 3;
    else if (argc >= 3 && !_wcsicmp(argv[2], L"mask") && argc >= 4)
    {
        ctl.Op = 2;
        ctl.Mask = (unsigned)wcstoul(argv[3], NULL, 16);
    }
    else if (argc >= 3 && !_wcsicmp(argv[2], L"dump"))
    {
        ctl.Op = 0;
        if (argc >= 4)
            ctl.Max = (unsigned)_wtoi(argv[3]);
    }
    else
        ctl.Op = 1; // stats
    unsigned char buf[4096 + 64] = {};
    if (!Call(SVMB_IOCTL_VLOG, &ctl, sizeof(ctl), buf, sizeof(buf)))
    {
        printf("[!] vlog failed (err %lu)\n", GetLastError());
        return 1;
    }
    SVMB_VLOG_OUT* o = (SVMB_VLOG_OUT*)buf;
    printf("vlog: count=%u dropped=%u\n", o->Count, o->Dropped);
    static const char* kinds[6] = { "accept", "reflect", "compose",
                                    "intent", "h-inject", "h-ctrl" };
    for (int k = 0; k < 6; ++k)
        if (o->KindCount[k])
            printf("  %-9s %llu\n", kinds[k],
                   (unsigned long long)o->KindCount[k]);
    printf("  top L2 exits the H handled:\n");
    for (int i = 0; i < 8; ++i)
        if (o->TopExits[i][1])
        {
            char d[64];
            VlogDecodeExit(o->TopExits[i][0], d, sizeof(d));
            printf("    %-34.34s %10llu\n", d,
                   (unsigned long long)o->TopExits[i][1]);
        }
    if (ctl.Op == 0)
    {
        SVMB_VLOG_REC* r = (SVMB_VLOG_REC*)(o + 1);
        for (unsigned i = 0; i < o->Count; ++i)
        {
            printf("#%u c%u ", r[i].seq, r[i].cpu);
            switch (r[i].kind)
            {
            case 1:
                printf("ACCEPT  hvmcb=%llx rip=%llx cr3=%llx np=%llu\n",
                       r[i].a, r[i].b, r[i].c, r[i].d & 1);
                break;
            case 2:
            {
                char d[64];
                VlogDecodeExit(r[i].a, d, sizeof(d));
                printf("REFLECT %-30.30s info1=%llx rip=%llx\n", d,
                       r[i].b, r[i].d);
                break;
            }
            case 3:
                printf("COMPOSE lvl=%llu gpa=%llx -> %llx perms=%llx\n",
                       r[i].d, r[i].a, r[i].b, r[i].c);
                break;
            case 4:
                VlogDecodeIntent(r[i].a, r[i].b, r[i].c);
                break;
            case 5:
                printf("H-INJECT eventinj=%llx (vec %llu type %llu)\n",
                       r[i].a, r[i].a & 0xFF, (r[i].a >> 8) & 7);
                break;
            case 6:
                printf("H-CTRL  %s old=%llx new=%llx\n",
                       r[i].a == 1 ? "nested-NCr3" : "TscOffset",
                       r[i].b, r[i].c);
                break;
            }
        }
    }
    return 0;
}

static int CmdHView(bool reg, unsigned pid)
{
    SVMB_HVIEW_CTL h = {};
    h.Register = reg ? 1 : 0;
    h.Pid = pid; // r185: 0 = self; 4 = the System ctx DriverEntry runs in
    if (!Call(SVMB_IOCTL_HVIEW, &h, sizeof(h), &h, sizeof(h)))
    {
        printf("[!] hview %s failed (err %lu)\n", reg ? "reg" : "unreg",
               GetLastError());
        return 1;
    }
    printf("[+] hview %s ok (cr3 context %s, pid=%u)\n",
           reg ? "reg" : "unreg", reg ? "is H" : "back to stealth", pid);
    return 0;
}

static int CmdL2Test(unsigned stage)
{
    SVMB_NPT_PROBE p = {};
    p.Stage = stage == 7 ? 25 : stage == 6 ? 24 : stage == 5 ? 23
                       : stage == 3 ? 21
                       : stage == 2 ? 20 : (stage == 1 ? 19 : 18);
    if (stage >= 13 && stage <= 15)
    {
        // r176 shadow v1 judges: s13 = INVLPGA invalidation (remap+flush
        // -> fresh compose), s14 = perms (H-NX fetch NPF reflects, fix-up
        // completes), s15 = 2MB large-page compose (offset-mapped single
        // large entry)
        p.Stage = stage == 13 ? 27 : stage == 14 ? 28 : 29;
    }
    if (stage == 16)
        p.Stage = 30; // r179: 4 concurrent H worlds on 4 cores
    if (stage == 17)
        p.Stage = 31;
    if (stage == 18)
    {
        p.Stage = 32; // r183: full-RAM hook-slot materialization
        p.Iterations = 0x1000; // bit12 = enable the (gated) hook scan
    }
    if (stage >= 8 && stage <= 12)
    {
        // r175: s8 = kernel-softint punt self-test (Stage 26), s9 = the
        // same with the punt BYPASSED (Iterations bit31) - the r174 killer
        // replay; the bench dying during s9 is the expected outcome.
        // s10/11/12 = the r175-b vehicle-toxicity bisect: fence-only /
        // __debugbreak / indirect-call controls.
        p.Stage = 26;
        if (stage == 9)
            p.Iterations = 0x80000000u | 8;
        else if (stage == 10)
            p.Iterations = 1;
        else if (stage == 11)
            p.Iterations = 4;
        else if (stage == 12)
            p.Iterations = 8;
    }
    if (!Call(SVMB_IOCTL_NPT_PROBE, &p, sizeof(p), &p, sizeof(p)))
    {
        printf("[!] NPT_PROBE s%lu failed (err %lu)\n",
               (unsigned long)p.Stage, GetLastError());
        return 1;
    }
    unsigned int exc32 = (unsigned int)p.Cr3Value;
    if (stage == 18)
    {
        unsigned long long r1 = (unsigned long long)p.HookHits;
        unsigned long long r2 = (unsigned long long)p.PagePa;
        unsigned long long se = (unsigned long long)p.HiddenPa;
        printf("l2test s18: r1=%llx r2=%llx legs=%llu slot=%llu inWin=%u\n",
               r1, r2, se & 0xFFFFFFFFull, se >> 32,
               (unsigned)p.OvDeniedOk);
        if (r1 == 0xAAAA2222AAAA2222ull && r2 == 0xBBBB3333BBBB3333ull &&
            (se & 0xFFFFFFFFull) == 2)
        {
            printf("[+] L2 s18 PASS (full-RAM hook: out-of-window slot "
                   "materialized + INVLPGA rescan served the remap)\n");
            return 0;
        }
        printf("[!] L2 s18 FAIL\n");
        return 1;
    }
    if (stage == 17)
    {
        printf("l2test s17: exc=%08x legs=%u vl/vs ok exit=%llx mark=%llx "
               "cs=%u\n",
               (unsigned)p.Cr3Value, (unsigned)p.OvDeniedOk,
               (unsigned long long)p.HiddenPa,
               (unsigned long long)p.PagePa, (unsigned)p.OvOtherOk);
        if (p.Cr3Value == 0 && p.OvDeniedOk == 1 && p.HiddenPa == 0x78 &&
            p.PagePa == 0xFFFFFFFFC0DE600Dull && p.OvOtherOk == 1)
        {
            printf("[+] L2 s17 PASS (reference-hypervisor shape: vmload/vmsave "
                   "emulation carried the launch + full exit loop)\n");
            return 0;
        }
        printf("[!] L2 s17 FAIL\n");
        return 1;
    }
    if (stage == 16)
    {
        printf("l2test s16: okMask=%llx rounds=%llu\n",
               (unsigned long long)p.HookHits,
               (unsigned long long)p.OrigHitsAfter);
        if (p.HookHits == 0xF && p.OrigHitsAfter == 200)
        {
            printf("[+] L2 s16 PASS (multi-core shadow: 4 H worlds x 50 "
                   "concurrent L2 round trips, one GPA -> four frames)\n");
            return 0;
        }
        printf("[!] L2 s16 FAIL\n");
        return 1;
    }
    if (stage == 13)
    {
        unsigned long long r1 = (unsigned long long)p.HookHits;
        unsigned long long r2 = (unsigned long long)p.PagePa;
        printf("l2test s13: r1=%llx r2=%llx legs=%u\n", r1, r2,
               (unsigned)p.OvDeniedOk);
        if (r1 == 0x1122334455667788ull && r2 == 0x8877665544332211ull &&
            p.OvDeniedOk == 3)
        {
            printf("[+] L2 s13 PASS (shadow v1 invalidation: INVLPGA wiped "
                   "the window, L2 observed H's remap)\n");
            return 0;
        }
        printf("[!] L2 s13 FAIL (stale compose or leg failure)\n");
        return 1;
    }
    if (stage == 14)
    {
        unsigned long long dExit = (unsigned long long)p.HiddenPa;
        unsigned long long dInfo1 = (unsigned long long)p.HookHits;
        printf("l2test s14: denied=%u exit=%llx info1=%llx mark=%llx "
               "term=%u\n", (unsigned)p.OvDeniedOk, dExit, dInfo1,
               (unsigned long long)p.PagePa, (unsigned)p.OvOtherOk);
        if (p.OvDeniedOk == 1 && dExit == 0x400 && (dInfo1 & 0x10) &&
            p.PagePa == 0xFFFFFFFFC0DE600Dull && p.OvOtherOk == 1)
        {
            printf("[+] L2 s14 PASS (shadow v1 perms: H-NX fetch NPF "
                   "reflected with exec bit, fix-up completed)\n");
            return 0;
        }
        printf("[!] L2 s14 FAIL\n");
        return 1;
    }
    if (stage == 15)
    {
        unsigned long long codeVa = (unsigned long long)p.Cr3Value;
        printf("l2test s15: exit=%llx rip=%llx mark=%llx codeVa=%llx\n",
               (unsigned long long)p.HiddenPa,
               (unsigned long long)p.HookHits,
               (unsigned long long)p.PagePa, codeVa);
        if (p.OvDeniedOk == 1 && p.HiddenPa == 0x78 &&
            p.HookHits == codeVa + 0xC &&  // hlt exit rip = the hlt itself
            p.PagePa == 0xFFFFFFFFC0DE600Dull) // r169 sign-extension law
        {
            printf("[+] L2 s15 PASS (shadow v1 large page: 2MB entry "
                   "composed via 512-fill, program ran)\n");
            return 0;
        }
        printf("[!] L2 s15 FAIL\n");
        return 1;
    }
    if (stage >= 8 && stage <= 12)
    {
        unsigned long long caught = (unsigned long long)p.HookHits;
        unsigned long long clean = (unsigned long long)p.OrigHitsHooked;
        printf("l2test s%u: caught=%llu clean=%llu runs=%u%s\n", stage,
               caught, clean, p.Iterations,
               stage == 9 ? " (punt BYPASSED - old lethal path)" : "");
        if (stage == 9)
        {
            printf("[!] L2 s9 RETURNED (old path survived a kernel softint - "
                   "surprise, the r174 killer needed more than the injection "
                   "alone)\n");
            return 0;
        }
        if (caught + clean == p.Iterations)
        {
            printf("[+] L2 s%u PASS (caught=%llu clean=%llu)\n", stage,
                   caught, clean);
            return 0;
        }
        printf("[!] L2 s%u FAIL (caught=%llu clean=%llu runs=%u)\n", stage,
               caught, clean, p.Iterations);
        return 1;
    }
    if (stage == 7)
    {
        printf("l2test s7: exc=%08x legs=%u exit=%llx rip=%llx mark=%llx "
               "intr=%u\n",
               exc32, (unsigned)p.OvDeniedOk,
               (unsigned long long)p.HiddenPa,
               (unsigned long long)p.HookHits,
               (unsigned long long)p.PagePa, (unsigned)p.OvOtherOk);
        // hlt sits at code+0xE (cpuid 2 + mov 12); 48 C7 /0 sign-extends
        // imm32 - 0xC0DE600D lands as 0xFFFFFFFFC0DE600D (the r169 law)
        if (exc32 == 0 && p.OvDeniedOk == 1 && p.HiddenPa == 0x78 &&
            p.HookHits == 0x50000Eull && p.PagePa == 0xFFFFFFFFC0DE600Dull)
        {
            printf("[+] L2 s7 PASS (shadow v0: L2 ran on virtual GPA "
                   "0x500000 through H's non-identity nested NPT)\n");
            return 0;
        }
        printf("[!] L2 s7 FAIL (exc=%08x exit=%llx rip=%llx mark=%llx)\n",
               exc32, (unsigned long long)p.HiddenPa,
               (unsigned long long)p.HookHits,
               (unsigned long long)p.PagePa);
        return 1;
    }
    if (stage == 6)
    {
        printf("l2test s6: pre(efer=%llx hsave=%llx) H(efer=%llx hsave=%llx)"
               " post(efer=%llx) cpuidH=%u cpuidOther=%u\n",
               (unsigned long long)p.Cr3Value,
               (unsigned long long)p.Pml4Pa,
               (unsigned long long)p.HiddenPa,
               (unsigned long long)p.HookHits,
               (unsigned long long)p.PagePa, (unsigned)p.OvDeniedOk,
               (unsigned)p.OvOtherOk);
        bool preStealth = ((p.Cr3Value >> 12) & 1) == 0 && p.Pml4Pa == 0;
        bool hView = ((p.HiddenPa >> 12) & 1) == 1 &&
                     p.HookHits == 0x12345000ull;
        bool postStealth = ((p.PagePa >> 12) & 1) == 0 &&
                           p.OvDeniedOk == 1 && p.OvOtherOk == 0;
        if (preStealth && hView && postStealth)
        {
            printf("[+] L2 s6 PASS (per-H view: SVME settable + own HSAVE + "
                   "CPUID SVM visible; stealth face restored post-unregister)\n");
            return 0;
        }
        printf("[!] L2 s6 FAIL (pre=%d H=%d post=%d)\n", (int)preStealth,
               (int)hView, (int)postStealth);
        return 1;
    }
    if (stage == 5)
    {
        printf("l2test s5: exc=%08x term=%u exit=%llx mark=%llx "
               "retries=%llu dt=%uM\n",
               exc32, (unsigned)p.OvDeniedOk,
               (unsigned long long)p.HiddenPa,
               (unsigned long long)p.HookHits,
               (unsigned long long)p.PagePa, (unsigned)p.OvOtherOk);
        // 48 C7 /0 stores imm32 SIGN-EXTENDED to 64 bits (REX.W):
        // 0xD15EA5ED (bit31 set) lands as 0xFFFFFFFFD15EA5ED
        if (exc32 == 0 && p.OvDeniedOk == 1 && p.HiddenPa == 0 &&
            p.HookHits == 0xFFFFFFFFD15EA5EDull)
        {
            printf("[+] L2 s5 PASS (sustained L2: 16k-iteration loop ran "
                   "to terminal hlt through %llu interrupt retries)\n",
                   (unsigned long long)p.PagePa);
            return 0;
        }
        printf("[!] L2 s5 FAIL (exc=%08x term=%u exit=%llx mark=%llx)\n",
               exc32, (unsigned)p.OvDeniedOk, (unsigned long long)p.HiddenPa,
               (unsigned long long)p.HookHits);
        return 1;
    }
    if (stage == 3)
    {
        printf("l2test s3: exc=%08x legs=%u/3 mm=%llx mark1=%llx mark2=%llx "
               "inj=%u intr=%u\n",
               exc32, (unsigned)p.OvDeniedOk,
               (unsigned long long)p.HiddenPa,
               (unsigned long long)p.PagePa,
               (unsigned long long)p.HookHits,
               (unsigned)p.TargetVa, (unsigned)p.OvOtherOk);
        // r168 verdict = the TRANSFER contract: H armed EVENTINJ, the
        // accept transferred it into the real VMCB and cleared H's Valid
        // bit (hardware-consumed semantics). The in-L2 DELIVERY of the
        // injected event hits a QEMU-TCG emulation seam (deterministic
        // #PF at a host-stack-shaped CR2 with a correctly installed L2
        // RSP; the delivery push context inside helper_vmrun/cpu_loop_exit
        // is inconsistent for software nesting - archived r168). Real HW
        // injects at the hardware level; the passthrough IS the contract.
        if (exc32 == 0 && p.OvDeniedOk >= 2 && p.TargetVa == 0)
        {
            printf("[+] L2 s3 PASS (EVENTINJ transfer: armed -> transferred "
                   "-> consumed; in-L2 delivery = QEMU seam, r168)\n");
            return 0;
        }
        if (exc32 == 0x80000003)
        {
            printf("[!] L2 s3 FAIL (rejected - v1 contract unmet, #BP)\n");
            return 1;
        }
        printf("[!] L2 s3 FAIL (exc=%08x legs=%u mm=%llx m1=%llx m2=%llx)\n",
               exc32, (unsigned)p.OvDeniedOk, (unsigned long long)p.HiddenPa,
               (unsigned long long)p.PagePa, (unsigned long long)p.HookHits);
        return 1;
    }
    if (stage == 2)
    {
        printf("l2test s2: exc=%08x legs=%u/6 mm=%llx pf=%llx mark=%llx "
               "intr=%u\n",
               exc32, (unsigned)p.OvDeniedOk,
               (unsigned long long)p.HiddenPa,
               (unsigned long long)p.HookHits,
               (unsigned long long)p.PagePa, (unsigned)p.OvOtherOk);
        if (exc32 == 0 && p.OvDeniedOk == 6 && p.HiddenPa == 0 &&
            p.HookHits == 0x300000 && p.PagePa == 0x5A5AF00D)
        {
            printf("[+] L2 s2 PASS (gauntlet: cpuid/hlt/rdmsr/#UD/#PF all "
                   "reflected, full exit record, marker executed in L2)\n");
            return 0;
        }
        if (exc32 == 0x80000003)
        {
            printf("[!] L2 s2 FAIL (rejected - v1 contract unmet, #BP)\n");
            return 1;
        }
        printf("[!] L2 s2 FAIL (exc=%08x legs=%u mm=%llx)\n", exc32,
               (unsigned)p.OvDeniedOk, (unsigned long long)p.HiddenPa);
        return 1;
    }
    if (stage == 1)
    {
        printf("l2test s1: exc=%08x hvmcb=%llx exit=%llx L2rip=%llx "
               "L2rax=%llx\n",
               exc32, (unsigned long long)p.Pml4Pa,
               (unsigned long long)p.HiddenPa,
               (unsigned long long)p.HookHits,
               (unsigned long long)p.PagePa);
        if (exc32 == 0 && p.HiddenPa == 0x72 && p.HookHits == 0x100000 &&
            p.PagePa == 0xA5A51122)
        {
            printf("[+] L2 s1 PASS (full round trip: VMRUN -> L2 cpuid -> "
                   "exit reflected -> H resumed)\n");
            return 0;
        }
        if (exc32 == 0x80000003)
        {
            printf("[!] L2 s1 FAIL (rejected - v1 contract unmet, #BP)\n");
            return 1;
        }
        printf("[!] L2 s1 FAIL (exc=%08x exit=%llx)\n", exc32,
               (unsigned long long)p.HiddenPa);
        return 1;
    }
    printf("l2test s0: exc=%08x vmcb=%llx svme=%u efer=%llx(%u) "
           "hsave=%llx(%u)\n",
           exc32,
           (unsigned long long)p.Pml4Pa, (unsigned)p.OvDeniedOk,
           (unsigned long long)p.HiddenPa,
           (unsigned)(p.OvOtherOk & 1u),
           (unsigned long long)p.HookHits,
           (unsigned)((p.OvOtherOk >> 1) & 1u));
    if (exc32 == 0x80000003u)
    {
        printf("[+] L2 s0 PASS (anti-nesting rejection clean: VMRUN -> #BP -> SEH)\n");
        return 0;
    }
    if (exc32 == 1)
    {
        printf("[!] L2 s0 FAIL (VMRUN EXECUTED - ic2.VMRUN not armed?!)\n");
        return 1;
    }
    printf("[!] L2 s0 FAIL (exc=%08x)\n", exc32);
    return 1;
}

// crypte2e            - full verdict: spawn target, baseline readvm,
//                       enc (readvm sees cipher, target self-checks stay
//                       plaintext-ok), dec, readvm restored.
// crypte2e hold       - enc and LEAVE live; write crypt_hold.txt
//                       ("pid va"); child keeps running for the caller's
//                       unload-restore leg (taskkill it after).
// crypte2e chk <pid> <va-hex> - readvm 16B vs the pattern (post-unload
//                       restore proof)
static int CmdCryptE2E(const wchar_t* mode, const wchar_t* a1,
                       const wchar_t* a2)
{
    if (!OpenDrv())
        return 1;
    if (mode && !_wcsicmp(mode, L"chk"))
    {
        if (!a1 || !a2)
        {
            printf("[!] usage: crypte2e chk <pid> <va-hex>\n");
            return 1;
        }
        svmb_u32 pid = (svmb_u32)wcstoul(a1, nullptr, 0);
        svmb_u64 va = _wcstoui64(a2, nullptr, 16);
        svmb_u8 b[16];
        DWORD err = 0;
        if (!pid || !va || !CryptReadVm16(pid, va, b, &err))
        {
            printf("[-] crype2e chk: readvm failed (err %lu)\n", err);
            return 1;
        }
        printf("chk bytes:");
        for (int i = 0; i < 16; ++i)
            printf(" %02x", b[i]);
        printf("\n");
        bool ok = PatternMatches16(b);
        printf(ok ? "[+] crype2e chk PASS (plaintext restored)\n"
                  : "[-] crype2e chk FAIL\n");
        return ok ? 0 : 1;
    }
    const bool hold = mode && !_wcsicmp(mode, L"hold");

    // a stale handshake file from a previous leg would hand us the DEAD
    // child's pid+va (a recycled-pid trap); the child rewrites it at
    // startup, we only poll a file we deleted first
    DeleteFileW(L"mcrypt_va.txt");

    TargetProc t;
    if (!SpawnMcryptTarget(t, hold))
        return 1;
    int rc = 1;
    bool childLeftAlive = false;
    bool encLive = false; // set once the block maps exist (cleanup dec)
    svmb_u8 plain[16], cur[16];
    do
    {
        // 1. handshake: pid + the two page VAs (pipe line in full-cycle
        // mode; the child's closed handshake file in hold mode)
        char line[256] = "";
        if (t.hOutRd)
        {
            if (!TargetGetLine(t, line, sizeof(line), 15000)
                || sscanf(line, "MCRYPT-TARGET pid=%lu va1=0x%llx va2=0x%llx",
                          &t.pid, &t.va, &t.va2) != 3)
            {
                printf("[-] crype2e handshake FAILED: %s\n", line);
                break;
            }
        }
        else if (!TargetHandshakeFromFile(t.pid, t.va, t.va2, 15000))
        {
            printf("[-] crype2e handshake (file) FAILED\n");
            break;
        }
        else
        {
            DeleteFileW(L"mcrypt_va.txt"); // consume: no stale reuse
        }
        printf("[+] target pid=%lu va1=%llx va2=%llx\n", t.pid, t.va,
               t.va2);

        // 2. baseline: the operator's readvm sees the plaintext pattern
        DWORD err = 0;
        if (!CryptReadVm16((svmb_u32)t.pid, t.va, plain, &err))
        {
            printf("[-] crype2e baseline readvm failed (err %lu)\n", err);
            break;
        }
        if (!PatternMatches16(plain))
        {
            printf("[-] crype2e baseline != pattern (target bug?)\n");
            break;
        }
        printf("[+] baseline readvm = plaintext pattern\n");

        // 3. encrypt BOTH pages as two separate blocks (the r133 review
        // P0-1 multi-block shape) - each runs the two-phase dance:
        // stage 1 (ARMED maps), per-core nudge, seal
        unsigned long encPages = 0;
        DWORD encErr = CryptEncStaged((svmb_u32)t.pid, t.va, 0x1000,
                                      0xC0FFEE1234567890ull, &encPages);
        if (!encErr)
            encErr = CryptEncStaged((svmb_u32)t.pid, t.va2, 0x1000,
                                    0x0F1E2D3C4B5A6978ull, &encPages);
        if (encErr)
        {
            printf("[-] crype2e enc failed (err %lu)\n", encErr);
            break;
        }
        encLive = true;
        printf("[+] enc ok pages=%lu (two blocks sealed)\n", encPages);

        // 4. (public build: the ovlstat disclosure leg rides protocol
        //    tail fields that are not part of the public ABI)

        // 5. operator view: readvm must now see CIPHERTEXT on BOTH pages
        if (!CryptReadVm16((svmb_u32)t.pid, t.va, cur, &err))
        {
            printf("[-] crype2e readvm-during failed (err %lu)\n", err);
            break;
        }
        if (memcmp(cur, plain, 16) == 0)
        {
            printf("[-] crype2e FAIL: readvm still plaintext after enc\n");
            break;
        }
        printf("[+] readvm during enc = ciphertext:");
        for (int i = 0; i < 16; ++i)
            printf(" %02x", cur[i]);
        printf("\n");
        if (!CryptReadVm16((svmb_u32)t.pid, t.va2, cur, &err)
            || memcmp(cur, plain, 16) == 0)
        {
            printf("[-] crype2e FAIL: page2 not ciphertext after enc\n");
            break;
        }
        printf("[+] readvm during enc page2 = ciphertext\n");

        // 6. target view: self-checks keep reporting plaintext-ok (pipe
        // mode only - hold mode redirects to a file whose verdict the
        // unload leg reads after the service cycle)
        int okLines = 0, badLines = 0;
        char firstBad[256] = "";
        if (t.hOutRd)
        {
            ULONGLONG dl = GetTickCount64() + 6000;
            while (GetTickCount64() < dl)
            {
                char l[256];
                DWORD left = (DWORD)(dl - GetTickCount64());
                if (!TargetGetLine(t, l, sizeof(l), left ? left : 1))
                    break;
                if (!strncmp(l, "MCRYPT-SELF", 11))
                {
                    if (strstr(l, "plaintext-ok"))
                        ++okLines;
                    if (strstr(l, "PLAINTEXT-DAMAGED"))
                    {
                        ++badLines;
                        if (!firstBad[0])
                            strncpy(firstBad, l, sizeof(firstBad) - 1);
                    }
                }
            }
            printf("[+] target self-checks during enc: ok=%d damaged=%d\n",
                   okLines, badLines);
            if (firstBad[0])
                printf("[+] first damaged detail: %s\n", firstBad);
            if (!okLines || badLines)
            {
                printf("[-] crype2e FAIL: target observed wrong bytes\n");
                break;
            }
        }

        if (hold)
        {
            // let the sealed state accumulate self-check lines in the
            // output file (the unload leg's verdict input)
            Sleep(4000);
            FILE* f = nullptr;
            if (fopen_s(&f, "crypt_hold.txt", "w") == 0 && f)
            {
                fprintf(f, "%lu %llx %llx\n", t.pid, t.va, t.va2);
                fclose(f);
            }
            printf("[+] crype2e HOLD: two blocks left encrypted, "
                   "crypt_hold.txt + mcrypt_hold_out.txt written, child "
                   "alive (taskkill after the unload leg)\n");
            rc = 0;
            childLeftAlive = true;
            break;
        }

        // 7. decrypt
        SVMB_CR3_CRYPT d = {};
        d.Action = 1;
        d.Pid = (svmb_u32)t.pid;
        d.BaseVa = t.va;
        if (!Call(SVMB_IOCTL_CR3_CRYPT, &d, sizeof(d), &d, sizeof(d)))
        {
            printf("[-] crype2e dec failed (err %lu)\n", GetLastError());
            break;
        }
        printf("[+] dec ok pages=%lu\n", (unsigned long)d.OutStatus);
        d = {};
        d.Action = 1;
        d.Pid = (svmb_u32)t.pid;
        d.BaseVa = t.va2;
        if (!Call(SVMB_IOCTL_CR3_CRYPT, &d, sizeof(d), &d, sizeof(d)))
        {
            printf("[-] crype2e dec page2 failed (err %lu)\n",
                   GetLastError());
            break;
        }
        encLive = false; // both blocks are gone
        printf("[+] dec page2 ok pages=%lu\n", (unsigned long)d.OutStatus);

        // 8. operator view restored to plaintext on BOTH pages
        if (!CryptReadVm16((svmb_u32)t.pid, t.va, cur, &err)
            || memcmp(cur, plain, 16) != 0)
        {
            printf("[-] crype2e FAIL: readvm not restored after dec "
                   "(err %lu)\n",
                   err);
            break;
        }
        printf("[+] readvm after dec = plaintext restored\n");
        if (!CryptReadVm16((svmb_u32)t.pid, t.va2, cur, &err)
            || memcmp(cur, plain, 16) != 0)
        {
            printf("[-] crype2e FAIL: page2 not restored after dec\n");
            break;
        }
        printf("[+] readvm after dec page2 = plaintext restored\n");

        SVMB_CR3_CRYPT l = {};
        l.Action = 2;
        if (Call(SVMB_IOCTL_CR3_CRYPT, &l, sizeof(l), &l, sizeof(l)))
            printf("[+] crypt list after dec: %lu live\n",
                   (unsigned long)l.OutStatus);
        printf("[+] crype2e PASS\n");
        rc = 0;
    } while (false);

    if (rc && encLive && !childLeftAlive)
    {
        // failure cleanup BEFORE the kill: leftover ARMED/SEALED blocks
        // would pin the process-view binding slot (the leg-2 DEVICE_BUSY
        // trap) and leak the overlays - dec BOTH best-effort while the
        // target still lives
        SVMB_CR3_CRYPT d = {};
        d.Action = 1;
        d.Pid = (svmb_u32)t.pid;
        d.BaseVa = t.va;
        BOOL ok1 = Call(SVMB_IOCTL_CR3_CRYPT, &d, sizeof(d), &d, sizeof(d));
        d.BaseVa = t.va2;
        BOOL ok2 = Call(SVMB_IOCTL_CR3_CRYPT, &d, sizeof(d), &d, sizeof(d));
        if (ok1 && ok2)
            printf("[+] cleanup dec ok (blocks removed)\n");
        else
            printf("[!] cleanup dec incomplete (crypt list to inspect)\n");
    }
    if (!childLeftAlive)
        TerminateProcess(t.hProc, 0);
    if (t.hProc)
        CloseHandle(t.hProc);
    if (t.hOutRd)
        CloseHandle(t.hOutRd);
    if (rc)
        printf("[-] crype2e FAIL\n");
    return rc;
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
           "  l2test [0|1|2|3]     r165-168: nested baseline/round trip/gauntlet/injection\n"
           "  hreg | hunreg        r177: register/unregister this process's\n"
           "                       CR3 as an H view (external-H face)\n"
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
           "  cr3 gve2e            fixround A4 E2E: GUESTVIEW hidden-copy\n"
           "                       offset (0 / mid-page / cross-page)\n"
           "  cr3 readvm-hole      r87 E2E: read reserved-only VA -> clean\n"
           "                       PARTIAL, never a crash\n"
           "  cr3 readvme2e        r88 E2E: strategy A vs B on untouched\n"
           "                       pages + kernel-va/wrap/flags rejection\n"
           "                       + NOACCESS fault-in (SEH path)\n"
           "  cr3 watch <exe> [key] watch image (arms now + on create); key = xor spoof key\n"
           "  cr3 unwatch          clear watch/spoof config\n"
           "  dbg events           drain debugger event ring\n"
           "  crypt enc <pid> <va-hex> <size> <key-hex>\n"
           "                       r133: encrypt a target block (dual-frame\n"
           "                       split view - target reads plaintext,\n"
           "                       readvm/scanners see ciphertext)\n"
           "  crypt dec <pid> <va-hex>\n"
           "                       r133: restore plaintext + identity map\n"
           "  crypt list           r133: live encrypted blocks\n"
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
                    argc >= 5 ? argv[4] : nullptr,
                    argc >= 6 ? argv[5] : nullptr);
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
    else if (!_wcsicmp(argv[1], L"crypt") && argc >= 3 &&
             !_wcsicmp(argv[2], L"e2e"))
        rc = CmdCryptE2E(argc >= 4 ? argv[3] : nullptr,
                         argc >= 5 ? argv[4] : nullptr,
                         argc >= 6 ? argv[5] : nullptr);
    else if (!_wcsicmp(argv[1], L"crypt") && argc >= 7 &&
             !_wcsicmp(argv[2], L"enc"))
        rc = CmdCrypt(L"enc", argv[3], argv[4], argv[5], argv[6]);
    else if (!_wcsicmp(argv[1], L"crypt") && argc >= 5 &&
             !_wcsicmp(argv[2], L"dec"))
        rc = CmdCrypt(L"dec", argv[3], argv[4], nullptr, nullptr);
    else if (!_wcsicmp(argv[1], L"crypt") && argc >= 3 &&
             !_wcsicmp(argv[2], L"list"))
        rc = CmdCrypt(L"list", nullptr, nullptr, nullptr, nullptr);
    else if (!_wcsicmp(argv[1], L"cr3") && argc >= 6 &&
             !_wcsicmp(argv[2], L"readvm"))
        rc = CmdCr3ReadVm(argv[3], argv[4], argv[5],
                          argc >= 7 ? argv[6] : nullptr,
                          argc >= 8 ? argv[7] : nullptr);
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"readvm-self"))
        rc = CmdCr3ReadVmSelf();
    else if (!_wcsicmp(argv[1], L"cr3") && !_wcsicmp(argv[2], L"gve2e"))
        rc = CmdCr3GveE2E();
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
    else if (!_wcsicmp(argv[1], L"hreg"))
        rc = CmdHView(true, argc >= 3 ? (unsigned)_wtoi(argv[2]) : 0);
    else if (!_wcsicmp(argv[1], L"hunreg"))
        rc = CmdHView(false, argc >= 3 ? (unsigned)_wtoi(argv[2]) : 0);
    else if (!_wcsicmp(argv[1], L"vlog"))
        rc = CmdVLog(argc, argv);
    else if (!_wcsicmp(argv[1], L"l2test"))
        rc = CmdL2Test(argc >= 3 ? (unsigned)_wtoi(argv[2]) : 0);
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
