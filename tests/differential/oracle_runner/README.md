# ADB + Frida Oracle Runner

> **Mixed status.** The active `trace_flatten` motion oracle is rebased to
> Kirikiroid2 1.3.9 Android arm64-v8a `libgame.so`. Other checked-in native
> offsets are historical and remain disabled until independently rebased.

Runs Kirikiroid2 1.3.9 `libgame.so` inside the repacked
`krkr2-harness.apk` on a real Android arm64 device or Redroid
container, driven from the host over `adb forward tcp:5039` +
`am start HarnessActivity`. Provides a return-value probe against the
specs and an optional local call-sequence tracer:

1. **Return-value diff** — the host pokes function calls into
   [libharness.so](harness/) loaded by the APK, reads return values,
   compares against the spec's `"expected"`.
2. **Call-sequence capture** — optional per-case Frida tracer attaches
   to the `HarnessActivity` process and hooks a curated set of
   sub-function offsets. Captures under `tests/differential/traces/`
   are generated local files and are ignored by Git.

The active differential workflow does not use scalar trace goldens. It
records fresh Android and Wasmtime `motion_playback` traces in the same
run, exchanges them as CI artifacts, and compares them directly.

For `motion_playback`, this directory also contains a libgame-side
recording path that captures Motion.Player per-frame state from natural
playback on the cocos2d GL thread. That path is useful as a state oracle,
but it is not yet a final visual oracle: it does not capture the
framebuffer, draw commands, texture identity, shader/blend state, or
pixel output.

## Status

| Family | Oracle path | Repository goldens | Notes |
|---|---|---|---|
| `geometry_hit_test` | **✓ 10/10** | — | `Player_hitTest` (0x690DF0), pure C leaf |
| `local_transform` | **✓ 8/8** | — | `sub_699940` (0x699940), libm sin/cos used by `rotate_90` |
| `bezier_curve` | **✓ 6/6** | — | `sub_69A754` (0x69A754). `empty_curve` + `size_mismatch` specs dropped — UB inputs (empty or mismatched arrays) where libkrkr2's behaviour is an OOB-read side effect / infinite loop rather than a designed contract; oracle doesn't apply |
| `position_interp` | **✓ 5/5** | — | `sub_69A4D4` (0x69A4D4). Adapter had `src_addr`/`dst_addr` wired into a2/a3 — libkrkr2's convention (matching port's `interpolatePosition69A4D4` signature) is a2=dst (returned at t=1), a3=src (returned at t=0). `rotation_coord*` specs dropped — empty `segments` arrays SIGSEGV inside libkrkr2's `sub_698454` (latent libkrkr2 bug, never hit by real assets); port's defensive sanitisation is intentionally non-matching |
| `psbfile_load` | committed raw PSB or existing reference material | — | Directly invokes `PSBFile.load(octet)` (0x598268), verifies the 0x68 raw owner and strict header refresh (0x598960); optional `--storage` covers 0x598538. Natural value modes cover integer tags 0x04..0x09, Real tags 0x1D..0x1F, String tags 0x15/0x16, Null, Array, and Dictionary through public TJS dispatch and raw getters/classification. `--shape-boundary` additionally verifies hidden-sret raw `GetRoot`/`Transfer`, raw Dictionary strict/non-strict/alias ownership, `NativeInstanceSupport`, the full 32-slot primary dispatch vtable, the secondary native lifecycle vtable, all 19 unsupported primary slots, `IsInstanceOf`, ordered value/no-value `EnumMembers`, dispatch/owner intrusive lifetimes, and invalidation edges; `--resource-boundary` verifies copied TJS Octet versus borrowed raw Resource. Media modes cover the exact 11-slot media vtable, intrusive reference/destructor boundaries, name normalization, replacement, borrowed-stream destruction, Dictionary order, and the null-adaptor boundary. `--trace` records the native call chains. No damaged fixture is generated or checked in |
| `psb_rl_decompress` | — | — | RL loop is inlined in a 53 KB PSB loader; no standalone entry, no adapter |
| `motion_playback` | fresh 1.3.9 libgame record + Wasmtime verify | — | Uses `STARTUP_FROM` to schedule the per-case `reference/xp3/logo_test_oracle_<case>_15hz.xp3` fixture inside libgame. The clock advances on a 15 Hz grid; script progress observes an initial zero delta and integer-ms intervals. Both recorders freeze node values after the final phase-3 helper and before cleanup, retaining observed deltas and sample order. The port verifier executes the same XP3/TJS path under Wasmtime. This is not yet a full visual oracle; see "Motion playback visual oracle status" below. |

## Motion playback visual oracle status

Target goal: use `tests/differential` to prove that the current port's
final visual output while playing `reference/xp3/logo_test/yuzulogo.mtn`
and `reference/xp3/logo_test/m2logo.mtn` matches 1.3.9 `libgame.so`.

Current oracle-runner side status:

- The runner can launch the real repacked Android APK and execute
  libgame on the same cocos2d/Java activity path used by the original
  app.
- `libharness.so` exposes `STARTUP_FROM <utf8_hex_path>`, constructs a
  real gnustl `std::string`, and calls
  `TVPMainScene::startupFrom(const std::string&)`. This avoids the old
  Python-side fake `std::string` ABI risk.
- The recording fixtures are
  `reference/xp3/logo_test_oracle_{yuzulogo,m2logo}_15hz.xp3`,
  deterministic wrappers around the `logo_test.xp3` playback path. They
  preserve the KAGParser `[ev]` / `[ev waitmovie]` boundary and drive
  `.mtn` playback through `AffineLayer` / `AffineSourceMotion` / `onPaint`,
  using a 15 Hz tick grid and the script's `tick - lastTick` delta. The TJS/KAG sources
  are versioned under `oracle_runner/fixtures/`; the large assets stay in the
  external `reference/xp3/logo_test` tree.
- `FridaMotionTracer` attaches to the harness process and the in-process
  JS agent hooks `Motion.Player.progress` and `Player_updateLayers`.
  Recording happens from natural playback on the GL thread; the host
  does not call Motion.Player methods from the RPC worker thread.
- CI records non-empty per-frame node state for both fixtures on every
  run and uploads the generated JSON files as the
  `motion_playback-oracle-traces` artifact.

What this proves today:

- It can produce a libgame baseline for Motion node evaluation:
  per-frame node count, node type, visibility/active flags, flip flags,
  accumulated position, scale, angle, opacity, and a limited blend-mode
  proxy.
- It is suitable as a state oracle for debugging the port's
  `Motion.Player` timeline and `Player_updateLayers` behaviour.

What it does not prove yet:

- It does not capture final framebuffer pixels or screenshots.
- It does not capture a complete draw-command stream, draw order
  contract, GL state, shader inputs, texture upload/sampling, clipping,
  mask/stencil behaviour, or blend results.
- `label` and `currentImage` in the current motion oracle schema are not
  populated with authoritative runtime names/textures, so texture
  identity and source image selection are not covered.
- The deterministic wrapper now uses the same AffineSourceMotion playback
  path as `logo_test.xp3`, but still fixes delta timing; it does not prove
  the original wall-clock timing behaviour.
- Normal push CI records both the Android oracle and Wasmtime port
  traces, then compares the two fresh artifacts directly. No
  `motion_playback` golden is stored in the repository.

Therefore, as of now, the oracle runner side is good enough to be a
libgame Motion state oracle for these two fixtures, but not enough to
claim final visual output equivalence. Reaching that goal requires
adding either framebuffer/pixel capture or a
render-command oracle that covers texture identity, draw order, clipping,
blend/stencil state, and final compositing.

## Motion tracer equivalence model

The Android Frida tracers and the macOS LLDB tracers are comparable only
as stage-specific semantic projections, not as proof that the two
runtimes share the same physical object layout.

- Android/Frida is the oracle side. It attaches to `libgame.so`, sets
  breakpoints or interceptors by binary address/offset, and reads fields
  from raw process memory using the libkrkr2 layout recovered from IDA.
- macOS/LLDB is the port side. It launches the full native engine,
  breaks on local C++ functions or source lines, and reads fields through
  debug symbols, typed expressions, or local helper projections.
- A field is considered comparable only when both tracers sample at the
  same logical stage boundary and project the same runtime meaning into
  the same JSON schema field. The comparison does not require identical
  pointer values, absolute sequence numbers, addresses, padding, STL
  layout, or private native-only bookkeeping.
- When adding or changing a motion stage tracer, document the hook point,
  the sampled object, and the field projection for both sides. If either
  side uses a fallback hook or a derived field, the stage is diagnostic
  only until the timing and projection are made explicit.

The Kirikiroid2 1.3.9 Android oracle enables `trace_flatten` plus the rebased
semantic `render_path` contract. `render_path` expands to exactly four
platform-independent stages: `draw_dispatch`, `render_prepare`,
`render_commands`, and `render_execute`. The Frida agent samples native
function boundaries, but it exports route/step/event semantics rather than
registers, pointers, native instructions, or pixels; the Wasmtime side exports
the same event model from Guest C++ scopes. The paired comparator therefore
does not depend on the host CPU architecture.

Layer/Bitmap layout-dependent image checkpoints, raw probes, visual readback,
and `layer_save` are deliberately outside the 1.3.9 semantic contract. The
Android runner fails closed if one of those options is requested. The detailed
four-binary address evidence and event mapping are recorded in
`analysis/motionplayer_render_path_semantic_oracle_1_3_9_2026-08-29.md`.

`trace_flatten` uses the `trace_flatten-semantic-v1` projection sampled at
`progressCompat.phase3-end.pre-cleanup`. Its comparable layer fields are:
`index`, `nodeType`, `visible`, `active`, `flipX`, `flipY`, `posX`,
`posY`, `posZ`, `angleDeg`, `scaleX`, `scaleY`, `slantX`, `slantY`,
`opacity`, and `stencilType`. The `stencilType` field is Android
`node+52` and native `MotionNode::stencilType`; it is intentionally not
named `blendMode` in the staged oracle schema.

Pointer values, `objthis`, `topPlayer`, player source ranges, traversal
layout, trace errors, and unsupported names/images are diagnostics only.
They may be stored under `diagnostics` for segmentation and debugging,
but they are not semantic `trace_flatten` diff fields.

## Prerequisites

**Kirikiroid2 1.3.9 APK** — local/Google Drive reference input:

```bash
# APK used for repacking; CI downloads this path from Google Drive.
reference/packages/Kirikiroid2_1.3.9.apk

# Its Android arm64 libgame.so matches this reference binary byte-for-byte.
reference/binaries/Kirikiroid2_1.3.9_Android_arm64-v8a.so
```

**Android device / Redroid** — API 24+ arm64-v8a. The ADB runners need
root so `frida-server` can attach to the non-debuggable APK. Current CI
uses `ubuntu-24.04-arm` with Redroid (`redroid/redroid:12.0.0_64only`)
so Android runs as an arm64 container sharing the host kernel. Local
development can use either Redroid or a rooted arm64 emulator.

**Harness APK** — the repacked `krkr2-harness.apk` contains
`libharness.so` (arm64, NDK r17c + `gnustl_static`) and a minimal `HarnessActivity`
that extends `Cocos2dxActivity`. Build with
[harness-apk/build.sh](harness-apk/build.sh); rebuild instructions for
the native .so live in [harness/README.md](harness/README.md).

**Python deps**:

```bash
pip install -r tests/differential/oracle_runner/requirements-oracle.txt
# → frida==16.4.10 (only needed when using --trace / --record-trace)
pip install -r tests/differential/python/requirements-wasm.txt
```

**Frida server** (for `--trace` and `motion_playback --record-oracle`
mode) — pinned to match `frida-python`:

```bash
# Operator step, idempotent
curl -L -o /tmp/frida-server.xz \
  https://github.com/frida/frida/releases/download/16.4.10/frida-server-16.4.10-android-arm64.xz
xz -d /tmp/frida-server.xz
mv /tmp/frida-server tools/bin/android/frida-server
```

## Running

### One-time provisioning on device

```bash
export PATH=$ANDROID_SDK_ROOT/platform-tools:$PATH
adb root && adb wait-for-device
adb push tools/bin/android/frida-server   /data/local/tmp/
adb shell "chmod 755 /data/local/tmp/frida-server"
adb shell "nohup /data/local/tmp/frida-server -D >/dev/null 2>&1 &"

# Build + install the harness APK (packages libharness.so inside).
export KRKR2_LEGACY_NDK=/path/to/android-ndk-r17c
./tests/differential/oracle_runner/harness/build_legacy.sh
./tests/differential/oracle_runner/harness-apk/build.sh
adb install -r tests/differential/oracle_runner/harness-apk/prebuilt/krkr2-harness.apk
```

### Return-value diff only (no Frida)

```bash
python3 tests/differential/python/run_geometry_hit_test_adb.py \
  --spec-dir tests/differential/specs/geometry_hit_test
```

Output: one JSON line per case on stdout (`runner: android-adb-oracle`);
mismatches on stderr; exit 0 iff all match.

### PSBFile raw/MDF oracle with an existing file

The committed `tests/test_files/emote/ezsave.pimg` provides a reproducible raw
`PSB\0` case. The repository also contains natural valid `mdf\0` inputs under
`reference/xp3/{caution_test,dracu_boot}/DRACU-RIOT/scn/`; for example,
`scenelist.scn` decompresses to `PSB\0`. These cover the MDF-success path only;
zlib-failure still requires an existing naturally failing input supplied
explicitly. Repeat `--input` to inspect more than one; this path never generates
or mutates PSB material.

```bash
python3 tests/differential/python/run_psbfile_load_adb.py \
  --input reference/xp3/caution_test/DRACU-RIOT/scn/scenelist.scn \
  --storage --trace

python3 tests/differential/python/run_psbfile_load_adb.py \
  --input tests/test_files/emote/e-mote3.0バニラパジャマa.psb \
  --decrypt-seed 742877301 --trace

python3 tests/differential/python/run_psbfile_load_adb.py \
  --integer-boundary \
  --startup-xp3 reference/xp3/caution_minimal/caution_minimal.xp3 \
  --trace

python3 tests/differential/python/run_psbfile_load_adb.py \
  --real-boundary --string-boundary \
  --startup-xp3 reference/xp3/caution_minimal/caution_minimal.xp3 \
  --trace

python3 tests/differential/python/run_psbfile_load_adb.py \
  --shape-boundary \
  --startup-xp3 reference/xp3/caution_minimal/caution_minimal.xp3 \
  --trace

python3 tests/differential/python/run_psbfile_load_adb.py \
  --resource-boundary \
  --startup-xp3 reference/xp3/caution_minimal/caution_minimal.xp3 \
  --trace

python3 tests/differential/python/run_psbfile_load_adb.py \
  --media-interface \
  --startup-xp3 reference/xp3/caution_minimal/caution_minimal.xp3 \
  --trace

python3 tests/differential/python/run_psbfile_load_adb.py \
  --media-lifecycle \
  --startup-xp3 reference/xp3/caution_minimal/caution_minimal.xp3 \
  --trace

python3 tests/differential/python/run_psbfile_load_adb.py \
  --media-dictionary \
  --startup-xp3 reference/xp3/caution_minimal/caution_minimal.xp3 \
  --trace

python3 tests/differential/python/run_psbfile_load_adb.py \
  --media-adaptor-null \
  --startup-xp3 reference/xp3/caution_minimal/caution_minimal.xp3 \
  --trace
```

The return-value checks cover the decoded `PSB\0` buffer size, intrusive owner
initial reference, inline header view, and strict offset refresh. `--storage`
also pushes a temporary copy to the configured device directory and exercises
0x598538's storage/stream path. `--trace` emits both native call sequences
without creating a golden. For an encrypted raw PSB, `--decrypt-seed` invokes
the Android filter call operator at 0x6863CC and compares every decrypted byte
with an independent host-side xorshift implementation before strict refresh.

`--integer-boundary` uses the existing natural `m2logo.mtn` and `autoskip.psb`,
pinned by SHA-256. Seven nodes cover tags 0x04..0x09: zero, negative signed
8/16-bit values, natural 24/32-bit values, and two tag-0x09 cases. The latter are
`0xFF000000 → GetInt(-16777216)` and the exact low-word edge
`0xFFFFFFFF → GetInt(-1)`. Each case constructs `new PSBFile(path)`, walks the
real root Dictionary/Array adaptors, and requires a real `tTJSVariant` of type
`tvtInteger` (`4`) with the full decoded value. An independent raw load invokes
the original `PSBRawNode::GetInt` (0x599438) and `GetDouble` (0x5992E8) on the
same pinned offset; X0 must equal the zero-extended W0 result, W0 must match the
signed low word, and D0 must equal the full value converted to double. With
`--trace`, each public chain includes 0x59B14C → 0x5980F4 → 0x598268 → 0x598538 →
0x598708 → 0x59B28C → 0x59B48C → 0x5981F8 → 0x597854/0x5976C4 → 0x59673C →
0xA0FF60, followed by the two direct raw getter probes. These observations do
not claim that optimized ARM64 alone uniquely reveals GetInt's source return
type spelling.

`--real-boundary` pins three natural nodes by physical-file SHA-256, decoded
offset, complete node bytes, and expected little-endian double bits. Tag `0x1D`
is the zero token, tag `0x1E` widens the exact float32 payload for `3.03`, and
tag `0x1F` reads the exact float64 payload for `-289.6` from an existing MDF
scene file. The MDF is decompressed only in memory to validate its decoded
offset; the device receives the unchanged original file. Each public path must
produce `tvtReal` (`5`) with identical payload bits, and direct
`PSBRawNode::GetDouble` (0x5992E8) must return the same D0 bits.

`--string-boundary` pins one natural tag-`0x15` node containing `レイヤ5` and
one tag-`0x16` node containing `src/ボタン/btsys_タイトルbtn_on`. The public
path creates a real `tvtString` (`2`), clears both script globals that own the
source PSBFile/value, then requires the independent `TJS_GLOBAL` Variant copy
to retain the same UTF-8 value. The raw path calls `PSBRawNode::GetString`
(0x598B58) and requires the result to equal the exact
`raw_owner_data + string_offset` pointer, retain its trailing NUL, and match the
same pinned SHA-256. This separates the copied public lifetime from the raw
borrowed pointer without modifying the input.

`--shape-boundary` uses the same immutable `m2logo.mtn` SHA-256 pin. A natural
tag-`0x01` node at `0x5365` (`source/logo/metadata`) must publish `tvtVoid`
(`0`) while direct `PSBRawNode::GetTypeCategory` (0x599554) returns category 0.
Before the TJS-backed cases, the raw holder case invokes `PSBFile::GetRoot`
(0x598A3C) and the stripped transfer helper (0x598A64) through `CALL_SRET`, whose
non-trivial result slot is supplied in AAPCS64 X8. `GetRoot` must publish the
same owner plus the independently decoded header entries pointer and change
owner refcount from 1 to 2. Transfer must move the source owner into its result,
clear the source, and keep the count at 2; transferring an empty holder must
produce another empty holder. Releasing the root result through Android's
one-pointer holder helper (0x695CBC) must reduce the count to 1 without clearing
the holder slot, and releasing the transferred result performs terminal owner
cleanup. No zero-ref owner or modified fixture is manufactured.
A second raw case uses the natural root Dictionary at decoded offset `0xB4B`.
Strict `object` lookup (0x598C58) must return a hidden-sret retained child at
`0xB72`; non-strict lookup (0x598D58) must retain the same child into a real
default-constructed output. A key absent from the global names trie and
`icon42` (globally named but absent from root) separately cover the first and
second packed-helper miss; both must preserve the complete output and owner
count. Reusing that output for `version@0x5399` must perform release-old then
retain-source with zero net count change. Passing the root as both self and
out must preserve Android's capture/release/reload-retain/write alias order,
mutate the node in place to `object`, and keep the count unchanged. Direct
IsValid (0x598E44) and ContainsDictionaryKey (0x5995D8) probes fix the exact
two-pointer predicate, temporary-node retain/release, Dictionary miss, and
non-Dictionary false boundaries. Final holder releases prove the full
`1 → 2 → 3 → 4 → 3 → 2 → 1 → terminal` chain.
A third raw case calls `GetDictionaryKeys` (0x598E64) through the 32-byte
`CALL_SRET` carrier and reads its live 24-byte Android gnustl
`std::vector<std::string>` in place. The natural root must return its nine keys
in packed order with `{begin,end,capacityEnd}`, exact `reserve(count)` capacity,
one 8-byte COW-string pointer per element, unique string reps, refcount zero,
and trailing NUL. The natural `object` child independently returns its one key;
the `version` Real child must return the exact all-zero empty-vector header
without entering reserve/name decoding. Each live vector is released through
the target's emitted `std::vector<std::string>` destructor (0x918690), which
must leave the three header pointer bits unchanged while freeing every COW rep
and the vector storage. The PSB owner count remains 4 throughout all three
calls and destructors; no STL object is destroyed by the harness runtime.
Fresh evidence and the exact ownership assertions are recorded in
[FOLLOWUP_RAW_DICTIONARY_KEYS_VECTOR_ORACLE_2026-08-03.md](../../../analysis/psbfile_function_audit_2026-07-25/FOLLOWUP_RAW_DICTIONARY_KEYS_VECTOR_ORACLE_2026-08-03.md).
The collection cases pin the complete packed-table shape of a 30-entry Array at
`0x448B` (34 table bytes) and a 36-entry Dictionary at `0x4972` (115 table
bytes), including the first 32 raw bytes. Their public results must be
`tvtObject` closures whose Object and ObjThis pointers are identical. After the
source PSBFile global is cleared, the script global plus the independent
`TJS_GLOBAL` copy must keep the dispatch and its raw owner alive. Full TJS may
also retain unrelated persistent Variant-stack temporaries, so their absolute
initial counts are diagnostic rather than a fixed golden. Clearing the script
value must release at least its Object/ObjThis pair while leaving at least the
independent pair alive; direct Android `AddRef`/`Release`
(0x597AC0/0x597A40) must then produce an exact local `+1/-1`. The Array
probe requires element 0 to be integer 29, and the Dictionary probe requires
`icon42/clip/left` to retain the exact zero Real bits. Direct vtable probes also
require the exact Android primary/secondary address points
`base+0x1A0B3D8`/`base+0x1A0B4E8`, primary/secondary offset-to-top values 0/-8,
null RTTI slots, every one of the 32 ordered primary entries, and the six exact
primary/secondary `Construct/Invalidate/Destruct` entries.
Primary and secondary `Construct` (0x597A30/0x597A38) must return 0 for ignored
sentinel arguments; both `Invalidate` (0x596F38/0x596F3C) and both `Destruct`
(0x597A28/0x597A2C) must be no-ops. The complete 0x30-byte dispatch and owner
reference count must remain unchanged before and after logical invalidation.
The 19 unsupported primary slots (`FuncCall[ByNum]`, `PropSet[ByNum/ByVS]`,
`GetCountByNum`, `DeleteMember[ByNum]`, `InvalidateByNum`, `IsValidByNum`,
`CreateNew[ByNum]`, `Reserved1/2/3`, `IsInstanceOfByNum`,
`Operation[ByNum]`, and `ClassInstanceInfo`) are called with the same writable
sentinel backing every ignored argument. Each must return `-1002` while
preserving all 64 sentinel bytes and every dispatch structural field, both
before and after logical invalidation. Per-call dispatch/owner refcount
transitions remain in the JSON as diagnostics; a long Frida trace may let Full
TJS compact unrelated persistent Variant-stack temporaries between probes, so
that concurrent decrement is not attributed to an unsupported slot.
The same direct probes cover `NativeInstanceSupport` (0x596D90) and its process-lifetime
`PSBValueClass` ID slot. A non-GET flag must return `-1002` without initializing
the cache or touching an output sentinel; a mismatched GET must lazily populate
the cache when needed, return `-1`, and still preserve the sentinel. A matching
GET must return the borrowed secondary `iTJSNativeInstance` base (`self+8` on
Android) without changing the immediately adjacent pre/post dispatch/owner
references, including after logical invalidation. `IsInstanceOf` (0x596E24) must match only the exact case-sensitive
`Array`/`Dictionary` class name, return `-1002` before reading the object for a
non-null member name, leave flag/hint/objthis untouched, and keep returning true
after logical invalidation because Android does not consult valid or owner in
this slot. They then invoke `EnumMembers` (0x596F50) with real TJS function closures whose ObjThis
field is deliberately cleared. Value enumeration must preserve every packed
member name in order, pass three arguments with flags 0, expose `Integer` for
all 30 Array children and `Object` for all 36 Dictionary children, and use the
enumerated dispatch as callback `this`; callback result `-777` must not stop
the loop. `TJS_ENUM_NO_VALUE` must preserve the same names while passing exactly
two arguments, and an invalidated dispatch must return `-1006` without another
callback. The remaining direct probes require named `PropGet` (0x597854) to
expose the Array's `count`, ordinary
missing names to clear a sentinel, strict missing names to return `-1001` and
preserve it, and a null name to return `-1002`. `GetCount` (0x5975E0) must
return 30 only for the Array; `[-1]` must resolve to its independently pinned
final tag-`0x04` zero, non-strict out-of-range must clear a sentinel Variant,
and strict out-of-range must return `-1001` without touching it. Dictionary
numeric access must return `-1001` without touching the same sentinel. Finally,
named invalidation must return `-1002` and preserve validity; whole-object
invalidation (0x596F0C) must change `IsValid`
(0x596EF0) from 1 to 2, make a repeat return `-1006`, and leave dispatch/owner
references unchanged across that immediate call. This covers public navigation, raw categories 6/7,
closure double references, count/index boundaries, logical invalidation, owner
retention, and terminal cleanup without generating a fixture.

`--resource-boundary` uses the existing `tests/test_files/emote/ezsave.pimg`,
pinned by file SHA-256. Its root member `2157.tlg` is a complete natural
tag-`0x19` node at offset `0x1FE`; resource-table index 0 resolves to 612 bytes
at absolute offset `0xB1C`, pinned independently by resource SHA-256. The public
path constructs `new PSBFile(path)`, obtains a real `tvtOctet` (`3`), clears the
PSBFile global, and verifies the copied bytes remain alive. It then destroys the
extra `TJS_GLOBAL` Variant reference and requires the Octet reference count to
decrease from an observed value of at least 2 while remaining at least 1,
without changing the byte allocation. Persistent Full-TJS Variant-stack
temporaries may add another live reference before compaction. A separate raw load
calls `PSBRawNode::GetResource` (0x5996E4), requiring the returned pointer to be
exactly `raw_owner_data + 0xB1C`, the written size to be 612, and the bytes to
match the same hash. The copied Octet data pointer must differ from that
borrowed raw pointer. With `--trace`, the target set also includes the Octet
allocation/copy-ref/release chain and the raw-owner destructor.

Natural scalar and Resource coverage can be inventoried without changing any
input:

```bash
python3 tests/differential/python/scan_psbfile_natural_boundaries.py
```

The scanner walks real reachable Array/Dictionary children instead of searching
for byte `0x0B`. Its current repository result is 222 physical PSB/MDF inputs,
112 unique decoded PSBs, and 23,415,372 reachable nodes with zero parse errors.
It reports counts, full-value extrema and maximum Variant/GetInt deltas for each
integer tag. It also reports bit-preserving Real extrema/classifications,
String-index extrema plus resolved UTF-8 hashes/previews, first reachable Null
nodes, Array/Dictionary entry-count extrema with packed-table prefixes, and
Resource-table size extrema without dereferencing malformed indices. All
10,500 tag-`0x1D`
nodes are zero; all 23,124 tag-`0x1E` and 70,527 tag-`0x1F` nodes are finite.
All 2,049,872 tag-`0x15` and 6,925,356 tag-`0x16` indices are inside their
String tables. It also finds 274,641 tag-`0x01` Null nodes, 4,655,138 Arrays
(0..16,997 entries), and 1,916,004 Dictionaries (0..453 entries). None of the
category-1 tags `0x02/0x03/0x27/0x2F/0x33/0x37/0x3B` is reachable, so the
scanner prints `boolean_tags_present=none` and no Boolean fixture is fabricated.
The known
`m2logo.mtn` tag-0x09 offset anchor matches, while no reachable tag-0x0A,
tag-0x0B or tag-0x0C integer node exists. The only raw tag-`0x1A` node has index
56395 against 73-entry tables and is therefore reported as invalid rather than
used as a successful oracle; no reachable tag-`0x1B`, `0x1C`, or `0x2D` node
exists. Those wider-tag oracles still need new natural assets; the scanner does
not manufacture one.

`--media-interface` uses the registered process-lifetime PSBMedia singleton and
does not require or manufacture PSB/MDF input. It first requires the exact Android
address point `base+0x1A0B510` and all 11 ordered vtable entries. Direct vslot calls
then pin the singleton's non-atomic reference transition `2 → 3 → 2`, overwrite a
caller-owned `ttstr` with UTF-16 `psb`, require both Normalize slots to preserve the
storage pointer and all 64 inspected string-object bytes, and require
`GetLocallyAccessibleName` to release/non-null-clear once and remain a no-op when
called again on an empty string. Two 0x28-byte empty objects allocated by the
target's own `operator new` isolate destructive edges from the singleton:
refcount zero must wrap to `0xFFFFFFFF` and stay alive for the complete destructor,
whereas refcount one must tail-dispatch the deleting destructor and free the
object. The trace set records the eight canonical media entries involved, while
the adapter observes the generic string/Variant helpers through their exact state
effects to avoid flooding Frida with process-wide helper traffic.
Fresh decompilation, the eight-line target pseudocode, local source mapping and
the cleanup/ownership protocol are recorded in
[FOLLOWUP_PSBMEDIA_INTERFACE_LIFECYCLE_ORACLE_2026-08-03.md](../../../analysis/psbfile_function_audit_2026-07-25/FOLLOWUP_PSBMEDIA_INTERFACE_LIFECYCLE_ORACLE_2026-08-03.md).

`--media-lifecycle` pushes the repository's existing `ezsave.pimg` and encrypted
motion PSB under ASCII-only device aliases. It opens
`psb-media-ezsave.pimg/2036.tlg`, then asks the same process-lifetime PSBMedia
singleton for the resource under the second container. The second container is
loadable without a filter but has a raw Resource root, so the lookup returns
false only after `EnsureContainer` (0x599E04) has replaced `_file` and
`_container`. The oracle then verifies the old `tTVPMemoryStream` still exposes
its unchanged size/reference metadata, invokes its deleting destructor at
0x8F7D68, and opens the first container again. This deleting entry executes the
same `Reference`-gated body as the complete destructor at 0x8F7D04, then calls
`operator delete` on the stream object. It deliberately never
dereferences the old stream's borrowed `Block` after replacement.
When combined with `--trace`, the Frida target set additionally includes
`Open` (0x59993C), `EnsureContainer` (0x599E04), `GetResourceData` (0x59A0B4),
`Resolve` (0x59A4B0), adaptor creation (0x59A330), and the stream constructor /
deleting destructor (0x8F7C74 / 0x8F7D68).

`--media-dictionary` stages the already-existing
`reference/xp3/caution_test/DRACU-RIOT/data/motion/autoskip.psb` under an
ASCII device alias. Its SHA-256 is pinned to
`131b436405c0aa8cd137a496c98fb77a77da95ca29e8af4597da1f7a42fd4a5d`.
The harness supplies an ABI-matched, vtable/layout-compatible lister surrogate,
calls the original `PSBMedia::GetListAt` at 0x5999F4, and returns the callback
strings; it does not implement PSB traversal itself. Its length-prefixed wire
format preserves embedded U+0000 and isolated UTF-16 surrogates, with a 32 KiB
aggregate binary-payload cap before hex encoding (UTF-8 `surrogatepass`/WTF-8).
The oracle requires the exact ordered result `arrow`, `auto`, `skip` for
`source/main/icon`. With `--trace`,
the observed native chain is 0x59849C → 0x5999F4 → 0x599E04 → 0x598538 →
0x598708 → 0x59A330 → 0x59A4B0.

`--media-array` stages the immutable `tests/test_files/emote/ezsave.pimg`
under an ASCII device alias and calls the same native `PSBMedia::GetListAt`
vslot for the root key `layers`. The input digest is pinned to
`d90d4ee955258b63efdc648f159990aa2c605dceef396ab9ea56eb8d281a7aa3`;
the natural Array is additionally pinned at decoded offset `0x20b` with tag
`0x20`, a count-32 packed table, and its exact 32-byte prefix. The ABI lister
must receive the ordered decimal strings `0` through `31`, proving the native
Array count decoder, `ttstr(index)` construction, callback order and per-item
cleanup without constructing or patching a fixture.

`--media-adaptor-null` stages the existing valid `ezsave.pimg`, then temporarily
clears the Android PSBFile class-object slot only around a direct native call to
`EnsureContainer` (0x599E04). It verifies that successful storage loading plus
`CreateAdaptor` (0x59A330) returning null still returns true, writes `_file` as
Void, and updates `_container`. The runner restores the exact class-object
pointer in a `finally` path and repeats the same request; the retry must reload
and publish an Object adaptor because the cache hit is gated on `_file` being
Object. This mode changes no PSB bytes and creates no malformed fixture.

The TJS-backed modes (`--integer-boundary`, `--real-boundary`,
`--string-boundary`, `--shape-boundary`, `--resource-boundary`,
`--media-interface`, `--media-lifecycle`,
`--media-dictionary`, `--media-array`, and `--media-adaptor-null`) need the
APK's Full TJS/NCB
startup, so the runner first copies the existing `--startup-xp3` into the
app-private files directory and calls
`TVPMainScene::startupFrom`. It waits for the real `TVPScriptEngine` global
before issuing `TJS_INIT`; calling `TJS_INIT` earlier would permanently select
the harness's bare-TJS fallback. By default it then waits four seconds
(`--startup-settle-seconds`) so `caution_minimal` can remove its GL-thread
continuous handler before harness-rpc enters the same TJS VM. This is required
for long Frida collection traces; concurrent GL/RPC TJS execution corrupts both
scripts and is an observer race, not a PSBFile result. Storage and media inputs are likewise staged
under `/data/user/0/org.github.krkr2/files` with the app UID. Android SELinux
does not let the untrusted APK consume KiriKiri storage directly from
`/data/local/tmp`, even though rooted ADB can write there. `--remote-dir` or
`KRKR2_DEVICE_DIR` remains available for an explicitly supplied app-readable
directory; when neither is set, app-private staging is the safe default.

The two lifecycle files themselves still do not provide a PSBMedia-reachable
Dictionary node: `ezsave.pimg` supplies the directly reachable `layers` Array,
while the unfiltered motion root is itself a Resource. Dictionary-listing
coverage therefore comes specifically from `--media-dictionary` and the
restored natural `autoskip.psb`; Array-listing coverage comes from
`--media-array` and the unchanged `ezsave.pimg`. These modes leave global NCB
registration untouched; the dedicated adaptor-null mode makes its single-slot
mutation explicit and restores the original pointer before any retry or
cleanup.

The current fixed Android ARM64-only APK passes all 24 raw/scalar/shape/resource/
media cases both without tracing and in one combined `--trace` invocation.
Target hashes, the exact command, per-case event counts and the refcount
methodology are recorded in
[FOLLOWUP_ANDROID_ARM64_RUNTIME_ORACLE_2026-08-03.md](../../../analysis/psbfile_function_audit_2026-07-25/FOLLOWUP_ANDROID_ARM64_RUNTIME_ORACLE_2026-08-03.md).

### With local Frida trace verification

```bash
# --record-trace: write a local trace capture under tests/differential/traces
# --trace       : compare against a previously recorded local capture
python3 tests/differential/python/run_bezier_curve_adb.py \
  --spec-dir tests/differential/specs/bezier_curve --trace
```

Without either flag the tracer stays off and `frida` is not even
imported — default runs have zero overhead.

On mismatch the runner prints, with the first divergent step:

```
TRACE MISMATCH single_segment_mid:
step 12: addr differs (sub_69A754 vs sub_698454)
  golden:  enter sub_69A754 depth=1 x0=<ptr> d0=0.5
  runtime: enter sub_698454 depth=1 x0=<ptr> d0=0.5
```

### Motion playback oracle recording

`motion_playback` is recorded from live libkrkr2 rather than by a scalar
`CALL`. It starts the APK harness, pushes
the selected `reference/xp3/logo_test_oracle_<case>_15hz.xp3`, calls
`STARTUP_FROM`, and records
the natural GL-thread playback with the specialised Frida motion tracer.

```bash
python3 tests/differential/python/run_motion_playback.py \
  --record-oracle --serial "$ANDROID_SERIAL"
```

The command writes:

```text
tests/differential/traces/motion_playback/yuzulogo.oracle.json
tests/differential/traces/motion_playback/m2logo.oracle.json
```

These generated files are ignored by Git. Treat them as transient
libkrkr2 Motion state captures, not as repository or final visual
goldens.

## Architecture

```
oracle_runner/
├── adb_engine.py       AdbHarnessEngine: pushes libs, launches
│                       HarnessActivity, speaks line-based RPC over a
│                       forwarded TCP socket, tracks pid for Frida attach.
├── arm64_abi.py        AAPCS64 register/stack packing (x0-x7, d0-d7)
├── guest_heap.py       Bump allocator at fixed guest VA 0x50000000
├── stl_layout.py       HitData / Affine2x3 builders
├── frida_tracer.py     FridaTracerEngine: attach to HarnessActivity pid,
│                       load agent.js, expose start_case/stop_case
├── frida_agent.js      Per-target `Interceptor.attach` recording x0-x7
│                       + d0-d7 at entry; x0/d0 at exit
├── frida_motion_agent.js
│                       Motion.Player progress/updateLayers recorder used
│                       only by motion_playback oracle recording.
├── frida_motion_tracer.py
│                       Host-side wrapper for frida_motion_agent.js.
├── trace_targets.py    Per-family target offsets + arity + return-kind
├── trace_diff.py       Golden read/write + first-divergence diff
├── adapters/           Per-family case-to-CALL translation
│   ├── geometry_hit_test.py
│   ├── local_transform.py
│   ├── bezier_curve.py
│   ├── position_interp.py
│   └── motion_playback.py
├── harness/            Native side of the harness (see harness/README.md)
│   ├── harness.cpp
│   ├── jni_bridge.cpp
│   ├── Android.mk / Application.mk
│   ├── build_legacy.sh
│   └── prebuilt/libharness.so
└── harness-apk/        APK wrapper around libharness.so (see harness-apk/README.md)
    ├── build.sh
    └── HarnessActivity.java
```

`run_*_adb.py` (siblings of `run_*_wasmtime.py`) instantiate
`AdbHarnessEngine` once, iterate specs, and optionally attach a
`FridaTracerEngine` configured with the family's target offset list.
`run_motion_playback.py --record-oracle` uses `FridaMotionTracer`
instead because it records a continuous playback rather than a single
leaf-function call.

## Implementation notes

**Pointer canonicalisation** — raw x-register values ≥ `0x1_0000_0000`
(bionic heap, libkrkr2 text, stack, TLS) are replaced with `<ptr>` at
normalisation time. Values below (our deterministic GuestHeap at
`0x50000000`, small scalars, bools) stay raw. Without this the trace
diff fires on every ASLR reshuffle.

**Arity masking** — AAPCS64 leaves unused argument registers in
whatever state the caller wrote last. Per-target `ARG_COUNTS` in
[trace_targets.py](trace_targets.py) caps the meaningful x/d register
count; beyond that we emit `-`. The return-value half uses
`RETURN_KINDS` to decide whether `x0` (int/pointer return) or `d0`
(double return) carries signal.

**Crash resilience** — `AdbHarnessEngine.is_alive()` polls the child
process; on SIGSEGV inside libkrkr2 the runner calls `restart()`,
re-spawns the harness, and re-attaches Frida. Crash cases produce no
golden trace (the script is torn down with the process); the tracer's
`stop_case()` swallows `frida.InvalidOperationError` so the adapter's
exception surface reflects the real crash, not the Frida-side
teardown.

**Script destroyed errors** — if you see `InvalidOperationError` from
`stop_case`, it means the target died mid-case *and* the canonical
swallow path didn't trigger. Verify `frida-server` on the device
matches the pinned `frida-python` version.

## Follow-ups

- Port-side scalar tracer — instrument the wasm build to emit the same
  event schema and compare fresh libkrkr2 and port call sequences. Scalar
  traces are currently local-only and are not enforced by CI.
- Visual motion oracle — capture either final framebuffer pixels or a
  complete draw-command stream for `yuzulogo.mtn` / `m2logo.mtn`,
  including texture identity, draw order, clipping, blend/stencil state,
  and compositing. This is required before claiming complete visual
  equivalence.
- `psb_rl_decompress` — needs static extraction of the RL loop from
  `sub_695DE8`, not in scope
- Richer target lists — hook `iTJSDispatch2::PropGet` call-sites
  inside `sub_69A754`/`sub_69A4D4` if the leaf-only coverage proves
  insufficient
