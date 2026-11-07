// M2: Renderer::parkedTargets keeps two sizes per render-target key so the
// SGSR2 menu switch (quality <-> output) reuses both directions.
#include <gpu/render_target_parking.h>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <unordered_map>

using namespace gpu::parking;

namespace
{
unsigned checks = 0;
void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }

struct Target { int size; uint64_t bytes; };
using Handle = std::unique_ptr<Target>;
using Map = std::unordered_map<int, Pair<Handle>>;

Handle Make(int size, uint64_t bytes = 1) { return std::make_unique<Target>(Target{size, bytes}); }

// One resize: park the active targets, then look the next size up.
struct Simulation
{
    Map parked;
    std::unordered_map<int, Handle> active;
    uint64_t generation = 0;
    unsigned reused = 0, made = 0, evicted = 0;
    uint64_t budget = ~0ull;

    void Resize()
    {
        ++generation;
        for (auto& [key, target] : active) {
            auto& pair = parked[key];
            const int size = target->size;
            const size_t slot = ChooseParkSlot(pair, [&](const Handle& p) { return p->size == size; });
            pair[slot] = {std::move(target), generation};
        }
        active.clear();
        evicted += unsigned(EvictToBudget(parked, budget, [](const Handle& t) { return t->bytes; }).size());
    }
    void Use(int key, int size, uint64_t bytes = 1)
    {
        auto found = parked.find(key);
        const size_t slot = found == parked.end() ? 2 :
            FindReusableSlot(found->second, [&](const Handle& t) { return t->size == size; });
        if (slot < 2) {
            active[key] = std::move(found->second[slot].target);
            found->second[slot] = {};
            if (!found->second[0].Used() && !found->second[1].Used()) parked.erase(found);
            ++reused;
            return;
        }
        active[key] = Make(size, bytes);
        ++made;
    }
};
}

int main()
{
    try {
        // Quality 800 -> output 1200 -> quality -> output: from the second
        // switch on every target is reused, in both directions.
        Simulation sim;
        constexpr int keys = 20;
        for (int k = 0; k < keys; ++k) sim.Use(k, 800);
        Check(sim.made == keys && sim.reused == 0, "first frame allocates");
        sim.Resize();
        for (int k = 0; k < keys; ++k) sim.Use(k, 1200);
        Check(sim.made == 2 * keys && sim.reused == 0, "first switch to the output size allocates");
        Check(Count(sim.parked) == keys, "the quality targets stay parked through output-size lookups");
        sim.Resize();
        sim.reused = sim.made = 0;
        for (int k = 0; k < keys; ++k) sim.Use(k, 800);
        Check(sim.reused == keys && sim.made == 0, "return to quality reuses every target");
        sim.Resize();
        sim.reused = sim.made = 0;
        for (int k = 0; k < keys; ++k) sim.Use(k, 1200);
        Check(sim.reused == keys && sim.made == 0, "return to output reuses every target");
        Check(Count(sim.parked) == keys, "one size parked per key while the other is active");

        // A third size replaces the older of the two parked sizes.
        sim.Resize();
        for (int k = 0; k < keys; ++k) sim.Use(k, 576);
        sim.Resize();
        sim.reused = sim.made = 0;
        for (int k = 0; k < keys; ++k) sim.Use(k, 1200);
        Check(sim.reused == keys, "the most recent other size survives a third size");
        for (const auto& [key, pair] : sim.parked)
            for (const auto& slot : pair)
                Check(!slot.Used() || slot.target->size != 800, "the oldest size was evicted for the third");

        // Re-parking the same size replaces its older allocation.
        Pair<Handle> pair{};
        pair[0] = {Make(800), 1};
        Check(ChooseParkSlot(pair, [](const Handle& t) { return t->size == 800; }) == 0, "same size replaces");
        Check(ChooseParkSlot(pair, [](const Handle& t) { return t->size == 1200; }) == 1, "other size takes the free slot");
        pair[1] = {Make(1200), 2};
        Check(ChooseParkSlot(pair, [](const Handle& t) { return t->size == 576; }) == 0, "full pair evicts the older");
        Check(FindReusableSlot(pair, [](const Handle& t) { return t->size == 576; }) == 2 &&
            pair[0].Used() && pair[1].Used(), "a mismatch leaves both parked");

        // Budget: oldest generations go first, newest stay.
        Simulation tight;
        tight.budget = 30;
        for (int k = 0; k < 3; ++k) tight.Use(k, 800, 10);
        tight.Resize();
        for (int k = 0; k < 3; ++k) tight.Use(k, 1200, 20);
        tight.Resize();
        Check(TotalBytes(tight.parked, [](const Handle& t) { return t->bytes; }) <= 30, "budget respected");
        Check(tight.evicted == 5 && Count(tight.parked) == 1, "older quality targets evicted first, then output targets until it fits");
        for (const auto& [key, p] : tight.parked)
            for (const auto& slot : p)
                Check(!slot.Used() || slot.target->size == 1200, "survivors are the newest generation");

        std::printf("PASS: %u render target parking checks\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }
}
