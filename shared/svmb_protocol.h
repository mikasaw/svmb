// svmb - Windows AMD-V (SVM) extensible virtualization framework
// R3 <-> R0 shared protocol definitions (single source of truth)
#ifndef SVMB_PROTOCOL_H
#define SVMB_PROTOCOL_H

#ifdef _KERNEL_MODE
#include <ntdef.h>
typedef unsigned char  svmb_u8;
typedef unsigned short svmb_u16;
typedef unsigned long  svmb_u32;
typedef unsigned long long svmb_u64;
#else
#include <stdint.h>
typedef uint8_t  svmb_u8;
typedef uint16_t svmb_u16;
typedef uint32_t svmb_u32;
typedef uint64_t svmb_u64;
#endif

#define SVMB_PROTO_MAGIC   0x53424D53UL   // 'SVMB'
#define SVMB_PROTO_VERSION 1

// ---- IOCTL codes ----
#define SVMB_DEVICE_NAME   L"\\Device\\svmb"
#define SVMB_DOS_NAME      L"\\DosDevices\\svmb"
#define SVMB_CTL_TYPE      0x8000   // user-defined device type for CTL_CODE
#define SVMB_CTL(fn)       CTL_CODE(SVMB_CTL_TYPE, (fn), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define SVMB_IOCTL_GET_INFO      SVMB_CTL(0x800)
#define SVMB_IOCTL_HV_CONTROL    SVMB_CTL(0x801)
#define SVMB_IOCTL_LOG_READ      SVMB_CTL(0x802)
#define SVMB_IOCTL_MODULE_CTL    SVMB_CTL(0x803)
#define SVMB_IOCTL_NPT_HOOK      SVMB_CTL(0x810)
#define SVMB_IOCTL_NPT_PROBE     SVMB_CTL(0x811)
#define SVMB_IOCTL_CR3_CONFIG    SVMB_CTL(0x820)
#define SVMB_IOCTL_CR3_STATS     SVMB_CTL(0x821)
#define SVMB_IOCTL_CR3_POISON    SVMB_CTL(0x822) // r71 test hook: corrupt one
                                                 // armed sentinel watermark
                                                 // (out: u32 slots poisoned)
#define SVMB_IOCTL_CR3_PROT      SVMB_CTL(0x823) // r75 protected regions:
                                                 // L1 blocking (guest MM) +
                                                 // L2 NPT sensing (see
                                                 // SVMB_CR3_PROT)
#define SVMB_IOCTL_CR3_PROBE     SVMB_CTL(0x824) // r76 L2-bypass test probe:
                                                 // kernel MDL write into a
                                                 // protected region (see
                                                 // SVMB_CR3_PROBE)
#define SVMB_IOCTL_DBG_CONFIG    SVMB_CTL(0x830)
#define SVMB_IOCTL_DBG_EVENTS    SVMB_CTL(0x831)
#define SVMB_IOCTL_STRESS        SVMB_CTL(0x840)
#define SVMB_IOCTL_SELFTEST      SVMB_CTL(0x850)
#define SVMB_IOCTL_EXITPROF      SVMB_CTL(0x851)

// ---- info query ----
#pragma pack(push, 8)

typedef struct _SVMB_INFO {
    svmb_u32 Magic;            // SVMB_PROTO_MAGIC
    svmb_u32 ProtoVersion;
    svmb_u32 DriverVersion;
    svmb_u32 VmxHvState;       // SVMB_HV_STATE_xxx
    svmb_u32 CoresVirtualized; // number of cores currently under SVM
    svmb_u32 CoreCount;        // active processor count
    svmb_u32 PaWidthBits;      // physical address width (CPUID.80000008)
    svmb_u32 NptViewCount;
    svmb_u64 ExitCounter;      // total vmexit count (approx, sum of per-core)
    svmb_u32 ModuleCount;
    svmb_u32 Reserved;
} SVMB_INFO, *PSVMB_INFO;

// r45 exit-rate telemetry: per-cause VMEXIT histogram. Slots 0x00..0xFF
// mirror the SVM exit-reason namespace 1:1; slot SVMB_XP_NPF catches the
// NPF exit (code 0x400); SVMB_XP_OTHER catches everything else. Sampled
// cumulatively - the rate is the difference between two samples.
#define SVMB_XP_DENSE  0x100
#define SVMB_XP_NPF    0x100
#define SVMB_XP_OTHER  0x101
#define SVMB_XP_SLOTS  0x102

typedef struct _SVMB_EXITPROF {
    svmb_u32 Version;
    svmb_u32 CpuCount;
    svmb_u64 Total;                       // sum of all histogram slots
    svmb_u64 Hist[SVMB_XP_SLOTS];         // summed across all vCPUs
} SVMB_EXITPROF, *PSVMB_EXITPROF;

typedef enum _SVMB_HV_STATE {
    SvmbHvStateOff = 0,
    SvmbHvStateStarting,
    SvmbHvStateRunning,
    SvmbHvStateStopping,
} SVMB_HV_STATE;

typedef struct _SVMB_HV_CONTROL {   // IOCTL_HV_CONTROL
    svmb_u32 Action;                 // 0 = start, 1 = stop, 2 = enter/exit stress loop
    svmb_u32 RepeatCount;            // for stress loop
} SVMB_HV_CONTROL, *PSVMB_HV_CONTROL;

typedef struct _SVMB_LOG_ENTRY {
    svmb_u32 Core;
    svmb_u32 Level;       // 1=err 2=warn 3=info 4=verbose
    svmb_u64 Tsc;
    svmb_u16 Length;
    char    Text[224];
} SVMB_LOG_ENTRY, *PSVMB_LOG_ENTRY;

#define SVMB_MAX_MODULES 16
typedef struct _SVMB_MODULE_INFO {
    char    Name[16];
    svmb_u32 Attached;     // 1 = handlers active
    svmb_u32 ExitHandled;  // count of exits handled by this module (approx)
} SVMB_MODULE_INFO;

typedef struct _SVMB_MODULE_LIST {  // MODULE_CTL with Action=2 (query)
    svmb_u32 Count;
    SVMB_MODULE_INFO Modules[SVMB_MAX_MODULES];
} SVMB_MODULE_LIST, *PSVMB_MODULE_LIST;

typedef struct _SVMB_MODULE_CTL {   // IOCTL_MODULE_CTL
    svmb_u32 Action;      // 0 = attach, 1 = detach, 2 = query list
    char    Name[16];
} SVMB_MODULE_CTL, *PSVMB_MODULE_CTL;

typedef struct _SVMB_NPT_HOOK_CTL {  // IOCTL_NPT_HOOK
    svmb_u32 Action;      // 0 = add, 1 = remove, 2 = list
    svmb_u32 Count;       // in: capacity for list; out: hook count
    svmb_u64 TargetVa;    // kernel VA to hook (add/remove)
    svmb_u64 DetourVa;    // replacement function VA (add)
    struct {
        svmb_u64 TargetVa;
        svmb_u64 DetourVa;
    } Hooks[32];
} SVMB_NPT_HOOK_CTL, *PSVMB_NPT_HOOK_CTL;

// M3 live-NPT probe (IOCTL_NPT_PROBE): deterministic exercises for the
// NPF exit path and the view-switch publish protocol. Requires the
// hypervisor running AND NptEnable=1.
typedef struct _SVMB_NPT_PROBE {
    svmb_u32 Stage;         // in: 0 = NPF slide cycle, 1 = view-switch cycle,
                            //     2 = install + byte dump + remove (no exec),
                            //     3 = real-kernel hook passthrough,
                            //     4 = real-kernel hook override (r35 callback
                            //       layer: deny cmd.exe, pass notepad.exe)
    svmb_u32 Iterations;    // in: stage 0 calls per phase (0 = default 16);
                            //     stage 3/4 opens per core (0 = default 4)
    svmb_u64 DetourHits;    // out s0: hidden-copy (detour) executions
    svmb_u64 OrigHitsHooked;// out s0: original-body executions while hooked
    svmb_u64 OrigHitsAfter; // out s0: original-body executions after remove
    svmb_u64 ExitDelta;     // out: vmexit delta across the stage
    svmb_u64 TargetVa;      // out s0/s2/s3/s4: hooked function VA
    svmb_u64 Pml4Pa;        // out s1: new active view PML4 PA
    svmb_u64 VmcbNcr3;      // out s1: local core VMCB NCr3 readback after switch
    svmb_u64 PagePa;        // out s2: hooked 4K page PA
    svmb_u64 HiddenPa;      // out s2: hidden copy page PA
    svmb_u64 TrampPa;       // out s2/s3/s4: trampoline page PA
    svmb_u32 StolenLen;     // out s2: stolen prefix length
    svmb_u32 Off;           // out s2: target offset within its page
    svmb_u8  OrigBytes[32];   // out s2: original page bytes @ target
    svmb_u8  HiddenBytes[32];// out s2: hidden-copy bytes @ target offset
    svmb_u8  TrampBytes[16]; // out s2: trampoline bytes
    svmb_u64 HookHits;      // out s3/s4: callback executions (all cores)
    svmb_u32 OpenOk;        // out s3: successful opens through the callback
    svmb_u32 LastStatus;    // out s3: last NtCreateFile status
    svmb_u32 HookId;        // out s3/s4: assigned callback slot id
    svmb_u32 OvDeniedOk;    // out s4: target-file opens denied by override
    svmb_u32 OvOtherOk;     // out s4: other-file opens passed through
    svmb_u64 Cr3Value;      // out s5: CR3 as this thread's kernel context
                            //     sees it (spoofed when cr3 watch targets
                            //     this process; see Cr3ResolveRead)
} SVMB_NPT_PROBE, *PSVMB_NPT_PROBE;

typedef struct _SVMB_CR3_CONFIG {   // IOCTL_CR3_CONFIG
    svmb_u32 EnableReadSpoof;   // spoof CR3 reads for monitored processes
    svmb_u32 EnableWriteMonitor;// monitor CR3 writes (context switches)
    svmb_u32 EnableProcessView; // switch NPT view per process
    svmb_u32 Flags;
    svmb_u64 XorKey;            // fake_cr3 = (real_cr3 ^ XorKey), 0 = identity
    svmb_u64 TargetCr3;         // process directory table base to monitor (0 = config only)
    svmb_u32 TargetPid;         // resolve through the seed table (0 = unused)
    char TargetImage[16];       // image basename to watch (ascii); arms the
                                // running process now and future creates via
                                // the seed notify path; overrides TargetCr3/Pid
} SVMB_CR3_CONFIG, *PSVMB_CR3_CONFIG;

typedef struct _SVMB_CR3_STATS {   // IOCTL_CR3_STATS
    svmb_u64 ReadExitCount;
    svmb_u64 WriteExitCount;
    svmb_u64 WriteApplyCount;    // writes actually applied (vmcb.cr3)
    svmb_u64 ViewSwitchCount;
    svmb_u32 MonitoredCount;     // entries in the cr3->view table
    svmb_u32 SentinelTrips;      // r48: watch-process tripwire hits
    svmb_u32 SentinelArmed;      // r48: pages currently W-denied
    svmb_u32 SentinelDiag;       // r49: low16 = arm-worker wakes,
                                 // high16 = last skip-why code (see
                                 // cr3_monitor.cpp kSentWhy*)
    svmb_u32 SentinelReuse;      // r68: watermark kill-switch hits (deny
                                 // torn off a recycled frame)
    svmb_u32 NpfHeals;           // r68: Unhandled hard-net resolve count
    svmb_u32 RegionTrips;        // r76: protected-region L2-bypass alerts
    svmb_u32 WiggleKicks;        // r77: NCr3 shadow-kick count (trip resolve)
    svmb_u32 ReadVmCalls;        // r87: CR3_READVM invocations
    svmb_u32 ReadVmPages;        // r87: pages physically resolved (wraps)
    svmb_u32 KillAttempts;       // r89: kill workitems queued
    svmb_u32 KillDenylisted;     // r89: refused (pid<4 / deny-list / unresolvable)
    svmb_u32 KillDropped;        // r89: trips dropped while a kill was in flight
    svmb_u32 SuspAttempts;       // r91: suspend workitems queued
    svmb_u32 SuspDenylisted;     // r91: refused (pid<4 / deny-list / unresolvable)
    svmb_u32 SuspDropped;        // r91: trips dropped while a suspend was in flight
    svmb_u32 SuspActive;         // r91: processes currently in the suspend table
} SVMB_CR3_STATS, *PSVMB_CR3_STATS;
// Mixed-deploy note (r89): the STATS tail (ReadVm*/Kill*/Susp*) is
// APPEND-ONLY - an old driver + new ctl reads a zeroed tail (the driver
// zero-fills short buffered outputs), so the direction is
// silent-but-harmless; a new driver + old ctl fails LOUD on the
// buffer-length checks (r82 convention).

// r91 plan C- suspension table (IOCTL_SUSP_LIST / IOCTL_SUSP_RESUME).
// Both run UNGATED (see SVMB_PROT_POLICY_SUSPEND): a suspended writer
// holds the gate inside its in-flight bypass IOCTL, and list/resume must
// work from outside. svmb_u32 Op: 0 = list, 1 = resume pid, 2 = resume all.
#define SVMB_IOCTL_SUSP_LIST    SVMB_CTL(0x826)
#define SVMB_IOCTL_SUSP_RESUME  SVMB_CTL(0x827)
#define SVMB_SUSP_MAX_ENTRIES   16
#define SVMB_SUSP_OP_LIST       0
#define SVMB_SUSP_OP_RESUME_PID 1
#define SVMB_SUSP_OP_RESUME_ALL 2
typedef struct _SVMB_SUSP_ENTRY {
    svmb_u32 Pid;               // suspended writer process
    svmb_u32 RegionId;          // the region whose trip suspended it
    svmb_u64 Tick;              // suspend time (KeQueryInterruptTime)
    char Image[16];             // image basename from the seed node
} SVMB_SUSP_ENTRY, *PSVMB_SUSP_ENTRY;
typedef struct _SVMB_SUSP {        // IOCTL_SUSP_LIST / SUSP_RESUME
    svmb_u32 Op;                   // SVMB_SUSP_OP_* (in)
    svmb_u32 Pid;                  // RESUME_PID in: target process
    svmb_u32 OutCount;             // out: entries returned / resumed count
    svmb_u32 SuspAvailable;        // out: PsSuspendProcess resolved flag
    SVMB_SUSP_ENTRY Entries[SVMB_SUSP_MAX_ENTRIES]; // list action
} SVMB_SUSP, *PSVMB_SUSP;

// r92 TSC probe readback: returns the driver's own __rdtsc(). The driver
// executes in L1 context (outside its VMRUN loop), so this reading is NOT
// offset by the guest VMCB's TscOffset - pairing it with the user-mode
// RDTSC (which IS offset-affected if the vhv honors the field) measures
// the TscOffset-honor question to microsecond skew. No input.
#define SVMB_IOCTL_RDTSC        SVMB_CTL(0x828)

// r99 syscall audit (sysaudit module): a passive execution hook on
// MmCopyVirtualMemory - the single kernel funnel for NtReadVirtualMemory
// and NtWriteVirtualMemory, i.e. every ReadProcessMemory/WriteProcessMemory
// - records caller (cr3->pid/image) + source/target process+address + size
// into a bounded ring; IOCTL_SYSCALL_AUDIT drains it (consume, like
// LOG_READ). r101: the unexported Nt* pages join via SSDT+SSN - KiService
// Table sits at RVA 0x424C10 in 18362.592 and its u32 entries are ABSOLUTE
// image RVAs (target = nt_base + entry; offline-verified against the guest
// image, SHA256 2ea1e2a2...). The four SSN->RVA constants are re-verified
// against the running image at attach; any mismatch refuses to arm rather
// than X-deny a wrong page (a wrongly denied page is a freeze generator).
// Entry fields for the SYSCALL-entry targets (NtCF/NtRd/NtWr/NtOp): at
// the entry fetch the ABI is already restored - KiSystemCall64+0x24 does
// `mov rcx, r10` (undone by nothing; crash-dump disasm-verified) - so
// arg0..3 are rcx/rdx/r8/r9. r10 is dispatcher scratch at that point
// (the service address; live saves show a >>16-shifted value) and is NOT
// an argument.
#define SVMB_IOCTL_SYSCALL_AUDIT SVMB_CTL(0x829)
#define SVMB_SYSAUD_FN_COPY     0       // MmCopyVirtualMemory (call-entry
                                        // regs: arg0..3 = rcx/rdx/r8/r9)
#define SVMB_SYSAUD_FN_NTCF     1       // NtCreateFile (exported anchor;
                                        // SSN 0x55)
#define SVMB_SYSAUD_FN_LOST     2       // ring-overflow tombstone (not an
                                        // entry - the drain consumes and
                                        // skips it to preserve sequence)
#define SVMB_SYSAUD_FN_NTRD     3       // NtReadVirtualMemory (SSN 0x3F)
#define SVMB_SYSAUD_FN_NTWR     4       // NtWriteVirtualMemory (SSN 0x3A)
#define SVMB_SYSAUD_FN_NTOP     5       // NtOpenProcess (SSN 0x26)
#define SVMB_SYSAUD_MAX_TARGETS 6       // sentinel above the max FnId
#define SVMB_SYSAUD_MAX_ENTRIES 64
typedef struct _SVMB_SYSAUD_ENTRY {
    svmb_u64 Tick;              // KeQueryInterruptTime at the call
    svmb_u64 SrcProcess;        // COPY: rcx src EPROCESS | Nt*: rcx arg0
                                // (process handle / out PHANDLE*)
    svmb_u64 SrcAddress;        // COPY: rdx src addr | Nt*: rdx arg1
                                // (target base / DesiredAccess)
    svmb_u64 DstProcess;        // COPY: r8 dst EPROCESS | Nt*: target pid
                                // once the deferred handle resolver has
                                // converged (r103; 0 = unresolved - the
                                // first calls of a new (caller, handle)
                                // pair land here)
    svmb_u64 DstAddress;        // COPY: r9 dst addr | Nt*: r8 arg2
                                // (buffer / OBJECT_ATTRIBUTES*)
    svmb_u64 Size;              // COPY: [rsp+28h] bytes | Nt*: r9 arg3
                                // (byte count / IO_STATUS_BLOCK* /
                                // CLIENT_ID*)
    svmb_u32 Pid;               // attributed caller (0 = unresolved)
    svmb_u32 FnId;              // SVMB_SYSAUD_FN_*
    char Image[16];             // caller image basename ("" when unresolved)
    svmb_u64 Commit;            // slot commit marker (seq+1; 0 = empty)
} SVMB_SYSAUD_ENTRY, *PSVMB_SYSAUD_ENTRY;
typedef struct _SVMB_SYSCALL_AUDIT {        // IOCTL_SYSCALL_AUDIT (out)
    svmb_u32 OutCount;          // entries returned this call
    svmb_u32 Armed;             // 1 = hook installed (module attached)
    svmb_u32 Resolved;          // 1 = MmCopyVirtualMemory resolved
    svmb_u32 FailStage;         // 0 = none; last attach failure stage
    svmb_u64 Total;             // watched-entry fetches (target hits)
    svmb_u64 Lost;              // ring-full drops since attach
    svmb_u64 Noise;             // non-target function fetches on armed
                                // pages (counted, not recorded)
    svmb_u64 Target;            // resolved MmCopyVirtualMemory (0 = none)
    svmb_u64 TargetFile;        // resolved NtCreateFile (second sense page)
    svmb_u64 TargetRd;          // r101: resolved NtReadVirtualMemory via
                                // SSDT (0 = not armed; was Reserved)
    SVMB_SYSAUD_ENTRY Entries[SVMB_SYSAUD_MAX_ENTRIES];
} SVMB_SYSCALL_AUDIT, *PSVMB_SYSCALL_AUDIT;

// r101 kernel-VA qword dump (diagnostic; the kd replacement since r68):
// safe-reads a small window of kernel address space - the SSDT table /
// candidate-region inspector. In and out share the buffered buffer.
// Production-refused (main.cpp DevCtrl) alongside the other raw
// introspection IOCTLs.
#define SVMB_IOCTL_KDUMP         SVMB_CTL(0x82A)
#define SVMB_KDUMP_MAX_QWORDS    96
#define SVMB_KDUMP_BAD           0xBADBADBADBADBADBULL
typedef struct _SVMB_KDUMP {        // IOCTL_KDUMP (in/out, buffered)
    svmb_u64 Va;                    // in: kernel VA to read from (only
                                    // canonical-kernel prefiltered - NOT
                                    // RAM-checked: MMIO reads reach devices)
    svmb_u32 Qwords;                // in: how many (clamped to MAX)
    svmb_u32 FailIdx;               // out: first unreadable slot (= ok
                                    // count); Q beyond it is BAD-filled
    svmb_u64 Q[SVMB_KDUMP_MAX_QWORDS];
} SVMB_KDUMP, *PSVMB_KDUMP;

// r102 behavioral analytics over the sysaudit capture (the README
// behavior-analysis MVP): per-caller cross-process syscall rates in a
// fixed 1s window with edge-triggered alerts. Counted for attributed
// NTRD/NTWR/NTOP entries only (NtCF is ambient file I/O; pid=0
// unattributed system threads are counted in Dropped, not the table).
// TELEMETRY ONLY - an alert logs LOUD and sets the slot flag; response
// (kill/suspend) stays a separate pending policy decision. Approximate
// under NPF-exit concurrency by design (no locks on the hot path - r99
// law): counters may lose increments, alerts never double-fire (edge =
// unique InterlockedIncrement value + per-window Alerted flag).
#define SVMB_IOCTL_SYSAUD_BEHAVIOR  SVMB_CTL(0x82B)
#define SVMB_SYSAUD_BEHAVE_SLOTS    16      // power of two (hash mask)
#define SVMB_SYSAUD_BEHAVE_WIN_MS   1000
// thresholds are registry-overridable at DriverEntry (Parameters\
// BehaveRdThs / BehaveWrThs / BehaveOpThs); these are the defaults
#define SVMB_SYSAUD_BEHAVE_RD_THS   128     // cross-process reads / window
#define SVMB_SYSAUD_BEHAVE_WR_THS   32      // cross-process writes / window
#define SVMB_SYSAUD_BEHAVE_OP_THS   16      // OpenProcess / window
// r107 freeze flight recorder (test builds only; production refuses the
// IOCTL): raw view of the breadcrumb ring - the last hops through the
// NPF consumer / re-deny DPC / TLB kick paths. The frozen-guest readout
// path is vmrun suspend -> .vmss -> offline magic scan (tools/).
#define SVMB_IOCTL_CRUMBS            SVMB_CTL(0x82C)
#define SVMB_CRUMBS_MAX              128
typedef struct _SVMB_CRUMB {
    svmb_u32 Tag;
    svmb_u32 Cpu;
    svmb_u64 Tick;
    svmb_u64 Seq;               // monotonic claim; order key
} SVMB_CRUMB, *PSVMB_CRUMB;
typedef struct _SVMB_CRUMBS {       // IOCTL_CRUMBS (out)
    svmb_u32 Magic0;                // 'SBVC'
    svmb_u32 Magic1;
    svmb_u32 W;                     // total claims
    svmb_u32 OutCount;
    svmb_u64 Dropped;               // claims lost to ring wrap (approx)
    SVMB_CRUMB Entries[SVMB_CRUMBS_MAX];
} SVMB_CRUMBS, *PSVMB_CRUMBS;

// r105 response knob (Parameters\BehaveResponse): 0 = telemetry only
// (DEFAULT), 1 = suspend the alerting caller via the r91 reversible
// suspension infrastructure (guards + resume belt; `cr3 resume all`
// releases). The kill-vs-suspend policy decision stays with the user.
#define SVMB_SYSAUD_BEHAVE_RESP_SUSPEND 1
typedef struct _SVMB_SYSAUD_CALLER {
    svmb_u32 Pid;               // 0 = unused slot
    svmb_u32 Alerted;           // alert raised in the current window
    svmb_u64 Rd;                // lifetime attributed read entries
    svmb_u64 Wr;
    svmb_u64 Op;
    svmb_u64 CurrRd;            // current-window counts
    svmb_u64 CurrWr;
    svmb_u64 CurrOp;
    svmb_u64 PeakRd;            // lifetime max within-window rate
    svmb_u64 PeakWr;
    char Image[16];             // caller image at slot claim
} SVMB_SYSAUD_CALLER, *PSVMB_SYSAUD_CALLER;
// r103 per-(caller,target) lifetime counters - the EDR view ("who reads
// whom"). TargetPid comes from the deferred PASSIVE handle resolver
// (exit context cannot touch the caller's handle table); pairs land
// here only after convergence.
#define SVMB_SYSAUD_TARGET_SLOTS    32      // power of two (hash mask)
typedef struct _SVMB_SYSAUD_TARGET {
    svmb_u32 CallerPid;         // 0 = unused row
    svmb_u32 TargetPid;
    svmb_u64 Rd;
    svmb_u64 Wr;
    svmb_u64 Op;                // r104: NtOpenProcess (workitem-resolved
                                // via the CLIENT_ID read; never stamped
                                // on the entry itself)
} SVMB_SYSAUD_TARGET, *PSVMB_SYSAUD_TARGET;
typedef struct _SVMB_SYSAUD_BEHAVIOR {  // IOCTL_SYSAUD_BEHAVIOR (out)
    svmb_u32 OutCount;          // used slots
    svmb_u32 WindowMs;          // echo of the driver's window (1000)
    svmb_u32 RdThresh;          // echo of the driver's thresholds
    svmb_u32 WrThresh;
    svmb_u64 OpThresh;
    svmb_u64 Alerts;            // lifetime alert edges
    svmb_u64 Dropped;           // attributed=0 entries not counted
    svmb_u64 TableFull;         // claims refused (16 callers exceed table)
    svmb_u64 Reserved;
    SVMB_SYSAUD_CALLER Callers[SVMB_SYSAUD_BEHAVE_SLOTS];
    // ---- r103 target-side attribution (appended: old ctl with a new
    // driver is loudly refused by the outLen check, the supported
    // mismatch direction) ----
    svmb_u32 TargetCount;       // used rows in Targets[]
    svmb_u32 ResTargetSlots;    // echo of SVMB_SYSAUD_TARGET_SLOTS
    svmb_u64 Resolved;          // lifetime successful handle resolutions
    svmb_u64 ResFail;           // failed (stale handle / dead caller)
    svmb_u64 ResDropped;        // pending-ring overflow (tombstoned)
    svmb_u64 ExcludedNtcf;      // NTCF entries excluded from analytics
    svmb_u64 TargetsFull;       // r104: per-target rows refused (32 full)
    SVMB_SYSAUD_TARGET Targets[SVMB_SYSAUD_TARGET_SLOTS];
} SVMB_SYSAUD_BEHAVIOR, *PSVMB_SYSAUD_BEHAVIOR;

// r75 protected regions (IOCTL_CR3_PROT): L1 blocking = ZwProtectVirtualMemory
// (guest MM delivers the fault - zero exception injection on this vhv);
// L2 sensing = NPT W-deny on the same frames (trips only on L1 bypasses,
// e.g. kernel physical-map writers). Region lifetime is explicit
// (register/unprotect); cap 32 regions, each up to 64 pages.
#define SVMB_PROT_ACTION_REGISTER 0
#define SVMB_PROT_ACTION_UNPROTECT 1
#define SVMB_PROT_ACTION_LIST 2
#define SVMB_PROT_MAX_ENTRIES 32 // r82: matches PROT_MAX - LIST used to
                                 // truncate at 8 while OutCount said 32
typedef struct _SVMB_CR3_PROT_ENTRY {
    svmb_u32 Id;                // monotonic region id from REGISTER (never
                                // 0 for a live slot)
    svmb_u32 Pid;               // owning process
    svmb_u64 Base;              // user VA, region base (page aligned)
    svmb_u64 Size;              // bytes (page multiples)
    // r91: tail-appended per the ABI discipline (ENTRY 24 -> 32 padded;
    // same-round dual rebuild)
    svmb_u32 Policy;            // SVMB_PROT_POLICY_* of the live region
} SVMB_CR3_PROT_ENTRY, *PSVMB_CR3_PROT_ENTRY;
typedef struct _SVMB_CR3_PROT {    // IOCTL_CR3_PROT
    svmb_u32 Action;               // SVMB_PROT_ACTION_*
    svmb_u32 Pid;                  // owning process
    svmb_u64 Base;                 // user VA, page aligned
    svmb_u64 Size;                 // bytes, page multiple (cap 64 pages)
    svmb_u32 OutId;                // out: region id / unprotect count
    svmb_u32 OutOrigProtect;       // out: pre-protect MM protection
    svmb_u32 OutCount;             // out: live region count (list)
    svmb_u32 InUnprotectId;        // r78: UNPROTECT by region id when != 0
                                   // (overrides Pid/Base matching)
    SVMB_CR3_PROT_ENTRY Entries[SVMB_PROT_MAX_ENTRIES]; // list action
    // r89 plan C (docs/L2_BLOCKING_DESIGN.md 4): tail-appended per the
    // ABI discipline - old ctl fails LOUD via the >= sizeof header checks.
    svmb_u32 Policy;               // REGISTER in: SVMB_PROT_POLICY_* (0 =
                                   // alert-only, the r75 semantics)
} SVMB_CR3_PROT, *PSVMB_CR3_PROT;
// kill policy: an L2 bypass trip on this region terminates the process the
// trip's CR3 attributes (deterrence model - the first bypass write still
// lands). Opt-in per region; owner self-writes are IN scope (tamper
// response). Denied for pid<4, unresolvable CR3s (pre-load processes) and
// the csrss/wininit/services/lsass/smss/winlogon deny list.
// Attribution boundary (r89 acceptance P3-9, honest limits): the trip's
// CR3 is whichever address space the bypassing thread executed in -
// KeStackAttachProcess-style cross-process writes (r76 probe, debuggers,
// ReadProcessMemory) attribute to the OWNER; and CR3 spoofing that points
// at another SEEDED live process would transfer the kill to that process
// (the deny list only shields the six critical images). Deterrence-model
// inherent limits - documented, not accidental.
#define SVMB_PROT_POLICY_KILL   0x1
// r91 plan C- (reversible precursor): a bypass trip SUSPENDS the attributed
// process instead of terminating it. Requires PsSuspendProcess in the
// guest kernel (present on 18362 - verified by export table; other builds
// degrade: REGISTER refuses the policy loudly). A suspended writer is
// listed via IOCTL_SUSP_LIST and released via IOCTL_SUSP_RESUME - both
// run UNGATED because the suspended process holds the IOCTL gate inside
// its in-flight bypass IOCTL.
#define SVMB_PROT_POLICY_SUSPEND 0x2

// r76 L2-bypass probe (IOCTL_CR3_PROBE): the driver writes the magic through
// a kernel MDL mapping of the target VA (bypasses the L1 readonly PTE), so a
// protected region MUST alert via the NPT W-deny (RegionTrips) while the L1
// guest MM block stays intact. OutMagic echoes the pattern written.
#define SVMB_PROT_PROBE_MAGIC 0x4C325052323672ll // "r76R2L2"
typedef struct _SVMB_CR3_PROBE {   // IOCTL_CR3_PROBE
    svmb_u32 Pid;                  // owning process
    svmb_u64 Base;                 // user VA, page aligned
    svmb_u32 Size;                 // bytes to write (<= page, recommended 8)
    svmb_u32 Reserved;
    svmb_u32 OutWritten;           // bytes actually written
    svmb_u32 OutPadding;
    svmb_u64 OutMagic;             // the pattern written (echo)
} SVMB_CR3_PROBE, *PSVMB_CR3_PROBE;

// r87 hypervisor-level process memory read (IOCTL_CR3_READVM): resolve the
// target's CR3 from the seed table, walk its guest page tables by physical
// reads (MmCopyMemory MM_COPY_MEMORY_PHYSICAL - no process attach, no
// handle, no guest kernel API), and copy [Va, Va+Size) into the payload.
// Reads never NPF (default NPT view is RWX for RAM), so the operation is
// invisible to both the guest and this driver's own L1/L2 protection: the
// caller holds the device handle, i.e. the trust root itself.
#define SVMB_IOCTL_CR3_READVM   SVMB_CTL(0x825)
#define SVMB_READVM_MAX         0x10000 // payload cap per call (64 KiB)
#define SVMB_READVM_F_PARTIAL   0x1     // out: some pages not present
#define SVMB_READVM_F_FAULTIN   0x2     // in (r88 strategy B): attach the
                                        // target and fault not-present pages
                                        // in before reading - completes the
                                        // read but perturbs the target's
                                        // working set (documented trace)
typedef struct _SVMB_CR3_READVM {  // IOCTL_CR3_READVM (in == out buffer)
    svmb_u32 Pid;                  // target process (seed table lookup)
    svmb_u32 Size;                 // bytes to read (<= SVMB_READVM_MAX)
    svmb_u64 Va;                   // target USER va (kernel va rejected)
    svmb_u32 OutResolved;          // bytes read from resident pages; the
                                   // payload keeps VA position regardless -
                                   // unresolved bytes are zero-filled in
                                   // place (flagged via SVMB_READVM_F_PARTIAL)
    svmb_u32 OutFlags;             // SVMB_READVM_F_*
    svmb_u32 InFlags;              // r88: SVMB_READVM_F_FAULTIN (0 = pure
                                   // strategy A, zero-trace)
    // payload bytes follow the struct in the output buffer.
    // Mixed-deploy note (r88): old r87 ctl builds a 24-byte header - all
    // its real call shapes fail LOUD on the new driver (inLen or
    // outLen-32 < size -> BUFFER_TOO_SMALL/INVALID_BUFFER_SIZE); there is
    // no silent-misread path. New ctl + old driver would misalign InFlags
    // reads - same-round dual rebuild + deploy SHA256 discipline covers it.
} SVMB_CR3_READVM, *PSVMB_CR3_READVM;
// r89 guest-view mode (InFlags): on NPT-hooked pages read what the GUEST
// observes - the hook's patched hidden copy - instead of the pristine
// original frame the physical walk yields. Unhooked pages read normally,
// so one call spans both worlds. Anti-tamper compare: ground truth (no
// flag) vs guest view (flag) differs exactly at the patch sites.
#define SVMB_READVM_F_GUESTVIEW 0x4
#define SVMB_READVM_F_STALE     0x8     // out (r96): the target's seed
                                        // entry changed during the walk -
                                        // payload may reflect recycled
                                        // frames (r87 TOCTOU marker)

typedef enum _SVMB_DBG_EVT {
    SvmbDbgEvtBreakpoint = 1,   // #BP (int3 / npt-hook cc)
    SvmbDbgEvtSingleStep,       // #DB with TF
    SvmbDbgEvtDebugReg,         // DR access
    SvmbDbgEvtInvalidOpcode,    // #UD
    SvmbDbgEvtProcessWatch,     // watched image name: process armed (Extra=pid)
    SvmbDbgEvtViewActivate,     // NPT view published to all VMCBs (Cr3=pml4,
                                // Extra=published core count)
    SvmbDbgEvtSentinelTrip,     // r47 Route-A: watched-process page tripwire
                                // fired (Cr3=current core CR3, Extra=sentinel
                                // gpa4k, Core=faulting core)
    SvmbDbgEvtRegionTrip,       // r75: protected-region L1 bypass write
                                // (Cr3=writer CR3, Extra=region gpa4k,
                                // Core=faulting core)
} SVMB_DBG_EVT;

typedef struct _SVMB_DBG_EVENT {
    svmb_u32 Type;         // SVMB_DBG_EVT
    svmb_u32 Core;
    svmb_u64 Rip;
    svmb_u64 Cr3;
    svmb_u64 Rsp;
    svmb_u64 Extra;        // e.g. dr number/value
} SVMB_DBG_EVENT, *PSVMB_DBG_EVENT;

#define SVMB_DBG_MAX_EVENTS 64
typedef struct _SVMB_DBG_EVENT_BUFFER {
    svmb_u32 Count;
    svmb_u32 LostCount;
    SVMB_DBG_EVENT Events[SVMB_DBG_MAX_EVENTS];
} SVMB_DBG_EVENT_BUFFER, *PSVMB_DBG_EVENT_BUFFER;

typedef struct _SVMB_DBG_CONFIG {   // IOCTL_DBG_CONFIG
    svmb_u32 EnableBp;       // intercept #BP and report
    svmb_u32 EnableDb;       // intercept #DB (single step) and report
    svmb_u32 EnableUd;       // intercept #UD and report
    svmb_u32 HideDr;         // 1 = hide DR accesses (shadow only, reads spoofed)
    svmb_u32 SpoofDr7Zero;   // 1 = DR7 reads return 0
    svmb_u32 ArmSingleStep;  // r37: 1 = arm one MTF step on the calling
                             // thread's core (consumed as a step event)
    svmb_u32 Reserved[2];
} SVMB_DBG_CONFIG, *PSVMB_DBG_CONFIG;

typedef struct _SVMB_STRESS {       // IOCTL_STRESS
    svmb_u32 Kind;   // 0 = npt hook add/remove loop, 1 = exit storm (cpuid)
    svmb_u32 Iterations;
    svmb_u64 Status;
} SVMB_STRESS, *PSVMB_STRESS;

typedef struct _SVMB_SELFTEST_RESULT {  // IOCTL_SELFTEST (offline, no start)
    svmb_u32 Magic;         // SVMB_PROTO_MAGIC
    svmb_u32 Total;         // checks executed
    svmb_u32 Failed;        // checks failed (0 = all green)
    svmb_u32 Reserved;
    char LastFail[96];      // name of the first failing check, "" when none
} SVMB_SELFTEST_RESULT, *PSVMB_SELFTEST_RESULT;

#pragma pack(pop)

#endif // SVMB_PROTOCOL_H
