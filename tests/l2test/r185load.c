// r185load.exe - the reference hypervisor (Amd-V-ReloadDbg) external-H loader. Registers
// this process's CR3 as an H view, then NtLoadDriver's the reference hypervisor whose
// DriverEntry virtualizes every core from THIS process's context, holds
// while the host observes (vlog/com1), then unloads and unregisters.
// Usage: r185load.exe [holdSeconds=180]
#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include "../../shared/svmb_protocol.h"

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0
#endif

typedef LONG NTSTATUS;
typedef NTSTATUS (WINAPI *PFN_NtLoadDriver)(PCUNICODE_STRING);
typedef NTSTATUS (WINAPI *PFN_NtUnloadDriver)(PCUNICODE_STRING);

#define L2T_IOCTL_PROBE CTL_CODE(0x8000, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _L2T_PROBE
{
    unsigned long long entryCr3, entryPid;
    unsigned long long entryVendor, entrySvm, entryNpt, entryVmcr;
    unsigned long long nowCr3, nowPid;
    unsigned long long nowVendor, nowSvm, nowNpt, nowVmcr;
} L2T_PROBE;

static const WCHAR SERVICE_KEY[] =
    L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\AmdVReloadDbg";
static const WCHAR L2T_SERVICE_KEY[] =
    L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\l2t2";

static HANDLE OpenDev(const char* path)
{
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
}

static int EnableLoadDriverPriv(void)
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return 0;
    LUID luid;
    if (!LookupPrivilegeValueA(NULL, "SeLoadDriverPrivilege", &luid))
    {
        CloseHandle(tok);
        return 0;
    }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    DWORD err = GetLastError();
    CloseHandle(tok);
    return err == ERROR_SUCCESS;
}

// in-band liveness anchor: the reference hypervisor creates \DosDevices\YCData only AFTER
// NptHookInitialize succeeded - if this opens, virtualization came up and
// DriverEntry completed with the device face alive.
static int ProbeYcData(void)
{
    HANDLE h = CreateFileA("\\\\.\\YCData", GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    CloseHandle(h);
    return 1;
}

// one HVIEW round trip: reg/unreg, self (pid=0) or an arbitrary pid
// (r185: NtLoadDriver runs DriverEntry in the System process, so pid 4
// must be registered for in-entry SVM checks to see the H face)
static int HviewX(int reg, unsigned pid)
{
    HANDLE s = OpenDev("\\\\.\\svmb");
    if (s == INVALID_HANDLE_VALUE)
        return 0;
    SVMB_HVIEW_CTL h = {0};
    h.Register = reg ? 1 : 0;
    h.Pid = pid;
    DWORD br = 0;
    BOOL ok = DeviceIoControl(s, SVMB_IOCTL_HVIEW, &h, sizeof(h), &h,
                              sizeof(h), &br, NULL);
    CloseHandle(s);
    return ok ? 1 : 0;
}

static int RunProbeMode(HMODULE ntdll)
{
    PFN_NtLoadDriver pLoad =
        (PFN_NtLoadDriver)GetProcAddress(ntdll, "NtLoadDriver");
    PFN_NtUnloadDriver pUnload =
        (PFN_NtUnloadDriver)GetProcAddress(ntdll, "NtUnloadDriver");

    // 1) register this process as H
    DWORD br = 0;
    HANDLE svmb = OpenDev("\\\\.\\svmb");
    if (svmb == INVALID_HANDLE_VALUE)
    {
        printf("[!] open svmb failed (err %lu)\n", GetLastError());
        return 1;
    }
    SVMB_HVIEW_CTL hv = {0};
    hv.Register = 1;
    if (!DeviceIoControl(svmb, SVMB_IOCTL_HVIEW, &hv, sizeof(hv), &hv,
                         sizeof(hv), &br, NULL))
    {
        printf("[!] hview reg failed (err %lu)\n", GetLastError());
        CloseHandle(svmb);
        return 1;
    }
    printf("[+] hview reg ok\n");
    CloseHandle(svmb);
    if (!HviewX(1, 4))
    {
        printf("[!] hview reg pid4 failed (driver pre-r185?)\n");
        return 1;
    }
    printf("[+] hview reg pid4 (System) ok - DriverEntry will see the H face\n");
    if (!EnableLoadDriverPriv())
    {
        printf("[!] SeLoadDriverPrivilege missing\n");
        return 1;
    }

    // 2) load l2t2 - its DriverEntry records the load-time context
    UNICODE_STRING key;
    key.Buffer = (PWSTR)L2T_SERVICE_KEY;
    key.Length = (USHORT)(wcslen(L2T_SERVICE_KEY) * sizeof(WCHAR));
    key.MaximumLength = key.Length;
    NTSTATUS st = pLoad(&key);
    printf("[probe] NtLoadDriver(l2t2) = 0x%08X\n", (unsigned)st);
    if (st != 0)
        return 1;

    // 3) ask it for entry-time vs ioctl-time vantages
    HANDLE l2t = OpenDev("\\\\.\\l2t2");
    if (l2t == INVALID_HANDLE_VALUE)
    {
        printf("[!] open l2t2 failed (err %lu)\n", GetLastError());
        return 1;
    }
    L2T_PROBE p;
    ZeroMemory(&p, sizeof(p));
    if (!DeviceIoControl(l2t, L2T_IOCTL_PROBE, NULL, 0, &p, sizeof(p),
                         &br, NULL) || br != sizeof(p))
    {
        printf("[!] probe ioctl failed (err %lu br=%lu)\n", GetLastError(), br);
        CloseHandle(l2t);
        return 1;
    }
    CloseHandle(l2t);

    printf("entry: cr3=%016llx pid=%llu vendor=%08llx%08llx svm=%llu "
           "npt=%llu vmcr=%016llx\n",
           p.entryCr3, p.entryPid, p.entryVendor >> 32,
           p.entryVendor & 0xFFFFFFFFull, p.entrySvm, p.entryNpt,
           p.entryVmcr);
    printf("ioctl: cr3=%016llx pid=%llu vendor=%08llx%08llx svm=%llu "
           "npt=%llu vmcr=%016llx\n",
           p.nowCr3, p.nowPid, p.nowVendor >> 32,
           p.nowVendor & 0xFFFFFFFFull, p.nowSvm, p.nowNpt, p.nowVmcr);

    // 4) cleanup
    st = pUnload(&key);
    printf("[probe] NtUnloadDriver(l2t2) = 0x%08X\n", (unsigned)st);
    HviewX(0, 4);
    HviewX(0, 0);
    printf("[+] hview unreg ok (pid4 + self)\n");
    return 0;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    if (argc > 1 && strcmp(argv[1], "probe") == 0)
        return RunProbeMode(ntdll);
    int hold = (argc > 1) ? atoi(argv[1]) : 180;
    if (hold < 0)
        hold = 0;
    DWORD br = 0;

    printf("[r185] loader start hold=%ds\n", hold);

    // 1) this process becomes the H view (CR3 registration)
    HANDLE svmb = OpenDev("\\\\.\\svmb");
    if (svmb == INVALID_HANDLE_VALUE)
    {
        printf("[!] open svmb failed (err %lu)\n", GetLastError());
        return 1;
    }
    SVMB_HVIEW_CTL hv = {0};
    hv.Register = 1;
    if (!DeviceIoControl(svmb, SVMB_IOCTL_HVIEW, &hv, sizeof(hv), &hv,
                         sizeof(hv), &br, NULL))
    {
        printf("[!] hview reg failed (err %lu)\n", GetLastError());
        CloseHandle(svmb);
        return 1;
    }
    printf("[+] hview reg ok (this process is H)\n");
    CloseHandle(svmb);
    if (!HviewX(1, 4))
    {
        printf("[!] hview reg pid4 failed (driver pre-r185?)\n");
        return 1;
    }
    printf("[+] hview reg pid4 (System) ok - DriverEntry will see the H face\n");

    if (!EnableLoadDriverPriv())
        printf("[!] SeLoadDriverPrivilege not held (NtLoadDriver may fail)\n");
    else
        printf("[+] SeLoadDriverPrivilege enabled\n");

    // 2) load the reference hypervisor - DriverEntry virtualizes every core in this ctx
    PFN_NtLoadDriver pLoad =
        (PFN_NtLoadDriver)GetProcAddress(ntdll, "NtLoadDriver");
    PFN_NtUnloadDriver pUnload =
        (PFN_NtUnloadDriver)GetProcAddress(ntdll, "NtUnloadDriver");

    UNICODE_STRING key;
    key.Buffer = (PWSTR)SERVICE_KEY;
    key.Length = (USHORT)(wcslen(SERVICE_KEY) * sizeof(WCHAR));
    key.MaximumLength = key.Length;

    printf("[r185] NtLoadDriver(AmdVReloadDbg) - DriverEntry virtualizes "
           "all cores from this context...\n");
    NTSTATUS st = pLoad(&key);
    printf("[r185] NtLoadDriver returned 0x%08X\n", (unsigned)st);
    if (st != 0)
    {
        printf("[!] load failed - un-registering and exiting\n");
        HviewX(0, 4);
        HviewX(0, 0);
        printf("[+] hview unreg ok (pid4 + self)\n");
        return 1;
    }
    printf("[+] the reference hypervisor DriverEntry returned - machine is H's guest now\n");

    // 3) behavioral anchor + hold with liveness ticks
    printf("[r185] YCData probe (post-entry): %s\n",
           ProbeYcData() ? "OPEN (device face alive)" : "closed");

    for (int t = 10; t <= hold; t += 10)
    {
        Sleep(10 * 1000);
        printf("[r185] tick %ds alive (running as H's L2)\n", t);
    }

    printf("[r185] YCData probe (pre-unload): %s\n",
           ProbeYcData() ? "OPEN" : "closed");

    // 4) unload - the reference hypervisor LeaveVirtualization per core, then unregister
    st = pUnload(&key);
    printf("[r185] NtUnloadDriver returned 0x%08X\n", (unsigned)st);

    HviewX(0, 4);
    HviewX(0, 0);
    printf("[+] hview unreg ok (pid4 + self; stealth back)\n");

    printf("[r185] loader done\n");
    return 0;
}
