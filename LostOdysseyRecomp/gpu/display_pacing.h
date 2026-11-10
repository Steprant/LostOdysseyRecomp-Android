#pragma once
// Steprant 2.0.1: frames held on screen for the same number of refreshes.
//
// The host paces the game at its frame rate (30 FPS) with a timer, then
// presents into a FIFO swapchain. A 120 Hz panel shows each image at the first
// refresh after it is ready, so a frame finished 1 ms earlier or later than
// its neighbours, near a refresh boundary, stays on screen 3 or 5 refreshes
// instead of 4: 25 and 42 ms instead of 33. The counter still reads 30 FPS,
// the motion stutters. With the display timing extension each present asks
// for the refresh it belongs to, k refreshes after the previous one, counted
// from when the display really showed an earlier frame. A frame that cannot
// make its refresh is shown as soon as possible and the cadence restarts
// from it, never caught up with a burst.
//
// Pure logic, shared with the host test; the caller owns the swapchain.
#include <algorithm>
#include <cstdint>

namespace gpu::display_pacing
{
struct Feedback
{
    uint32_t id = 0;
    uint64_t desired = 0, actual = 0;
};

struct Stats
{
    uint32_t shown = 0;       // presents with a known display time
    uint32_t exact = 0;       // shown exactly `refreshes` refreshes after the previous one
    uint32_t shorter = 0;     // shown earlier than that
    uint32_t longer = 0;      // shown later than that
    uint32_t late = 0;        // shown more than half a refresh after the time it asked for
    uint32_t immediate = 0;   // presented without a desired time (pacing off or restarting)
    uint32_t restarts = 0;    // cadence restarted from a late or far frame
    uint32_t refreshes = 0;   // refreshes per frame of the current cadence (0: off)
    uint64_t refreshNs = 0;
};

// Refreshes per content frame, or 0 when the frame period is not a whole
// number of refreshes (within 4%), e.g. 30 FPS on a 144 Hz panel.
inline uint32_t RefreshesPerFrame(uint64_t refreshNs, uint64_t periodNs)
{
    if (refreshNs < 2000000ull || refreshNs > 50000000ull || periodNs < refreshNs / 2) return 0;
    const uint64_t k = (periodNs + refreshNs / 2) / refreshNs;
    if (k == 0 || k > 8) return 0;
    const uint64_t interval = k * refreshNs;
    const uint64_t error = interval > periodNs ? interval - periodNs : periodNs - interval;
    return error * 25 <= periodNs ? uint32_t(k) : 0;
}

class Scheduler
{
public:
    // The desired display time for the present with `id` (0: as soon as possible).
    uint64_t Next(uint32_t id, uint64_t nowNs, uint64_t refreshNs, uint64_t periodNs, bool enabled)
    {
        const uint32_t k = enabled ? RefreshesPerFrame(refreshNs, periodNs) : 0;
        if (!k) {
            if (m_k) ++m_stats.restarts;
            m_k = 0;
            m_anchorValid = false;
            ++m_stats.immediate;
            return 0;
        }
        if (k != m_k || refreshNs != m_refreshNs) {
            // A new cadence (frame rate or panel mode changed): start from the
            // next frame the display reports.
            m_k = k;
            m_refreshNs = refreshNs;
            m_anchorValid = false;
        }
        m_stats.refreshes = k;
        m_stats.refreshNs = refreshNs;
        if (!m_anchorValid) {
            ++m_stats.immediate;
            return 0;
        }
        const uint64_t interval = uint64_t(k) * refreshNs;
        const int64_t frames = int64_t(id) - int64_t(m_anchorId);
        if (frames <= 0 || frames > 64) {
            m_anchorValid = false;
            ++m_stats.restarts;
            ++m_stats.immediate;
            return 0;
        }
        // A quarter refresh early: the compositor shows a buffer at the first
        // refresh not earlier than the request, so this lands on the intended
        // refresh even with rounding in the reported period.
        const uint64_t target = m_anchorActual + uint64_t(frames) * interval - refreshNs / 4;
        // Too late to make its refresh (the compositor needs the buffer about
        // one refresh ahead), or so far ahead that the host runs faster than
        // the display: restart the cadence from this frame.
        if (target < nowNs + refreshNs / 2 || target > nowNs + 4 * interval) {
            m_anchorValid = false;
            ++m_stats.restarts;
            ++m_stats.immediate;
            return 0;
        }
        return target;
    }

    // Feedback from the display, oldest first.
    void Observe(const Feedback& f)
    {
        if (!f.actual) return;
        ++m_stats.shown;
        if (m_lastActual && f.actual > m_lastActual && m_k && m_refreshNs) {
            const uint64_t delta = f.actual - m_lastActual;
            const uint32_t gaps = f.id > m_lastId ? f.id - m_lastId : 1;
            const uint64_t expected = uint64_t(m_k) * m_refreshNs * gaps;
            if (delta + m_refreshNs / 2 < expected) ++m_stats.shorter;
            else if (delta > expected + m_refreshNs / 2) ++m_stats.longer;
            else ++m_stats.exact;
        }
        if (f.desired && f.actual > f.desired + m_refreshNs / 2 + m_refreshNs / 4) ++m_stats.late;
        m_lastActual = f.actual;
        m_lastId = f.id;
        // The newest displayed frame anchors the cadence.
        if (m_k && (!m_anchorValid || f.id >= m_anchorId)) {
            m_anchorValid = true;
            m_anchorId = f.id;
            m_anchorActual = f.actual;
        }
    }

    Stats TakeStats()
    {
        Stats s = m_stats;
        m_stats = {};
        m_stats.refreshes = s.refreshes;
        m_stats.refreshNs = s.refreshNs;
        return s;
    }

    uint32_t Refreshes() const { return m_k; }

private:
    uint32_t m_k = 0;
    uint64_t m_refreshNs = 0;
    bool m_anchorValid = false;
    uint32_t m_anchorId = 0;
    uint64_t m_anchorActual = 0;
    uint64_t m_lastActual = 0;
    uint32_t m_lastId = 0;
    Stats m_stats;
};
} // namespace gpu::display_pacing
