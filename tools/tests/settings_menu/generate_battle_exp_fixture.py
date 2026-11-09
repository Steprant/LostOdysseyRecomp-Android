"""Exercise the production EXP hook with synthetic guest memory and call boundaries."""
import argparse
from pathlib import Path
import re

PREAMBLE = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <settings/config.h>
#include <debug/battle_exp.h>
#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
struct Reg { union { uint64_t u64 = 0; uint32_t u32; }; };
struct PPCContext { Reg r3, r4; uint64_t lr = 0, cookie = 0x12345678; };
#define PPC_FUNC(name) void name(PPCContext& ctx, uint8_t* base)
uint32_t Load32(const uint8_t* base, uint32_t p) {
    return uint32_t(base[p]) << 24 | uint32_t(base[p+1]) << 16 |
        uint32_t(base[p+2]) << 8 | base[p+3];
}
void Store32(uint8_t* base, uint32_t p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i) base[p+i] = uint8_t(v >> (24-i*8));
}
#define PPC_LOAD_U32(p) Load32(base, uint32_t(p))
#define PPC_STORE_U32(p,v) Store32(base, uint32_t(p), uint32_t(v))
unsigned checks = 0, saves = 0, originals = 0;
bool saveSucceeds = true;
settings::Config config;
namespace settings {
Config GetConfig() { return config; }
bool SaveBattleExpMultiplier(uint32_t multiplier) {
    ++saves;
    if (!saveSucceeds) return false;
    config.battleExpMultiplier = multiplier;
    return true;
}
}
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
using Memory = std::array<uint8_t, 4096>;
Memory afterOriginal;
constexpr uint32_t Result = 0x100;
std::array<uint32_t, 5> gains, characters;
extern "C" PPC_FUNC(__imp__sub_82AC1DC8) {
    ++originals;
    Check(ctx.r3.u32 == Result, "original receives result manager");
    // The guest calculator owns these outputs; simulate it at this boundary.
    for (uint32_t i = 0; i < 5; ++i) {
        Store32(base, Result + 0x234 + i*0x80, characters[i]);
        Store32(base, Result + 0x238 + i*0x80, gains[i]);
    }
    std::copy_n(base, afterOriginal.size(), afterOriginal.begin());
    // Deliberately clobber volatile state. The wrapper must capture inputs first
    // and preserve all state left by the original function.
    ctx.r3.u64 = 0xabcdef;
    ctx.r4.u64 = 0xface;
    ctx.lr = 0xfeed;
}
'''

TESTS = r'''
void Run(uint32_t caller, const std::array<uint32_t, 5>& expected) {
    Memory memory;
    memory.fill(0x5a); // Includes SP, gold, skill/link state and level-up flags.
    PPCContext ctx;
    ctx.r3.u64 = Result;
    ctx.lr = caller;
    const unsigned before = originals;
    sub_82AC1DC8(ctx, memory.data());
    Check(originals == before + 1, "original calculator runs exactly once");
    Check(ctx.r3.u64 == 0xabcdef && ctx.r4.u64 == 0xface &&
        ctx.lr == 0xfeed && ctx.cookie == 0x12345678, "guest register state preserved");
    Memory expectedMemory = afterOriginal;
    for (uint32_t i = 0; i < 5; ++i) {
        Check(Load32(memory.data(), Result + 0x238 + i*0x80) == expected[i], "EXP gain");
        Store32(expectedMemory.data(), Result + 0x238 + i*0x80, expected[i]);
    }
    Check(memory == expectedMemory, "only eligible EXP words change; SP/gold/skills untouched");
}
int main(int argc, char** argv) {
    // Separate process verifies a persisted multiplier at startup instead of an initialized atomic.
    config.battleExpMultiplier = argc == 2 ? uint32_t(std::atoi(argv[1])) : 1;
    Check(debug_menu::BattleExpMultiplier() == settings::ValidBattleExpMultiplier(config.battleExpMultiplier),
        "startup setting restored");
    characters = {0x900, 0x910, 0x920, 0x930, 0x940};
    gains = {1, 25, 34, 50, 100};
    if (argc == 2) {
        const uint32_t m = debug_menu::BattleExpMultiplier();
        std::array<uint32_t, 5> expected;
        for (uint32_t i = 0; i < 5; ++i) expected[i] = std::min(gains[i] * m, 100u);
        Run(0x82AC6E14, expected);
        return 0;
    }
    Run(0x82AC6E14, gains); // Default off (x1).
    debug_menu::SetBattleExpMultiplier(2);
    Check(saves == 1 && config.battleExpMultiplier == 2 && debug_menu::BattleExpMultiplier() == 2, "x2 persists");
    Run(0x82AC6E14, {2, 50, 68, 100, 100});
    debug_menu::SetBattleExpMultiplier(3);
    Check(saves == 2 && config.battleExpMultiplier == 3, "x3 persists");
    Run(0x82AC6E14, {3, 75, 100, 100, 100});
    debug_menu::SetBattleExpMultiplier(4);
    Check(saves == 3 && config.battleExpMultiplier == 4, "x4 persists");
    Run(0x82AC6E14, {4, 100, 100, 100, 100});
    debug_menu::SetBattleExpMultiplier(10);
    Check(saves == 4 && config.battleExpMultiplier == 10, "x10 persists");
    Run(0x82AC6E14, {10, 100, 100, 100, 100});
    debug_menu::SetBattleExpMultiplier(4);
    Check(saves == 5 && config.battleExpMultiplier == 4, "back to x4");
    Run(0x82AC6E10, gains); // Exact return address, not a nearby call.
    Run(0x12345678, gains); // Other callers always run unchanged.
    gains = {0, 20, 99, 101, 0xffffffff};
    Run(0x82AC6E14, {0, 80, 100, 101, 0xffffffff});
    characters[1] = 0;
    Run(0x82AC6E14, {0, 20, 100, 101, 0xffffffff}); // Empty party slot.
    debug_menu::SetBattleExpMultiplier(9); // Out of range falls back to retail.
    Check(saves == 6 && config.battleExpMultiplier == 1 && debug_menu::BattleExpMultiplier() == 1, "invalid is x1");
    debug_menu::SetBattleExpMultiplier(1);
    Check(saves == 7 && config.battleExpMultiplier == 1 && debug_menu::BattleExpMultiplier() == 1, "disable persists");
    Run(0x82AC6E14, gains);
    saveSucceeds = false;
    debug_menu::SetBattleExpMultiplier(2);
    Check(saves == 8 && config.battleExpMultiplier == 1 && debug_menu::BattleExpMultiplier() == 2,
        "failed persistence still applies the live preference");
    for (uint32_t m = 0; m <= 11; ++m)
        for (uint32_t g = 0; g <= 120; ++g) {
            const uint32_t v = settings::ValidBattleExpMultiplier(m);
            const uint32_t expected = g == 0 || g > 100 ? g : std::min(g * v, 100u);
            Check(settings::ScaledBattleExp(g, m) == expected, "scaled EXP table");
        }
    std::printf("battle EXP production hook: %u checks passed\n", checks);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = (args.root / "LostOdysseyRecomp/debug/battle_exp.cpp").read_text(encoding="utf-8")
    # Replace includes and external services only; the production hook stays intact.
    source = re.sub(r"^#include[^\n]*\n", "", source, flags=re.MULTILINE)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(PREAMBLE + source + TESTS, encoding="utf-8")


if __name__ == "__main__":
    main()
