// svmb - compact x86-64 instruction length decoder for hook stealing.
// Deliberately conservative: covers common function-prologue instructions;
// anything not in the tables (three-byte opcodes, x87 escapes beyond D8-DF,
// invalid-in-long-mode opcodes) returns 0 and the caller must refuse to hook.
#ifndef SVMB_INSN_LEN_H
#define SVMB_INSN_LEN_H

#include "platform/base.h"

namespace svmb
{

constexpr u32 INSN_F_RIPREL = 0x10000; // out-flag: instruction is RIP-relative
constexpr u32 INSN_F_CTLX = 0x20000;   // out-flag: control transfer (call/jmp/
                                       // jcc/loop/jrcxz/ret) - its displacement
                                       // is PC-relative and would target a
                                       // different address when re-executed
                                       // from a trampoline

// length in bytes [1..15], or 0 when unsupported/undecodable.
// ripFlagsOut may be null; INSN_F_RIPREL is set when any operand used
// RIP-relative addressing (such instructions must not be copied into a
// trampoline without relocation). INSN_F_CTLX is set for control transfers:
// a stolen prefix containing one must be rejected outright (r35).
u32 InsnLen64(const u8* code, u32 maxLen, u32* ripFlagsOut = nullptr);

} // namespace svmb

#endif // SVMB_INSN_LEN_H
