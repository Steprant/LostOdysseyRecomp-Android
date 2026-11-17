#include <stdafx.h>
#include <os/logger.h>
#include "crt_native.h"

// Steprant 2.2: the game's CRT memcpy and memset, and its stack probe, run
// as host code (patches/crt_native.h).

extern "C" PPC_FUNC(__imp__sub_82B7A0B0);
extern "C" PPC_FUNC(__imp__sub_82B7BC40);
extern "C" PPC_FUNC(__imp__sub_82B7E504);

namespace
{
void ReportOnce()
{
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed))
        LOG_INFO("crt native: memcpy, memset and the stack probe run as host code");
}
}

PPC_FUNC(sub_82B7A0B0)
{
    if (crt_native::Enabled()) {
        ReportOnce();
        if (crt_native::Memcpy(ctx, base))
            return;
    }
    __imp__sub_82B7A0B0(ctx, base);
}

PPC_FUNC(sub_82B7BC40)
{
    if (crt_native::Enabled() && crt_native::Memset(ctx, base))
        return;
    __imp__sub_82B7BC40(ctx, base);
}

PPC_FUNC(sub_82B7E504)
{
    // The guest stack is committed memory: the page probe has nothing to do.
    if (crt_native::Enabled())
        return;
    __imp__sub_82B7E504(ctx, base);
}
