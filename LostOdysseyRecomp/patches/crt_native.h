#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>

// Steprant 2.2: the game's CRT memcpy and memset, and its stack probe, as
// host code.
//
// sub_82B7A0B0 is memcpy (r3 = destination, r4 = source, r5 = size; returns
// the destination in r3): a forward copy by bytes, words, doublewords and
// 128-byte blocks, with prefetch hints. In the 16/11 profile it is the
// game function with the most executed blocks (3.8%), called 731k times per
// second. Translated, every 8 bytes go through a volatile load, two byte
// swaps that cancel out and a volatile store.
//
// sub_82B7BC40 is memset (r3 = destination, low byte of r4 = value, r5 =
// size; r3 unchanged): bytes up to 4-byte alignment, 16-byte groups of
// words, the rest by words and bytes. 40k calls per second.
//
// sub_82B7E504 is the stack probe called by functions with large frames (r12
// = minus the frame size): it reads one word per 4 KiB page below the stack
// pointer, which on the Xbox commits the pages. Guest stacks here are plain
// memory, already committed, so the probe has nothing to do. Its callers
// read r12 afterwards (unchanged by the probe) and nothing else it changes.
//
// What the host versions give back, byte for byte the same as the generated
// functions (tools/crt_native_equivalence.py compares them on every build):
// all of guest memory, r3, and for memcpy the doubleword it saves at
// r1 - 8. The volatile registers the guest code also leaves changed (r0,
// r4-r12, ctr, cr0) are not reproduced: tools/ppc_register_check.py --native
// checks on every build that no caller reads one of them after these calls.
//
// Overlapping memcpy regions and ranges that wrap past 4 GiB run the
// generated code, whose exact result depends on its copy order.
// LO_CRT_NATIVE=0 runs the generated code everywhere.

namespace crt_native
{
inline bool Enabled()
{
    static const bool enabled = [] {
        const char* value = std::getenv("LO_CRT_NATIVE");
        return !(value && value[0] == '0' && !value[1]);
    }();
    return enabled;
}

#if defined(PPC_FUNC)
// memcpy: true when done here, false when the generated code must run.
inline bool Memcpy(PPCContext& __restrict ctx, uint8_t* base)
{
    const uint64_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;
    if (dst + size > 0x100000000ull || src + size > 0x100000000ull)
        return false;
    if (size && dst < src + size && src < dst + size)
        return false;
    // std r3,-8(r1) at the entry, ld r3,-8(r1) at every return (a copy
    // over that doubleword, below the stack pointer, changes the result).
    PPC_STORE_U64(ctx.r1.u32 - 8, ctx.r3.u64);
    if (size)
        std::memcpy(base + dst, base + src, size);
    ctx.r3.u64 = PPC_LOAD_U64(ctx.r1.u32 - 8);
    return true;
}

// memset: true when done here, false when the generated code must run.
inline bool Memset(PPCContext& __restrict ctx, uint8_t* base)
{
    // The size counts down from the 64-bit r5 in the alignment loop and in
    // 32 bits after it: sizes past 32 bits run the generated code.
    if (ctx.r5.u64 >> 32)
        return false;
    const uint64_t dst = ctx.r3.u32, size = ctx.r5.u32;
    if (dst + size > 0x100000000ull)
        return false;
    if (size)
        std::memset(base + dst, int(ctx.r4.u32 & 0xFF), size);
    return true;
}
#endif
}
