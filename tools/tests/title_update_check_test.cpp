// Steprant 2.0.2: the Android update check starts at the title menu, once,
// and offers the download only there (updater/title_check.h).
#include <updater/title_check.h>
#include <cstdio>
#include <stdexcept>
using namespace updater::title_check;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        Check(EnabledFor(nullptr) && EnabledFor("") && EnabledFor("1"), "at the title screen by default");
        Check(!EnabledFor("0"), "LO_UPDATE_AT_TITLE=0 restores the check at the start");

        Machine off;
        Check(off.OnTitleIdle(false, true) == Action::None && off.state == State::Idle, "nothing when it is disabled");
        Machine noUpdates;
        for (int i = 0; i < 100; ++i)
            Check(noUpdates.OnTitleIdle(true, false) == Action::None, "nothing while automatic updates are off");
        Check(noUpdates.OnTitleIdle(true, true) == Action::StartCheck, "turning them on in Settings starts the check at the title");

        // Up to date: one request, nothing else for the rest of the process.
        Machine m;
        Check(m.OnTitleIdle(true, true) == Action::StartCheck, "the first idle title frame starts the check");
        unsigned started = 0;
        for (int i = 0; i < 1000; ++i) started += m.OnTitleIdle(true, true) != Action::None;
        Check(started == 0 && m.state == State::Checking, "while the request runs the title frames start nothing");
        Check(!m.Settled(), "not settled while checking");
        m.CheckFinished(false, false);
        Check(m.state == State::UpToDate && m.Settled(), "up to date settles it");
        Check(m.OnTitleIdle(true, true) == Action::None, "and it is not asked again in this process");

        // Offline.
        Machine offline;
        offline.OnTitleIdle(true, true);
        offline.CheckFinished(false, true);
        Check(offline.state == State::Failed && offline.Settled() && offline.OnTitleIdle(true, true) == Action::None,
            "a failed request is not retried in this process");

        // A newer release: the dialog only from a title frame, once.
        Machine update;
        update.OnTitleIdle(true, true);
        // The player started the game before the answer came: no title frames.
        update.CheckFinished(true, false);
        Check(update.state == State::Available && !update.Settled(), "the answer waits; nothing is shown during the game");
        // Back at the title menu.
        Check(update.OnTitleIdle(true, true) == Action::StartPrompt, "the next idle title frame offers the download");
        Check(update.OnTitleIdle(true, true) == Action::None, "one dialog, not one per frame");
        update.PromptFinished();
        Check(update.state == State::Done && update.Settled() && update.OnTitleIdle(true, true) == Action::None,
            "after the dialog nothing more happens");

        // Out-of-order calls change nothing.
        Machine odd;
        odd.CheckFinished(true, false);
        odd.PromptFinished();
        Check(odd.state == State::Idle, "results without a request are ignored");
        std::printf("title update check: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("title update check test failed: %s\n", e.what());
        return 1;
    }
}
