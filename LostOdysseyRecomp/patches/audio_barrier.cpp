#include <stdafx.h>
#include <os/logger.h>
#include "audio_barrier.h"

// Steprant 2.1.6: the audio workers' rendezvous waits instead of spinning
// (patches/audio_barrier.h).

extern "C" PPC_FUNC(__imp__sub_82CC3C50);

PPC_FUNC(sub_82CC3C50)
{
    const auto& settings = audio_barrier::Config();
    audio_barrier::g_stats.calls.fetch_add(1, std::memory_order_relaxed);
    // The guest loop as generated: switched off, or a flag word the 64-bit
    // accesses below could not read whole (never seen).
    if (!settings.enabled || (ctx.r4.u32 & 7) != 0) {
        __imp__sub_82CC3C50(ctx, base);
        return;
    }
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true))
        LOG_INFO("audio barrier: waiting on the flag word (spin {} ns, wfe {} us, then sleeps of {} us)",
            settings.spinNs, settings.wfeUs, settings.sleepUs);
    audio_barrier::Run(ctx, base, settings, audio_barrier::g_stats);
}
