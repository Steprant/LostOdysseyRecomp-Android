#pragma once
#include <cstdint>

namespace settings
{
// Stable settings.ini values; the legacy no_random_encounters=1 maps to None.
// ThreeQuarters was added after None, so its stored value is 3; menus list it
// between Normal and Half (see EncounterRateMenuOrder).
enum class EncounterRate : uint32_t { Normal, Half, None, ThreeQuarters };
inline constexpr EncounterRate ValidEncounterRate(EncounterRate rate)
{
    return uint32_t(rate) <= uint32_t(EncounterRate::ThreeQuarters) ? rate : EncounterRate::Normal;
}

// Menu order, from most to fewest walking battles.
inline constexpr EncounterRate EncounterRateMenuOrder[] = {
    EncounterRate::Normal, EncounterRate::ThreeQuarters, EncounterRate::Half, EncounterRate::None};

inline constexpr uint32_t EncounterRateMenuIndex(EncounterRate rate)
{
    for (uint32_t i = 0; i < sizeof(EncounterRateMenuOrder) / sizeof(EncounterRateMenuOrder[0]); ++i)
        if (EncounterRateMenuOrder[i] == rate) return i;
    return 0;
}

// Whether the successful walking draw number `draw` (counted from 0 for each
// rate change) may start a battle. The game's draw has already reset its
// distance and threshold, so a rejected draw simply waits for the next one:
// Half rejects every other draw, ThreeQuarters one draw in four.
inline constexpr bool EncounterDrawAllowed(EncounterRate rate, uint32_t draw)
{
    switch (rate)
    {
    case EncounterRate::None: return false;
    case EncounterRate::Half: return (draw & 1u) != 0;
    case EncounterRate::ThreeQuarters: return (draw & 3u) != 0;
    default: return true;
    }
}
}
