#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>

// One continuous guest clock for mftb and KeTimeStampBundle. Host rendering,
// UI deadlines and I/O waits keep their own unscaled clocks. The audio driver
// follows AudioRate(): it mixes that many guest frames per output frame.
namespace debug_menu::fast_forward {
inline constexpr unsigned Rates[] = {2, 3, 4, 6, 8};
inline constexpr uint64_t InputLeaseNs = 250000000;
// Outside free field walking (battles, events, the intro), one game frame
// may advance at most this much game time. The game was written for 33 ms
// frames; a steady 133 ms (4x) to 267 ms (8x) per frame stalled battle
// actions and the intro until the boost stopped (Stefano, 9/11). The limit
// caps the RATE, an integer the audio driver follows too: at 30 FPS it is 2x
// (67 ms frames), at 60 FPS up to 4x. Never a flat clock inside a frame, and
// game time and audio keep one speed. Free field walking keeps the chosen rate.
inline constexpr uint64_t LimitedFrameNs = 83333333;
// The rate the limit allows for a real frame of frameNs (at least 1).
inline constexpr unsigned LimitedRate(unsigned selected, uint64_t frameNs) {
    if (frameNs == 0) return selected;
    const uint64_t allowed = LimitedFrameNs / frameNs;
    return unsigned(std::max<uint64_t>(1, std::min<uint64_t>(selected, allowed)));
}
enum class Mode { Hold, Toggle };
enum class Button { LT = 0, LB = 1 };
struct Buttons { uint8_t lt = 0, rt = 0, lb = 0, rb = 0; };
struct ButtonInput { uint8_t button, pair; };
inline ButtonInput SelectButton(Button button, const Buttons& buttons) {
    switch (button) {
    case Button::LB: return {buttons.lb, buttons.rb};
    default: return {buttons.lt, buttons.rt};
    }
}
inline uint64_t NowNs() {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
struct Clock {
    uint64_t last = 0, value = 0, lease = 0;
    unsigned rate = 1;
    bool initialized = false, paused = false;
    void Add(uint64_t elapsed, unsigned factor) {
        const auto room = std::numeric_limits<uint64_t>::max() - value;
        value += elapsed > room / factor ? room : elapsed * factor;
    }
    uint64_t Read(uint64_t now) {
        if (!initialized) { initialized = true; last = value = now; }
        if (now < last) return value;
        if (!paused) {
            const auto boostedEnd = std::clamp(lease, last, now);
            Add(boostedEnd - last, rate);
            Add(now - boostedEnd, 1);
        }
        last = now;
        if (now >= lease) rate = 1;
        return value;
    }
};
struct Status { bool enabled = false, active = false; unsigned multiplier = 2; Mode mode = Mode::Hold; bool keepAudioPitch = true; Button button = Button::LT; bool limited = false; unsigned effective = 1; };
struct Control {
    Clock clock;
    bool enabled = false, armed = false, held = false;
    unsigned multiplier = 2;
    Mode mode = Mode::Hold;
    bool keepAudioPitch = true;
    Button button = Button::LT;
    // Frame limit (see LimitedFrameNs): on outside free field walking.
    bool limited = false;
    unsigned rateCap = ~0u;
    uint64_t frameStart = 0;
    unsigned Boost() const { return std::max(1u, std::min(multiplier, rateCap)); }
    // Engine tick, guest game thread: the previous frame's real length sets
    // the cap for this one.
    void BeginFrame(uint64_t now, bool limit) {
        clock.Read(now);
        const uint64_t frame = frameStart && now > frameStart ? now - frameStart : 0;
        frameStart = now;
        limited = limit;
        rateCap = limit ? LimitedRate(multiplier, frame) : ~0u;
        if (clock.rate > 1) clock.rate = Boost();
    }
    void Stop(uint64_t now) {
        clock.Read(now); clock.rate = 1; clock.lease = now;
        held = armed = false;
    }
    void Enable(bool value, uint64_t now) { Stop(now); enabled = value; }
    void SetMode(Mode value, uint64_t now) { Stop(now); mode = value; }
    void SetButton(Button value, uint64_t now) {
        Stop(now); button = value >= Button::LT && value <= Button::LB ? value : Button::LT;
    }
    void SetRate(unsigned value, uint64_t now) {
        if (std::find(std::begin(Rates), std::end(Rates), value) == std::end(Rates)) return;
        clock.Read(now); multiplier = value;
        if (clock.rate > 1) clock.rate = Boost();
    }
    void Pause(bool value, uint64_t now) {
        Stop(now); clock.paused = value;
    }
    void Sample(uint8_t button, uint8_t pair, bool allowed, uint64_t now) {
        clock.Read(now);
        if (!allowed || clock.paused || !enabled) { Stop(now); return; }
        // Require a release after focus/menu/enable/binding changes. Hysteresis
        // prevents trigger noise; paired buttons remain available for shortcuts.
        if (button <= 32) {
            if (mode == Mode::Hold) held = false;
            armed = true;
        } else if (button >= 64 && armed) {
            if (mode == Mode::Hold) held = true;
            else if (pair <= 32) held = !held;
            armed = false;
        }
        clock.rate = held && pair <= 32 ? Boost() : 1;
        clock.lease = now > std::numeric_limits<uint64_t>::max() - InputLeaseNs
            ? std::numeric_limits<uint64_t>::max() : now + InputLeaseNs;
    }
    Status Get(uint64_t now) {
        clock.Read(now);
        return {enabled, clock.rate > 1 && !clock.paused, multiplier, mode, keepAudioPitch, button,
                limited, ActiveRate()};
    }
    unsigned ActiveRate() const { return clock.rate > 1 && !clock.paused ? clock.rate : 1; }
    // The bound button is the speed control while fast-forward is on and its
    // pair is not held (a pair chord stays the game's / the Quick tools').
    bool OwnsButton(uint8_t pair) const { return enabled && pair <= 32; }
};
inline std::mutex mutex;
inline Control control;
// The clock's current rate for the audio driver, read without the lock.
inline std::atomic<unsigned> audioRate{1};
inline std::atomic<bool> audioKeepPitch{true};
// Caller holds mutex.
inline void PublishRate() {
    const unsigned rate = control.ActiveRate();
    if (audioRate.load(std::memory_order_relaxed) != rate) audioRate.store(rate, std::memory_order_relaxed);
}
inline unsigned AudioRate() { return audioRate.load(std::memory_order_relaxed); }
inline bool KeepAudioPitch() { return audioKeepPitch.load(std::memory_order_relaxed); }
inline uint64_t GameTimeNs() {
    std::lock_guard lock(mutex);
    const uint64_t value = control.clock.Read(NowNs());
    PublishRate();
    return value;
}
inline Status GetStatus() { std::lock_guard lock(mutex); const Status status = control.Get(NowNs()); PublishRate(); return status; }
inline void Enable(bool enabled) { std::lock_guard lock(mutex); control.Enable(enabled, NowNs()); PublishRate(); }
inline void SetMode(Mode mode) { std::lock_guard lock(mutex); control.SetMode(mode, NowNs()); PublishRate(); }
inline void SetButton(Button button) { std::lock_guard lock(mutex); control.SetButton(button, NowNs()); PublishRate(); }
inline void SetRate(unsigned rate) { std::lock_guard lock(mutex); control.SetRate(rate, NowNs()); PublishRate(); }
inline void SetKeepAudioPitch(bool keep) {
    std::lock_guard lock(mutex);
    control.keepAudioPitch = keep;
    audioKeepPitch.store(keep, std::memory_order_relaxed);
}
inline void SetPaused(bool paused) { std::lock_guard lock(mutex); control.Pause(paused, NowNs()); PublishRate(); }
inline void Release() { std::lock_guard lock(mutex); control.Stop(NowNs()); PublishRate(); }
// Engine tick on the guest game thread: one game frame begins.
inline void BeginFrame(bool limited) {
    std::lock_guard lock(mutex); control.BeginFrame(NowNs(), limited); PublishRate();
}
inline bool OwnsButton(uint8_t pair) { std::lock_guard lock(mutex); return control.OwnsButton(pair); }
inline void Sample(uint8_t button, uint8_t pair, bool allowed) {
    std::lock_guard lock(mutex); control.Sample(button, pair, allowed, NowNs()); PublishRate();
}
} // namespace debug_menu::fast_forward
