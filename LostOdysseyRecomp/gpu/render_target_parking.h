#pragma once
// Render targets kept across an internal-resolution change (Renderer::
// parkedTargets). The SGSR2 menu switch alternates two sizes, so every key
// keeps up to two parked allocations: one per size. A lookup at one size
// never discards the allocation of the other size, which the next switch
// back will want. The policy is generic over the stored handle so the host
// test can exercise it without a GPU.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

namespace gpu::parking
{
template<class Handle>
struct Slot
{
    Handle target{};
    uint64_t generation = 0; // resize that parked it; older is evicted first
    uint64_t parkedAtMs = 0; // Steprant 2.0.2: when it was parked (EvictOlderThan)
    bool Used() const { return static_cast<bool>(target); }
};

template<class Handle>
using Pair = std::array<Slot<Handle>, 2>;

// Where a target being parked goes: the slot holding the same size (that
// older allocation is replaced), else a free slot, else the older slot.
template<class Handle, class SameSize>
size_t ChooseParkSlot(const Pair<Handle>& pair, SameSize&& sameSize)
{
    for (size_t i = 0; i < pair.size(); ++i)
        if (pair[i].Used() && sameSize(pair[i].target)) return i;
    for (size_t i = 0; i < pair.size(); ++i)
        if (!pair[i].Used()) return i;
    return pair[0].generation <= pair[1].generation ? 0 : 1;
}

// The slot whose allocation fits the request, or pair.size() if none. A
// mismatch leaves both slots as they are.
template<class Handle, class Fits>
size_t FindReusableSlot(const Pair<Handle>& pair, Fits&& fits)
{
    for (size_t i = 0; i < pair.size(); ++i)
        if (pair[i].Used() && fits(pair[i].target)) return i;
    return pair.size();
}

// Budget: evict the oldest generations first until the total fits. Returns
// the evicted handles (the caller retires them after the GPU is done).
template<class Map, class Bytes>
auto EvictToBudget(Map& map, uint64_t budget, Bytes&& bytes)
{
    using Handle = decltype(map.begin()->second[0].target);
    std::vector<Handle> evicted;
    uint64_t total = 0;
    for (const auto& [key, pair] : map)
        for (const auto& slot : pair)
            if (slot.Used()) total += bytes(slot.target);
    while (total > budget) {
        Slot<Handle>* oldest = nullptr;
        for (auto& [key, pair] : map)
            for (auto& slot : pair)
                if (slot.Used() && (!oldest || slot.generation < oldest->generation)) oldest = &slot;
        if (!oldest) break;
        total -= bytes(oldest->target);
        evicted.push_back(std::move(oldest->target));
        oldest->target = Handle{};
        oldest->generation = 0;
    }
    for (auto it = map.begin(); it != map.end();)
        it = (!it->second[0].Used() && !it->second[1].Used()) ? map.erase(it) : std::next(it);
    return evicted;
}

// Steprant 2.0.2: a size that has not been used for maxAgeMs is not coming
// back soon (phone to TV and back took the PSS from 2.1 to 4.1 GB in the
// 10/11 session with both sizes parked). Evicts every slot parked at least
// that long ago and returns the handles. A slot taken back by a lookup is no
// longer in the map, so only allocations nobody asked for age here.
template<class Map>
auto EvictOlderThan(Map& map, uint64_t nowMs, uint64_t maxAgeMs)
{
    using Handle = decltype(map.begin()->second[0].target);
    std::vector<Handle> evicted;
    for (auto it = map.begin(); it != map.end();) {
        for (auto& slot : it->second)
            if (slot.Used() && nowMs >= slot.parkedAtMs && nowMs - slot.parkedAtMs >= maxAgeMs) {
                evicted.push_back(std::move(slot.target));
                slot = {};
            }
        it = (!it->second[0].Used() && !it->second[1].Used()) ? map.erase(it) : std::next(it);
    }
    return evicted;
}

template<class Map, class Bytes>
uint64_t TotalBytes(const Map& map, Bytes&& bytes)
{
    uint64_t total = 0;
    for (const auto& [key, pair] : map)
        for (const auto& slot : pair)
            if (slot.Used()) total += bytes(slot.target);
    return total;
}

template<class Map>
size_t Count(const Map& map)
{
    size_t count = 0;
    for (const auto& [key, pair] : map)
        for (const auto& slot : pair) count += slot.Used();
    return count;
}
}
