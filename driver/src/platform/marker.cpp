// r141: death-proof progress marker - the QEMU bench's re-entry death
// kills every live channel (serial can drop bytes QEMU-side, SSH/HTTP die
// with the guest), so the only trustworthy record of how far a
// start/stop/cycle got is one that lands on DISK and survives the
// post-death reboot. Appends one line per marker to
// \??\C:\svmb_trace.txt from PASSIVE_LEVEL call sites only.
#include "platform/marker.h"
#include <ntifs.h>
#include <ntstrsafe.h>

namespace svmb {

static void MarkerAppend(const char* line)
{
    // PASSIVE-only by contract (Zw file IO); callers are the IOCTL and
    // Start/Stop orchestration paths, never the VMM exit path.
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;

    UNICODE_STRING us;
    RtlInitUnicodeString(&us, L"\\??\\C:\\svmb_trace.txt");
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               nullptr, nullptr);
    IO_STATUS_BLOCK iosb = {};
    HANDLE h = nullptr;
    NTSTATUS st = ZwCreateFile(&h, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb,
                               nullptr, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ,
                               FILE_OPEN_IF, FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
                               nullptr, 0);
    if (!NT_SUCCESS(st))
        return;

    LARGE_INTEGER sys;
    KeQuerySystemTime(&sys);
    char out[256];
    RtlStringCbPrintfA(out, sizeof(out), "%llu.%03u Kd %s\r\n",
                       (unsigned long long)(sys.QuadPart / 10000000ULL),
                       (unsigned)((sys.QuadPart / 10000) % 1000), line);
    ZwWriteFile(h, nullptr, nullptr, nullptr, &iosb, out, (ULONG)strlen(out),
                nullptr, nullptr);
    ZwClose(h);
}

void TraceMarker(const char* tag)
{
    MarkerAppend(tag);
}

void TraceMarkerFmt(const char* fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    NTSTATUS st = RtlStringCbVPrintfA(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (NT_SUCCESS(st))
        MarkerAppend(line);
}

} // namespace svmb
