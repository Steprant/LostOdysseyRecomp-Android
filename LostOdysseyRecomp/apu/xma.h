#pragma once

// XMA hardware decoder emulation, modelled on Xenia's XmaDecoder /
// XmaContextFake (BSD). The title only imports XMACreateContext and
// XMAReleaseContext; everything else happens through the 320-entry context
// array in physical memory and the MMIO registers at 0x7FEA0000, which the
// statically linked XAudio driver pokes directly (stwbrx / lwbrx). Guest MMIO
// commands are dispatched on each store; a worker decodes enabled contexts.
//
// Assemble XMA frames across packet/input-buffer boundaries and decode them
// with Xenia's FFmpeg XMAFRAMES codec. The guest consumes big-endian int16
// subframes from its output ring.

#include <cstdint>
#include "xma_frame_join.h"

namespace apu::xma
{
    constexpr uint32_t kContextCount = 320;
    constexpr uint32_t kContextSize = 64;
    constexpr uint32_t kRegisterBase = 0x7FEA0000;

    // Value is the little-endian register value after the MMIO byte swap.
    bool WriteCommand(uint32_t address, uint32_t value);

    void Init();
    void Shutdown();

    // XMACreateContext: guest virtual address of a free context, or 0.
    uint32_t AllocateContext();
    // XMAReleaseContext.
    void ReleaseContext(uint32_t guestAddress);

    // The calling thread's waits for the decoder mutex are reported
    // separately (the audio driver: a wait there delays the next frame).
    // Per-context decoder locks instead of one global lock (before Init).
    void SetContextLocks(bool enabled);
    void MarkLatencySensitiveThread();
    // Decoder mutex peaks since the previous call, in microseconds: the
    // longest wait by any command, by a latency-sensitive thread, the longest
    // hold by the worker, and contended latency-sensitive acquisitions.
    struct LockStats
    {
        uint64_t waitMaxUs = 0;
        uint64_t waitLatencySensitiveMaxUs = 0;
        uint64_t holdMaxUs = 0;
        uint32_t contendedLatencySensitive = 0;
    };
    LockStats TakeLockStats();
    // The same peaks without resetting them (diagnostics between reports).
    LockStats PeekLockStats();
    // Steprant 2.0.2 D13: frames sent to the decoder since the start, the
    // ones it rejected and what was done about them (xma_frame_join.h).
    DecodeSummary GetDecodeSummary();
}
