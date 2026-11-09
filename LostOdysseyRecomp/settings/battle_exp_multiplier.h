#pragma once
#include <algorithm>
#include <cstdint>

namespace settings
{
// Battle EXP multiplier of the Quick tools (settings.ini battle_exp_multiplier).
// 1 is the retail award; ×10 fills the 100 EXP cap from any award of 10 or
// more. Profiles from before ×3/×4 stored only
// battle_exp_double=1, which reads as 2; saving keeps writing that key so an
// older build still finds its ×2 switch.
inline constexpr uint32_t BattleExpMultipliers[] = {1, 2, 3, 4, 10};
inline constexpr uint32_t BattleExpMultiplierIndex(uint32_t multiplier)
{
    for (uint32_t i = 0; i < sizeof(BattleExpMultipliers) / sizeof(BattleExpMultipliers[0]); ++i)
        if (BattleExpMultipliers[i] == multiplier) return i;
    return 0;
}

inline constexpr uint32_t ValidBattleExpMultiplier(uint32_t multiplier)
{
    return BattleExpMultipliers[BattleExpMultiplierIndex(multiplier)];
}

// The victory calculator awards each character 0..100 EXP; the game's own
// application can grant at most one level and discards overflow, so a scaled
// award keeps the 100 cap. Values outside the retail range are left alone.
inline constexpr uint32_t ScaledBattleExp(uint32_t gain, uint32_t multiplier)
{
    multiplier = ValidBattleExpMultiplier(multiplier);
    if (gain == 0 || gain > 100 || multiplier == 1)
        return gain;
    return std::min(gain * multiplier, 100u);
}
}
