# Differential sampling boundaries (2026-09-06)

## Fresh evidence and scope

This change affects only `KRKR2_WASMTIME_HEADLESS` instrumentation and the
differential recorders/comparators. It does not change production motion
evaluation, cleanup, rendering, or fixture inputs.

All four binaries and their matching `.i64` files were checked readable.
Native `mcp__idalib__idb_open` and `server_health` verified the sessions below.
The iOS armv7 IDB retains the historical thin-slice input name; its Mach-O header
and updateLayers entry bytes match the armv7 slice of the checked-in fat input
(slice file offset 16384, image base 0x4000).

| Binary | Session | updateLayers | progress raw callback | progress bridge | Canvas envelope |
|---|---|---|---|---|---|
| Kirikiroid2_1.3.9_Android_arm64-v8a.so | 6af1bb5c | 0x6B871C | 0x6CFE78 | inlined in raw callback | 0x6C4820 |
| Kirikiroid2_1.3.9_Android_armabi-v7a.so | fa363b5d | 0x5856E0 | 0x595598 | 0x595570 | 0x58E2CC |
| Kirikiroid2_1.3.9_iOS_arm64 | 9b2ba545 | 0x10010E544 | 0x100121204 | 0x1001211C0 | 0x1001186E0 |
| Kirikiroid2_1.3.9_iOS_armv7 | 90d3ac78 | 0x10BE5C | 0x11FFB4 | 0x11FF88 | 0x11653C |

Each mapped function above was freshly decompiled with its explicit database.
Canvas pseudocode exceeds the MCP response limit, so fresh disassembly of all
four entries through the builder call and all four normal return tails supplies
the boundary evidence (no reliance on the truncated comment-only response).

The existing accurate-SLA scope was also checked against fresh decompilation of
its direct-SLA callers: Android arm64 `0x6D2A38`, Android armv7 `0x597328`,
iOS arm64 `0x1001233C8`, iOS armv7 `0x12257C`. All four select accurate SLA
unconditionally under software rendering, otherwise consult a guarded
process-lifetime `ogl_accurate_render` flag (default false). The accurate render
call precedes target-Variant copying and post-draw update. This boundary is
retained and now labeled independently of the ordinary Canvas boundary.

The two iOS Canvas-entry `Layer` literals were confirmed as UTF-16LE bytes,
corrected from truncated `L` to `unsigned short[6]`, and the four IDBs saved.

## Shared control flow and differences

```
progress raw callback:
    resolve native Player; reject missing receiver (-1008)
    reject argc < 1 (-1004)
    deltaMs = argv[0].AsReal()
    install raw objthis
    frameProgress(deltaMs * 60 / 1000)
    updateLayers()
    calcBounds()
    dispatchPendingEvents(live currentDispatch)
    clear currentDispatch on normal return

updateLayers:
    phase1; phase2; position-delta pass; clear camera dirty
    cameraConstraint; vertex; visibility; cameraNode; shapeAABB; shapeGeometry
    motionSubNode; particleEmitter; particleSystem; anchorNode
    [node snapshot boundary]
    clear node flags/accumulated dirty; clear parameter modes
    clear noUpdateYet and queuing

renderToCanvas:
    [execute_enter]
    construct Layer-class accessor, then target accessor
    if !priorDraw: clear draw region
    query width, then height
    if !priorDraw: buildRenderCommands(main, aux, targetClip)
    execute items; reset target clip
    destroy target accessor, then Layer-class accessor
    [execute_leave]
```

The last phase-3 helper is AnchorNode in every target:
Android arm64 0x6BD908, Android armv7 0x589C00,
iOS arm64 0x100113024, iOS armv7 0x110908.
This corrects the old legacy recorder's camera-helper description for the active
1.3.9 boundary. Child updateLayers calls complete inside parent phase 3, so
snapshots follow actual return order, not entry order with the first item rotated.

Android arm64 inlines the progress bridge; the other three retain a helper.
Deque iteration, object offsets and parameter strides differ by ABI; none is
used as a portable sampling identity. Android armv7 reverses the two independent
per-node cleanup stores and the final two Player-byte stores relative to the
64-bit bodies. Both are after the chosen snapshot boundary. iOS armv7 uses
SjLj exception registration; normal Canvas tails in all four release target then
Layer class. No conditional phase-3 call or alternative sampling boundary was
found. All normal Canvas tails reset the clip even for an empty item list.

## Local mapping and per-case decision, before changes

- `PlayerFrameProgress.cpp`: the trace scope was in the shared progress bridge,
  also observing engine-owned progress without a TJS progress call. Move it to
  the raw callback, matching the oracle's window; capture the already-converted
  delta without performing another TJS conversion.
- `PlayerUpdateLayers.cpp`: replace entry-time pointer collection with a value
  snapshot immediately after AnchorNode and before the first cleanup store.
- `wasmtime/motion_playback_wasmtime.cpp`: serialize captured scalar values;
  never dereference saved Players at progress return and never rotate their order.
- `PlayerRenderTargets.cpp`: ordinary execute scope belongs around the complete
  Canvas body, before local accessor construction and after their destruction.
  Remove the inner item-executor scope from `PlayerRenderExecute.cpp`.
- Accurate SLA already has a scope around its complete renderer call; retain
  that boundary and identify it separately from ordinary Canvas in the schema.
- `yuzulogo` (63 frames) and `m2logo` (25 frames): preserve existing XP3/TJS,
  first zero delta, 15 Hz cadence, and natural completion. Both use the same
  phase-3 snapshot contract; runtime reports determine their render route.
- Scalar hit-test/interpolation/transform cases keep their synchronous
  function-return boundaries. RL decompression has no active Android counterpart;
  it is not given a fictitious paired capture boundary.

The normalized frame protocol must retain sampling phase/order, actual progress
delta and sampled Player layer counts. Render comparison must also check the
interleaved prepare/build/execute order; separate per-stage kind lists cannot
detect a builder crossing the execute boundary. Historical captures without the
new evidence must be re-recorded, not silently relabeled.

## Validation results

- Built `krkr2_wasmtime_guest` successfully with Emscripten and Bison 3.8.2.
- Passed 26 Python timing/strict-frame/render-boundary tests, the Node tests of
  the actual Frida callbacks (with native reads substituted), Python compile
  checks, JavaScript syntax checks and `git diff --check`. These protocol tests
  are now part of the active differential CI job.
- Ran the unchanged per-case XP3 files under the rebuilt guest, with fresh
  normalized traces and semantic render artifacts in
  `tests/differential/artifacts/capture_boundaries_20260906/`.
- `yuzulogo`: 63 frames; each snapshot contains 25 nodes from one Player.
- `m2logo`: 25 frames; 16 snapshots contain only the 31-node root Player,
  eight contain four 2-node children then the 31-node root, and the final one
  contains four 1-node children then the root. All preserve actual return order.
- Both cases use accurate SLA under software rendering and observe
  `0,67,67,66,67,67,66,...` progress deltas. Every observed builder lies inside
  its execute envelope. Combined artifact validation: 2 cases, 88 frames,
  1,384 events, 0 images.
- Comparing only the existing node fields to the retained Android captures
  from 2026-08-28 gives zero differences for both cases. Those older files lack
  the new sampling evidence and are deliberately not accepted as fresh protocol
  validation or relabeled as new captures.
- This host has no ADB/Android runtime, so a fresh Android/Wasmtime pair could
  not be collected here. The actual Frida agent callbacks were executed under
  Node with substituted native reads to verify value freezing, return order,
  numeric delta capture and nested progress-window restoration.
- The ordinary Canvas boundary has fresh four-binary static evidence and
  comparator regression coverage, but neither available logo fixture takes
  that route. No artificial game fixture was constructed to claim coverage.

Guest render sequence numbers restart when the host drains a tick's buffer;
the comparison uses `(frameId, seq)` for uniqueness and intra-frame ordering.
Cross-frame reuse is valid; reuse within one frame is rejected.
Frame IDs are allocated on progress entry to distinguish nested windows, while
the host-visible completed-frame count advances only after return and emission.
This preserves the oracle host's stable-capture polling contract: a long native
call must not be counted as an already-recorded final frame.
