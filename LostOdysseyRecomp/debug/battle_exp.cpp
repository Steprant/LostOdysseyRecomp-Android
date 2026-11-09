#include <stdafx.h>
#include <os/logger.h>
#include <settings/config.h>
#include "battle_exp.h"
#include <atomic>

extern "C" PPC_FUNC(__imp__sub_82AC1DC8);

namespace
{
    std::atomic<uint32_t>& Multiplier()
    {
        static std::atomic<uint32_t> multiplier{
            settings::ValidBattleExpMultiplier(settings::GetConfig().battleExpMultiplier)};
        return multiplier;
    }
}

uint32_t debug_menu::BattleExpMultiplier()
{
    return Multiplier().load(std::memory_order_relaxed);
}

void debug_menu::SetBattleExpMultiplier(uint32_t multiplier)
{
    multiplier = settings::ValidBattleExpMultiplier(multiplier);
    Multiplier().store(multiplier, std::memory_order_relaxed);
    if (!settings::SaveBattleExpMultiplier(multiplier))
        LOG_WARNING("debug menu: failed to persist battle EXP multiplier setting");
    LOG_INFO("debug menu: battle EXP x{}", multiplier);
}

// Victory initializer sub_82AC6D88 calls this calculator once (LR 82AC6E14),
// then sub_82AC1FA0 applies its five per-character EXP awards. The calculator
// already handles eligibility, enemy/character level differences and the
// game's EXP skill bonus. Multiply those awards (x2, x3 or x4), retaining its 100 EXP cap:
// the original application can award only one level and discards overflow.
// There is no shared base EXP pool to multiply before the level curve.
// SP/skill learning (82AC1CE0/82AC6C60) and gold (82AC31B0) are separate.
PPC_FUNC(sub_82AC1DC8)
{
    const uint32_t caller = static_cast<uint32_t>(ctx.lr);
    const uint32_t result = ctx.r3.u32;
    __imp__sub_82AC1DC8(ctx, base);
    const uint32_t multiplier = debug_menu::BattleExpMultiplier();
    if (caller != 0x82AC6E14 || multiplier <= 1)
        return;

    for (uint32_t slot = 0; slot < 5; ++slot)
    {
        const uint32_t entry = result + 0x234 + slot * 0x80;
        if (!PPC_LOAD_U32(entry))
            continue;
        const uint32_t gain = PPC_LOAD_U32(entry + 4);
        // The retail calculator produces 0..100. Unexpected values stay as they are.
        const uint32_t scaled = settings::ScaledBattleExp(gain, multiplier);
        if (scaled != gain)
            PPC_STORE_U32(entry + 4, scaled);
    }
}
