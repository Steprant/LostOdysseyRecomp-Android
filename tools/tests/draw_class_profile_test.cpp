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
        std::printf("PASS: %u draw class profile checks\n", checks);
        return 0;
    } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
}
