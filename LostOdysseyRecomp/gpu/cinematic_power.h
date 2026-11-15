#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace gpu::cinematic_power
{
inline constexpr int kWarmBatteryTenths = 415;
inline constexpr int kCoolBatteryTenths = 405;
inline constexpr uint32_t kSceneHeight = 720;

// Engine thread only. The temperature is the latest Android battery sample.
// Hysteresis and the short scene-end grace avoid repeated target rebuilds.
class Policy
{
    using Clock = std::chrono::steady_clock;
    bool warm = false, limited = false;
    Clock::time_point lastScene{};

public:
    bool Update(int batteryTenths, bool scene, Clock::time_point now)
    {
        if (batteryTenths >= kWarmBatteryTenths) warm = true;
        else if (batteryTenths <= kCoolBatteryTenths) warm = false;
        if (scene) lastScene = now;
        limited = warm && (scene || (limited && now - lastScene < std::chrono::seconds(3)));
        return limited;
    }
};

inline std::atomic<int> batteryTenths{0};
inline std::atomic<bool> limited{false};
inline bool Active() { return limited.load(std::memory_order_relaxed); }
}
