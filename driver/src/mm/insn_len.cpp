#include "mm/insn_len.h"

namespace svmb
{

namespace
{

constexpr u8 F_MOD = 0x01;   // has ModRM byte
constexpr u8 F_IMM8 = 0x02;  // trailing imm8
constexpr u8 F_IMMV = 0x04;  // trailing imm (2/4/8 by osz/REX.W)
constexpr u8 F_IMM16 = 0x08; // trailing imm16
constexpr u8 F_CTLX = 0x10;  // control transfer (PC-relative displacement)
constexpr u8 F_BAD = 0x80;   // invalid in long mode / unsupported

// one-byte opcode classes (0x40-0x4F never reach here: consumed as REX)
u8 Classify1(u8 op)
{
    if (op <= 0x3F)
    {
        u8 k = op & 7;
        if (k == 4)
            return F_IMM8;
        if (k == 5)
            return F_IMMV;
        if (op == 0x06 || op == 0x07 || op == 0x0E || op == 0x16 || op == 0x17 ||
            op == 0x1E || op == 0x1F || op == 0x27 || op == 0x2F || op == 0x37 ||
            op == 0x3F)
            return F_BAD; // segment push/pop, BCD adjust - invalid in long mode
        return F_MOD;     // 00-03, 08-0B, ... group-1 arithmetic
    }
    if (op <= 0x5F)
        return 0; // push/pop r (REX range lives above)
    if (op == 0x60 || op == 0x61 || op == 0x62)
        return F_BAD; // pusha/popa/bound - invalid in long mode
    if (op == 0x63)
        return F_MOD; // movsxd
    if (op == 0x68)
        return F_IMMV;
    if (op == 0x69)
        return F_MOD | F_IMMV;
    if (op == 0x6A)
        return F_IMM8;
    if (op == 0x6B)
        return F_MOD | F_IMM8;
    if (op <= 0x6F)
        return 0; // ins/outs
    if (op <= 0x7F)
        return F_IMM8 | F_CTLX; // jcc rel8
    if (op == 0x80)
        return F_MOD | F_IMM8;
    if (op == 0x81)
        return F_MOD | F_IMMV;
    if (op == 0x82)
        return F_BAD; // group-1 32-bit form - invalid in long mode
    if (op == 0x83)
        return F_MOD | F_IMM8;
    if (op <= 0x8B)
        return F_MOD; // test/xchg/mov r/m
    if (op == 0x8C || op == 0x8E)
        return F_MOD; // mov seg
    if (op == 0x8D)
        return F_MOD; // lea
    if (op == 0x8F)
        return F_MOD; // pop r/m
    if (op <= 0x97)
        return 0; // xchg eAX,r
    if (op <= 0x99)
        return 0; // cwde/cdq
    if (op == 0x9A)
        return F_BAD; // far call
    if (op <= 0x9B)
        return 0; // wait
    if (op <= 0x9F)
        return 0; // pushf/popf/sahf/lahf
    if (op <= 0xA3)
        return F_IMMV; // mov moffs (REX.W -> imm64)
    if (op <= 0xA7)
        return 0; // movs/cmps
    if (op == 0xA8)
        return F_IMM8;
    if (op == 0xA9)
        return F_IMMV;
    if (op <= 0xAF)
        return 0; // stos/lods/scas
    if (op <= 0xB7)
        return F_IMM8; // mov r8,imm8
    if (op <= 0xBF)
        return F_IMMV; // mov r,imm (REX.W -> imm64)
    if (op == 0xC0 || op == 0xC1)
        return F_MOD | F_IMM8; // shift imm
    if (op == 0xC2)
        return F_IMM16 | F_CTLX; // ret imm16 - prefix would return early
    if (op == 0xC3)
        return F_CTLX; // ret - thunk-style prologue, unsafe to steal past
    if (op == 0xC6)
        return F_MOD | F_IMM8; // mov r/m,imm8 (reg==0)
    if (op == 0xC7)
        return F_MOD | F_IMMV; // mov r/m,imm (reg==0)
    if (op == 0xC8)
        return F_BAD; // enter (imm16+imm8) - not a hook target
    if (op == 0xC9 || op == 0xCC || op == 0xCF)
        return 0; // leave / int3 / bswap
    if (op == 0xCA)
        return F_IMM16; // retf imm16
    if (op == 0xCD)
        return F_IMM8; // int imm8
    if (op == 0xCE)
        return F_BAD; // into - invalid in long mode
    if (op == 0xD6)
        return F_BAD; // salc - invalid in long mode
    if (op == 0xD7)
        return 0; // xlat
    if (op <= 0xDF)
        return F_MOD; // x87 FPU (D8-DF)
    if (op == 0xE8 || op == 0xE9)
        return F_IMMV | F_CTLX; // call/jmp rel32 (66 -> rel16)
    if (op == 0xEA)
        return F_BAD; // far jmp - invalid in long mode
    if (op == 0xEB)
        return F_IMM8 | F_CTLX; // jmp rel8
    if (op <= 0xE3)
        return F_IMM8 | F_CTLX; // loopcc/jrcxz - PC-relative like jcc
    if (op <= 0xE7)
        return F_IMM8; // in/out imm8
    if (op <= 0xEF)
        return 0; // in/out dx
    if (op == 0xF6)
        return F_MOD | F_IMM8; // test group (imm only when reg<2, decoded later)
    if (op == 0xF7)
        return F_MOD | F_IMMV; // test group (imm only when reg<2)
    if (op == 0xFE || op == 0xFF)
        return F_MOD;
    return 0; // F4 hlt, F5, F8-FD flags, F1 - all single byte
}

// two-byte opcode classes (called with the byte after 0x0F)
u8 Classify2(u8 op)
{
    if (op == 0x38 || op == 0x3A)
        return F_BAD; // three-byte opcode maps - unsupported
    if (op <= 0x4F)
    {
        if (op == 0x24 || op == 0x25 || op == 0x26 || op == 0x27)
            return F_BAD; // invalid in long mode
        if (op == 0x34 || op == 0x35)
            return 0; // sysenter/sysexit (single instruction)
        // 0x00-0x23 groups/modrm, 0x28-0x2F sse, 0x40-0x4F cmov
        return F_MOD;
    }
    if (op == 0x0A || op == 0x0C)
        return F_BAD; // ud / invalid
    if (op == 0x05 || op == 0x06 || op == 0x08 || op == 0x09 || op == 0x0B ||
        op == 0x0E)
        return 0; // syscall/clts/invd/wbinvd/ud2/femms - no operands
    if (op >= 0x30 && op <= 0x33)
        return 0; // wrmsr/rdtsc/rdmsr/rdmsr - no operands
    if (op == 0x34 || op == 0x35)
        return 0; // sysenter/sysexit
    if (op == 0x37)
        return 0; // rdtscp
    if (op == 0x70)
        return F_MOD | F_IMM8;
    if (op <= 0x73)
        return F_MOD | F_IMM8; // ps* groups
    if (op == 0x77)
        return 0; // emms
    if (op <= 0x7F)
        return F_MOD; // 0x50-0x6F sse/pack, 0x74-0x7F
    if (op <= 0x8F)
        return F_IMMV | F_CTLX; // jcc rel32
    if (op <= 0x9F)
        return F_MOD; // setcc
    if (op == 0xA0 || op == 0xA1 || op == 0xA8 || op == 0xA9)
        return 0; // push/pop fs/gs
    if (op == 0xA2 || op == 0xAA)
        return 0; // cpuid / rsm
    if (op == 0xA4 || op == 0xAC)
        return F_MOD | F_IMM8; // shld/shrd imm
    if (op == 0xA6 || op == 0xA7)
        return F_BAD;
    if (op <= 0xB7)
        return F_MOD; // bt*/shld/lss/lfs/lgs/movzx/cmpxchg
    if (op == 0xB9)
        return F_BAD; // ud1
    if (op == 0xBA)
        return F_MOD | F_IMM8; // group-8 imm
    if (op <= 0xC1)
        return F_MOD; // btc/bsf/bsr/movsx/xadd
    if (op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6)
        return F_MOD | F_IMM8; // sse imm forms
    if (op <= 0xC7)
        return F_MOD; // movnti / group-9
    if (op <= 0xCF)
        return 0; // bswap r32/r64
    return F_MOD; // D0-FF sse/misc - remaining are modrm
}

} // namespace

u32 InsnLen64(const u8* c, u32 maxLen, u32* ripFlagsOut)
{
    if (ripFlagsOut)
        *ripFlagsOut = 0;
    if (maxLen == 0)
        return 0;

    u32 i = 0;
    bool osz = false;
    bool rexW = false;
    bool hasRex = false;
    for (;;)
    {
        if (i >= maxLen)
            return 0;
        u8 b = c[i];
        if (b == 0x66)
        {
            osz = true;
            ++i;
            continue;
        }
        if (b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x26 || b == 0x2E || b == 0x36 || b == 0x3E || b == 0x64 ||
            b == 0x65)
        {
            ++i;
            continue;
        }
        if (b >= 0x40 && b <= 0x4F)
        {
            hasRex = true;
            rexW = (b & 8) != 0;
            ++i;
            continue;
        }
        break;
    }

    u8 op = c[i++];
    u8 flags;
    if (op == 0x0F)
    {
        if (i >= maxLen)
            return 0;
        flags = Classify2(c[i++]);
    }
    else
    {
        flags = Classify1(op);
    }
    if (flags & F_BAD)
        return 0;
    if ((flags & F_CTLX) && ripFlagsOut)
        *ripFlagsOut |= INSN_F_CTLX;

    u32 total = i;
    if (flags & F_MOD)
    {
        if (i >= maxLen)
            return 0;
        u8 modrm = c[i++];
        ++total;
        u8 mod = modrm >> 6;
        u8 reg = (modrm >> 3) & 7;
        u8 rm = modrm & 7;
        if (mod != 3)
        {
            if (rm == 4)
            {
                if (i >= maxLen)
                    return 0;
                u8 sib = c[i++];
                ++total;
                if ((sib & 7) == 5 && mod == 0)
                    total += 4;
            }
            else if (mod == 0 && rm == 5)
            {
                total += 4;
                if (ripFlagsOut)
                    *ripFlagsOut |= INSN_F_RIPREL;
            }
            if (mod == 1)
                total += 1;
            else if (mod == 2)
                total += 4;
        }

        // opcode-specific corrections after the ModRM is known
        if (op == 0xF6 && reg >= 2)
            flags &= (u8)~F_IMM8;
        if (op == 0xF7 && reg >= 2)
            flags &= (u8)~F_IMMV;
        if ((op == 0xC6 || op == 0xC7) && reg != 0)
            return 0; // undefined forms
    }

    if (flags & F_IMM8)
        total += 1;
    else if (flags & F_IMM16)
        total += 2;
    else if (flags & F_IMMV)
    {
        // only mov r,imm (B8-BF) and mov moffs (A0-A3) widen to imm64 with
        // REX.W; everything else stays imm32/imm16
        bool canImm64 = hasRex && rexW &&
                        ((op >= 0xB8 && op <= 0xBF) || (op >= 0xA0 && op <= 0xA3));
        total += canImm64 ? 8 : (osz ? 2 : 4);
    }

    if (total > maxLen || total > 15)
        return 0;
    return total;
}

} // namespace svmb
