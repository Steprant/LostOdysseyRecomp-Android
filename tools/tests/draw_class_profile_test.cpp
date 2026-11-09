#include <cmath>
// P4: draw-class profile accumulation and its log line (gpu/draw_class_profile.h).
#include <gpu/draw_class_profile.h>
#include <cstdio>
#include <stdexcept>
#include <string>
using namespace gpu::draw_profile;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        Check(Classify(true, false, true, false, false) == DrawClass::DepthOnly, "depth only");
        Check(Classify(false, true, true, true, true) == DrawClass::Shadow, "shadow wins");
        Check(Classify(false, false, true, false, false) == DrawClass::ColorOpaque, "opaque");
        Check(Classify(false, false, true, true, false) == DrawClass::ColorBlend, "blend");
        Check(Classify(false, false, false, false, true) == DrawClass::Fullscreen, "fullscreen");
        Check(Classify(false, false, false, true, false) == DrawClass::Interface, "interface");
        Accumulator a;
        std::vector<DrawRecord> draws{{1, 2, DrawClass::DepthOnly}, {1, 2, DrawClass::DepthOnly},
            {3, 4, DrawClass::ColorOpaque}, {0, 0, DrawClass::Upscaler}, {5, 6, DrawClass::Interface}};
        const uint64_t ts[] = {1000, 3000, 5000, 15000, 1015000, ~0ull};
        a.AddList(draws, ts);
        a.EndFrame();
        Check(a.Class(DrawClass::DepthOnly).ns == 4000 && a.Class(DrawClass::DepthOnly).draws == 2, "depth time and count");
        Check(a.Class(DrawClass::ColorOpaque).ns == 10000, "opaque time");
        Check(a.Class(DrawClass::Upscaler).ns == 1000000, "upscaler bracket");
        Check(a.Class(DrawClass::Interface).ns == 0 && a.Class(DrawClass::Interface).draws == 1, "unavailable timestamp charges nothing");
        const uint64_t backwards[] = {5000, 4000};
        a.AddList({{7, 8, DrawClass::ColorBlend}}, backwards);
        Check(a.Class(DrawClass::ColorBlend).ns == 0, "backwards timestamp charges nothing");
        a.AddList(draws, nullptr);
        const auto line = a.Format(3);
        std::printf("%s\n", line.c_str());
        Check(line.find("frames=1") != std::string::npos && line.find("upscaler=1.00ms/1") != std::string::npos &&
            line.find("untimed=2") != std::string::npos, "summary line");
        Check(line.find("0000000000000000/0000000000000000 upscaler 1.00ms/1;") != std::string::npos,
            "top pairs ranked by time");
        // WP0.4 untimed sampling: whole-list time, classified counts, and the
        // least-squares cost per draw over frames with different draw counts:
        // frames of 200..1400 draws at 12 us per draw plus 3 ms fixed.
        Accumulator u;
        for (uint32_t frame = 0; frame < 7; ++frame) {
            const uint32_t count = 200 + frame * 200;
            std::vector<DrawRecord> list;
            for (uint32_t i = 0; i < count; ++i)
                list.push_back({1, 2, i % 4 == 0 ? DrawClass::ColorOpaque : DrawClass::DepthOnly});
            list.push_back({0, 0, DrawClass::Upscaler});
            // Two lists in the frame: the time splits, the fit sees the frame.
            const uint64_t total = 3000000ull + uint64_t(count) * 12000ull;
            u.AddUntimedList({list.begin(), list.begin() + count / 2}, 1000, 1000 + total / 2);
            u.AddUntimedList({list.begin() + count / 2, list.end()}, 50000000, 50000000 + total - total / 2);
            u.EndFrame();
        }
        const auto fit = u.Regression();
        Check(u.Untimed() && fit.samples == 7, "untimed samples per frame");
        Check(std::abs(fit.nsPerDraw - 12000.0) < 1.0 && std::abs(fit.fixedNs - 3000000.0) < 100.0 && fit.r2 > 0.999,
            "least squares recovers the cost per draw and the fixed part");
        Check(u.Class(DrawClass::Upscaler).draws == 7 && u.Class(DrawClass::DepthOnly).draws + u.Class(DrawClass::ColorOpaque).draws == 5600,
            "untimed draws classified and counted, upscaler apart");
        const auto untimed = u.Format();
        std::printf("%s\n", untimed.c_str());
        Check(untimed.find("untimed: list start/end only") != std::string::npos &&
            untimed.find("fit: us_per_draw=12.00 fixed_ms=3.00") != std::string::npos, "untimed summary line");
        Accumulator few;
        few.AddUntimedList({{1, 2, DrawClass::DepthOnly}}, 0, 1000);
        few.EndFrame();
        Check(few.Regression().samples == 1 && few.Regression().nsPerDraw == 0, "no fit below three frames");
        std::printf("PASS: %u draw class profile checks\n", checks);
        return 0;
    } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
}
