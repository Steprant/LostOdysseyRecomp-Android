#include <gpu/pass_profile.h>
#include <cassert>
#include <cstdio>

int main()
{
    using gpu::pass_profile::Reporter;
    using gpu::pass_profile::Summary;
    plume::RenderPassProfile scene;
    scene.timed = true;
    scene.listNs = 20'000'000;
    scene.barrierCalls = 6; scene.copyCalls = 2; scene.dispatches = 1;
    plume::RenderPassProfile::Pass big{2403, 1080, 1, true, false, 0, 100, 20'000'000, 20'000'000, 12'000'000};
    plume::RenderPassProfile::Pass shadow{1024, 1024, 0, true, false, 1u << 30, 50, 0, 4'000'000, 3'000'000};
    plume::RenderPassProfile::Pass empty{2403, 1080, 1, true, false, 0, 0, 20'000'000, 20'000'000, 1'000'000};
    scene.passes = {big, shadow, empty};
    plume::RenderPassProfile present;
    present.timed = true; present.listNs = 2'000'000;
    present.passes = {{2670, 1200, 1, false, false, 0, 1, 0, 2670ull * 1200 * 4, 1'500'000}};

    // Content census: 1 MB of depth stored and cleared unread per scene list.
    scene.flow.storeCleared[1] = 1u << 20;
    scene.flow.storeRead[0] = 3u << 20;
    scene.flow.loadUndefined[0] = 1u << 19;
    plume::RenderPassProfile::FlowEvent cleared;
    cleared.width = 1024; cleared.height = 1024; cleared.passDepth = true; cleared.depth = true;
    cleared.bucket = plume::RenderPassProfile::FlowBucket::Cleared; cleared.bytes = 1u << 20;
    scene.flowEvents = {cleared};
    scene.passEnds[size_t(plume::RenderPassProfile::PassEnd::BarrierOther)] = 3;
    scene.pipelineBinds = 40; scene.pipelineBindsSkipped = 10; scene.descriptorBinds = 80;

    Summary summary;
    for (int frame = 0; frame < 2; ++frame) { summary.Add(scene, false); summary.Add(present, true); }
    assert(summary.frames == 2 && summary.lists == 4 && summary.untimedLists == 0);
    const auto& stats = summary.targets.at({2403, 1080, 1, true});
    assert(stats.passes == 4 && stats.emptyPasses == 2 && stats.draws == 200 && stats.gpuNs == 26'000'000);
    assert(summary.targets.at({1024, 1024, 0, true}).clearStarts == 2);
    // Outside time: list minus its passes (20 - 16 ms) plus present (2 - 1.5 ms), per frame.
    assert(summary.listNs - summary.passNs == 2 * 4'000'000ull + 2 * 500'000ull);
    const std::string line = summary.Format();
    std::printf("%s\n", line.c_str());
    assert(line.find("passes=3.0") != std::string::npos);        // per frame
    assert(line.find("outside_ms=4.50") != std::string::npos);
    assert(line.find("2403x1080 cd passes=2.0 empty=1.0") != std::string::npos);
    assert(line.find("present: passes=1.0 ms=1.50") != std::string::npos);
    assert(line.find("color store_read=3.0 store_loaded=0.0 store_cleared=0.0 load_valid=0.0 load_undefined=0.5;") != std::string::npos);
    assert(line.find("depth store_read=0.0 store_loaded=0.0 store_cleared=1.0") != std::string::npos);
    assert(line.find("stored_then_cleared: 1024x1024 -d depth=1.0;") != std::string::npos);
    assert(line.find("pass_ends_per_frame: barrier_other=3.0;") != std::string::npos);
    assert(line.find("state_per_frame: pipeline_binds=40.0 pipeline_skipped=10.0 descriptor_binds=80.0") != std::string::npos);

    Reporter reporter;
    const auto t0 = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
    assert(reporter.Add(scene, false, t0).empty());
    assert(reporter.Add(present, true, t0 + std::chrono::seconds(5)).empty());
    assert(!reporter.Add(present, true, t0 + std::chrono::seconds(11)).empty());
    assert(reporter.Add(present, true, t0 + std::chrono::seconds(12)).empty()); // new window
    std::printf("PASS pass profile summary and reporting window\n");
}
