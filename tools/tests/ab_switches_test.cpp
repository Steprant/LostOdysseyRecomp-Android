// P5: diagnostics/ab_switches.h persists device A/B switches and turns them
// into development variables at startup, without overriding the environment.
#include <diagnostics/ab_switches.h>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <unistd.h>
using namespace diagnostics::ab;
static unsigned checks = 0;
static void Check(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        const auto dir = std::filesystem::temp_directory_path() / ("lo_ab_test_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir);
        const auto path = dir / "ab_switches.ini";
        for (size_t i = 0; i < Count; ++i) unsetenv(Definitions[i].variable);
        auto summary = ApplyAtStartup(path);
        Check(Get(Switch::MenuNativeSwitch) == 1 && Get(Switch::TurnipMode) == 0 && Get(Switch::PositionOnlyVs) == 0,
            "defaults without a file");
        for (size_t i = 0; i < Count; ++i) Check(std::getenv(Definitions[i].variable) == nullptr, "defaults set nothing");
        Check(summary.find("menu_native_switch=1") != std::string::npos, "summary lists every switch");
        Check(Set(Switch::PositionOnlyVs, 1) && Set(Switch::MenuNativeSwitch, 0) && Set(Switch::TurnipMode, 2) &&
            Set(Switch::Sgsr2Smaa, 1) && Set(Switch::DiscardUndefined, 1) && Set(Switch::RenderArea, 1) && Set(Switch::PerformanceHints, 1), "values saved");
        Check(!Set(Switch::TurnipMode, 3), "out-of-range value refused");
        setenv("LO_SR_VIEWPORT_JITTER", "dev", 1);
        Set(Switch::ViewportJitter, 1);
        Values()[size_t(Switch::PositionOnlyVs)] = 0; // forget, reload from disk
        summary = ApplyAtStartup(path);
        Check(Get(Switch::PositionOnlyVs) == 1, "persisted value reloaded");
        Check(std::string(std::getenv("LO_VK_KEEP_VS_OUTPUTS")) == "1", "position-only off keeps VS outputs");
        Check(std::string(std::getenv("LO_SGSR2_NATIVE_MENUS")) == "1", "old menu switch on");
        Check(std::string(std::getenv("TU_DEBUG")) == "gmem", "Turnip GMEM");
        Check(std::string(std::getenv("LO_SGSR2_SMAA")) == "0", "SMAA off");
        Check(std::string(std::getenv("LO_VK_DISCARD_UNDEFINED")) == "0", "undefined attachments loaded again");
        Check(std::string(std::getenv("LO_VK_RENDER_AREA")) == "0", "whole-target render passes again");
        Check(std::string(std::getenv("LO_ADPF")) == "1", "performance hints on");
        Check(std::string(std::getenv("LO_SR_VIEWPORT_JITTER")) == "dev" &&
            summary.find("(env LO_SR_VIEWPORT_JITTER=dev)") != std::string::npos, "environment wins and is logged");
        std::filesystem::remove_all(dir);
        std::printf("PASS: %u A/B switch checks\n", checks);
        return 0;
    } catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
}
