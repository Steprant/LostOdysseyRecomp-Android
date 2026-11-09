#pragma once
#include <cstdint>

namespace debug_menu
{
    // 1 (retail), 2, 3 or 4; persisted as settings.ini battle_exp_multiplier.
    uint32_t BattleExpMultiplier();
    void SetBattleExpMultiplier(uint32_t multiplier);
}
