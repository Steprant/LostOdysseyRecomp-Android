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
// sub_82B73198 sets voice +28 to the source channel count and +46 to its
// speaker map (FL, FR, FC, LFE, BL, BR). A mono FC source uses the Voice
// slider. When it has a companion, lower the companion's mix under it.
// Field dialogue can also use an effects bank, whose cues share their gain
// with ordinary effects. Lower active BGM while the field script has control.

extern "C" PPC_FUNC(__imp__sub_82B6C650);
extern "C" PPC_FUNC(__imp__sub_82B6D400);
extern "C" PPC_FUNC(__imp__sub_82B71E68);
// Steprant 2.1.1-F: the two other callers of sub_82B71E68 (found in the
// built library on 15/11), the per-voice updates of bank sounds.
extern "C" PPC_FUNC(__imp__sub_82B6EA80);
extern "C" PPC_FUNC(__imp__sub_82B6F708);

namespace
{
constexpr uint32_t SoundSystem = 0x832D268C;
constexpr uint32_t StreamVoices = 0x832D2DA0;
constexpr uint32_t ChannelType = 11, ChannelGainDirty = 20;
constexpr uint32_t PrimaryHandle = 100, CompanionHandle = 124;
constexpr uint32_t ChannelPath = 127;
constexpr size_t PathCapacity = 320 - ChannelPath; // the channel object is 320 bytes
constexpr uint8_t LiveBgmType = 19, PairedStreamType = 35;
constexpr double DialogueGain = 1.25;
constexpr double CompanionBalance = 0.60;
constexpr double FieldBgmBalance = 0.50;

std::atomic<bool> fieldEventMix{false};

struct StreamInfo
{
    bool active = false, paired = false, dialogue = false, companion = false;
    uint8_t sources = 0, speakers = 0, type = 0;
};
thread_local StreamInfo current;

bool IsBgm(uint8_t type) { return type == 17 || type == LiveBgmType; }
bool FieldMusicDucked()
{
    return fieldEventMix.load(std::memory_order_relaxed) && apu::VoiceVolume() != 0;
}

// Steprant 2.1.1-E: one line per stream path, layout and outcome, so a run
// log shows how every stream the game played was routed (the 15/11 session
// heard quiet voices in dialogues that never reached the mono FC rule).
enum class Mix { Retail, Voice, DialogueCompanion, FieldMusic };
void LogStream(std::string_view path, const StreamInfo& info, double retail, double applied, Mix mix)
{
    struct Seen { std::string path; uint8_t sources, speakers; Mix mix; };
    static std::mutex mutex;
    static std::vector<Seen> seen;
    std::lock_guard lock(mutex);
    if (seen.size() >= 512 || std::any_of(seen.begin(), seen.end(), [&](const Seen& entry) {
        return entry.path == path && entry.sources == info.sources &&
            entry.speakers == info.speakers && entry.mix == mix;
    })) return;
    seen.push_back({std::string(path), info.sources, info.speakers, mix});
    constexpr const char* mixNames[] = {"retail", "voice", "dialogue companion", "field music"};
    LOG_INFO("sound stream: {} sources={} speakers={:#x} paired={} type={} gain={:.2f} ({}; retail {:.2f})",
        path, info.sources, info.speakers, info.paired, info.type, applied, mixNames[static_cast<unsigned>(mix)], retail);
}

uint32_t ResolveVoice(uint8_t* base, uint16_t handle)
{
    if (!handle) return 0;
    const uint32_t voices = PPC_LOAD_U32(StreamVoices);
    const uint32_t table = voices ? PPC_LOAD_U32(voices + 8) : 0;
    return table ? PPC_LOAD_U32(table + handle * 4 - 4) : 0;
}

bool MonoCenter(uint8_t* base, uint32_t voice)
{
    return voice && PPC_LOAD_U8(voice + 28) == 1 && PPC_LOAD_U8(voice + 46) == 2;
}
}

void apu::SetFieldDialogueMix(bool active)
{
    fieldEventMix.store(active, std::memory_order_relaxed);
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
    const uint16_t companion = PPC_LOAD_U16(channel + CompanionHandle);
    const uint32_t voice = ctx.r4.u32 ? ctx.r4.u32 : ResolveVoice(base, PPC_LOAD_U16(channel + PrimaryHandle));
    const StreamInfo previous = current;
    current = {.active = true, .paired = companion != 0, .type = PPC_LOAD_U8(channel + ChannelType)};
    if (voice)
    {
        current.sources = PPC_LOAD_U8(voice + 28);
        current.speakers = PPC_LOAD_U8(voice + 46);
        current.dialogue = !IsBgm(current.type) && (current.paired || !pairedOnly) &&
            current.sources == 1 && current.speakers == 2;
        if (!current.dialogue && companion && ctx.r4.u32 && !IsBgm(current.type))
        {
            const uint32_t primary = ResolveVoice(base, PPC_LOAD_U16(channel + PrimaryHandle));
            current.companion = primary != voice && MonoCenter(base, primary) &&
                voice == ResolveVoice(base, companion);
        }
    }
    __imp__sub_82B6C650(ctx, base);
    current = previous;
}

PPC_FUNC(sub_82B6D400)
{
    // The sound worker refreshes gains when channel byte +20 is dirty.
    // Mark native BGM on field transitions and paired streams when their
    // localized voice becomes ready after the companion was opened.
    const uint32_t channel = ctx.r3.u32;
    const uint8_t type = PPC_LOAD_U8(channel + ChannelType);
    if (type == LiveBgmType || (type == PairedStreamType && PPC_LOAD_U16(channel + CompanionHandle)))
    {
        struct StreamUpdate { uint32_t channel = 0; bool lowered = false; };
        static thread_local std::array<StreamUpdate, 8> updates;
        static thread_local size_t next = 0;
        const bool lowered = type == LiveBgmType ? FieldMusicDucked()
            : apu::VoiceVolume() != 0 && MonoCenter(base, ResolveVoice(base, PPC_LOAD_U16(channel + PrimaryHandle)));
        auto it = std::find_if(updates.begin(), updates.end(),
            [channel](const StreamUpdate& value) { return value.channel == channel; });
        if (it == updates.end())
        {
            it = updates.begin() + next++ % updates.size();
            *it = {channel, !lowered};
        }
        if (it->lowered != lowered)
        {
            it->lowered = lowered;
            PPC_STORE_U8(channel + ChannelGainDirty, PPC_LOAD_U8(channel + ChannelGainDirty) | 0x02);
        }
    }
    __imp__sub_82B6D400(ctx, base);
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
    const uint8_t type = PPC_LOAD_U8(channel + ChannelType);
    __imp__sub_82B71E68(ctx, base);
    LogCategories(base);
    if (!current.active)
    {
        if (bankCaller) LogBank(bankCaller, type, ctx.f1.f64);
        return;
    }
    const char* path = reinterpret_cast<const char*>(base + channel + ChannelPath);
    const std::string_view name(path, strnlen(path, PathCapacity));
    const double retail = ctx.f1.f64;
    Mix mix = Mix::Retail;
    if (IsBgm(type))
    {
        const bool ducked = type == LiveBgmType && FieldMusicDucked();
        ctx.f1.f64 *= BgmBalance() * (ducked ? FieldBgmBalance : 1.0);
        if (ducked) mix = Mix::FieldMusic;
    }
    else if (current.dialogue)
    {
        const uint32_t system = PPC_LOAD_U32(SoundSystem);
        const double master = system ? double(PPC_LOAD_U8(system + 616)) / 128.0 : 1.0;
        ctx.f1.f64 = master * apu::VoiceVolume() / 100.0 * DialogueGain;
        mix = Mix::Voice;
    }
    else if (current.companion && apu::VoiceVolume() != 0)
    {
        ctx.f1.f64 *= CompanionBalance;
        mix = Mix::DialogueCompanion;
    }
    LogStream(name, current, retail, ctx.f1.f64, mix);
}
