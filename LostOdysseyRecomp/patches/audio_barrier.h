#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

// Steprant 2.1.6: the rendezvous of the game's audio workers (sub_82CC3C50).
//
// The audio engine's worker threads (entries sub_82CC4D98 and sub_82CC4E60,
// both through sub_82CC4710, which also calls XAudio category volumes and
// KeReleaseSemaphore) meet at sub_82CC3C50 up to five times per pass. With
// r3 = the engine object and r4 = an 8-byte flag array, one byte per Xbox
// hardware thread:
//   - if the word at r3+0x130 is 0, return;
//   - flags[processor number at r13+0x10c] = 1;
//   - expected = the bytes (r3+0x134+4*i != 0) for i = 0..5;
//   - re-read the 64-bit flag word until it is 0 (another participant has
//     closed the round: return) or equals expected (all arrived: store 0
//     and return).
// The re-read is a plain loop with no import call, so a waiting worker keeps
// its core fully busy. In the profile of the 16/11 17:26 session the loop
// was 78% of every guest block executed (1.56e12 iterations for 15.2 million
// calls); the two workers used 10-22% of a core in the recent sessions.
//
// Here the loop waits instead: a short spin, then (arm64) WFE on the word
// with the exclusive monitor armed, which sleeps the core until another
// core writes that cache line (a participant's flag, or the closing 0) or
// the kernel's timer event stream ticks. The flag word is then read again,
// so every exit happens on the same values the guest loop would have seen;
// no latency is added while participants arrive. Only a wait longer than
// wfeUs (2 ms, the audio engine would already be late) sleeps between
// checks. Memory accesses keep the guest's widths (one byte store, 64-bit
// loads and store). Registers end as the guest function leaves them
// (r9, r10, r11; cr6 and xer.ca when they are context fields). A
// misaligned flag array runs the original. The CI build checks the result
// against the generated function (tools/audio_barrier_equivalence.py).
//
// LO_AUDIO_BARRIER=0 runs the original loop; LO_AUDIO_BARRIER_SPIN_NS,
// _WFE_US and _SLEEP_US tune the phases. On by default on arm64 only.
namespace audio_barrier
{
struct Settings
{
    bool enabled = false;
    uint32_t spinNs = 2000;
    uint32_t wfeUs = 2000;
    uint32_t sleepUs = 100;
};

inline uint32_t EnvValue(const char* name, uint32_t fallback, uint32_t limit)
{
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    return (end && *end == 0 && parsed <= limit) ? uint32_t(parsed) : fallback;
}

inline const Settings& Config()
{
    static const Settings settings = [] {
        Settings s;
#if defined(__aarch64__)
        s.enabled = true;
#endif
        if (const char* value = std::getenv("LO_AUDIO_BARRIER"); value && value[0] && !value[1])
            s.enabled = value[0] == '1';
        s.spinNs = EnvValue("LO_AUDIO_BARRIER_SPIN_NS", s.spinNs, 1'000'000);
        s.wfeUs = EnvValue("LO_AUDIO_BARRIER_WFE_US", s.wfeUs, 1'000'000);
        s.sleepUs = std::max<uint32_t>(1, EnvValue("LO_AUDIO_BARRIER_SLEEP_US", s.sleepUs, 100'000));
        return s;
    }();
    return settings;
}

struct Stats
{
    std::atomic<uint64_t> calls{0}, waits{0}, waitNs{0}, over50us{0}, over1ms{0}, sleeps{0}, maxNs{0};
};
inline Stats g_stats;

inline int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline void CpuRelax()
{
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

inline uint64_t Load(const uint64_t* word) { return __atomic_load_n(word, __ATOMIC_ACQUIRE); }

#if defined(__aarch64__)
// The exclusive load arms the monitor on the word's cache line; any store
// to that line by another core clears it and generates the event WFE waits
// for (the pattern of the Linux kernel's smp_cond_load_acquire on arm64).
inline uint64_t LoadExclusive(const uint64_t* word)
{
    uint64_t value;
    asm volatile("ldaxr %0, [%1]" : "=r"(value) : "r"(word) : "memory");
    return value;
}
inline void ClearExclusive() { asm volatile("clrex" ::: "memory"); }
inline void WaitForEvent() { asm volatile("wfe" ::: "memory"); }
#endif

inline void PreciseSleeps()
{
#if defined(__linux__)
    // App threads sleep with 50 us of timer slack by default.
    static thread_local bool set = false;
    if (!set) { prctl(PR_SET_TIMERSLACK, 10'000UL, 0, 0, 0); set = true; }
#endif
}

// Waits until the raw (guest big-endian) 64-bit word is 0 or expectedRaw and
// returns the value that ended the wait.
inline uint64_t WaitForWord(const uint64_t* word, uint64_t expectedRaw, const Settings& s, Stats& st)
{
    const auto done = [&](uint64_t v) { return v == 0 || v == expectedRaw; };
    uint64_t v = Load(word);
    if (done(v)) return v;
    st.waits.fetch_add(1, std::memory_order_relaxed);
    const int64_t start = NowNs();
    int64_t now = start;
    while (now - start < int64_t(s.spinNs)) {
        for (int i = 0; i < 32; ++i) {
            CpuRelax();
            if (done(v = Load(word))) goto finished;
        }
        now = NowNs();
    }
#if defined(__aarch64__)
    while (now - start < int64_t(s.wfeUs) * 1000) {
        v = LoadExclusive(word);
        if (done(v)) { ClearExclusive(); goto finished; }
        WaitForEvent();
        if (done(v = Load(word))) goto finished;
        now = NowNs();
    }
#else
    while (now - start < int64_t(s.wfeUs) * 1000) {
        for (int i = 0; i < 32; ++i) {
            CpuRelax();
            if (done(v = Load(word))) goto finished;
        }
        std::this_thread::yield();
        if (done(v = Load(word))) goto finished;
        now = NowNs();
    }
#endif
    PreciseSleeps();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(s.sleepUs));
        st.sleeps.fetch_add(1, std::memory_order_relaxed);
        if (done(v = Load(word))) break;
    }
finished:
    const uint64_t waited = uint64_t(NowNs() - start);
    st.waitNs.fetch_add(waited, std::memory_order_relaxed);
    if (waited > 50'000) st.over50us.fetch_add(1, std::memory_order_relaxed);
    if (waited > 1'000'000) st.over1ms.fetch_add(1, std::memory_order_relaxed);
    uint64_t seen = st.maxNs.load(std::memory_order_relaxed);
    while (waited > seen && !st.maxNs.compare_exchange_weak(seen, waited, std::memory_order_relaxed)) {}
    return v;
}

#if defined(PPC_FUNC)
// The guest function, with the wait above in place of its re-read loop.
inline void Run(PPCContext& __restrict ctx, uint8_t* base, const Settings& s, Stats& st)
{
    const uint32_t object = ctx.r3.u32;
    const uint32_t flags = ctx.r4.u32;
    const auto finish = [&](bool pastEnable) {
        ctx.r11.u64 = 0;
#if !defined(PPC_CONFIG_CR_AS_LOCAL)
#if !defined(PPC_CONFIG_XER_AS_LOCAL)
        const uint8_t so = ctx.xer.so;
#else
        const uint8_t so = 0;
#endif
        ctx.cr6.lt = 0;
        ctx.cr6.gt = 0;
        ctx.cr6.eq = 1;
        ctx.cr6.so = so;
#endif
#if !defined(PPC_CONFIG_XER_AS_LOCAL)
        if (pastEnable) ctx.xer.ca = 0;
#else
        (void)pastEnable;
#endif
    };
    if (PPC_LOAD_U32(object + 0x130) == 0) {
        finish(false);
        return;
    }
    const uint8_t processor = PPC_LOAD_U8(ctx.r13.u32 + 0x10C);
    __atomic_store_n(base + uint32_t(flags + processor), uint8_t(1), __ATOMIC_RELEASE);
    uint64_t expected = 0;
    for (uint32_t i = 0; i < 6; ++i)
        if (PPC_LOAD_U32(object + 0x134 + 4 * i) != 0) expected |= uint64_t(1) << (56 - 8 * i);
    ctx.r9.u64 = expected & ~(uint64_t(0xFF) << 56);
    ctx.r10.u64 = expected;
    auto* word = reinterpret_cast<uint64_t*>(base + flags);
    const uint64_t seen = WaitForWord(word, __builtin_bswap64(expected), s, st);
    if (seen != 0) __atomic_store_n(word, uint64_t(0), __ATOMIC_RELEASE);
    finish(true);
}
#endif
}
