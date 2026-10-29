// l2test.sys - r177 external-H judge: the first out-of-tree consumer of
// svmb's software nesting. A user loader (l2load.exe) registers this
// process's CR3 as an H view through svmb's real IOCTL face, then sends
// RUN here: this driver (running in that same process context) builds
// H's world - a VMCB with a nested-NPT session, L2 page tables, and a
// cpuid+mov+hlt program - and executes VMRUN. svmb accepts, L2 runs,
// exits reflect back to THIS driver (it is H), and the terminal-hlt
// verdict proves the whole external round trip end to end.
//
// VMCB offsets per AMD APM Vol.2 ch.15 (validated against svmb's
// hw/vmcb.h static_asserts): only the fields this test touches are
// declared - the accept path reads the rest from the inherited L1 state.
#include <ntddk.h>
#include <intrin.h>

#define L2T_DEVICE_NAME  L"\\Device\\l2t2"
#define L2T_DOS_NAME     L"\\??\\l2t2"
#define L2T_IOCTL_RUN    CTL_CODE(0x8000, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define L2T_IOCTL_PROBE  CTL_CODE(0x8000, 0x901, METHOD_BUFFERED, FILE_ANY_ACCESS)

// r185 probe: what execution context does DriverEntry get when the driver
// is NtLoadDriver'd from a user loader that just registered itself as an
// H view? the reference hypervisor's CheckSVM (CPUID 8000_0001/8000_000A + VM_CR read)
// runs entirely in ITS DriverEntry - if that context's CR3 is not the
// registered one, the stealth face applies and its init dies with the
// catch-all STATUS_INSUFFICIENT_RESOURCES.
typedef struct _L2T_PROBE
{
    unsigned long long entryCr3, entryPid;
    unsigned long long entryVendor; // leaf0 EBX|EDX<<32 (AuthenticAMD check)
    unsigned long long entrySvm;    // 8000_0001h ECX bit2
    unsigned long long entryNpt;    // 8000_000Ah EDX bit0
    unsigned long long entryVmcr;   // 0xC0010114 read (bit4 = SVMDIS)
    unsigned long long nowCr3, nowPid;
    unsigned long long nowVendor, nowSvm, nowNpt, nowVmcr;
} L2T_PROBE;

static volatile L2T_PROBE g_probe;

// ---- minimal VMCB field offsets (APM / svmb vmcb.h asserts) ----
#define VMCB_OP1        0x000u  // InterceptOpcode1 (ic1 word)
#define VMCB_EXITCODE   0x070u
#define VMCB_NCR3       0x0B0u
#define VMCB_NPCTL      0x090u  // NP_ENABLE bit0
#define VMCB_RFLAGS     0x570u
#define VMCB_RIP        0x578u
#define VMCB_CR3        0x550u
#define VMCB_RSP        0x5D8u
#define VMCB_RAX        0x5F8u

typedef struct _L2T_RESULT
{
    unsigned long long exc;     // SEH code from the vmrun guard (0 = clean)
    unsigned long long exitCode;// terminal exit (expect 0x78 hlt)
    unsigned long long rip;     // terminal rip (expect 0x50000E - the hlt)
    unsigned long long mark;    // L2's marker (expect 0xFFFFFFFFC0DE600D -
                                // imm32 sign-extension, the r169 law)
    unsigned long long rax;     // L2's rax at terminal
    unsigned long long eferH;   // H-view EFER readback after the SVME write
    unsigned long long hsaveH;  // H-view HSAVE readback
    unsigned long legs;         // L2 legs completed (expect >= 1)
    unsigned intrSeen;          // 0x60 exits retried
} L2T_RESULT;

#define L2T_TAG '2LTS'

static void* Alloc2K(void)
{
    return ExAllocatePoolWithTag(NonPagedPool, 0x2000, L2T_TAG);
}
static void Free2K(void* p)
{
    if (p)
        ExFreePoolWithTag(p, L2T_TAG);
}
static unsigned long long PaOf(void* p)
{
    PHYSICAL_ADDRESS pa = MmGetPhysicalAddress(p);
    return (unsigned long long)pa.QuadPart;
}
// 4K-align inside a 2-page allocation
static unsigned char* PageAt(void* raw)
{
    return (unsigned char*)(((unsigned long long)(ULONG_PTR)raw + 0xFFFull) &
                            ~0xFFFull);
}
static void W64(unsigned char* vmcb, unsigned off, unsigned long long v)
{
    *(unsigned long long*)(vmcb + off) = v;
}
static unsigned long long R64(const unsigned char* vmcb, unsigned off)
{
    return *(unsigned long long*)(vmcb + off);
}

static NTSTATUS RunExternalH(L2T_RESULT* res)
{
    RtlZeroMemory(res, sizeof(*res));
    // pages: vmcb, code, stack, mark, l2 pml4/pdpt/pd/pt, hn pml4/pdpt/pd/pt
    void* pages[12] = {};
    for (int i = 0; i < 12; ++i)
    {
        pages[i] = Alloc2K();
        if (!pages[i])
        {
            for (int j = 0; j < i; ++j)
                Free2K(pages[j]);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    unsigned char* vmcb = PageAt(pages[0]);
    unsigned char* code = PageAt(pages[1]);
    unsigned char* stack = PageAt(pages[2]);
    unsigned long long* mark = (unsigned long long*)PageAt(pages[3]);
    unsigned long long* l2t[4] = {
        (unsigned long long*)PageAt(pages[4]),
        (unsigned long long*)PageAt(pages[5]),
        (unsigned long long*)PageAt(pages[6]),
        (unsigned long long*)PageAt(pages[7])
    };
    unsigned long long* hn[4] = {
        (unsigned long long*)PageAt(pages[8]),
        (unsigned long long*)PageAt(pages[9]),
        (unsigned long long*)PageAt(pages[10]),
        (unsigned long long*)PageAt(pages[11])
    };
    const unsigned long long L2_CODE_VA = 0x500000;
    const unsigned long long L2_STACK_VA = 0x511000;
    const unsigned long long L2_MARK_VA = 0x513000;

    // L2's own CR3 tables: VA->GPA identity for the three pages
    l2t[0][0] = PaOf(l2t[1]) | 3;
    l2t[1][0] = PaOf(l2t[2]) | 3;
    l2t[2][2] = PaOf(l2t[3]) | 3;
    l2t[3][(L2_CODE_VA >> 12) & 0x1FF] = L2_CODE_VA | 3;
    l2t[3][(L2_STACK_VA >> 12) & 0x1FF] = L2_STACK_VA | 3;
    l2t[3][(L2_MARK_VA >> 12) & 0x1FF] = L2_MARK_VA | 3;

    // H's nested NPT: window GPAs -> the real frames
    hn[0][0] = PaOf(hn[1]) | 3;
    hn[1][0] = PaOf(hn[2]) | 3;
    hn[2][2] = PaOf(hn[3]) | 3;
    hn[3][(L2_CODE_VA >> 12) & 0x1FF] = PaOf(code) | 7;
    hn[3][(L2_STACK_VA >> 12) & 0x1FF] = PaOf(stack) | 7;
    hn[3][(L2_MARK_VA >> 12) & 0x1FF] = PaOf(mark) | 7;

    // L2 program: cpuid; mov qword [0x513000], 0xC0DE600D; hlt
    static const unsigned char l2prog[] = {
        0x0F, 0xA2,                                    // cpuid
        0x48, 0xC7, 0x04, 0x25, 0x00, 0x30, 0x51, 0x00,
        0x0D, 0x60, 0xDE, 0xC0,                        // mov [mark]
        0xF4,                                          // hlt (at +0xE)
    };
    RtlCopyMemory(code, l2prog, sizeof(l2prog));
    mark[0] = 0;

    // H-view exercise (r171): with this process's CR3 registered, the
    // EFER/HSAVE writes land in the virtual copies - the real MSRs stay
    // untouched. Readbacks prove the H view is live for THIS context.
    unsigned long long efer0 = 0;
    __try { efer0 = __readmsr(0xC0000080); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { __writemsr(0xC0000080, efer0 | (1ull << 12)); } // EFER.SVME
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { __writemsr(0xC0010117, 0x12345000ull); }        // VM_HSAVE_PA
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    res->eferH = 0; res->hsaveH = 0;
    __try { res->eferH = __readmsr(0xC0000080); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    __try { res->hsaveH = __readmsr(0xC0010117); } __except (EXCEPTION_EXECUTE_HANDLER) {}

    RtlZeroMemory(vmcb, 0x1000);
    // ic1 (InterceptOpcode1) lives at VMCB +0x0C - offset 0x00 is the
    // CR-read word (the r181 vlog intent record caught this misarm).
    // r186: HLT (bit24) armed too - the intent gate reflects only the
    // exits H itself ordered, so the terminal hlt must be H's request.
    *(unsigned long*)((unsigned char*)vmcb + 0x0C) = (1ul << 18) | (1ul << 24);
    W64(vmcb, VMCB_NPCTL, 1);             // NP_ENABLE: the shadow session
    W64(vmcb, VMCB_NCR3, PaOf(hn[0]));    // H's nested root
    W64(vmcb, VMCB_RIP, L2_CODE_VA);
    W64(vmcb, VMCB_RSP, L2_STACK_VA + 0xFF8);
    W64(vmcb, VMCB_RFLAGS, 0x2);          // IF clear (the r173 storm law)
    W64(vmcb, VMCB_RAX, 0x7BADBEEFull);
    W64(vmcb, VMCB_CR3, PaOf(l2t[0]));
    unsigned long long vmcbPa = PaOf(vmcb);

    DbgPrintEx(77, 3, "[l2t2] run: vmcb=%llx eferH=%llx hsaveH=%llx\n",
               vmcbPa, res->eferH, res->hsaveH);

    for (unsigned guard = 0; guard < 256; ++guard)
    {
        __try
        {
            __svm_vmrun(vmcbPa); // size_t: the full 64-bit PA (truncating
                                 // it reads a garbage low-half VMCB)
            res->exc = 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            res->exc = (unsigned long long)GetExceptionCode();
            break;
        }
        unsigned long long ec = R64(vmcb, VMCB_EXITCODE);
        unsigned long long rip = R64(vmcb, VMCB_RIP);
        if (ec == 0x60) // external intr: IF=0 flows to L1 - retry
        {
            ++res->intrSeen;
            continue;
        }
        if (ec == 0x72 && rip == L2_CODE_VA) // L2's cpuid reflected to H
        {
            ++res->legs;
            W64(vmcb, VMCB_RIP, rip + 2); // past the cpuid
            continue;
        }
        res->exitCode = ec;
        res->rip = rip;
        res->rax = R64(vmcb, VMCB_RAX);
        break;
    }
    res->mark = mark[0];
    // r178: H teardown invalidation (the r176 contract) - without it a
    // later world whose pool allocations reuse this one's frames can be
    // served this world's stale shadow composes
    __try { __svm_invlpga(0, 1); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    DbgPrintEx(77, 3, "[l2t2] done: exc=%llx exit=%llx rip=%llx mark=%llx "
               "legs=%u intr=%u\n",
               res->exc, res->exitCode, res->rip, res->mark, res->legs,
               res->intrSeen);
    for (int i = 0; i < 12; ++i)
        Free2K(pages[i]);
    return STATUS_SUCCESS;
}

static DRIVER_UNLOAD L2tUnload;
static void L2tUnload(PDRIVER_OBJECT drv)
{
    UNICODE_STRING dos = RTL_CONSTANT_STRING(L2T_DOS_NAME);
    IoDeleteSymbolicLink(&dos);
    IoDeleteDevice(drv->DeviceObject);
}

static DRIVER_DISPATCH L2tDispatch;

// capture one context snapshot (CR3/PID + the three CheckSVM inputs)
static void ProbeFill(volatile L2T_PROBE* p, int entry)
{
    int c[4];
    __cpuidex(c, 0, 0);
    unsigned long long vendor = (unsigned long long)(unsigned)c[1] |
                                ((unsigned long long)(unsigned)c[3] << 32);
    __cpuidex(c, 0x80000001, 0);
    unsigned long long svm = (c[2] >> 2) & 1;
    __cpuidex(c, 0x8000000A, 0);
    unsigned long long npt = c[3] & 1;
    unsigned long long vmcr = __readmsr(0xC0010114);
    if (entry)
    {
        p->entryCr3 = __readcr3();
        p->entryPid = (unsigned long long)(ULONG_PTR)PsGetCurrentProcessId();
        p->entryVendor = vendor;
        p->entrySvm = svm;
        p->entryNpt = npt;
        p->entryVmcr = vmcr;
    }
    else
    {
        p->nowCr3 = __readcr3();
        p->nowPid = (unsigned long long)(ULONG_PTR)PsGetCurrentProcessId();
        p->nowVendor = vendor;
        p->nowSvm = svm;
        p->nowNpt = npt;
        p->nowVmcr = vmcr;
    }
}

static NTSTATUS L2tDispatch(PDEVICE_OBJECT dev, PIRP irp)
{
    UNREFERENCED_PARAMETER(dev);
    PIO_STACK_LOCATION io = IoGetCurrentIrpStackLocation(irp);
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG info = 0;
    if (io->MajorFunction == IRP_MJ_CREATE || io->MajorFunction == IRP_MJ_CLOSE)
    {
        status = STATUS_SUCCESS;
    }
    else if (io->MajorFunction == IRP_MJ_DEVICE_CONTROL &&
        io->Parameters.DeviceIoControl.IoControlCode == L2T_IOCTL_RUN &&
        io->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(L2T_RESULT))
    {
        status = RunExternalH((L2T_RESULT*)irp->AssociatedIrp.SystemBuffer);
        if (NT_SUCCESS(status))
            info = sizeof(L2T_RESULT);
    }
    else if (io->MajorFunction == IRP_MJ_DEVICE_CONTROL &&
        io->Parameters.DeviceIoControl.IoControlCode == L2T_IOCTL_PROBE &&
        io->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(L2T_PROBE))
    {
        L2T_PROBE* out = (L2T_PROBE*)irp->AssociatedIrp.SystemBuffer;
        ProbeFill(&g_probe, 0);
        *out = g_probe;
        status = STATUS_SUCCESS;
        info = sizeof(L2T_PROBE);
    }
    irp->IoStatus.Status = status;
    irp->IoStatus.Information = info;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return status;
}

NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    UNREFERENCED_PARAMETER(reg);
    // r185: record the load-time context BEFORE anything else - this is
    // the exact vantage the reference hypervisor's CheckSVM sees.
    ProbeFill(&g_probe, 1);
    PDEVICE_OBJECT dev = NULL;
    UNICODE_STRING name = RTL_CONSTANT_STRING(L2T_DEVICE_NAME);
    UNICODE_STRING dos = RTL_CONSTANT_STRING(L2T_DOS_NAME);
    NTSTATUS st = IoCreateDevice(drv, 0, &name, FILE_DEVICE_UNKNOWN,
                                 FILE_DEVICE_SECURE_OPEN, FALSE, &dev);
    if (!NT_SUCCESS(st))
        return st;
    st = IoCreateSymbolicLink(&dos, &name);
    if (!NT_SUCCESS(st))
    {
        IoDeleteDevice(dev);
        return st;
    }
    drv->MajorFunction[IRP_MJ_CREATE] = L2tDispatch;
    drv->MajorFunction[IRP_MJ_CLOSE] = L2tDispatch;
    drv->MajorFunction[IRP_MJ_DEVICE_CONTROL] = L2tDispatch;
    drv->DriverUnload = L2tUnload;
    DbgPrintEx(77, 3, "[l2t2] external-H driver loaded (r177)\n");
    return STATUS_SUCCESS;
}
