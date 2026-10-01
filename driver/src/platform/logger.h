// svmb - logger: per-core lock-free ring buffer + optional DbgPrint / COM1 mirror
#ifndef SVMB_LOGGER_H
#define SVMB_LOGGER_H

#include "platform/base.h"
#include "svmb_protocol.h"

namespace svmb
{

enum class LogLevel : u8
{
    None = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Verbose = 4,
};

void LogInit();
void LogDeinit();
// global minimum level (default Info); serial mirror switch (for crash forensics)
void LogSetLevel(LogLevel level);
void LogSetSerial(bool enable);

// unbuffered direct COM1 write, independent of LogInit/LogSetSerial - the
// DriverEntry flight recorder: mirrors "<tag>\r\n" to the VMware serial file
// so a triple fault cannot lose the last step taken
void LogSerialTrace(const char* tag);

// dumps the first-call UART register probe (captured by SerialWrite) through
// LogWrite/KDNET - diagnostics for the serial channel itself
void LogUartProbe();

void LogWrite(LogLevel level, const char* fmt, ...);

// ============================================================================
// SVMB_LOG_FORCE_VISIBLE — debug-time "show every line" switch
//
// Default OFF: SVMB_LOGI / SVMB_LOGV go through DbgPrintEx at their native
// DPFLTR_*_LEVEL, which is gated by nt!Kd_IHVDRIVER_Mask (LogInit opens it
// to 0xF). SVMB_LOGE / SVMB_LOGW always use DPFLTR_ERROR_LEVEL, which
// Windows never filters.
//
// Turn ON by adding /DSVMB_LOG_FORCE_VISIBLE to the driver's ClCompile
// preprocessor definitions (already in driver/svmb.vcxproj). When defined,
// ALL four log levels are emitted at DPFLTR_ERROR_LEVEL, completely ignoring
// Kd_IHVDRIVER_Mask. Same bring-up-robustness pattern Cr3Encrypt uses.
// ============================================================================
#if defined(SVMB_LOG_FORCE_VISIBLE)
#  define SVMB_LOG_FORCE_VISIBLE_ENABLED 1
#else
#  define SVMB_LOG_FORCE_VISIBLE_ENABLED 0
#endif

#define SVMB_LOGE(...) ::svmb::LogWrite(::svmb::LogLevel::Error,   __VA_ARGS__)
#define SVMB_LOGW(...) ::svmb::LogWrite(::svmb::LogLevel::Warn,    __VA_ARGS__)
#define SVMB_LOGI(...) ::svmb::LogWrite(::svmb::LogLevel::Info,    __VA_ARGS__)
#define SVMB_LOGV(...) ::svmb::LogWrite(::svmb::LogLevel::Verbose, __VA_ARGS__)

// IOCTL drain: copies up to maxEntries into out; returns number written
u32 LogDrain(SVMB_LOG_ENTRY* out, u32 maxEntries, u32* lostTotal);

} // namespace svmb

#endif // SVMB_LOGGER_H
