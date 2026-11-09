#pragma once
#include <settings/encounter_rate.h>

namespace debug_menu
{
    // Only the walking encounter draw is filtered; scripted battles still run.
    settings::EncounterRate GetEncounterRate();
    void SetEncounterRate(settings::EncounterRate rate);
    // Session-only: every walking step in an area with random encounters starts
    // a battle. Enabling it restores Normal; 0.75/Half/None turn it off.
    bool EncounterEveryStepEnabled();
    void SetEncounterEveryStepEnabled(bool enabled);
}
