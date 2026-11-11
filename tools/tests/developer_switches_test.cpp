// Steprant 2.0.2: every developer-page switch really changes its variable
// (diagnostics/developer_switches.h). In 2.0.1b five rows turned nothing off.
#include <diagnostics/developer_switches.h>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>
using namespace diagnostics::developer;
static unsigned checks = 0;
static void Check(bool ok, const std::string& what) { ++checks; if (!ok) throw std::runtime_error(what); }
int main()
{
    try {
        std::set<std::string> keys;
        for (size_t i = 0; i < Count; ++i) {
            const auto which = Switch(i);
            const auto& d = Definitions[i];
            const std::string name = d.key ? d.key : "(null)";
            Check(d.key && d.label && d.labelZh && d.variable, name + ": complete definition");
            Check(keys.insert(name).second, name + ": unique key");
            Check(d.defaultValue < d.choices, name + ": default within its choices");
            Check(VariableValue(which, d.defaultValue) == nullptr, name + ": the default leaves the variable unset");
            if (which == Switch::TurnipMode) {
                Check(d.choices == 3 && std::strcmp(VariableValue(which, 1), "sysmem") == 0 &&
                    std::strcmp(VariableValue(which, 2), "gmem") == 0, name + ": Turnip modes");
                continue;
            }
            Check(d.choices == 2, name + ": on/off row");
            // Rows show 0 as On and 1 as Off.
            const uint32_t other = 1 - d.defaultValue;
            const char* value = VariableValue(which, other);
            Check(value != nullptr, name + ": the non-default choice sets the variable");
            Check(std::strcmp(value, other == 1 ? "0" : "1") == 0, name + ": Off writes 0, On writes 1");
        }
        // The rows 2.0.1b broke.
        for (const auto which : {Switch::FrameGraph, Switch::DeferredCopies, Switch::ScissorRenderArea,
                Switch::CompactTargets, Switch::Sgsr2Continuation}) {
            const char* off = VariableValue(which, 1);
            Check(off && std::strcmp(off, "0") == 0, std::string(Definitions[size_t(which)].key) + ": Off turns the 2.0 path off");
            Check(VariableValue(which, 0) == nullptr, std::string(Definitions[size_t(which)].key) + ": On is the built-in default");
        }
        Check(std::strcmp(VariableValue(Switch::DisplayTiming, 0), "1") == 0 && VariableValue(Switch::DisplayTiming, 1) == nullptr,
            "display timing stays off unless switched on");
        Check(VariableValue(Switch::Count, 0) == nullptr, "no value for the count sentinel");
        std::printf("developer switches: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "developer switches test failed: %s\n", e.what());
        return 1;
    }
}
