#!/usr/bin/env python3
"""Steprant 2.1.6: check patches/audio_barrier.h against the generated guest function.

Runs in the Android APK workflow right after the game code is generated. It
copies the generated body of __imp__sub_82CC3C50 (the audio workers'
rendezvous) into a host test next to audio_barrier::Run, runs both on the same
guest memory and context (single-thread cases that end at the first check,
and cases where a second thread brings the missing flags or closes the round)
and compares every byte of memory and of the context afterwards.

The output names the function and counts only: the generated code is derived
from the game and stays out of the public log, as do compiler messages that
quote it.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FUNCTION = '__imp__sub_82CC3C50'

HARNESS = r'''
#include "ppc_recomp_shared.h"
@FUNCTION_BODY@
#include <patches/audio_barrier.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <unistd.h>

namespace
{
constexpr uint32_t kMemory = 1 << 16, kObject = 0x1000, kFlags = 0x2000, kPcr = 0x3000;

struct Case
{
    uint32_t enable = 0;
    uint32_t participants[6] = {};
    uint8_t processor = 0;
    uint8_t word[8] = {};      // flag bytes before the call
    int late = 0;              // 0: none; 1: another thread sets the missing flags; 2: it stores 0
    uint8_t missing[8] = {};
};

uint64_t Expected(const Case& c)
{
    uint64_t raw = 0;
    for (int i = 0; i < 6; ++i)
        if (c.participants[i]) raw |= uint64_t(1) << (8 * i);
    return raw;
}

uint64_t WordRaw(const uint8_t* bytes) { uint64_t v; std::memcpy(&v, bytes, 8); return v; }

struct Result { std::vector<uint8_t> memory; PPCContext ctx; };

template<class Call>
Result Run(const Case& c, uint32_t seed, Call call)
{
    uint8_t* base = static_cast<uint8_t*>(std::aligned_alloc(64, kMemory));
    std::mt19937 fill(seed);
    for (uint32_t i = 0; i < kMemory; ++i) base[i] = uint8_t(fill());
    auto store32 = [&](uint32_t a, uint32_t v) { v = __builtin_bswap32(v); std::memcpy(base + a, &v, 4); };
    store32(kObject + 0x130, c.enable);
    for (int i = 0; i < 6; ++i) store32(kObject + 0x134 + 4 * i, c.participants[i]);
    base[kPcr + 0x10C] = c.processor;
    std::memcpy(base + kFlags, c.word, 8);
    alignas(64) PPCContext ctx;
    for (size_t i = 0; i < sizeof(ctx); ++i) reinterpret_cast<uint8_t*>(&ctx)[i] = uint8_t(fill());
    ctx.r3.u64 = kObject;
    ctx.r4.u64 = kFlags;
    ctx.r13.u64 = kPcr;
    std::thread helper;
    if (c.late)
        helper = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::microseconds(800));
            if (c.late == 2) {
                __atomic_store_n(reinterpret_cast<uint64_t*>(base + kFlags), uint64_t(0), __ATOMIC_RELEASE);
                return;
            }
            for (int i = 0; i < 8; ++i)
                if (c.missing[i]) {
                    __atomic_store_n(base + kFlags + i, uint8_t(1), __ATOMIC_RELEASE);
                    std::this_thread::sleep_for(std::chrono::microseconds(150));
                }
        });
    call(ctx, base);
    if (helper.joinable()) helper.join();
    Result r{std::vector<uint8_t>(base, base + kMemory), ctx};
    std::free(base);
    return r;
}

bool Same(const Result& a, const Result& b)
{
#if LO_GUEST_R11_LOCAL
    // Steprant 2.2: the generated function keeps r11 as a local (register
    // promotion), so its r11 never reaches the context; the override still
    // stores the 0 the guest code leaves there. No caller reads r11 after
    // the call (tools/ppc_register_check.py), so that field is not compared.
    PPCContext x = a.ctx, y = b.ctx;
    y.r11 = x.r11;
    return a.memory == b.memory && std::memcmp(&x, &y, sizeof(PPCContext)) == 0;
#else
    return a.memory == b.memory && std::memcmp(&a.ctx, &b.ctx, sizeof(PPCContext)) == 0;
#endif
}

// True when the guest loop ends at its first check (no other thread needed).
bool EndsAlone(const Case& c)
{
    if (c.enable == 0) return true;
    uint8_t bytes[8];
    std::memcpy(bytes, c.word, 8);
    if (c.processor < 8) bytes[c.processor] = 1;
    const uint64_t cur = WordRaw(bytes);
    return cur == 0 || cur == Expected(c);
}
}

int main()
{
    alarm(600);
    audio_barrier::Settings settings;
    settings.enabled = true;
    audio_barrier::Stats stats;
    const auto original = [](PPCContext& ctx, uint8_t* base) { @FUNCTION_NAME@(ctx, base); };
    const auto replaced = [&](PPCContext& ctx, uint8_t* base) { audio_barrier::Run(ctx, base, settings, stats); };
    std::mt19937 rng(20261116);
    const uint8_t processors[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 63, 200, 255};
    const uint32_t enables[] = {0, 1, 0x80000000u, 0xFFFFFFFFu, 0x00010000u};
    unsigned single = 0, twoThread = 0, generated = 0;
    for (int n = 0; n < 200000 && single < 6000; ++n) {
        Case c;
        c.enable = enables[rng() % 5];
        for (auto& p : c.participants) p = (rng() % 3 == 0) ? 0 : (rng() % 4 == 0 ? 0x80000000u : uint32_t(rng() | 1));
        c.processor = processors[rng() % 12];
        const uint64_t expected = Expected(c);
        switch (rng() % 4) {
            case 0: std::memcpy(c.word, &expected, 8); break;
            case 1: break;
            case 2: std::memcpy(c.word, &expected, 8); if (c.processor < 8) c.word[c.processor] = 0; break;
            default: for (auto& b : c.word) b = uint8_t(rng() % 2); break;
        }
        ++generated;
        if (!EndsAlone(c)) continue;
        const uint32_t seed = rng();
        if (!Same(Run(c, seed, original), Run(c, seed, replaced))) {
            std::printf("audio barrier equivalence: FAILED single-thread case %u\n", single);
            return 1;
        }
        ++single;
    }
    for (int n = 0; twoThread < 120 && n < 100000; ++n) {
        Case c;
        c.enable = 1;
        for (auto& p : c.participants) p = (rng() % 3 == 0) ? 0 : uint32_t(rng() | 1);
        const uint64_t expected = Expected(c);
        if (expected == 0) continue;
        c.processor = uint8_t(rng() % 10);
        uint8_t full[8];
        std::memcpy(full, &expected, 8);
        // Own byte must belong to the expected set when it lands in the word,
        // or the guest loop could only end with a closing 0.
        if (c.processor < 8 && !full[c.processor]) continue;
        int dropped = 0;
        for (int i = 0; i < 8; ++i) {
            c.word[i] = full[i];
            if (full[i] && i != c.processor && rng() % 2) { c.word[i] = 0; c.missing[i] = 1; ++dropped; }
        }
        c.late = (rng() % 4 == 0) ? 2 : 1;
        if (c.late == 1 && dropped == 0) continue;
        if (c.late == 2) {
            // Ends only through the closing 0: make sure it is not complete already.
            uint8_t bytes[8];
            std::memcpy(bytes, c.word, 8);
            if (c.processor < 8) bytes[c.processor] = 1;
            if (WordRaw(bytes) == expected) continue;
        }
        const uint32_t seed = rng();
        if (!Same(Run(c, seed, original), Run(c, seed, replaced))) {
            std::printf("audio barrier equivalence: FAILED two-thread case %u (%s)\n", twoThread, c.late == 2 ? "closed by another thread" : "late arrivals");
            return 1;
        }
        ++twoThread;
    }
    std::printf("audio barrier equivalence: %s matches audio_barrier::Run in %u single-thread and %u two-thread cases (memory and context compared byte by byte; %u cases generated); %llu waits\n",
        "@FUNCTION_NAME@", single, twoThread, generated, (unsigned long long)stats.waits.load());
    return single >= 1000 && twoThread >= 100 ? 0 : 1;
}
'''


def extract(ppc_dir: Path) -> str:
    start = f'PPC_FUNC_IMPL({FUNCTION})'
    for path in sorted(ppc_dir.glob('ppc_recomp.*.cpp')):
        text = path.read_text(encoding='utf-8', errors='replace')
        index = text.find(start)
        if index < 0:
            continue
        end = text.find('\n}\n', index)
        if end < 0:
            break
        return text[index:end + 3]
    sys.exit(f'audio barrier equivalence: {FUNCTION} not found in {ppc_dir}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ppc_dir', type=Path)
    parser.add_argument('--cxx', default='clang++', help='the generated code uses clang builtins')
    args = parser.parse_args()
    body = extract(args.ppc_dir)
    source = HARNESS.replace('@FUNCTION_BODY@', body).replace('@FUNCTION_NAME@', FUNCTION)
    r11_local = '\tPPCRegister r11{};' in body
    source = f'#define LO_GUEST_R11_LOCAL {int(r11_local)}\n' + source
    with tempfile.TemporaryDirectory() as tmp:
        cpp = Path(tmp) / 'audio_barrier_equivalence.cpp'
        exe = Path(tmp) / 'audio_barrier_equivalence'
        cpp.write_text(source)
        command = [args.cxx, '-std=c++20', '-O1', '-pthread', '-w',
                   '-I', str(args.ppc_dir.resolve()),
                   '-I', str(ROOT / 'tools/XenonRecomp/thirdparty/simde'),
                   '-I', str(ROOT / 'LostOdysseyRecomp'),
                   str(cpp), '-o', str(exe)]
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode != 0:
            # Compiler output can quote generated lines: show the messages only.
            messages = [re.sub(r'^.*?: (error|fatal error): ', r'\1: ', line)
                        for line in build.stderr.splitlines() if 'error' in line and ': ' in line]
            print(f'audio barrier equivalence: the host test did not build ({len(messages)} errors)')
            for message in messages[:10]:
                print('  ' + message[:160])
            return 1
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=900)
        print(run.stdout.strip() + (' (r11 is a local of the generated function: not compared)' if r11_local else ''))
        if run.returncode != 0:
            print(f'audio barrier equivalence: exit status {run.returncode}')
        return run.returncode


if __name__ == '__main__':
    sys.exit(main())
