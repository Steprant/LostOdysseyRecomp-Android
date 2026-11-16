#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace kernel::wait {
inline constexpr uint32_t Success = 0, Timeout = 0x102, Invalid = 0xC000000D;
inline constexpr uint32_t Infinite = 0xFFFFFFFF;
// Steprant 2.1.1-D: a thread inside Multiple registers on each of its objects
// and sleeps on its own condition variable; a change of one object wakes the
// threads waiting on that object, and only them. Until 2.1.1-C every change of
// any object woke every thread inside Multiple (one shared domain): in the
// 15/11 session six game threads woke about 3,000 times a second each, for
// about 500 waits a second, re-checking objects nobody had touched.
struct MultiWaiter {
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t signals = 0; // guarded by mutex
};
// Wakeups of threads inside Multiple and their waits, for the Tester.
inline std::atomic<uint64_t> g_multipleWaits{0}, g_multipleWakes{0};
class Target {
public:
    std::mutex mutex;
    std::condition_variable cv;
    virtual ~Target() = default;
    // Caller owns mutex. Inspecting readiness never consumes state.
    virtual bool Ready(uint32_t owner) const = 0;
    virtual void Consume(uint32_t owner) = 0;
    // After a change made under mutex: wake single waiters and the Multiple
    // waiters registered here. A waiter registers under mutex before it
    // checks readiness; a signaller changes state under mutex before it reads
    // the count, so a zero count means no registered waiter can miss it.
    void Notify() {
        cv.notify_all();
        if (!multiWaiterCount.load()) return;
        std::lock_guard lock(mutex);
        for (MultiWaiter* waiter : multiWaiters) {
            { std::lock_guard wake(waiter->mutex); ++waiter->signals; }
            waiter->cv.notify_one();
        }
    }
    uint32_t Wait(uint32_t timeout, uint32_t owner = 0) {
        std::unique_lock lock(mutex);
        const auto ready = [&] { return Ready(owner); };
        if (timeout == Infinite) cv.wait(lock, ready);
        else if (!cv.wait_for(lock, std::chrono::milliseconds(timeout), ready)) return Timeout;
        Consume(owner);
        return Success;
    }
    // Steprant 2.1.5: a wait of at most us microseconds (kernel/poll_backoff.h).
    uint32_t WaitFor(uint32_t us, uint32_t owner = 0) {
        std::unique_lock lock(mutex);
        if (!cv.wait_for(lock, std::chrono::microseconds(us), [&] { return Ready(owner); })) return Timeout;
        Consume(owner);
        return Success;
    }
    // Caller owns mutex.
    void Register(MultiWaiter* waiter) { multiWaiters.push_back(waiter); multiWaiterCount.fetch_add(1); }
    void Unregister(MultiWaiter* waiter) {
        std::lock_guard lock(mutex);
        auto it = std::find(multiWaiters.begin(), multiWaiters.end(), waiter);
        if (it == multiWaiters.end()) return;
        *it = multiWaiters.back();
        multiWaiters.pop_back();
        multiWaiterCount.fetch_sub(1);
    }
    size_t MultiWaiterCount() const { return multiWaiterCount.load(); }
private:
    std::vector<MultiWaiter*> multiWaiters; // guarded by mutex
    std::atomic<uint32_t> multiWaiterCount{0};
};
class Event final : public Target {
    bool manual, signaled;
public:
    Event(bool manualReset, bool initial) : manual(manualReset), signaled(initial) {}
    bool Ready(uint32_t) const override { return signaled; }
    void Consume(uint32_t) override { if (!manual) signaled = false; }
    bool Set() {
        bool previous;
        { std::lock_guard lock(mutex); previous = signaled; signaled = true; }
        Notify();
        return previous;
    }
    bool Reset() { std::lock_guard lock(mutex); bool previous = signaled; signaled = false; return previous; }
    bool IsSignaled() { std::lock_guard lock(mutex); return signaled; }
};
class Semaphore final : public Target {
    uint32_t count, maximum;
public:
    Semaphore(uint32_t initial, uint32_t limit) : count(initial), maximum(limit) {}
    void Reset(uint32_t initial, uint32_t limit) {
        {
            std::lock_guard lock(mutex);
            count = initial;
            maximum = limit;
        }
        Notify();
    }
    bool Ready(uint32_t) const override { return count != 0; }
    void Consume(uint32_t) override { --count; }
    bool Release(uint32_t amount, uint32_t* previous) {
        {
            std::lock_guard lock(mutex);
            if (previous) *previous = count;
            if (!amount || amount > maximum - count) return false;
            count += amount;
        }
        Notify(); return true;
    }
};
class Mutant final : public Target {
    uint32_t owner = 0, recursion = 0;
public:
    explicit Mutant(uint32_t initialOwner = 0) : owner(initialOwner), recursion(initialOwner ? 1 : 0) {}
    bool Ready(uint32_t self) const override { return !owner || owner == self; }
    void Consume(uint32_t self) override { owner = self; ++recursion; }
    bool Release(uint32_t self, uint32_t* previous = nullptr) {
        bool released;
        {
            std::lock_guard lock(mutex);
            if (owner != self || !recursion) return false;
            if (previous) *previous = recursion;
            released = --recursion == 0;
            if (released) owner = 0;
        }
        if (released) Notify();
        return true;
    }
};
inline uint32_t Multiple(std::span<Target* const> objects, bool all,
    uint32_t timeout, uint32_t owner = 0)
{
    if (objects.empty() || objects.size() > 64) return Invalid;
    std::array<Target*, 64> ordered{};
    for (size_t i = 0; i < objects.size(); ++i) {
        if (!objects[i]) return Invalid;
        ordered[i] = objects[i];
    }
    auto end = ordered.begin() + objects.size();
    std::sort(ordered.begin(), end, std::less<Target*>{});
    const auto uniqueEnd = std::unique(ordered.begin(), end);
    if (all && uniqueEnd != end) return Invalid;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    // Declared before the loop: on every return the object locks of the
    // loop body are released first, then the registration is undone.
    MultiWaiter self;
    struct Registration {
        Target* const* first;
        Target* const* last;
        bool active = false;
        ~Registration() { if (active) for (auto it = first; it != last; ++it) (*it)->Unregister(selfWaiter); }
        MultiWaiter* selfWaiter;
    } registration{ordered.data(), ordered.data() + (uniqueEnd - ordered.begin()), false, &self};
    for (;;) {
        uint64_t observed;
        {
            std::array<std::unique_lock<std::mutex>, 64> locks;
            for (auto it = ordered.begin(); it != uniqueEnd; ++it)
                locks[it - ordered.begin()] = std::unique_lock((*it)->mutex);
            if (all) {
                if (std::all_of(objects.begin(), objects.end(), [&](Target* p) { return p->Ready(owner); })) {
                    for (auto* p : objects) p->Consume(owner);
                    return Success;
                }
            } else {
                for (size_t i = 0; i < objects.size(); ++i)
                    if (objects[i]->Ready(owner)) { objects[i]->Consume(owner); return uint32_t(i); }
            }
            if (!timeout || (timeout != Infinite && std::chrono::steady_clock::now() >= deadline)) return Timeout;
            // Register under the object locks, before they are released: a
            // change after this point notifies this waiter.
            if (!registration.active) {
                for (auto it = ordered.begin(); it != uniqueEnd; ++it) (*it)->Register(&self);
                registration.active = true;
                g_multipleWaits.fetch_add(1, std::memory_order_relaxed);
            }
            std::lock_guard wake(self.mutex);
            observed = self.signals;
        }
        std::unique_lock lock(self.mutex);
        const auto changed = [&] { return self.signals != observed; };
        if (timeout == Infinite) self.cv.wait(lock, changed);
        else if (!self.cv.wait_until(lock, deadline, changed)) return Timeout;
        g_multipleWakes.fetch_add(1, std::memory_order_relaxed);
    }
}
}
