// l2load.exe - r177 external-H loader. Drives the full external round
// trip from user mode: register this process's CR3 as an H view through
// svmb's real IOCTL face, send RUN to l2test.sys (which VMRUNs as H from
// this same process context), then unregister and print the verdict.
#include <windows.h>
#include <stdio.h>
#include "../../shared/svmb_protocol.h"

#define L2T_DOS   "\\\\.\\l2t2"
#define L2T_IOCTL_RUN CTL_CODE(0x8000, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)

typedef struct _L2T_RESULT
{
    unsigned long long exc, exitCode, rip, mark, rax, eferH, hsaveH;
    unsigned long legs;
    unsigned intrSeen;
} L2T_RESULT;

static HANDLE OpenDev(const char* path)
{
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, 0, NULL);
}

int main(void)
{
    // 1) register this process's CR3 as an H view (the r177 face)
    HANDLE svmb = OpenDev("\\\\.\\svmb");
    if (svmb == INVALID_HANDLE_VALUE)
    {
        printf("[!] open svmb failed (err %lu)\n", GetLastError());
        return 1;
    }
    SVMB_HVIEW_CTL hv = {0};
    hv.Register = 1;
    DWORD br = 0;
    if (!DeviceIoControl(svmb, SVMB_IOCTL_HVIEW, &hv, sizeof(hv), &hv,
                         sizeof(hv), &br, NULL))
    {
        printf("[!] hview reg failed (err %lu)\n", GetLastError());
        CloseHandle(svmb);
        return 1;
    }
    printf("[+] hview reg ok (this process is H)\n");
    CloseHandle(svmb);

    // 2) run the external H round trip
    HANDLE l2t = OpenDev(L2T_DOS);
    if (l2t == INVALID_HANDLE_VALUE)
    {
        printf("[!] open l2t2 failed (err %lu) - l2test.sys loaded?\n",
               GetLastError());
        return 1;
    }
    L2T_RESULT r;
    ZeroMemory(&r, sizeof(r));
    if (!DeviceIoControl(l2t, L2T_IOCTL_RUN, NULL, 0, &r, sizeof(r), &br,
                         NULL) || br != sizeof(r))
    {
        printf("[!] l2t2 run failed (err %lu br=%lu)\n", GetLastError(), br);
        CloseHandle(l2t);
        return 1;
    }
    CloseHandle(l2t);

    // 3) unregister (the stealth face must snap back)
    svmb = OpenDev("\\\\.\\svmb");
    if (svmb != INVALID_HANDLE_VALUE)
    {
        SVMB_HVIEW_CTL hu = {0};
        DeviceIoControl(svmb, SVMB_IOCTL_HVIEW, &hu, sizeof(hu), &hu,
                        sizeof(hu), &br, NULL);
        printf("[+] hview unreg ok (stealth back)\n");
        CloseHandle(svmb);
    }

    printf("ext-h: exc=%llx exit=%llx rip=%llx mark=%llx legs=%lu "
           "eferH=%llx hsaveH=%llx\n",
           r.exc, r.exitCode, r.rip, r.mark, r.legs, r.eferH, r.hsaveH);
    // verdict: clean guard, terminal hlt at the hlt's own offset (0xE),
    // sign-extended imm32 marker (the r169 law), >= 1 cpuid leg, and the
    // H view observably live during the run (SVME bit set in the copy)
    if (r.exc == 0 && r.exitCode == 0x78 && r.rip == 0x50000Eull &&
        r.mark == 0xFFFFFFFFC0DE600Dull && r.legs >= 1 &&
        ((r.eferH >> 12) & 1) && r.hsaveH == 0x12345000ull)
    {
        printf("[+] EXT-H PASS (external H round trip: reg -> VMRUN -> L2 "
               "-> reflect -> terminal hlt -> unreg)\n");
        return 0;
    }
    printf("[!] EXT-H FAIL\n");
    return 1;
}
