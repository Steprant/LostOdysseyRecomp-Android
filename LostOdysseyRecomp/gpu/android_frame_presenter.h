#pragma once

#include <cassert>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace gpu::android_fg {

// One submission and one result may be outstanding. The producer owns the
// underlying swapchain, fence and semaphores until Take() returns. There is
// no unbounded presentation queue and neither thread polls the other.
template<class Job, class Result>
class Presenter {
    using Execute = Result (*)(const Job&);
public:
    explicit Presenter(Execute execute) : worker([this, execute] {
        std::unique_lock lock(mutex);
        for (;;) {
            ready.wait(lock, [&] { return stopping || request.has_value(); });
            if (!request) return;
            auto job = std::move(*request);
            request.reset();
            lock.unlock();
            auto result = execute(job);
            lock.lock();
            response = std::move(result);
            ready.notify_all();
        }
    }) {}

    ~Presenter()
    {
        {
            const std::lock_guard lock(mutex);
            stopping = true;
        }
        ready.notify_all();
        worker.join();
    }

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    bool Submit(Job job)
    {
        const std::lock_guard lock(mutex);
        if (occupied) return false;
        occupied = true;
        request = std::move(job);
        ready.notify_all();
        return true;
    }

    Result Take()
    {
        std::unique_lock lock(mutex);
        assert(occupied);
        ready.wait(lock, [&] { return response.has_value(); });
        auto result = std::move(*response);
        response.reset();
        occupied = false;
        return result;
    }

private:
    std::mutex mutex;
    std::condition_variable ready;
    std::optional<Job> request;
    std::optional<Result> response;
    bool occupied = false, stopping = false;
    std::thread worker;
};

} // namespace gpu::android_fg
