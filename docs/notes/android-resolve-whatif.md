# Resolve what-if: which resolve copies could be avoided (Steprant 2.1.5)

Investigation tool. Branch `steprant/v2.1.5`. Code: `gpu/resolve_whatif.h` (the classes, host test `tools/tests/resolve_whatif_test.cpp`), hooks in `gpu/renderer.cpp`. Measurement only: nothing recorded changes.

## Why

A resolve copies a render target (EDRAM) into the texture the game samples next: the scene for post-processing and presentation, depth for fog and depth of field, the bloom and blur steps. Each copy reads and writes a whole image (at 900p a full-screen FP16 copy moves about 29 MB). In October these copies cost 2-5 ms of GPU per frame (`render-pass-merging-design-2026-10-10.md`, "copy-on-write for resolve copies", stage 3, never built). The same note warned that the main EDRAM area is written several times per frame, so many copies may be unavoidable. This tool counts how many really are, before building copy-free resolves.

## What it measures

Every resolve is followed until its surface is resolved again (or dropped) and then counted in one class:

| Class | Meaning | What would avoid the copy |
|---|---|---|
| `unread` | never looked up | skipping it |
| `free` | same host format, whole surface, same size as the source, never looked up after its source was next written | sampling the source image itself |
| `rename` | as `free`, but looked up after the source was next written by a clear of the whole source | giving the source a fresh image at that clear |
| `copy` | as `free`, but looked up after the source was partly written (a draw, a partial clear) | nothing: a copy is still needed, only later |
| `format` | a conversion to another host format (shader copy) | nothing without changing the result |
| `partial` | not the whole surface | nothing simple |
| `size` | whole surface, but the source has another size (padding) | sampling with scaled coordinates (shader change) |

Depth resolves use the same classes (an eligible one would be sampled through a depth view of the source). Copies made for cropped fetch views (a sampled size smaller than the resolve, such as the 428-wide blur in a 448-pixel surface) are counted apart.

A lookup is any use of the surface: draw fetches, presentation, diagnostics. "Looked up after the write" therefore errs towards `copy`: the avoidable numbers are a lower bound.

## The log line

Every 10 s in `runtime.log`, per frame:

```
resolve what-if: frames= per_frame: resolves= mb= (copy= blit= depth=) | avoidable: unread=n/mb free=n/mb rename=n/mb | kept: copy=n/mb format=n/mb partial=n/mb size=n/mb | cropped_views=n/mb | avoidable_mb= (%)
```

`mb` counts bytes read plus bytes written by the copies. As a rough conversion on the Adreno 750, one MB of copy is about 0.02-0.03 ms of GPU.

`LO_RESOLVE_WHATIF=0` turns it off.

## Decision

Per scene (city, field, battle, TV):

- `avoidable_mb` of 25 MB per frame or more (at least one full-screen HDR copy): copy-free resolves are worth building (aliasing for `free`, renaming for `rename`, skipping `unread`).
- Under 10 MB: not worth it; the resolve copies stay.
- In between: decide with the GPU time of the copies from the pass profile.

Not measured on the phone yet.

## Steprant 2.2: what deferral would save, and which surfaces

An unread copy can only be skipped by deferring it until the surface is read (and dropping it when the surface is resolved again). If the source is written in the meantime, the deferred copy must be made at that write, before the source changes, and nothing is saved. Measured on 16/11: about 75 MB per frame of unread copies, but not whether their sources stayed intact.

The line now ends with:

```
... | unread_intact=n/mb | unread_top: base=0x... fmt=N n= mb= intact=P%; (three surfaces)
```

- `unread_intact`: the unread copies whose source was not written until the surface was resolved again or dropped. Only these would be saved by deferred resolves.
- `unread_top`: the three destination surfaces with the most unread bytes (destination base and format), with the share of their unread copies that kept the source intact.

Decision rule as before, on `unread_intact` (plus `free` and `rename`) instead of all unread bytes.
