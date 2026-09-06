# Wasmtime differential tests

This directory has two deliberately separate paths:

1. **Automated differential execution** uses explicit Wasm exports and guest
   linear-memory traces. It never attaches a native debugger or reads host CPU
   registers.
2. **Interactive Guest/Wasm-only debugging** uses the Wasmtime 44+ built-in
   guest gdbstub and a Wasm-aware LLDB. LLDB sees Wasm instructions, locals,
   source locations, and linear memory rather than JIT-native AArch64/x86-64
   state.

## Build and run every scalar family

Activate Emscripten, install a Wasmtime 44+ CLI, then run:

```sh
python3 tests/differential/run_all.py
```

The authoritative CI version is Wasmtime 48.0.0 LTS. The five scalar families
are:

- `geometry_hit_test`
- `psb_rl_decompress`
- `bezier_curve`
- `local_transform`
- `position_interp`

Use `--family` to restrict the run, `--no-build` to reuse modules, or
`--build-only` to compile without executing them:

```sh
python3 tests/differential/run_all.py \
  --family geometry_hit_test \
  --family local_transform
```

The Python Wasmtime embedding remains available as a fallback:

```sh
python3 tests/differential/python/run_scalar_wasmtime_direct.py \
  --family geometry_hit_test \
  --family local_transform
```

It is not the authoritative portable lane because an embedded JIT can inherit
host-process execution restrictions that do not affect the standalone CLI.

## Interactive Wasm bytecode debugging

Requirements:

- Wasmtime 44.0.0 or newer; official release CLIs include the guest gdbstub.
- A recent LLDB with the WebAssembly process plugin. Use the LLDB distributed
  by wasi-sdk; most system LLDB builds do not provide `--plugin wasm`.
- A module built with `-g3 -O0 --profiling-funcs` for source/variable mapping.

Guest debugging does not require adding a probe function to the C/C++ source.
The example below sets a breakpoint directly on the real exported
`krkr2_hit_test_run` implementation. The explicit result exports used by the
automated differential lane belong to the test ABI, not to the debugger.

Print the commands without launching anything:

```sh
python3 tests/differential/python/run_wasm_guest_debug.py \
  --print-only \
  --invoke krkr2_hit_test_run \
  --breakpoint krkr2_hit_test_run \
  tests/differential/wasmtime/geometry_hit_test.wasm \
  -- 1 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0
```

Start the Wasmtime guest debug server and connect from another terminal:

```sh
python3 tests/differential/python/run_wasm_guest_debug.py \
  --invoke krkr2_hit_test_run \
  --breakpoint krkr2_hit_test_run \
  tests/differential/wasmtime/geometry_hit_test.wasm \
  -- 1 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0
```

```text
(lldb) process connect --plugin wasm connect://127.0.0.1:1234
(lldb) breakpoint set --name krkr2_hit_test_run
(lldb) continue
(lldb) thread step-inst
```

Or let the launcher start wasi-sdk LLDB:

```sh
python3 tests/differential/python/run_wasm_guest_debug.py \
  --launch-lldb \
  --lldb /opt/wasi-sdk/bin/lldb \
  --invoke krkr2_hit_test_run \
  --breakpoint krkr2_hit_test_run \
  tests/differential/wasmtime/geometry_hit_test.wasm \
  -- 1 0 0 0 0 1 0 0 0 0 0 0 0 0 0 0 0 0
```

## Full motion playback

The paired sampling contract is defined by semantic call boundaries:

| Cases / route | Boundary |
|---|---|
| `yuzulogo`, `m2logo` node state | Every Player's final phase-3 helper return, before cleanup; copied immediately in actual helper-return order |
| Ordinary Layer execution | Entire `renderToCanvas` call, including command construction and local accessor destruction |
| Accurate SeparateLayerAdaptor execution | Entire accurate-SLA renderer call, before its post-draw update |
| Scalar cases | Synchronous function return (unchanged) |

Motion frame windows cover the script-visible `progress` callback, matching
Frida; engine-owned calls to the shared progress bridge do not create frames.
The normalized frames retain `samplePoint`, `sampleOrder`, observed `deltaMs`,
and `playerLayerCounts`. Comparisons reject missing or different sampling
evidence before comparing node values. The 15 Hz setting is a clock cadence,
not evidence that each observed delta is identical: the first delta is zero
and subsequent integer millisecond deltas must match between the two captures.

Render events carry `captureContract: motion-render-boundaries-v1` and identify
ordinary Canvas versus accurate SLA through `executeBoundary`. The comparator
uses the original sequence numbers to compare interleaved prepare/build/execute
boundaries, in addition to each stage's own event order. Historic artifacts
without this metadata must be re-recorded; do not stamp the new contract onto
an old capture. The Android 1.3.9 lane currently supports semantic events only,
not paired image checkpoints.

`krkr2_wasmtime_guest.wasm` imports the custom Emscripten/GL host environment
implemented in `tests/differential/python/run_motion_playback_wasmtime.py`, so
it cannot be instantiated by a plain `wasmtime run` command. Its automated
path records motion and render events inside the Wasm guest and reads them via
the following exports:

- `krkr2_wasm_get_motion_trace_ptr`
- `krkr2_wasm_get_motion_trace_len`
- `krkr2_wasm_clear_motion_trace`

Optional logo-chain logs use the `env.krkr2_wasmtime_logo_trace_enabled` import.
Set `KRKR2_WASMTIME_TRACE_LOGO_CHAIN=1` in the Python host to enable them. This
switch is independent of the motion sampling exports above, which remain active
without it. Browser URL switches and browser motion/layer JSON output have been
removed; the host no longer inspects embedded JavaScript to enable diagnostics.

This is architecture-neutral and is the only automated motion-playback path.
Interactive Wasm-only stepping of the full guest would require a Wasmtime Rust
embedder that supplies the same custom imports while enabling guest debugging;
the old native-JIT LLDB register sampler is intentionally not retained.
