#pragma once
// Developer page of the Steprant 1.9.x test builds (D1): A/B switches for the
// 2.0 packages. Players cannot set environment variables on a phone, so each
// switch is persisted in developer_switches.ini beside settings.ini and, at
// the next start, written as the development variable the renderer, Plume,
// the audio code or the driver already read, before any of them looks at it.
// A variable already in the environment wins (development override). Every
// switch applies after restarting; the startup log lists them all.
// Only builds with LO_STEPRANT_DEVELOPER_PAGE show the page or read the file;
// the public 2.0 keeps the confirmed values built in.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace diagnostics::developer
{
enum class Switch : uint32_t
{
    Sgsr2Smaa,            // SMAA on SGSR2's input in Quality (on)
    DrawTimestamps,       // WP0.4: per-draw timestamps on the profiled frames (on); off measures the same frames without them
    DeadStoreDiscard,     // WP-A.3 (diagnostic only): stores known dead are DONT_CARE (off)
    GameThreadsOnBigCores,// WP3.2: guest main and GPU threads on the prime/big cores (off)
    UiPrewarm,            // WP3.4: settings menu and achievement toast resources prepared ahead (on)
    VoicePrefetch,        // WP3.8: cutscene voice streams read ahead on their own thread (on)
    Sgsr2Reactive,        // WP5 G6 / WP7.2.5: SGSR2 does not keep history no current neighbour explains (on)
    TurnipMode,           // Turnip render mode: 0 auto, 1 sysmem, 2 gmem
    // Steprant 2.0.1 A/B switches for the reflection, flicker and stutter
    // reports of 10/11 (each on by default; off restores the 2.0 path or the
    // one before the package).
    FrameGraph,           // SR4 G-2: commands kept and replayed, dead stores, exact render areas (on)
    DeferredCopies,       // 1.4c/G-2: resolves and texture copies wait for the end of the pass (on)
    ScissorRenderArea,    // 1.4a/G-4: render passes over the drawn area, not the whole target (on)
    CompactTargets,       // 1.6: 8_8_8_8 and 2_10_10_10 targets in 32-bit host formats (on)
    Sgsr2Continuation,    // 2.0.1: SGSR2 keeps scene frames whose motion is not ready (on)
    DisplayTiming,        // 2.0.1: frames held for the same number of refreshes (off: it made the cadence worse on the phone)
    ScreenDrawJitter,     // 2.0.1: screen-space scene draws move with the SGSR2 jitter (on)
    // Steprant 2.0.2 (refinements): one switch per intervention, off restores the 2.0 behaviour.
    TransitionPromotion,  // A1: loading screens and movies drawn at the output size from their first draw (on)
    AudioWorkerPriority,  // A1: the game's audio worker thread raised by its measured CPU share (on); off leaves it alone
    ScreenDrawJitterWide, // C11: the 2.0.1 rule for the SGSR2 jitter on screen draws, every draw without a depth test (off)
    AsyncLog,             // B6: log lines written by a writer thread through a bounded queue (on)
    Count
};
inline constexpr size_t Count = size_t(Switch::Count);

struct Definition
{
    const char* key;          // developer_switches.ini key and log name
    const wchar_t* label;     // English row label
    const wchar_t* labelZh;   // Traditional Chinese row label
    uint32_t defaultValue;    // on/off rows: 0 On, 1 Off
    uint32_t choices;         // 2 for on/off, 3 for Turnip
    const char* variable;     // development variable
};

inline constexpr std::array<Definition, Count> Definitions{{
    {"sgsr2_smaa", L"SMAA in SGSR2 Quality", L"SGSR2 品質模式中的 SMAA", 0, 2, "LO_SGSR2_SMAA"},
    {"draw_timestamps", L"Per-draw timestamps (profile)", L"逐次繪製時間戳（分析）", 0, 2, "LO_DRAW_TIMESTAMPS"},
    {"dead_store_discard", L"Discard dead stores (test only)", L"捨棄無用儲存（僅測試）", 1, 2, "LO_VK_DIAG_DISCARD_DEAD_STORES"},
    {"game_threads_big_cores", L"Game threads on big cores", L"遊戲執行緒使用大核心", 1, 2, "LO_PIN_GAME_THREADS"},
    {"ui_prewarm", L"Prepare menu resources ahead", L"預先準備選單資源", 0, 2, "LO_UI_PREWARM"},
    {"voice_prefetch", L"Read cutscene voices ahead", L"預先讀取過場語音", 0, 2, "LO_VOICE_PREFETCH"},
    {"sgsr2_reactive", L"SGSR2 reactive mask", L"SGSR2 反應遮罩", 0, 2, "LO_SGSR2_REACTIVE"},
    {"turnip_mode", L"Turnip render mode", L"Turnip 渲染模式", 0, 3, "TU_DEBUG"},
    {"frame_graph", L"Frame graph (2.0)", L"幀圖（2.0）", 0, 2, "LO_VK_FRAME_GRAPH"},
    {"deferred_copies", L"Copies at the end of the pass", L"於渲染通道結束時複製", 0, 2, "LO_VK_DEFER_COPY"},
    {"scissor_render_area", L"Render area of the draws", L"依繪製範圍的渲染區域", 0, 2, "LO_VK_RENDER_AREA"},
    {"compact_targets", L"Compact render targets", L"精簡渲染目標", 0, 2, "LO_COMPACT_RT"},
    {"sgsr2_continuation", L"SGSR2 without motion ready", L"動態向量未就緒時保持 SGSR2", 0, 2, "LO_SGSR2_CONTINUATION"},
    {"display_timing_test", L"Display timing (test)", L"顯示時序（測試）", 1, 2, "LO_DISPLAY_TIMING"},
    {"screen_draw_jitter", L"SGSR2 jitter on screen draws", L"螢幕繪製套用 SGSR2 抖動", 0, 2, "LO_SR_JITTER_SCREEN_DRAWS"},
    {"transition_promotion", L"Loading screens and movies at output size", L"載入畫面與影片以輸出解析度繪製", 0, 2, "LO_SGSR2_TRANSITION_PROMOTION"},
    {"audio_worker_priority", L"Game audio thread priority", L"遊戲音訊執行緒優先權", 0, 2, "LO_AUDIO_WORKER_PRIORITY"},
    {"screen_draw_jitter_wide", L"SGSR2 jitter on every screen draw (2.0.1)", L"所有螢幕繪製套用 SGSR2 抖動（2.0.1）", 1, 2, "LO_SR_JITTER_SCREEN_DRAWS_WIDE"},
    {"async_log", L"Asynchronous log", L"非同步記錄", 0, 2, "LO_LOG_ASYNC"},
}};

// The value each stored choice writes to its variable (nullptr: unset, the
// built-in default).
// Steprant 2.0.2: one rule for every on/off row instead of a case per switch.
// The default choice leaves the variable unset; the other one writes "0" (Off)
// or "1" (On). The per-switch table of 2.0.1b had the five 2.0 rows (frame
// graph, copies at the end of the pass, render area, compact targets, SGSR2
// continuation) share the display timing case: On wrote "1" and Off wrote
// nothing, so in that build those five switches turned nothing off.
inline const char* VariableValue(Switch which, uint32_t value)
{
    if (which == Switch::Count) return nullptr;
    if (which == Switch::TurnipMode) return value == 1 ? "sysmem" : value == 2 ? "gmem" : nullptr;
    const auto& definition = Definitions[size_t(which)];
    if (definition.choices != 2 || value >= 2 || value == definition.defaultValue) return nullptr;
    return value == 1 ? "0" : "1";
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
        char* end = nullptr;
        const unsigned long number = std::strtoul(line.c_str() + equals + 1, &end, 10);
        if (end == line.c_str() + equals + 1) continue;
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
// choices. Returns one log line with every switch, and which ones the
// environment already overrode.
inline std::string ApplyAtStartup(const std::filesystem::path& path)
{
    Load(path);
    std::string summary = "developer switches:";
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
