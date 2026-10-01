// svmb - common base for all driver sources: types, tags, constants
#ifndef SVMB_BASE_H
#define SVMB_BASE_H

#include <ntddk.h>
#include <intrin.h>
#include <ntstrsafe.h>

#pragma warning(disable : 4201) // nameless struct/union

namespace svmb
{

using u8  = unsigned char;
using u16 = unsigned short;
using u32 = unsigned long;
using u64 = unsigned long long;
using i32 = long;
using i64 = long long;
using uptr = unsigned long long;

constexpr u32 MAKE_TAG_(char c1, char c2, char c3, char c4)
{
    return ((u32)c1) | ((u32)c2 << 8) | ((u32)c3 << 16) | ((u32)c4 << 24);
}

// allocation tags
constexpr u32 TAG_SVMB   = MAKE_TAG_('s', 'v', 'm', 'b'); // generic
constexpr u32 TAG_VCPU   = MAKE_TAG_('v', 'c', 'p', 'u'); // per-core contexts
constexpr u32 TAG_MSRPM  = MAKE_TAG_('m', 's', 'r', 'p');
constexpr u32 TAG_IOPM   = MAKE_TAG_('i', 'o', 'p', 'm');
constexpr u32 TAG_NPT    = MAKE_TAG_('n', 'p', 't', ' '); // page tables
constexpr u32 TAG_NPTHK  = MAKE_TAG_('n', 'p', 't', 'h'); // hook engine
constexpr u32 TAG_HOOK   = MAKE_TAG_('h', 'o', 'o', 'k'); // swap pages / trampolines
constexpr u32 TAG_DBG    = MAKE_TAG_('s', 'd', 'b', 'g'); // debugger module
constexpr u32 TAG_CR3    = MAKE_TAG_('s', 'c', 'r', '3'); // cr3 module

// misc
constexpr uptr INVALID_ADDR = (uptr)-1;

// r85 audit P2-9: canonical kernel range only - the old (va >> 48) != 0
// also accepted non-canonical addresses (0x8000'0000'0000'0000 etc.),
// turning a single NPT_HOOK install IOCTL into an instant bugcheck
inline bool IsKernelVa(uptr va)
{
    return va >= 0xFFFF800000000000ull && (va >> 47) == 0x1FFFFull;
}

// module identity tokens handed out by the module registry (0 = core built-ins)
using ModuleToken = u32;
constexpr ModuleToken CORE_TOKEN = 0;

} // namespace svmb

#endif // SVMB_BASE_H
