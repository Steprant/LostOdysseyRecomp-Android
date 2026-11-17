#!/usr/bin/env python3
"""Steprant 2.2: check patches/crt_native.h against the generated CRT functions.

Runs in the Android APK workflow right after the game code is generated. It
copies the generated bodies of __imp__sub_82B7A0B0 (memcpy) and
__imp__sub_82B7BC40 (memset) into a host test next to crt_native::Memcpy and
crt_native::Memset, runs both on the same guest memory and context and
compares all of the memory and r3 afterwards (the volatile registers the guest
code also changes are checked unread by tools/ppc_register_check.py --native).
Sizes from 0 to 4 KiB, every alignment of source and destination, regions
next to each other and next to the saved stack doubleword, overlapping
regions (the host version hands those to the generated code) and sizes with
high bits set.

The output names the functions and counts only: the generated code is
derived from the game and stays out of the public log, as do compiler
messages that quote it.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FUNCTIONS = ('__imp__sub_82B7A0B0', '__imp__sub_82B7BC40')

HARNESS = r'''
#include "ppc_recomp_shared.h"
@FUNCTION_BODIES@
#include <patches/crt_native.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace
{
constexpr uint32_t kMemory = 1 << 16, kStack = 0xFF00;

struct Result { std::vector<uint8_t> memory; uint64_t r3; };

template<class Call>
Result Run(uint32_t seed, uint64_t r3, uint64_t r4, uint64_t r5, Call call)
{
    uint8_t* base = static_cast<uint8_t*>(std::aligned_alloc(64, kMemory));
    std::mt19937 fill(seed);
    for (uint32_t i = 0; i < kMemory; ++i) base[i] = uint8_t(fill());
    alignas(64) PPCContext ctx;
    for (size_t i = 0; i < sizeof(ctx); ++i) reinterpret_cast<uint8_t*>(&ctx)[i] = uint8_t(fill());
    ctx.r1.u64 = kStack;
    ctx.r3.u64 = r3;
    ctx.r4.u64 = r4;
    ctx.r5.u64 = r5;
    call(ctx, base);
    Result r{std::vector<uint8_t>(base, base + kMemory), ctx.r3.u64};
    std::free(base);
    return r;
}

bool Same(const Result& a, const Result& b) { return a.memory == b.memory && a.r3 == b.r3; }

uint32_t Size(std::mt19937& rng)
{
    switch (rng() % 6) {
        case 0: return rng() % 9;
        case 1: return rng() % 40;
        case 2: return rng() % 200;
        case 3: return 120 + rng() % 20;
        case 4: return rng() % 1100;
        default: return rng() % 4097;
    }
}
}

int main()
{
    std::mt19937 rng(20261117);
    const auto copyOriginal = [](PPCContext& ctx, uint8_t* base) { __imp__sub_82B7A0B0(ctx, base); };
    const auto copyNative = [](PPCContext& ctx, uint8_t* base) { if (!crt_native::Memcpy(ctx, base)) __imp__sub_82B7A0B0(ctx, base); };
    const auto setOriginal = [](PPCContext& ctx, uint8_t* base) { __imp__sub_82B7BC40(ctx, base); };
    const auto setNative = [](PPCContext& ctx, uint8_t* base) { if (!crt_native::Memset(ctx, base)) __imp__sub_82B7BC40(ctx, base); };
    unsigned copies = 0, overlapping = 0, nearStack = 0, sets = 0, highSizes = 0, handled = 0;
    for (int n = 0; n < 40000; ++n) {
        const int kind = rng() % 10;
        uint32_t size = Size(rng);
        if (kind == 1 && size > 200) size %= 200;
        uint32_t dst, src;
        if (kind == 0) {            // overlapping: the generated code runs
            src = 0x1000 + rng() % 0x6000;
            dst = src + int32_t(rng() % (2 * (size + 1))) - int32_t(size);
            ++overlapping;
        } else if (kind == 1) {     // around the saved doubleword at r1 - 8
            dst = kStack - 8 - rng() % (size + 9);
            src = 0x1000 + rng() % 0x3000;
            ++nearStack;
        } else {
            dst = 0x1000 + rng() % 0x6000;
            src = 0x8000 + rng() % 0x6000;
            if (rng() % 2) std::swap(dst, src);
        }
        const uint64_t high = (rng() % 8 == 0) ? (uint64_t(rng()) << 32) : 0;
        const uint32_t seed = rng();
        // Upper halves of r3/r4 are kept by the guest code in r3 (std/ld):
        // vary them too.
        const uint64_t r3 = dst | (rng() % 4 == 0 ? uint64_t(rng()) << 32 : 0);
        const uint64_t r4 = src | (rng() % 4 == 0 ? uint64_t(rng()) << 32 : 0);
        const uint64_t r5 = size | high;
        if (!Same(Run(seed, r3, r4, r5, copyOriginal), Run(seed, r3, r4, r5, copyNative))) {
            std::printf("crt native equivalence: FAILED memcpy case %u (size %u, dst %% 8 = %u, src %% 8 = %u, kind %d)\n",
                copies, size, dst % 8, src % 8, kind);
            return 1;
        }
        ++copies;
    }
    for (int n = 0; n < 30000; ++n) {
        const uint32_t size = Size(rng);
        const uint32_t dst = 0x1000 + rng() % 0xD000;
        const bool high = rng() % 10 == 0;
        const uint64_t r5 = size | (high ? (uint64_t(1 + rng() % 3) << 32) : 0);
        if (high) {
            ++highSizes;
            continue;   // the host version hands these to the generated code (which may run 4 GiB)
        }
        const uint64_t r4 = (uint64_t(rng()) << 32) | rng();
        const uint64_t r3 = dst | (rng() % 4 == 0 ? uint64_t(rng()) << 32 : 0);
        const uint32_t seed = rng();
        if (!Same(Run(seed, r3, r4, r5, setOriginal), Run(seed, r3, r4, r5, setNative))) {
            std::printf("crt native equivalence: FAILED memset case %u (size %u, dst %% 4 = %u)\n", sets, size, dst % 4);
            return 1;
        }
        ++sets;
    }
    // Host version taken: non-overlapping in range.
    {
        alignas(64) PPCContext ctx{};
        static uint8_t memory[kMemory];
        ctx.r1.u64 = kStack; ctx.r3.u64 = 0x1000; ctx.r4.u64 = 0x2000; ctx.r5.u64 = 64;
        handled += crt_native::Memcpy(ctx, memory);
        ctx.r3.u64 = 0x1000; ctx.r4.u64 = 0x1010; ctx.r5.u64 = 64;
        handled += !crt_native::Memcpy(ctx, memory);
        ctx.r3.u64 = 0x1000; ctx.r4.u64 = 7; ctx.r5.u64 = 64;
        handled += crt_native::Memset(ctx, memory);
    }
    std::printf("crt native equivalence: memcpy %u cases (%u overlapping, %u next to the saved doubleword), memset %u cases "
                "(%u sizes past 32 bits left to the generated code): memory and r3 byte for byte equal; host path checks %u/3\n",
        copies, overlapping, nearStack, sets, highSizes, handled);
    return copies >= 30000 && sets >= 20000 && handled == 3 ? 0 : 1;
}
'''


def extract(ppc_dir: Path, function: str) -> str:
    start = f'PPC_FUNC_IMPL({function})'
    for path in sorted(ppc_dir.glob('ppc_recomp.*.cpp')):
        text = path.read_text(encoding='utf-8', errors='replace')
        index = text.find(start + ' {')
        if index < 0:
            continue
        end = text.find('\n}\n', index)
        if end < 0:
            break
        return text[index:end + 3]
    sys.exit(f'crt native equivalence: {function} not found in {ppc_dir}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ppc_dir', type=Path)
    parser.add_argument('--cxx', default='clang++', help='the generated code uses clang builtins')
    args = parser.parse_args()
    bodies = '\n'.join(extract(args.ppc_dir, function) for function in FUNCTIONS)
    source = HARNESS.replace('@FUNCTION_BODIES@', bodies)
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / 'crt_native_equivalence.cpp'
        exe = Path(tmp) / 'crt_native_equivalence'
        cpp.write_text(source)
        command = [args.cxx, '-std=c++20', '-O1', '-w',
                   '-I', str(args.ppc_dir.resolve()),
                   '-I', str(ROOT / 'tools/XenonRecomp/thirdparty/simde'),
                   '-I', str(ROOT / 'LostOdysseyRecomp'),
                   str(cpp), '-o', str(exe)]
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode != 0:
            # Compiler output can quote generated lines: show the messages only.
            messages = [re.sub(r'^.*?: (error|fatal error): ', r'\1: ', line)
                        for line in build.stderr.splitlines() if 'error' in line and ': ' in line]
            print(f'crt native equivalence: the host test did not build ({len(messages)} errors)')
            for message in messages[:10]:
                print('  ' + message[:160])
            return 1
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=900)
        print(run.stdout.strip())
        if run.returncode != 0:
            print(f'crt native equivalence: exit status {run.returncode}')
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
