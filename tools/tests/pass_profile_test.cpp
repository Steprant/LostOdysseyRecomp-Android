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
    scene.barrierEndLayouts[1][3][2] = 6; // other texture: color_write > shader_read
    scene.copiesDeferred = 5; scene.deferredFlushedEarly = 2;
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
    assert(line.find("barrier_ends_per_frame: other:color_write>shader_read=6.0;") != std::string::npos);
    assert(line.find("deferred_copies_per_frame: deferred=5.0 run_early=2.0 early_draw_over=0.0") != std::string::npos);
    assert(line.find("state_per_frame: pipeline_binds=40.0 pipeline_skipped=10.0 descriptor_binds=80.0") != std::string::npos);

    Reporter reporter;
    const auto t0 = std::chrono::steady_clock::time_point(std::chrono::seconds(100));
    assert(reporter.Add(scene, false, t0).empty());
    // One frame a second: under the 2 s gap that starts a new window (2.1.1).
    for (int second = 1; second <= 9; ++second) assert(reporter.Add(present, true, t0 + std::chrono::seconds(second)).empty());
    {
        const std::string window = reporter.Add(present, true, t0 + std::chrono::seconds(10));
        assert(!window.empty());
        // Two lines: gpu passes, then the frame graph what-if (WP1 stage 1a).
        const auto newline = window.find('\n');
        assert(newline != std::string::npos && window.rfind("gpu passes:", 0) == 0);
        assert(window.compare(newline + 1, 21, "frame graph what-if: ") == 0);
    }
    assert(reporter.Add(present, true, t0 + std::chrono::seconds(12)).empty()); // new window
    {
        // What-if arithmetic: 2 MB dead stores, one resumed pass that
        // reloaded 1 MB (merge_mb counts its store too), per frame.
        gpu::pass_profile::Summary whatIf;
        plume::RenderPassProfile frame;
        frame.flow.storeCleared[0] = 1u << 20;
        frame.flow.storeCleared[1] = 1u << 20;
        frame.whatIfMergeablePasses = 1;
        frame.whatIfMergeBytes = 1u << 20;
        frame.diagDiscardedStores = 3;
        frame.diagDiscardedStoreBytes = 3u << 20;
        whatIf.Add(frame, true);
        const std::string line = whatIf.FormatWhatIf();
        std::printf("%s\n", line.c_str());
        assert(line.find("dead_store_mb=2.0 merge_passes=1.0 merge_mb=2.0 savable_mb=4.0") != std::string::npos);
        assert(line.find("diag_discarded_stores=3.0 diag_discarded_mb=3.0") != std::string::npos);
    }
    {
        // Steprant 2.1.1-C: the replay breakdown, per frame, and its line in
        // the reporting window only when the replays were timed.
        using Kind = plume::RenderPassProfile::ReplayKind;
        gpu::pass_profile::Summary replay;
        plume::RenderPassProfile frame;
        frame.replayAnalyzeNs = 200'000;
        frame.replayNs[size_t(Kind::Draw)] = 1'500'000; frame.replayCmds[size_t(Kind::Draw)] = 450;
        frame.replayNs[size_t(Kind::Push)] = 300'000; frame.replayCmds[size_t(Kind::Push)] = 480;
        frame.replayNs[size_t(Kind::Other)] = 0; frame.replayCmds[size_t(Kind::Other)] = 0;
        frame.replayDriverDrawNs = 900'000; frame.replayPassBeginNs = 400'000; frame.replayPassBegins = 66;
        frame.replayPassEndNs = 250'000; frame.replayPassEnds = 66;
        frame.replayRepeatedSetBinds = 4000;
        frame.frameGraphFilteredSetBinds = 3600; frame.frameGraphFilteredLayoutBinds = 450;
        frame.frameGraphFilteredDrawState = 975;
        replay.Add(frame, true);
        replay.Add(frame, true);
        assert(replay.HasReplayTiming());
        const std::string line = replay.FormatReplay();
        std::printf("%s\n", line.c_str());
        assert(line.rfind("frame graph replay: frames=2 per_frame_ms: total=2.00 analyze=0.20 |", 0) == 0);
        assert(line.find(" draw=1.50/450 pipeline=0.00/0 sets=0.00/0 push=0.30/480 ") != std::string::npos);
        assert(line.find(" other=0.00/0 | driver_ms: draws=0.90 pass_begin=0.40/66.0 pass_end=0.25/66.0 | repeated_set_binds=4000") != std::string::npos);
        // Steprant 2.4.2: the binds the capture left out, per frame.
        assert(line.find(" repeated_set_binds=4000 filtered_set_binds=3600 filtered_layout_binds=450 filtered_draw_state=975") != std::string::npos);
        // Steprant 2.1.5: no parallel part without replays recorded in parallel.
        assert(line.find("parallel:") == std::string::npos);
        assert(!gpu::pass_profile::Summary().HasReplayTiming());
        {
            gpu::pass_profile::Summary parallel;
            plume::RenderPassProfile recorded = frame;
            recorded.parallelReplays = 1; recorded.parallelChunks = 6; recorded.parallelOwnChunks = 1;
            recorded.parallelRecordNs = 2'400'000; recorded.parallelOwnNs = 300'000; recorded.parallelWaitNs = 100'000;
            parallel.Add(recorded, true);
            parallel.Add(recorded, true);
            const std::string parallelLine = parallel.FormatReplay();
            std::printf("%s\n", parallelLine.c_str());
            assert(parallelLine.find(" | repeated_set_binds=4000 filtered_set_binds=3600 filtered_layout_binds=450 filtered_draw_state=975 | parallel: replays=1.0 chunks=6.0 helpers_ms=2.40 own=1.0/0.30 wait_ms=0.10") != std::string::npos);
        }

        Reporter timed, untimed;
        const auto t1 = std::chrono::steady_clock::time_point(std::chrono::seconds(500));
        for (int second = 0; second <= 9; ++second) {
            assert(timed.Add(frame, true, t1 + std::chrono::seconds(second)).empty());
            assert(untimed.Add(present, true, t1 + std::chrono::seconds(second)).empty());
        }
        const std::string window = timed.Add(frame, true, t1 + std::chrono::seconds(10));
        assert(window.find("\nframe graph what-if: ") != std::string::npos);
        assert(window.find("\nframe graph replay: frames=11 ") != std::string::npos);
        // Untimed windows keep their two lines.
        const std::string plain = untimed.Add(present, true, t1 + std::chrono::seconds(10));
        assert(!plain.empty() && plain.find("frame graph replay") == std::string::npos);
        // A gap over 2 s drops the open window: the next line starts after it.
        assert(timed.Add(frame, true, t1 + std::chrono::seconds(14)).empty());
    }
    std::printf("PASS pass profile summary and reporting window\n");
}
