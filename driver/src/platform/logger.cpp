#include "platform/logger.h"
#include "platform/util.h"
#include "../../shared/svmb_protocol.h"

namespace svmb
{

namespace
{

constexpr u32 RING_BYTES_PER_CORE = 8192; // power of two
constexpr u32 RING_MASK = RING_BYTES_PER_CORE - 1;
constexpr u32 MAX_MSG = sizeof(((SVMB_LOG_ENTRY*)0)->Text);
constexpr u32 MAX_CORES = 256;

// all debugger output goes through DPFLTR_IHVDRIVER_ID (component 77).
// NOTE: default Windows filters suppress this component - to actually see
// the prints, set on the guest:
//   reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Debug
//            Print Filter" /v IHVDRIVER /t REG_DWORD /d 0xF
// (bits: 1=ERROR 2=WARN 4=TRACE 8=INFO; 0xF shows everything), or live in
// WinDbg: ed nt!Kd_IHVDRIVER_Mask 0xf
ULONG LevelToDpfltr(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Error:   return DPFLTR_ERROR_LEVEL;
    case LogLevel::Warn:    return DPFLTR_WARNING_LEVEL;
    case LogLevel::Verbose: return DPFLTR_TRACE_LEVEL;
    case LogLevel::Info:
    default:                return DPFLTR_INFO_LEVEL;
    }
}

struct LogState
{
    struct Core
    {
        volatile LONG64 Write;   // total bytes committed (monotonic)
        volatile LONG64 Read;    // bytes consumed by drain
        u8* Buf;
        volatile LONG Lost;
    };

    Core* Cores = nullptr;
    u32 CoreCount = 0;
    volatile LONG MinLevel = (LONG)LogLevel::Info;
    volatile LONG Serial = 0;
};

LogState gLog;

// ---- COM1 polling output (survives bugchecks when VMware serial -> file) ----
constexpr u16 COM1 = 0x3F8;

// One-time 16550A init. Without it a freshly-booted guest UART never clocks
// our bytes out to the VMware file sink: on 2026-09-07 every LogSerialTrace
// tag (S0..S3, E1..E8, E3b) was silently lost, while a .NET SerialPort test -
// which initializes the UART through serial.sys before writing - landed in
// the same file fine. Baud is irrelevant to the file sink; 115200/8N1 chosen
// for consistency with the WinDbg pipe profile.
void SerialInit()
{
    __outbyte(COM1 + 1, 0x00); // IER: no UART interrupts
    __outbyte(COM1 + 3, 0x80); // LCR: DLAB on
    __outbyte(COM1 + 0, 0x01); // DLL: divisor 1 = 115200 baud
    __outbyte(COM1 + 1, 0x00); // DLM
    __outbyte(COM1 + 3, 0x03); // LCR: 8N1, DLAB off
    __outbyte(COM1 + 2, 0xC7); // FCR: FIFO enable + clear, 14-byte rx trigger
    __outbyte(COM1 + 4, 0x0B); // MCR: DTR | RTS | OUT2
}

void SerialPut(char c)
{
    // wait for transmit empty (bounded). A stalled/holographic UART reads
    // LSR as 0xFF (floating bus) - detected once in SerialWrite and reported
    // through LogUartProbe; skip the pointless wait in that case.
    for (int i = 0; i < 10000; ++i)
    {
        u8 lsr = __inbyte(COM1 + 5);
        if (lsr == 0xFF)
            break; // no device decoding: write blind
        if (lsr & 0x20)
            break;
        __nop();
    }
    __outbyte(COM1, (u8)c);
}

// first-call UART ground truth, reported through the KDNET channel because
// the serial channel itself is what's under test (2026-09-07 bring-up)
volatile LONG gUartLsr = -1;
volatile LONG gUartIir = -1;

void SerialWrite(const char* s, u32 len)
{
    static volatile LONG s_uartInited = 0;
    if (InterlockedCompareExchange(&s_uartInited, 1, 0) == 0)
    {
        SerialInit();
        gUartLsr = __inbyte(COM1 + 5);
        gUartIir = __inbyte(COM1 + 2);
    }
    for (u32 i = 0; i < len; ++i)
        SerialPut(s[i]);
    SerialPut('\r');
    SerialPut('\n');
}

} // namespace

// DriverEntry flight recorder: direct polled UART write, gated on nothing.
// Used before the log subsystem is up; the VMware serial -> file sink keeps
// every line even across a triple fault.
void LogSerialTrace(const char* tag)
{
    SerialWrite(tag, (u32)strlen(tag));
}

void LogUartProbe()
{
    LogWrite(LogLevel::Info, "uart probe lsr=%02x iir=%02x",
             (unsigned)gUartLsr, (unsigned)gUartIir);
}

void LogInit()
{
    if (gLog.Cores)
        return;
    // component 77's default filter mask suppresses everything but ERROR -
    // open all levels so an attached kernel debugger sees every line without
    // the user having to patch Kd_IHVDRIVER_Mask first
    DbgSetDebugFilterState(DPFLTR_IHVDRIVER_ID, DPFLTR_MASK, TRUE);
    // Liveness beacon - must be the FIRST thing the kernel debugger sees so
    // we can confirm the log subsystem entered this path. Bypasses gLog.Cores
    // on purpose: if alloc fails below, this line still proves LogInit ran.
    DbgPrint("[svmb][loginit] ENTER cores=%p cores=%lu\n", (void*)gLog.Cores, 0UL);
    RtlZeroMemory(&gLog, sizeof(gLog));
    // RtlZeroMemory wipes the in-class MinLevel default too - without this
    // restore MinLevel stays None(0) and EVERY LogWrite (ring, DbgPrint,
    // serial mirror) returns at the level check, silencing the whole log
    // subsystem (2026-09-06: found via kd memory inspection during a live
    // devirt-hang repro - gLog.MinLevel was 0 while Serial was 1)
    gLog.MinLevel = (LONG)LogLevel::Info;
    gLog.CoreCount = min(MAX_CORES, KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS));
    SIZE_T total = (SIZE_T)gLog.CoreCount * RING_BYTES_PER_CORE;
    u8* blob = (u8*)AllocNonPaged((SIZE_T)sizeof(LogState::Core) * gLog.CoreCount + total, TAG_SVMB);
    if (!blob)
    {
        DbgPrint("[svmb][loginit] AllocNonPaged FAILED want=%lu cores=%lu\n",
                 (ULONG)((SIZE_T)sizeof(LogState::Core) * gLog.CoreCount + total),
                 (ULONG)gLog.CoreCount);
        return;
    }
    RtlZeroMemory(blob, (SIZE_T)sizeof(LogState::Core) * gLog.CoreCount + total);
    gLog.Cores = (LogState::Core*)blob;
    u8* ringBase = blob + (SIZE_T)sizeof(LogState::Core) * gLog.CoreCount;
    for (u32 i = 0; i < gLog.CoreCount; ++i)
        gLog.Cores[i].Buf = ringBase + (SIZE_T)i * RING_BYTES_PER_CORE;
    DbgPrint("[svmb][loginit] OK cores=%lu bytes=%lu\n",
             (ULONG)gLog.CoreCount,
             (ULONG)((SIZE_T)sizeof(LogState::Core) * gLog.CoreCount + total));
}

void LogDeinit()
{
    // flag off BEFORE freeing so late LogWrite/LogDrain callers bail out on
    // the null check instead of touching freed memory
    LogState::Core* cores = gLog.Cores;
    gLog.Cores = nullptr;
    gLog.CoreCount = 0;
    if (cores)
        ExFreePoolWithTag(cores, TAG_SVMB);
}

void LogSetLevel(LogLevel level) { InterlockedExchange(&gLog.MinLevel, (LONG)level); }
void LogSetSerial(bool enable) { InterlockedExchange(&gLog.Serial, enable ? 1 : 0); }

void LogWrite(LogLevel level, const char* fmt, ...)
{
    // Liveness beacon on first call only: prove LogWrite path was reached even
    // if gLog.Cores later turns out to be NULL (2026-09-06 bring-up debugging).
    static volatile LONG s_firstCall = 0;
    if (InterlockedCompareExchange(&s_firstCall, 1, 0) == 0)
    {
        DbgPrint("[svmb][logwrite] ENTER level=%d cores=%p minlevel=%d\n",
                 (int)level, (void*)gLog.Cores, (int)gLog.MinLevel);
    }

    if (!gLog.Cores || (LONG)level > gLog.MinLevel)
        return;

    char text[MAX_MSG];
    va_list ap;
    va_start(ap, fmt);
    NTSTATUS st = RtlStringCbVPrintfA(text, sizeof(text), fmt, ap);
    va_end(ap);
    u32 len = NT_SUCCESS(st) ? (u32)strlen(text) : 0;
    if (len == 0)
        return;

    bool serial = gLog.Serial != 0;
    u32 core = CurrentCpuIndex();
#if SVMB_LOG_FORCE_VISIBLE_ENABLED
    // SVMB_LOG_FORCE_VISIBLE: every line at DPFLTR_ERROR_LEVEL, Kd_IHVDRIVER_Mask
    // is ignored. Brings up on any host with any kernel debugger, no 'ed' needed.
    UNREFERENCED_PARAMETER(level);
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
               "[svmb][c%u] %s\n", core, text);
#else
    // Two-tier output, mirroring Cr3Encrypt's bring-up-robust pattern:
    //   ERROR / WARN -> DPFLTR_ERROR_LEVEL  : Windows never filters ERROR
    //                                       even with Kd_IHVDRIVER_Mask == 0.
    //                                       Brings up the driver on any host
    //                                       that has a kernel debugger attached
    //                                       (no manual 'ed nt!Kd_IHVDRIVER_Mask'
    //                                       needed). Cr3Encrypt works this way
    //                                       and we were losing everything when
    //                                       the mask was at default.
    //   INFO / TRACE -> DPFLTR_IHVDRIVER_ID  : gated by Kd_IHVDRIVER_Mask so
    //                                       noise can be silenced by setting
    //                                       the mask to 0 (ERROR stays on).
    //                                       LogInit() calls DbgSetDebugFilterState
    //                                       to set the IHV mask to 0xF so by
    //                                       default these are also visible.
    if (level <= LogLevel::Warn)
    {
        // ERROR level: always visible, never masked
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,
                   "[svmb][c%u] %s\n", core, text);
    }
    else
    {
        // INFO / VERBOSE: gated by Kd_IHVDRIVER_Mask (set to 0xF by LogInit)
        DbgPrintEx(DPFLTR_IHVDRIVER_ID, LevelToDpfltr(level),
                   "[svmb][c%u] %s\n", core, text);
    }
#endif
    if (serial)
        SerialWrite(text, len);

    if (core >= gLog.CoreCount)
        core = 0;
    LogState::Core& c = gLog.Cores[core];

    // ring message layout: [level byte][text][NUL]
    u32 need = len + 2;
    LONG64 r = c.Read;
    LONG64 w = c.Write;
    if (w - r + (LONG64)need > RING_BYTES_PER_CORE)
    {
        InterlockedIncrement(&c.Lost); // ring full, drop WITHOUT advancing Write
        return;                        // (a phantom advance would feed the reader garbage)
    }
    // reserve our range atomically, then fill it. A concurrent reader may see
    // the range before the fill completes; LogDrain treats a missing NUL as an
    // in-flight message and retries on the next drain.
    LONG64 my = InterlockedAdd64(&c.Write, need) - need;
    c.Buf[my & RING_MASK] = (u8)level;
    for (u32 i = 0; i <= len; ++i)
        c.Buf[(my + 1 + i) & RING_MASK] = (u8)text[i];
}

u32 LogDrain(SVMB_LOG_ENTRY* out, u32 maxEntries, u32* lostTotal)
{
    u32 written = 0;
    u32 lost = 0;
    if (!gLog.Cores)
        return 0;
    for (u32 core = 0; core < gLog.CoreCount && written < maxEntries; ++core)
    {
        LogState::Core& c = gLog.Cores[core];
        lost += (u32)c.Lost;
        LONG64 r = c.Read;
        LONG64 w = c.Write;
        while (r < w && written < maxEntries)
        {
            u8 level = c.Buf[r & RING_MASK];
            char text[MAX_MSG];
            u32 len = 0; // chars before the NUL
            bool complete = false;
            while (len < MAX_MSG - 1 && r + 1 + len < w)
            {
                char ch = (char)c.Buf[(r + 1 + len) & RING_MASK];
                if (ch == '\0')
                {
                    complete = true;
                    break;
                }
                text[len++] = ch;
            }
            if (!complete)
                break; // producer mid-write: leave bytes for the next drain
            u32 consumed = 1 + len + 1; // level + text + NUL
            if (len > 0)
            {
                SVMB_LOG_ENTRY& e = out[written++];
                e.Core = core;
                e.Level = level;
                e.Tsc = __rdtsc();
                e.Length = (u16)len;
                text[len] = '\0';
                RtlCopyMemory(e.Text, text, len + 1);
            }
            r += consumed;
        }
        InterlockedExchange64(&c.Read, r);
    }
    if (lostTotal)
        *lostTotal = lost;
    return written;
}

} // namespace svmb
