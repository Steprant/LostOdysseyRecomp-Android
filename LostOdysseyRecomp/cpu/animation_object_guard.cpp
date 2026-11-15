#include <stdafx.h>
#include <atomic>
#include <os/logger.h>

// The Matinee skeletal update dereferences the result of its animation
// object lookup before testing it. Mack's battle exit reached it with r3=0.
bool SkipMissingAnimationObject(PPCRegister& result, PPCRegister& owner)
{
    if (result.u32) return false;

    static std::atomic<uint32_t> reported{0};
    if (reported.fetch_add(1, std::memory_order_relaxed) < 8)
        LOG_WARNING("animation object unavailable: skipped setup for owner {:#x} at 0x825FA510", owner.u32);
    return true;
}
