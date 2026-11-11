#pragma once
// Steprant 2.0.2: the Android update check, moved from the start of the
// process to the title screen and to its own thread.
//
// Until 2.0 the check ran on the main thread before the game started: one
// HTTPS request to the release API with an 8 s timeout, on every launch. A
// slow or half-connected network held the start for that long.
// Now nothing is asked at the start. The first time the title menu waits for
// input, a worker thread makes the request. If a newer release with an
// Android package exists, and the title menu is (still, or again) waiting for
// input, a system dialog offers the download. Nothing runs, waits or shows
// while a game is in progress: both steps are started only by the title
// menu's own tick, once per process each.
// LO_UPDATE_AT_TITLE=0 restores the check at the start (2.0).
#include <cstdint>

namespace updater::title_check {

enum class State : uint8_t { Idle, Checking, UpToDate, Failed, Available, Prompting, Done };
enum class Action : uint8_t { None, StartCheck, StartPrompt };

// Pure state machine (host test). The caller serialises the calls.
struct Machine
{
    State state = State::Idle;

    // The title menu is waiting for input in this frame.
    Action OnTitleIdle(bool enabled, bool automaticUpdates)
    {
        if (!enabled) return Action::None;
        if (state == State::Idle) {
            if (!automaticUpdates) return Action::None;
            state = State::Checking;
            return Action::StartCheck;
        }
        if (state == State::Available) {
            state = State::Prompting;
            return Action::StartPrompt;
        }
        return Action::None;
    }
    void CheckFinished(bool available, bool failed)
    {
        if (state != State::Checking) return;
        state = available ? State::Available : failed ? State::Failed : State::UpToDate;
    }
    void PromptFinished()
    {
        if (state == State::Prompting) state = State::Done;
    }
    // Nothing more can happen in this process.
    bool Settled() const { return state == State::UpToDate || state == State::Failed || state == State::Done; }
};

inline bool EnabledFor(const char* value)
{
    return !(value && value[0] == '0' && !value[1]);
}

// Android: LO_UPDATE_AT_TITLE is not 0. False elsewhere.
bool Enabled();
// From the title menu's tick while it waits for input. One atomic load once
// the check is settled. No-op off Android.
void OnTitleIdle();

}
