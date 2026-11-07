#pragma once
// A/B switches for device tests (Android diagnostics page). Players cannot set
// environment variables on a phone, so each switch is persisted in
// ab_switches.ini beside settings.ini and, at the next start, turned into the
// development variable that the renderer, Plume or the driver already read
// (before any of them looks at it). A variable already present in the
// environment wins: it stays a development override. Every switch applies
// after restarting, and the startup log lists them all.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace diagnostics::ab
{
enum class Switch : uint32_t
{
    PositionOnlyVs,     // depth-only pipelines use the position-only vertex module
    KillFreePs,         // pixel shaders without discard unless the alpha test is on
    DepthOnlyWithoutPs, // depth-only draws drop their no-op pixel shader
    ViewportJitter,     // unknown scene draws under SR take the jitter as a viewport offset
    ParkTargets,        // keep the other size's render targets across a resize
    Sgsr2Smaa,          // SMAA on SGSR2's input in Quality
    MenuNativeSwitch,   // menus switch the render size (old) instead of promoting the UI
    TurnipMode,         // Turnip render mode: 0 auto, 1 sysmem, 2 gmem
    Count
};
inline constexpr size_t Count = size_t(Switch::Count);

struct Definition
{
    const char* key;          // ab_switches.ini key and log name
    const wchar_t* label;     // English row label (translations_eu.h has the European ones)
    const wchar_t* labelZh;   // Traditional Chinese row label
    uint32_t defaultValue;
    uint32_t choices;         // 2 for on/off (0 = on, 1 = off), 3 for Turnip
    const char* variable;     // development variable
};

// For on/off rows the stored value is the row index: 0 On, 1 Off. The
// variable written for each value; nullptr leaves it unset (the default).
inline constexpr std::array<Definition, Count> Definitions{{
    {"position_only_vs", L"Position-only depth vertex shaders", L"僅位置的深度頂點著色器", 0, 2, "LO_VK_KEEP_VS_OUTPUTS"},
    {"kill_free_ps", L"Pixel shaders without discard", L"無 discard 的像素著色器", 0, 2, "LO_ALPHATEST_SPEC"},
    {"depth_only_without_ps", L"Depth-only draws without pixel shader", L"僅深度繪製不用像素著色器", 0, 2, "LO_KEEP_DEPTH_ONLY_PS"},
    {"viewport_jitter", L"Viewport jitter for unknown draws", L"未知繪製的視口抖動", 0, 2, "LO_SR_VIEWPORT_JITTER"},
    {"park_targets", L"Keep render targets across resizes", L"調整大小時保留渲染目標", 0, 2, "LO_PARK_TARGETS"},
    {"sgsr2_smaa", L"SMAA in SGSR2 Quality", L"SGSR2 品質模式中的 SMAA", 0, 2, "LO_SGSR2_SMAA"},
    {"menu_native_switch", L"Menus: render-size switch (old)", L"選單：切換渲染尺寸（舊）", 1, 2, "LO_SGSR2_NATIVE_MENUS"},
    {"turnip_mode", L"Turnip render mode", L"Turnip 渲染模式", 0, 3, "TU_DEBUG"},
}};

// The value each stored choice writes to its variable (nullptr: unset).
inline const char* VariableValue(Switch which, uint32_t value)
{
    switch (which)
    {
    case Switch::PositionOnlyVs: return value == 1 ? "1" : nullptr;
    case Switch::KillFreePs: return value == 1 ? "0" : nullptr;
    case Switch::DepthOnlyWithoutPs: return value == 1 ? "1" : nullptr;
    case Switch::ViewportJitter: return value == 1 ? "0" : nullptr;
    case Switch::ParkTargets: return value == 1 ? "0" : nullptr;
    case Switch::Sgsr2Smaa: return value == 1 ? "0" : nullptr;
    // On (0): the old switch; Off (1, the default): UI promotion.
    case Switch::MenuNativeSwitch: return value == 0 ? "1" : nullptr;
    case Switch::TurnipMode: return value == 1 ? "sysmem" : value == 2 ? "gmem" : nullptr;
    case Switch::Count: break;
    }
    return nullptr;
}

inline std::array<uint32_t, Count>& Values()
{
    static std::array<uint32_t, Count> values = [] {
        std::array<uint32_t, Count> initial{};
        for (size_t i = 0; i < Count; ++i) initial[i] = Definitions[i].defaultValue;
        return initial;
    }();
    return values;
}

inline uint32_t Get(Switch which) { return Values()[size_t(which)]; }

inline std::filesystem::path& StorePath()
{
    static std::filesystem::path path;
    return path;
}

inline void Load(const std::filesystem::path& path)
{
    StorePath() = path;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        const auto equals = line.find('=');
        if (equals == std::string::npos) continue;
        const std::string_view key(line.data(), equals);
        const unsigned long number = std::strtoul(line.c_str() + equals + 1, nullptr, 10);
        for (size_t i = 0; i < Count; ++i)
            if (key == Definitions[i].key && number < Definitions[i].choices) Values()[i] = uint32_t(number);
    }
}

inline bool Save()
{
    if (StorePath().empty()) return false;
    const auto temporary = std::filesystem::path(StorePath()).concat(".tmp");
    {
        std::ofstream out(temporary, std::ios::trunc);
        if (!out) return false;
        for (size_t i = 0; i < Count; ++i) out << Definitions[i].key << '=' << Values()[i] << '\n';
        if (!out.flush()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(temporary, StorePath(), ec);
    return !ec;
}

inline bool Set(Switch which, uint32_t value)
{
    if (value >= Definitions[size_t(which)].choices) return false;
    Values()[size_t(which)] = value;
    return Save();
}

// Loads the store and writes the development variables of non-default
// choices. Returns one log line (names, values, and which ones the
// environment already overrode).
inline std::string ApplyAtStartup(const std::filesystem::path& path)
{
    Load(path);
    std::string summary = "A/B switches:";
    for (size_t i = 0; i < Count; ++i) {
        const auto which = Switch(i);
        const auto& definition = Definitions[i];
        const char* existing = std::getenv(definition.variable);
        const char* wanted = VariableValue(which, Values()[i]);
        summary += ' ';
        summary += definition.key;
        summary += '=';
        summary += std::to_string(Values()[i]);
        if (existing && *existing) {
            summary += "(env ";
            summary += definition.variable;
            summary += '=';
            summary += existing;
            summary += ')';
            continue;
        }
        if (!wanted) continue;
#ifdef _WIN32
        _putenv_s(definition.variable, wanted);
#else
        setenv(definition.variable, wanted, 1);
#endif
        summary += "(set ";
        summary += definition.variable;
        summary += '=';
        summary += wanted;
        summary += ')';
    }
    return summary;
}
}
