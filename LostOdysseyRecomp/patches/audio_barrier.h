#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdlib>
#include <thread>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
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
// its core busy. It is the most executed block of the 16/11 17:26 profile
// (1.56e12 executions for 15.2 million calls; a block of a few instructions,
// so that says how often, not how long). The two workers used 10-22% of a
// core in the recent sessions, work and waiting together.
//
// Here the loop waits in three phases, each ending as soon as the word reads
// 0 or expected:
//   1. spinNs of spinning with a pause hint (arrivals within microseconds);
//   2. up to wfeUs of WFE on the word (arm64), with the exclusive monitor
//      armed: the core is clock-gated until another core writes that cache
//      line. The kernel still counts the thread as running;
//   3. a futex sleep: the thread leaves the CPU, so the scheduler and the
//      frequency governor see it idle. Every participant arrives through
//      this override, so each flag store and each closing 0 bumps a host
//      sequence for the word and wakes its sleepers. A write made elsewhere
//      in guest code is still seen at the next timeout (sleepUs).
// Every exit happens on the values the guest loop would have seen. Memory
// accesses keep the guest's widths (one byte store, 64-bit loads and store).
// Registers end as the guest function leaves them (r9, r10, r11; cr6 and
// xer.ca when they are context fields). A misaligned flag array runs the
// original. The CI build checks the result against the generated function
// (tools/audio_barrier_equivalence.py).
//
// LO_AUDIO_BARRIER=0 runs the original loop; LO_AUDIO_BARRIER_SPIN_NS,
// _WFE_US and _SLEEP_US (futex timeout) tune the phases. On by default on
// arm64 only.
namespace audio_barrier
{
struct Settings
{
    bool enabled = false;
    uint32_t spinNs = 2000;
    uint32_t wfeUs = 50;
    uint32_t sleepUs = 1000;
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
    std::atomic<uint64_t> calls{0}, waits{0}, waitNs{0}, over50us{0}, over1ms{0};
    std::atomic<uint64_t> sleeps{0}, timeouts{0}, wakes{0}, maxNs{0};
};
inline Stats g_stats;

// Host-side wake channel per flag word (hashed; a collision only wakes a
// sleeper that then finds its word unchanged and sleeps again).
struct alignas(64) Slot
{
    std::atomic<uint32_t> sequence{0};
    std::atomic<uint32_t> sleepers{0};
};
inline Slot g_slots[64];
inline Slot& SlotFor(uint32_t flags) { return g_slots[((flags >> 3) * 0x9E3779B1u) >> 26]; }

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

inline uint64_t Load(const uint64_t* word) { return __atomic_load_n(word, __ATOMIC_SEQ_CST); }

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

// True when the sleep ran to its timeout.
inline bool SleepOn(std::atomic<uint32_t>& sequence, uint32_t seen, uint32_t us)
{
#if defined(__linux__)
    // App threads sleep with 50 us of timer slack by default.
    static thread_local bool slack = false;
    if (!slack) { prctl(PR_SET_TIMERSLACK, 10'000UL, 0, 0, 0); slack = true; }
    timespec timeout{time_t(us / 1'000'000), long(us % 1'000'000) * 1000};
    return syscall(SYS_futex, reinterpret_cast<uint32_t*>(&sequence), FUTEX_WAIT_PRIVATE, seen, &timeout, nullptr, 0) != 0 &&
        errno == ETIMEDOUT;
#else
    (void)sequence; (void)seen;
    std::this_thread::sleep_for(std::chrono::microseconds(us));
    return true;
#endif
}

// After a participant's write to the flag word: wake whoever sleeps on it.
inline void Notify(Slot& slot, Stats& st)
{
    slot.sequence.fetch_add(1, std::memory_order_seq_cst);
    if (slot.sleepers.load(std::memory_order_seq_cst) == 0) return;
    st.wakes.fetch_add(1, std::memory_order_relaxed);
#if defined(__linux__)
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&slot.sequence), FUTEX_WAKE_PRIVATE, INT_MAX, nullptr, nullptr, 0);
#endif
}

// Waits until the raw (guest big-endian) 64-bit word is 0 or expectedRaw and
// returns the value that ended the wait.
inline uint64_t WaitForWord(const uint64_t* word, uint64_t expectedRaw, Slot& slot, const Settings& s, Stats& st)
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
#endif
    for (;;) {
        // Registered before the sequence is read: a participant that writes
        // after this either moves the sequence (the futex returns at once)
        // or sees a sleeper and wakes it.
        slot.sleepers.fetch_add(1, std::memory_order_seq_cst);
        const uint32_t seen = slot.sequence.load(std::memory_order_seq_cst);
        if (done(v = Load(word))) { slot.sleepers.fetch_sub(1, std::memory_order_seq_cst); break; }
        const bool timedOut = SleepOn(slot.sequence, seen, s.sleepUs);
        slot.sleepers.fetch_sub(1, std::memory_order_seq_cst);
        st.sleeps.fetch_add(1, std::memory_order_relaxed);
        if (timedOut) st.timeouts.fetch_add(1, std::memory_order_relaxed);
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

// A participant's writes (also used by the host test).
inline void StoreFlag(uint8_t* byte, Slot& slot, Stats& st)
{
    __atomic_store_n(byte, uint8_t(1), __ATOMIC_SEQ_CST);
    Notify(slot, st);
}
inline void CloseRound(uint64_t* word, Slot& slot, Stats& st)
{
    __atomic_store_n(word, uint64_t(0), __ATOMIC_SEQ_CST);
    Notify(slot, st);
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
    Slot& slot = SlotFor(flags);
    const uint8_t processor = PPC_LOAD_U8(ctx.r13.u32 + 0x10C);
    StoreFlag(base + uint32_t(flags + processor), slot, st);
    uint64_t expected = 0;
    for (uint32_t i = 0; i < 6; ++i)
        if (PPC_LOAD_U32(object + 0x134 + 4 * i) != 0) expected |= uint64_t(1) << (56 - 8 * i);
    ctx.r9.u64 = expected & ~(uint64_t(0xFF) << 56);
    ctx.r10.u64 = expected;
    auto* word = reinterpret_cast<uint64_t*>(base + flags);
    const uint64_t seen = WaitForWord(word, __builtin_bswap64(expected), slot, s, st);
    if (seen != 0) CloseRound(word, slot, st);
    finish(true);
}
#endif
}
