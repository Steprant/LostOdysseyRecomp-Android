#pragma once
// Steprant 2.0, Super Roadmap 4.0 M-6: where the game is while it stalls.
//
// A host thread wakes every 5 ms. When no swap has happened for more than
// 60 ms (the game is loading or busy: cp_idle) it samples every running guest
// thread: the live LR (the function the thread is in) and the first two
// return addresses of its guest back chain. When the next swap arrives after
// a stall of at least 100 ms, one log line gives the most frequent places.
// The audio driver reports each guest callback; one running for more than
// 30 ms has its own thread sampled the same way, and a line follows when it
// ends after at least 50 ms. Reads of guest state are racy and for diagnostics
// only; pauses of the game (overlay, background) are left out. Lines are
// limited per session. LO_STALL_SAMPLER=0 turns the thread off.
#include <cstdint>

struct PPCContext;

namespace diagnostics::stall_sampler
{
// GPU command thread, at each guest swap.
void NoteSwap();
// Audio driver thread, around each guest audio callback (ctx: the context the
// callback runs on; nullptr at the end).
void NoteAudioCallback(PPCContext* ctx);
}
