# General register promotion and native memcpy/memset (Steprant 2.2)

Reference. Branch `steprant/v2.2`. Generator: `tools/patches/XenonRecomp-lostodyssey.patch` (`registers_in_context_functions`). Config: `LostOdysseyRecompLib/config/LostOdysseyRecomp.toml`, `registers_in_context.toml`, `register_check.toml`. Check: `tools/ppc_register_check.py`. Native CRT: `LostOdysseyRecomp/patches/crt_native.h/.cpp`, `tools/crt_native_equivalence.py`.

## What changes

Until 2.1.6 every general, float and vector register of the translated game lived in `PPCContext`: each use was a load or a store of the context, kept around every call because the callee receives the same context. 2.1.5 made CR, XER and the reservation function locals. 2.2 does the same for the rest of the registers the ABI allows:

- `non_argument_as_local`: r0, r2, r11, r12, f0, VMX128 v32-v63;
- `non_volatile_as_local`: r14-r31, f14-f31, v14-v31, v64-v127.

The argument registers (r3-r10, f1-f13, v0-v13), r1 and r13 stay in the context. With the non-volatile registers as locals, the calls to the save/restore helpers (`__savegprlr_N`, `__restgprlr_N`, `__savefpr_N`, `__restfpr_N`, `__savevmx_N`, `__restvmx_N`) go away: the compiler saves what it needs in host registers. In the 16/11 profile those helpers alone were about 26 million calls per second.

Measured on the hottest 46 source files (97% of the guest blocks of the profile), compiled for arm64 with clang 19 at -O3, without PGO in both cases: dynamic instructions about -20% (about -10% inside the functions, plus the helpers, which were 11.7% of the guest instructions), memory accesses -29%, calls -18%, code -12.6%. Time saved is less than instruction count: what goes is mostly loads, stores, byte swaps and calls.

## Where it is not exact, and what keeps it exact

A local is exact where every read of a register sees a value the same function wrote. `tools/ppc_register_check.py` follows every register through the control flow of every generated function (63k) and finds the reads that can see a value from outside:

- from the caller: read from the entry with no write first. 13,105 are the C++ exception unwind funclets (they read the frame from r12; exceptions are not supported, so they never run); the rest are about 75 functions, mostly in 0x82FA-0x8303 and two CRT pieces next to memcpy;
- from a callee: a volatile register read after a call that may change it. The Xbox compiler knew which registers some callees leave alone (f0 kept across a call, the stack probe and r12) and one function returns a value in r11 (`sub_82F99F48`, the same chain that keeps cr0 in the context). Callees that never write the register (transitively) do not count;
- registers read by host patches (`gpu/cinematic_bars.cpp`, `patches/occlusion_history.cpp`, `patches/battle_script_timer.cpp`, `debug/asset_failure.cpp`): `register_check.toml`, `site_reads`.

Those functions keep every register in the context, as before, together with the callers that pass them a value (followed upwards while the value at the call site is still the caller's own entry value), the callees that return one (followed downwards), and every function with setjmp (`ctx = env` restores the context, not locals). The patched XenonRecomp reads the list from `registers_in_context.toml` (13,352 functions, all but about 250 of them funclets) and keeps the helper calls in those functions. With a list in use the context keeps all its register fields, so host code that reads them (crash dumps, debug traces) still compiles.

In the profile, the functions that keep their registers in the context are 99 executed ones out of 20,886, about 0.2% of the guest blocks.

Not covered by the analysis:

- a function reached only through a pointer cannot be followed to its callers: 4 executed ones (and the entry point `_xstart`) need a register from their caller. Each chain ends in a large function (`sub_82FDB200`, 23 levels down, and `sub_82824AD8`) where the read sits on a path the data flow cannot rule out but that only correlated branches would take: the paths that set the register are the ones that read it. The context keeps these functions; from an indirect caller they would see the last value written to the context;
- `sub_83111958` and `sub_83111CA0` store and load all VMX128 registers (a vector context save or switch, never executed): with locals there is nothing to save; a real context switch cannot work in translated code anyway.

`android-apk.yml` runs `ppc_register_check.py check` on every build (strict): it fails if a local can see an outside value, if a function the analysis needs is missing from the list, or if a caller reads a register the native memcpy/memset leave unchanged. When the generated code changes (new boundaries, new hooks), regenerate the list:

```
python tools/ppc_register_check.py analyse <sources generated with both options off> \
    --config LostOdysseyRecompLib/config/register_check.toml \
    --write-list LostOdysseyRecompLib/config/registers_in_context.toml
```

The workflow input `promotion=off` turns these two options off together with CR/XER/reservation.

## Native memcpy, memset and stack probe

- `sub_82B7A0B0`, memcpy: the game function with the most executed blocks (3.8%), 731k calls per second. Translated, every 8 bytes are a volatile load, two byte swaps that cancel and a volatile store. Now `std::memcpy`, with the doubleword the guest code saves at r1 - 8 and r3 read back from it. Overlapping regions and ranges past 4 GiB run the generated code.
- `sub_82B7BC40`, memset: `std::memset` with the low byte of r4; sizes past 32 bits run the generated code.
- `sub_82B7E504`, the stack probe: a no-op (guest stacks are committed memory).

`tools/crt_native_equivalence.py` runs the generated memcpy and memset next to the host versions on every build: 40,000 memcpy cases (sizes 0-4096, every alignment, overlapping, over the saved doubleword, high bits in r5) and 27,000 memset cases, all of memory and r3 compared byte for byte. The volatile registers the guest code also leaves changed are not reproduced; the register check proves no caller reads them. `LO_CRT_NATIVE=0` runs the generated code.

## PGO

The function hashes of the PGO profile depend on the control flow, which promotion mostly leaves alone: with the 16/11 profile, in the files checked, 75% of the functions (94% weighted by calls) keep their profile; the others compile as without one. A new profile from the 2.2 code restores them all.
