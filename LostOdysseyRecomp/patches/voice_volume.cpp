#include <stdafx.h>
#include <array>
#include <cmath>
#include <atomic>
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <os/logger.h>
#include <apu/audio.h>

// Voice volume (#394). The retail game has four volume categories in its
// sound system (0x832D268C): sub_82870E38 hands config+8 (Music) to category 3
// and config+12 (Sound effects) to categories 2 and 1, scaled to 0-128 by
// sub_82B5C918, which keeps master * category >> 7 at +620 + category. A
// channel's type at +11 selects its category (sub_82B71D90).
//
// Localized streams are opened first (channel + 100): sub_82B6DBD0 passes
// localize=1 to sub_82B783F0. If present, the companion voice at +124 is
// opened by sub_82B6D7E0 with localize=0, so it plays the base file instead.
// The handle positions do not identify music or dialogue. In particular,
// the base RT stream can contain effects or voices, and some language files
// contain a full surround mix. Keep those on their retail category; only
// streams the game marks as BGM follow Music (#631).
//
// sub_82B73198 sets voice +28 to the actual source channel count and +46 to
// its speaker map (FL, FR, FC, LFE, BL, BR). For a paired stream, an actual
// mono FC source is the separate dialogue track. In the per-voice volume
// update sub_82B6C650, replace its category gain from sub_82B71E68 with
// master * Voice volume (#394). At 100 this matches retail with Sound
// effects at 100. Other layouts, missing language files and voices inside
// effect banks (.xse) retain the game's category and volume curves.

extern "C" PPC_FUNC(__imp__sub_82B6C650);
extern "C" PPC_FUNC(__imp__sub_82B71E68);
// Steprant 2.1.1-F: the two other callers of sub_82B71E68 (found in the
// built library on 15/11), the per-voice updates of bank sounds.
extern "C" PPC_FUNC(__imp__sub_82B6EA80);
extern "C" PPC_FUNC(__imp__sub_82B6F708);

namespace
{
constexpr uint32_t SoundSystem = 0x832D268C;
constexpr uint32_t StreamVoices = 0x832D2DA0;
constexpr uint32_t ChannelPath = 127;
constexpr size_t PathCapacity = 320 - ChannelPath; // the channel object is 320 bytes

thread_local bool languageTrack = false;

// Steprant 2.1.1-E: what sub_82B6C650 is updating, for the log of every
// stream (below) and the dialogue rule.
struct StreamInfo
{
    bool active = false;
    uint32_t voice = 0;
    bool paired = false;
    uint8_t sources = 0, speakers = 0, type = 0;
};
thread_local StreamInfo current;

// Steprant 2.1.1-E: one line per stream path, layout and outcome, so a run
// log shows how every stream the game played was routed (the 15/11 session
// heard quiet voices in dialogues that never reached the mono FC rule).
void LogStream(std::string_view path, const StreamInfo& info, double retail, double applied, bool dialogue)
{
    static std::mutex mutex;
    static std::vector<std::string> seen;
    std::string key = std::string(path) + (dialogue ? "|d" : "|r") + std::to_string(info.sources) + ":" + std::to_string(info.speakers);
    std::lock_guard lock(mutex);
    if (seen.size() >= 512 || std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(std::move(key));
    LOG_INFO("sound stream: {} sources={} speakers={:#x} paired={} type={} gain={:.2f}{}",
        path, info.sources, info.speakers, info.paired, info.type, applied,
        dialogue ? fmt::format(" (voice; retail {:.2f})", retail) : std::string(" (retail)"));
}

// A null voice argument selects the primary, as sub_82B6C650 does itself.
uint32_t PrimaryVoice(uint8_t* base, uint32_t channel)
{
    const uint32_t handle = PPC_LOAD_U16(channel + 100);
    const uint32_t voices = PPC_LOAD_U32(StreamVoices);
    const uint32_t table = voices ? PPC_LOAD_U32(voices + 8) : 0;
    return handle && table ? PPC_LOAD_U32(table + handle * 4 - 4) : 0;
}

// Logs each stream once per gain, so run logs show what was replaced.
void Log(std::string_view path, float gain, double retail)
{
    static std::mutex mutex;
    static std::array<std::pair<std::string, float>, 8> recent;
    static size_t next = 0;
    std::lock_guard lock(mutex);
    for (const auto& [p, g] : recent)
        if (g == gain && p == path) return;
    recent[next++ % recent.size()] = {std::string(path), gain};
    LOG_INFO("voice volume: {:.2f} instead of {:.2f} for the mono FC track of {}",
        gain, retail, path);
}
}

// Steprant 2.1.1-E: an actual mono FC source is dialogue whether or not the
// stream is paired with a base mix. Until 2.1.1-D only paired streams were
// recognized, and the 15/11 session (Cooke and Mack's house, the ghost town)
// had quiet dialogue that never reached the rule. LO_VOICE_PAIRED_ONLY=1
// keeps the paired-only rule.
PPC_FUNC(sub_82B6C650)
{
    static const bool pairedOnly = [] {
        const char* value = std::getenv("LO_VOICE_PAIRED_ONLY");
        return value && std::strcmp(value, "1") == 0;
    }();
    const uint32_t channel = ctx.r3.u32;
    const uint32_t voice = ctx.r4.u32 ? ctx.r4.u32 : PrimaryVoice(base, channel);
    current = {};
    current.active = true;
    current.voice = voice;
    current.paired = PPC_LOAD_U16(channel + 124) != 0;
    current.type = PPC_LOAD_U8(channel + 11);
    if (voice)
    {
        current.sources = PPC_LOAD_U8(voice + 28);
        current.speakers = PPC_LOAD_U8(voice + 46);
    }
    languageTrack = voice && (current.paired || !pairedOnly) && current.sources == 1 && current.speakers == 2;
    __imp__sub_82B6C650(ctx, base);
    languageTrack = false;
    current.active = false;
}

// Steprant 2.1.1-F: the game's own stream BGM (types 17 and 19, the Music
// category) plays at this share of its retail gain. The 15/11 08:59 session
// played no dialogue stream at all: dialogue in the field comes from sound
// banks (sub_82B6EA80, sub_82B6F708), whose category gain was 0.44 while the
// BGM streams ran at 1.00 (both before the Voice and Music settings). The BGM
// covered the voices by about 7 dB. 0.5 (-6 dB) brings them level, and the
// dialogue-forward fold-down puts voices on top. LO_BGM_BALANCE=<0.05-1>.
float BgmBalance()
{
    static const float balance = [] {
        const char* value = std::getenv("LO_BGM_BALANCE");
        const float parsed = value ? float(std::atof(value)) : 0.5f;
        return parsed >= 0.05f && parsed <= 1.0f ? parsed : 0.5f;
    }();
    return balance;
}

// Steprant 2.1.1-F: which bank path asks for a category gain (1: 82B6EA80,
// 2: 82B6F708), for the log of how field dialogue is routed.
thread_local int bankCaller = 0;

void LogBank(int caller, uint8_t type, double gain)
{
    static std::mutex mutex;
    static std::vector<uint32_t> seen;
    const uint32_t key = (uint32_t(caller) << 24) | (uint32_t(type) << 16) | uint32_t(std::lround(gain * 1000.0));
    std::lock_guard lock(mutex);
    if (seen.size() >= 256 || std::find(seen.begin(), seen.end(), key) != seen.end()) return;
    seen.push_back(key);
    LOG_INFO("sound bank voice: path={} type={} gain={:.3f} (retail)", caller == 1 ? "82B6EA80" : "82B6F708", type, gain);
}

// Steprant 2.1.1-F: master and the four category gains the game caches
// (system +616, +620..+623), logged when they change.
void LogCategories(uint8_t* base)
{
    const uint32_t system = PPC_LOAD_U32(SoundSystem);
    if (!system) return;
    const uint32_t packed = (uint32_t(PPC_LOAD_U8(system + 620)) << 24) | (uint32_t(PPC_LOAD_U8(system + 621)) << 16) |
        (uint32_t(PPC_LOAD_U8(system + 622)) << 8) | uint32_t(PPC_LOAD_U8(system + 623));
    const uint32_t master = PPC_LOAD_U8(system + 616);
    static std::atomic<uint64_t> last{~0ull};
    const uint64_t now = (uint64_t(master) << 32) | packed;
    if (last.load(std::memory_order_relaxed) == now || last.exchange(now, std::memory_order_relaxed) == now) return;
    LOG_INFO("sound categories: master={} cat0={} cat1={} cat2={} cat3(music)={} (of 128); BGM balance {:.2f}",
        master, packed >> 24, (packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF, BgmBalance());
}

PPC_FUNC(sub_82B6EA80)
{
    const int previous = bankCaller;
    bankCaller = 1;
    __imp__sub_82B6EA80(ctx, base);
    bankCaller = previous;
}

PPC_FUNC(sub_82B6F708)
{
    const int previous = bankCaller;
    bankCaller = 2;
    __imp__sub_82B6F708(ctx, base);
    bankCaller = previous;
}

PPC_FUNC(sub_82B71E68)
{
    const uint32_t channel = ctx.r3.u32;
    const uint8_t type = PPC_LOAD_U8(channel + 11);
    __imp__sub_82B71E68(ctx, base);
    LogCategories(base);
    if (!current.active)
    {
        if (bankCaller) LogBank(bankCaller, type, ctx.f1.f64);
        return;
    }
    const char* path = reinterpret_cast<const char*>(base + channel + ChannelPath);
    const std::string_view name(path, strnlen(path, PathCapacity));
    if (!languageTrack)
    {
        const double retail = ctx.f1.f64;
        if (type == 17 || type == 19) ctx.f1.f64 = retail * BgmBalance();
        LogStream(name, current, retail, ctx.f1.f64, false);
        return;
    }
    const uint32_t system = PPC_LOAD_U32(SoundSystem);
    const float master = system ? float(PPC_LOAD_U8(system + 616)) / 128.0f : 1.0f;
    const float gain = master * float(apu::VoiceVolume()) / 100.0f;
    Log(name, gain, ctx.f1.f64);
    LogStream(name, current, ctx.f1.f64, gain, true);
    ctx.f1.f64 = double(gain);
}
