#pragma once
#include <atomic>
#include <cstdint>

// Last SGSR2 frame's SMAA and filter state, for the performance overlay and
// the runtime log (2.0, WP7.2.2). Written by the SGSR2 controller on the
// render thread, read anywhere. Zero until SGSR2 records a frame.
namespace gpu::sgsr2 {
struct Sgsr2Status {
    bool recorded = false, smaaActive = false, smaaRequested = false, soft = false;
    static constexpr uint32_t Pack(bool smaaActive, bool smaaRequested, bool soft)
    {
        return 1u | (smaaActive ? 2u : 0u) | (smaaRequested ? 4u : 0u) | (soft ? 8u : 0u);
    }
    static constexpr Sgsr2Status Unpack(uint32_t value)
    {
        return {(value & 1u) != 0, (value & 2u) != 0, (value & 4u) != 0, (value & 8u) != 0};
    }
};
inline std::atomic<uint32_t> lastStatus{0};
inline Sgsr2Status LastStatus() { return Sgsr2Status::Unpack(lastStatus.load(std::memory_order_relaxed)); }
// The SGSR2 Soft filter (settings sgsr2_filter=1): read by the renderer for
// the halved scene mip bias and by the temporal upscaler for the kernel.
inline std::atomic<bool> softFilter{false};
} // namespace gpu::sgsr2
