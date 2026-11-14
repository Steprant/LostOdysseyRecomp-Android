#include <diagnostics/android_tester.h>
#include <gpu/gpu_busy.h>
#include <cassert>
#include <memory>
#include <thread>

using namespace diagnostics::android_tester;
int main() {
    uint64_t previousGpuEnd = 0;
    const uint64_t gpuBefore = gpu::gpu_busy::Total();
    assert(gpu::gpu_busy::AddBatch(10, 30, previousGpuEnd));
    assert(!gpu::gpu_busy::AddBatch(10, 30, previousGpuEnd));
    assert(!gpu::gpu_busy::AddBatch(0, 50, previousGpuEnd));
    assert(gpu::gpu_busy::AddBatch(31, 40, previousGpuEnd));
    assert(gpu::gpu_busy::Total() - gpuBefore == 29);
    auto recorder = std::make_unique<Recorder>();
    auto& r = *recorder;
    assert(!r.enabled && !r.recording);
    assert(!r.Start(1, 30));
    r.enabled = true;
    assert(r.Start(1, 30));
    r.SetRenderer(7, 123); r.SetAudio(4, 2);
    r.Add(100000000, 1000, 500, true, 10000, 1);
    r.Add(133333333, 1500, 600, true, 20000, 2);
    r.Add(233333333, 2000, 700, true, 30000, 3);
    auto frames = r.Drain();
    assert(frames.size() == 3 && frames[0].intervalNs == 0);
    assert(frames[1].intervalNs == 33333333 && !frames[1].spike);
    assert(frames[2].intervalNs == 100000000 && frames[2].spike);
    assert(frames[2].draws == 123 && frames[2].audioStarved == 4 && frames[2].gpuBatches == 3);
    r.Stop(300000000);
    assert(!r.recording && !r.Start(400000000, 30)); // collector still finishing
    r.collectorIdle = true;
    r.profile = uint32_t(Profile::Intensive);
    assert(r.Start(1, 60));
    // Steprant 2.1.1: heavy profiling only in the last 12 s of each minute;
    // the light measurements for the whole recording.
    assert(!r.CollectGpu() && r.CollectLight());
    r.Add(1 + 30'000'000'000ull, 0, 0, true, 0, 0);
    assert(!r.CollectGpu());
    r.Add(1 + 48'000'000'000ull, 0, 0, true, 0, 0);
    assert(r.CollectGpu());
    r.Add(1 + 59'999'999'999ull, 0, 0, true, 0, 0);
    assert(r.CollectGpu());
    r.Add(1 + 60'000'000'000ull, 0, 0, true, 0, 0);
    assert(!r.CollectGpu());
    {
        auto burstFrames = r.Drain();
        assert(burstFrames.size() == 4);
        // Each frame carries the state it was produced under.
        assert(!burstFrames[0].profiling && !burstFrames[1].profiling && burstFrames[2].profiling && burstFrames[3].profiling);
    }
    assert(ProfilingBurst(48'000'000'000ull) && !ProfilingBurst(47'999'999'999ull) && ProfilingBurst(108'000'000'000ull));
    constexpr uint64_t longSession = 3ull * 60 * 60 * 1000000000ull;
    r.Add(longSession, 1, 1, true, 0, 0);
    assert(r.recording && r.Duration(longSession + 100) == longSession + 99);
    r.Stop(longSession + 100);
    assert(!r.recording && r.Duration(longSession + 200) == longSession + 99);
    assert(!r.CollectGpu() && !r.CollectLight());
    r.collectorIdle = true; r.profile = 0; assert(r.Start(1, 30));
    for (size_t i=0; i<Recorder::Capacity+5; ++i) r.Add(1000+i, 0, 0, true, 0, 0);
    assert(r.Dropped() == 5 && r.Drain().size() == Recorder::Capacity);
    r.Stop(1000000); r.collectorIdle = true; assert(r.Start(1, 30));
    std::thread producer([&] { for (uint64_t i=0; i<50000; ++i) r.Add(i+100, 0, 0, true, 0, 0); });
    for (int i=0; i<100; ++i) r.Drain();
    producer.join();
    r.Stop(1000000);
    assert(r.Drain().size() <= Recorder::Capacity);
}
