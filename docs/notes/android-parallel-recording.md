# Parallel Vulkan recording (Steprant 2.1.5)

Reference. Branch `steprant/v2.1.5`. Plume overlay `tools/patches/plume-lostodyssey.patch` (`parallel_record` in `plume_vulkan.cpp`), runtime `gpu/vulkan_command_recording.h`, `gpu/video.cpp` (`SubmitVulkan`), `gpu/renderer.cpp` (slot lists), `gpu/pass_profile.h`.

## What it changes

Since 2.0 the frame graph keeps every command of a list and replays it into the Vulkan command buffer when the list ends. That replay runs on the GPU command thread and does two kinds of work: Plume's own logic (render pass begins and ends, look-ahead, deferred copies, layouts, render areas) and the driver calls it makes (`vkCmdDraw*`, `vkCmdBindDescriptorSets`, `vkCmdPushConstants`, render pass begins, barriers...). On Turnip the driver calls are the heavier part: each draw emits its state into the GPU command stream.

For the renderer's slot lists the replay that ends the list now records in parallel:

1. The command thread runs the same replay, in the same order, through the same Plume code. Every driver call it makes is encoded instead of executed, with its arguments resolved (barrier arrays, clear values, push constant bytes, dynamic offsets), into a chunk.
2. A chunk closes at the first point outside a render pass and outside queries once it holds enough calls, and goes to the helper threads (`GPU Record 1`, `GPU Record 2`). Each helper records its chunks into primary command buffers of its own while the command thread goes on with the replay.
3. When the replay ends, the command thread records the chunks no helper has taken yet, then waits for the rest. `end()` returns only when every chunk is recorded, so everything after it (submit, fences, resource lifetimes) is unchanged.
4. The submission adds the chunks' command buffers after the list's own, in stream order (`VulkanCommandList::appendSubmitBuffers`, used by `SubmitVulkan` and Plume's `executeCommandLists`).

A new command buffer starts with no state. Each chunk therefore starts by binding again what the stream had bound at its start: graphics and compute pipelines, descriptor set binds (whole calls, with their dynamic offsets), push constants, vertex and index buffers, viewports, scissors, depth bias, blend constants, stencil values and the extended dynamic state of linked pipelines. They are re-emitted in the order the stream set them, so the order-dependent Vulkan rules (descriptor sets disturbed by incompatible layouts, pipelines with static state) end the same way.

Plume's logic and every object it changes (texture layouts, `undefinedContent`, render pass variants created on first use, profile counters) stay on the command thread. The helpers only make driver calls, from bytes the command thread wrote before handing the chunk over (under a mutex). Nothing about what the GPU executes changes: the same calls in the same order, split into several command buffers at points outside render passes.

## When it applies

- Lists that allow it: the renderer's `list`, `srIsolated` and `srContinuation` of each GPU slot. The presentation list and every other list record as before.
- Only the replay that ends the list (`end()`, `EndCommands`), and only when nothing was recorded directly into the list's command buffer since it began (no external commands, no earlier replay into it): the chunks must start from no bound state.
- Only replays of at least `LO_VK_PARALLEL_RECORD_MIN` commands (4096): small lists stay direct.

The helpers start on the first thread that records in parallel (the GPU command thread) and keep its CPU affinity and priority: the performance cores.

## Switches (environment)

| Variable | Default | Meaning |
|---|---|---|
| `LO_VK_PARALLEL_RECORD` | on (Android), off elsewhere | `0` records directly; `1` turns it on outside Android (needs `LO_VK_FRAME_GRAPH=1` there) |
| `LO_VK_PARALLEL_RECORD_THREADS` | 2 | helper threads (1-6) |
| `LO_VK_PARALLEL_RECORD_MIN` | 4096 | replayed commands before a list records in parallel |
| `LO_VK_PARALLEL_RECORD_CHUNK` | 0 | driver calls per chunk; 0: about two chunks per recording thread, at least 256 |

## Measuring it on the phone

- `threads.csv`: the `GPU CmdProc` thread's CPU time per frame should drop by about the driver's share of the replay; `GPU Record 1/2` appear with that time.
- `runtime.log`, `frame graph replay:` (heavy-profiling bursts): with parallel recording the kinds are the command thread's encoding (no driver inside), and the line ends with `| parallel: replays= chunks= helpers_ms= own=chunks/ms wait_ms=` per frame. `driver_ms` reads 0 for such replays. Compare `total` and `wait_ms` with a session with `LO_VK_PARALLEL_RECORD=0`.
- The scene to watch is the split-screen one with two cameras (about 20 FPS in 2.1, command thread 32-38 ms per frame).

## Tests (lavapipe)

- `pass_sequence_test`: three more configurations, frame graph with parallel recording (also poisoned discards, and no deferral with 3 helpers), chunks as small as possible (`_CHUNK=1`: every point outside a pass ends a chunk, about 115 command buffers per replay). Every target matches the CPU model; each replay must have been split into chunks.
- `frame_graph_draw_test`: two more configurations (chunks of 1 and of 3 with 3 helpers, poisoned discards). Frames with draws, blending, sampling through descriptor sets, push constants and depth: byte-identical to direct recording, about 80 command buffers per frame, each binding again pipeline, set, push constants, viewport and scissor.
- Both pass under AddressSanitizer and ThreadSanitizer (reports from inside lavapipe and LLVM suppressed; none from Plume or the helpers).
- `pass_profile_test`: the `parallel:` part of the replay line.

On lavapipe, whose driver calls only copy their arguments, a 3,000-draw list ended in 0.94 ms recorded directly and 0.67 ms in parallel with one helper (2-core host). On Turnip the driver does more per call; the phone session decides.

Not measured on the phone yet.
