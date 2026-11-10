#pragma once
#include "cmdproc_profile.h"
#include <plume_render_interface.h>
#include "draw_attachment_policy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>
#if defined(__ANDROID__)
#include <diagnostics/android_tester.h>
#endif

// Per-render-pass GPU cost (plume RenderPassProfile), summarised every ten
// seconds in the runtime log while the Android Tester records with the
// Advanced or Intensive profile, or with LO_PASS_PROFILE=1. Each command list
// is read when it is next begun: by then its fence has completed, so no GPU
// wait is added. Timestamps bracket passes outside them: on a tile-based GPU a
// pass's time includes loading and storing its attachments. Load/store bytes
// follow the chosen Vulkan load/store ops; they are not measured bandwidth.
namespace gpu::pass_profile
{
// Enabled() lives in cmdproc_profile.h (no plume dependency).

// Guest draws by Xenos colour render-target format (RB_COLOR_INFO format,
// 0-15) and depth-only draws, counted by the renderer while profiling: which
// EDRAM formats the frame's bandwidth belongs to.
inline std::atomic<uint64_t> g_colorFormatDraws[16];
inline std::atomic<uint64_t> g_depthOnlyDraws{0};
inline void CountDraw(uint32_t xenosColorFormat, bool depthOnly)
{
    if (depthOnly) g_depthOnlyDraws.fetch_add(1, std::memory_order_relaxed);
    else g_colorFormatDraws[xenosColorFormat & 15].fetch_add(1, std::memory_order_relaxed);
}
inline std::string TakeFormatDraws(double frames)
{
    static constexpr const char* names[16] = {"8888", "8888_gamma", "2101010", "7e3", "16_16", "16_16_16_16",
        "16_16f", "16_16_16_16f", "f8", "f9", "2101010_as_16", "f11", "7e3_as_16", "f13", "32f", "32_32f"};
    std::string line = " format_draws_per_frame:";
    char buffer[64];
    for (uint32_t i = 0; i < 16; ++i)
        if (const uint64_t n = g_colorFormatDraws[i].exchange(0, std::memory_order_relaxed)) {
            std::snprintf(buffer, sizeof(buffer), " %s=%.1f", names[i], double(n) / frames);
            line += buffer;
        }
    std::snprintf(buffer, sizeof(buffer), " depth_only=%.1f depth_only_keeps_color=%d",
        double(g_depthOnlyDraws.exchange(0, std::memory_order_relaxed)) / frames,
        ::gpu::draw_attachment::KeepColorForDepthOnly() ? 1 : 0);
    return line + buffer;
}

struct TargetKey
{
    uint32_t width = 0, height = 0;
    uint8_t colors = 0;
    bool depth = false;
    bool operator<(const TargetKey& other) const
    {
        return std::tie(width, height, colors, depth) < std::tie(other.width, other.height, other.colors, other.depth);
    }
};

struct TargetStats
{
    uint64_t passes = 0, emptyPasses = 0, clearStarts = 0, draws = 0, gpuNs = 0, loadBytes = 0, storeBytes = 0;
};

class Summary
{
public:
    std::map<TargetKey, TargetStats> targets;
    TargetStats present;
    uint64_t frames = 0, lists = 0, untimedLists = 0;
    uint64_t listNs = 0, passNs = 0;
    uint64_t barriers = 0, copies = 0, dispatches = 0, drawsOutside = 0;
    uint64_t pipelineBinds = 0, pipelineBindsSkipped = 0, descriptorBinds = 0, pushConstants = 0, vertexBinds = 0, indexBinds = 0;
    uint64_t copiesDeferred = 0, deferredFlushedEarly = 0, deferredEarlyReasons[5] = {};
    uint64_t textureCopiesDeferred = 0, textureCopyHazards = 0; // SR4 G-2
    uint64_t frameGraphCommands = 0, frameGraphReplays = 0, frameGraphDeadStores = 0, frameGraphDeadStoreBytes = 0; // SR4 G-2 1b/1c
    uint64_t whatIfMergeablePasses = 0, whatIfMergeBytes = 0, diagDiscardedStores = 0, diagDiscardedStoreBytes = 0;
    plume::RenderPassProfile::ContentFlow flow;
    uint64_t passEnds[size_t(plume::RenderPassProfile::PassEnd::Count)] = {};
    uint64_t barrierEndLayouts[2][plume::RenderPassProfile::LayoutCount][plume::RenderPassProfile::LayoutCount] = {};
    // Stores loaded by the next pass ([0]) or cleared unread ([1]), by the
    // pass that stored them and the attachment kind (colour, depth).
    std::map<std::pair<TargetKey, bool>, uint64_t> flowLoaded, flowCleared;

    void Add(const plume::RenderPassProfile& profile, bool presentation)
    {
        ++lists;
        if (presentation) ++frames;
        if (!profile.timed) ++untimedLists;
        barriers += profile.barrierCalls;
        copies += profile.copyCalls;
        dispatches += profile.dispatches;
        drawsOutside += profile.drawsOutsidePasses;
        pipelineBinds += profile.pipelineBinds;
        pipelineBindsSkipped += profile.pipelineBindsSkipped;
        descriptorBinds += profile.descriptorBinds;
        pushConstants += profile.pushConstants;
        vertexBinds += profile.vertexBinds;
        indexBinds += profile.indexBinds;
        copiesDeferred += profile.copiesDeferred;
        deferredFlushedEarly += profile.deferredFlushedEarly;
        textureCopiesDeferred += profile.textureCopiesDeferred;
        frameGraphCommands += profile.frameGraphCommands;
        frameGraphReplays += profile.frameGraphReplays;
        frameGraphDeadStores += profile.frameGraphDeadStores;
        frameGraphDeadStoreBytes += profile.frameGraphDeadStoreBytes;
        textureCopyHazards += profile.textureCopyHazards;
        for (int i = 0; i < 5; ++i) deferredEarlyReasons[i] += profile.deferredEarlyReasons[i];
        whatIfMergeablePasses += profile.whatIfMergeablePasses;
        whatIfMergeBytes += profile.whatIfMergeBytes;
        diagDiscardedStores += profile.diagDiscardedStores;
        diagDiscardedStoreBytes += profile.diagDiscardedStoreBytes;
        for (int k = 0; k < 2; ++k)
        {
            flow.storeRead[k] += profile.flow.storeRead[k];
            flow.storeLoaded[k] += profile.flow.storeLoaded[k];
            flow.storeCleared[k] += profile.flow.storeCleared[k];
            flow.loadValid[k] += profile.flow.loadValid[k];
            flow.loadUndefined[k] += profile.flow.loadUndefined[k];
        }
        for (size_t i = 0; i < std::size(passEnds); ++i) passEnds[i] += profile.passEnds[i];
        for (uint32_t k = 0; k < 2; ++k)
            for (uint32_t a = 0; a < plume::RenderPassProfile::LayoutCount; ++a)
                for (uint32_t b = 0; b < plume::RenderPassProfile::LayoutCount; ++b)
                    barrierEndLayouts[k][a][b] += profile.barrierEndLayouts[k][a][b];
        for (const auto& event : profile.flowEvents)
        {
            const auto key = std::make_pair(TargetKey{event.width, event.height, event.colors, event.passDepth}, event.depth);
            if (event.bucket == plume::RenderPassProfile::FlowBucket::Loaded) flowLoaded[key] += event.bytes;
            else if (event.bucket == plume::RenderPassProfile::FlowBucket::Cleared) flowCleared[key] += event.bytes;
        }
        uint64_t inPasses = 0;
        for (const auto& pass : profile.passes)
        {
            TargetStats& stats = presentation ? present
                : targets[TargetKey{pass.width, pass.height, pass.colors, pass.depth}];
            ++stats.passes;
            stats.emptyPasses += pass.draws == 0;
            stats.clearStarts += pass.clearMask != 0;
            stats.draws += pass.draws;
            stats.gpuNs += pass.gpuNs;
            stats.loadBytes += pass.loadBytes;
            stats.storeBytes += pass.storeBytes;
            inPasses += pass.gpuNs;
        }
        if (profile.timed)
        {
            listNs += profile.listNs;
            passNs += std::min(inPasses, profile.listNs);
        }
    }

    // Roadmap 1.1: where the stored attachment bytes went (MB per frame).
    // store_cleared is a store nothing read before the next pass cleared or
    // discarded it; load_undefined loads a texture nothing had written.
    std::string FormatFlow(double f) const
    {
        auto mb = [&](uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0) / f; };
        std::string line = " content_mb:";
        char buffer[256];
        static constexpr const char* kinds[2] = {"color", "depth"};
        for (int k = 0; k < 2; ++k)
        {
            std::snprintf(buffer, sizeof(buffer), " %s store_read=%.1f store_loaded=%.1f store_cleared=%.1f load_valid=%.1f load_undefined=%.1f;",
                kinds[k], mb(flow.storeRead[k]), mb(flow.storeLoaded[k]), mb(flow.storeCleared[k]),
                mb(flow.loadValid[k]), mb(flow.loadUndefined[k]));
            line += buffer;
        }
        auto top = [&](const char* name, const std::map<std::pair<TargetKey, bool>, uint64_t>& map) {
            std::vector<std::pair<std::pair<TargetKey, bool>, uint64_t>> sorted(map.begin(), map.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            line += std::string(" ") + name + ":";
            for (size_t i = 0; i < sorted.size() && i < 6; ++i)
            {
                const auto& [key, bytesTotal] = sorted[i];
                std::snprintf(buffer, sizeof(buffer), " %ux%u%s%s %s=%.1f;", key.first.width, key.first.height,
                    key.first.colors ? (key.first.colors > 1 ? " mrt" : " c") : " -", key.first.depth ? "d" : "",
                    key.second ? "depth" : "color", mb(bytesTotal));
                line += buffer;
            }
        };
        top("stored_then_cleared", flowCleared);
        top("stored_then_loaded", flowLoaded);
        static constexpr const char* endNames[] = {"framebuffer", "barrier_attachment", "barrier_other", "barrier_buffer",
            "copy", "resolve", "dispatch", "external", "query_reset", "list_end", "other", "render_area"};
        static_assert(std::size(endNames) == size_t(plume::RenderPassProfile::PassEnd::Count));
        line += " pass_ends_per_frame:";
        for (size_t i = 0; i < std::size(endNames); ++i)
        {
            if (!passEnds[i]) continue;
            std::snprintf(buffer, sizeof(buffer), " %s=%.1f", endNames[i], double(passEnds[i]) / f);
            line += buffer;
        }
        line += ";";
        // The transitions behind barrier_attachment ("own") and barrier_other.
        {
            static constexpr const char* layoutNames[] = {"unknown", "general", "shader_read", "color_write", "depth_write",
                "depth_read", "copy_source", "copy_dest", "resolve_source", "resolve_dest", "present"};
            static_assert(std::size(layoutNames) == plume::RenderPassProfile::LayoutCount);
            std::vector<std::tuple<uint64_t, uint32_t, uint32_t, uint32_t>> transitions;
            for (uint32_t k = 0; k < 2; ++k)
                for (uint32_t a = 0; a < plume::RenderPassProfile::LayoutCount; ++a)
                    for (uint32_t b = 0; b < plume::RenderPassProfile::LayoutCount; ++b)
                        if (barrierEndLayouts[k][a][b]) transitions.emplace_back(barrierEndLayouts[k][a][b], k, a, b);
            std::sort(transitions.begin(), transitions.end(), [](const auto& x, const auto& y) { return std::get<0>(x) > std::get<0>(y); });
            line += " barrier_ends_per_frame:";
            for (size_t i = 0; i < transitions.size() && i < 8; ++i)
            {
                const auto& [count, k, a, b] = transitions[i];
                std::snprintf(buffer, sizeof(buffer), " %s:%s>%s=%.1f", k ? "other" : "own", layoutNames[a], layoutNames[b], double(count) / f);
                line += buffer;
            }
            line += ";";
        }
        std::snprintf(buffer, sizeof(buffer),
            " state_per_frame: pipeline_binds=%.1f pipeline_skipped=%.1f descriptor_binds=%.1f push_constants=%.1f vertex_binds=%.1f index_binds=%.1f;",
            double(pipelineBinds) / f, double(pipelineBindsSkipped) / f, double(descriptorBinds) / f,
            double(pushConstants) / f, double(vertexBinds) / f, double(indexBinds) / f);
        line += buffer;
        std::snprintf(buffer, sizeof(buffer), " deferred_copies_per_frame: deferred=%.1f run_early=%.1f early_draw_over=%.1f early_sampled=%.1f early_draw_into=%.1f early_clear=%.1f early_other=%.1f texture=%.1f texture_hazards=%.1f;",
            double(copiesDeferred) / f, double(deferredFlushedEarly) / f, double(deferredEarlyReasons[0]) / f, double(deferredEarlyReasons[1]) / f,
            double(deferredEarlyReasons[2]) / f, double(deferredEarlyReasons[3]) / f, double(deferredEarlyReasons[4]) / f,
            double(textureCopiesDeferred) / f, double(textureCopyHazards) / f);
        line += buffer;
        return line;
    }

    // Steprant 2.0 WP1 stage 1a: what a frame graph could save, per frame,
    // from this window's profiled lists, without changing any command.
    // dead_store_mb: stores cleared before anything read them (STORE ->
    // DONT_CARE). merge_passes: passes that resume the framebuffer the
    // previous pass ended on with only unrelated barriers or query resets in
    // between; merge_mb: what those resumed passes loaded again, plus the
    // matching store (an upper bound, the pair's attachments at full size).
    // copies_to_pass_end: resolve copies already waiting for the pass end
    // (1.4c) and those that ran early. The gate of WP1 (stage 1a): at least
    // 25% of load+store traffic, 8 passes per frame or 3 ms.
    // diag_discarded_*: the WP-A.3 experiment, when its switch is on.
    std::string FormatWhatIf() const
    {
        const double f = double(std::max<uint64_t>(frames, 1));
        auto mb = [&](uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0) / f; };
        TargetStats all;
        for (const auto& [key, stats] : targets) { all.loadBytes += stats.loadBytes; all.storeBytes += stats.storeBytes; }
        const uint64_t dead = flow.storeCleared[0] + flow.storeCleared[1];
        const uint64_t merge = whatIfMergeBytes * 2;
        const uint64_t traffic = all.loadBytes + all.storeBytes;
        char buffer[768];
        std::snprintf(buffer, sizeof(buffer),
            "frame graph what-if: frames=%llu per_frame: traffic_mb=%.1f dead_store_mb=%.1f merge_passes=%.1f merge_mb=%.1f "
            "savable_mb=%.1f savable_share=%.1f%% copies_to_pass_end=%.1f copies_early=%.1f passes=%.1f "
            "diag_discarded_stores=%.1f diag_discarded_mb=%.1f commands=%.0f replays=%.1f discarded_stores=%.1f discarded_mb=%.1f",
            static_cast<unsigned long long>(frames), mb(traffic), mb(dead), double(whatIfMergeablePasses) / f, mb(merge),
            mb(dead + merge), traffic ? 100.0 * double(std::min(traffic, dead + merge)) / double(traffic) : 0.0,
            double(copiesDeferred) / f, double(deferredFlushedEarly) / f, double(PassCount()) / f,
            double(diagDiscardedStores) / f, mb(diagDiscardedStoreBytes), double(frameGraphCommands) / f,
            double(frameGraphReplays) / f, double(frameGraphDeadStores) / f, mb(frameGraphDeadStoreBytes));
        return buffer;
    }
    uint64_t PassCount() const
    {
        uint64_t passes = 0;
        for (const auto& [key, stats] : targets) passes += stats.passes;
        return passes;
    }

    // One log line: per-frame averages, then the most expensive targets.
    std::string Format(size_t topTargets = 8) const
    {
        const double f = double(std::max<uint64_t>(frames, 1));
        auto ms = [&](uint64_t ns) { return double(ns) / 1e6 / f; };
        auto mb = [&](uint64_t bytes) { return double(bytes) / (1024.0 * 1024.0) / f; };
        TargetStats all;
        for (const auto& [key, stats] : targets)
        {
            all.passes += stats.passes; all.emptyPasses += stats.emptyPasses; all.clearStarts += stats.clearStarts;
            all.draws += stats.draws; all.gpuNs += stats.gpuNs; all.loadBytes += stats.loadBytes; all.storeBytes += stats.storeBytes;
        }
        std::string line = "gpu passes: frames=" + std::to_string(frames) + " lists=" + std::to_string(lists) +
            " untimed_lists=" + std::to_string(untimedLists);
        char buffer[512];
        std::snprintf(buffer, sizeof(buffer),
            " per_frame: passes=%.1f empty=%.1f clear_start=%.1f draws=%.1f pass_ms=%.2f outside_ms=%.2f "
            "load_mb=%.1f store_mb=%.1f barriers=%.1f copies=%.1f dispatches=%.1f draws_outside=%.1f "
            "present: passes=%.1f ms=%.2f load_mb=%.1f store_mb=%.1f",
            all.passes / f, all.emptyPasses / f, all.clearStarts / f, all.draws / f, ms(all.gpuNs),
            ms(listNs > passNs ? listNs - passNs : 0), mb(all.loadBytes), mb(all.storeBytes),
            barriers / f, copies / f, dispatches / f, drawsOutside / f,
            present.passes / f, ms(present.gpuNs), mb(present.loadBytes), mb(present.storeBytes));
        line += buffer;
        line += FormatFlow(f);
        std::vector<std::pair<TargetKey, TargetStats>> sorted(targets.begin(), targets.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
            return a.second.gpuNs != b.second.gpuNs ? a.second.gpuNs > b.second.gpuNs
                : a.second.loadBytes + a.second.storeBytes > b.second.loadBytes + b.second.storeBytes;
        });
        line += " top:";
        for (size_t i = 0; i < sorted.size() && i < topTargets; ++i)
        {
            const auto& [key, stats] = sorted[i];
            std::snprintf(buffer, sizeof(buffer), " %ux%u%s%s passes=%.1f empty=%.1f draws=%.1f ms=%.2f load_mb=%.1f store_mb=%.1f;",
                key.width, key.height, key.colors ? (key.colors > 1 ? " mrt" : " c") : " -", key.depth ? "d" : "",
                stats.passes / f, stats.emptyPasses / f, stats.draws / f, ms(stats.gpuNs), mb(stats.loadBytes), mb(stats.storeBytes));
            line += buffer;
        }
        return line;
    }
};

// Thread-safe accumulator with a ten-second reporting window.
class Reporter
{
    std::mutex mutex;
    Summary summary;
    std::chrono::steady_clock::time_point windowStart{};

public:
    // Returns the finished window's line, or empty while it is still open.
    std::string Add(const plume::RenderPassProfile& profile, bool presentation,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now())
    {
        std::lock_guard lock(mutex);
        if (windowStart == std::chrono::steady_clock::time_point{}) windowStart = now;
        summary.Add(profile, presentation);
        if (now - windowStart < std::chrono::seconds(10) || summary.frames == 0) return {};
        // Two lines: the pass profile, then the frame graph what-if (WP1 1a).
        std::string line = summary.Format() + TakeFormatDraws(double(summary.frames)) + TakeFormatCmdproc() +
            "\n" + summary.FormatWhatIf();
        summary = Summary();
        windowStart = now;
        return line;
    }
};

inline Reporter& GlobalReporter()
{
    static Reporter reporter;
    return reporter;
}
}
