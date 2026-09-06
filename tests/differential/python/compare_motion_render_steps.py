#!/usr/bin/env python3
"""Compare motion_playback render semantics and optional image checkpoints."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tests" / "differential"))
from oracle_runner.motion_timing import (  # noqa: E402
    SIMULATION_DELTA_MS,
    SIMULATION_FPS,
    validate_simulation_contract,
)
from oracle_runner.png_artifacts import image_pixel_hash, rgba_sha256_file


ITEM_FIELDS = (
    "index",
    "nodeIndex",
    "sourceKey",
    "flags",
    "layerIds",
    "sortKey64",
    "paintBox",
    "clipRect",
    "buildClipRect",
    "dirtyRect",
    "viewportRect",
    "sourceGate232",
    "stencilType244",
    "parentItemIndex",
    "parentItem264",
    "childItemCount",
    "meshType280",
    "leafLayerVariantTag",
    "composedLayerVariantTag",
    "leafBuilt",
    "composedBuilt",
    "executedDirect",
)
BUILD_FLOW_FIELDS = (
    "inputItemCount",
    "builtItemCount",
    "validDrawableItemCount",
    "leafBuiltCount",
    "composedBuiltCount",
)
EXPECTED_STAGE_KIND_SEQUENCES = {
    "render_prepare": (
        "prepare_enter",
        "prepare_leave",
        "apply_projection_enter",
        "apply_projection_leave",
    ),
    "render_execute": (
        "execute_enter",
        "execute_leave",
    ),
}
COMPARABLE_EXECUTE_KINDS = frozenset(
    EXPECTED_STAGE_KIND_SEQUENCES["render_execute"])
RENDER_CAPTURE_CONTRACT = "motion-render-boundaries-v1"
EXECUTE_BOUNDARIES = frozenset((
    "Player.renderToCanvas", "Player.renderAccurateSeparateLayerAdaptor",
))


def capture_boundary_sequences(
    stages: dict[str, list[dict[str, Any]]],
) -> dict[int, list[tuple[str, str, str | None]]]:
    """Recover interleaved call boundaries, independently of native addresses."""
    merged = []
    seen_seq: set[tuple[int, int]] = set()
    for stage, events in stages.items():
        for event in events:
            if event.get("captureContract") != RENDER_CAPTURE_CONTRACT:
                raise ValueError(f"{stage}: missing capture contract; re-record artifact")
            seq, frame_id = event.get("seq"), event.get("frameId")
            if type(seq) is not int or type(frame_id) is not int or \
                    (frame_id, seq) in seen_seq:
                raise ValueError(f"{stage}: invalid/duplicate sequence or frame identity")
            # The guest drains its buffer after each host tick and may restart
            # sequence numbers. Only order within the same frame is compared.
            seen_seq.add((frame_id, seq))
            boundary = event.get("executeBoundary") if stage == "render_execute" else None
            if stage == "render_execute":
                if boundary not in EXECUTE_BOUNDARIES:
                    raise ValueError("render_execute: missing or unknown executeBoundary")
                suffix = ".enter" if event["kind"] == "execute_enter" else ".leave"
                if event.get("samplePoint") != boundary + suffix:
                    raise ValueError("render_execute: samplePoint does not match executeBoundary")
            merged.append((frame_id, seq, stage, event["kind"], boundary))
    out: dict[int, list[tuple[str, str, str | None]]] = {}
    for frame_id, _, stage, kind, boundary in sorted(merged):
        out.setdefault(frame_id, []).append((stage, kind, boundary))
    for frame_id, sequence in out.items():
        execute_stack: list[str | None] = []
        for stage, kind, boundary in sequence:
            if kind == "execute_enter":
                execute_stack.append(boundary)
            elif kind == "execute_leave":
                if not execute_stack or execute_stack.pop() != boundary:
                    raise ValueError(f"frame {frame_id}: unmatched execute boundary")
            elif kind in ("build_commands_enter", "build_commands_leave") and \
                    not execute_stack:
                raise ValueError(f"frame {frame_id}: command construction outside execute envelope")
        if execute_stack:
            raise ValueError(f"frame {frame_id}: incomplete execute envelope")
    return out


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Compare oracle/Wasmtime motion render step artifacts")
    p.add_argument("--oracle-root", type=Path, required=True,
                   help="Oracle render-stage artifact root")
    p.add_argument("--wasmtime-root", type=Path, required=True,
                   help="Wasmtime render-stage artifact root")
    p.add_argument("--spec-dir", type=Path,
                   default=REPO_ROOT / "tests" / "differential" /
                           "specs" / "motion_playback",
                   help="Directory of motion_playback spec JSON files")
    p.add_argument("--case", action="append", default=[],
                   help="Case id to compare; defaults to all oracle cases")
    p.add_argument("--allow-render-flow-diagnostics", action="store_true",
                   help="Report build-flow semantic mismatches without "
                        "failing the stage comparison")
    p.add_argument("--ignore-layer-save", action="store_true",
                   help="Do not compare fixture saveLayerImage initial/"
                        "post_draw PNGs; use direct render checkpoint PNGs")
    p.add_argument(
        "--semantic-only", action="store_true",
        help="Compare platform-independent prepare/command/execute event "
             "shape and build flow without loading native image artifacts")
    return p.parse_args(argv)


def load_events(path: Path) -> list[dict[str, Any]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    events = data.get("events", [])
    if not isinstance(events, list):
        raise ValueError(f"{path} has no events list")
    return [event for event in events if isinstance(event, dict)]


def case_ids(oracle_root: Path) -> list[str]:
    event_dir = oracle_root / "events" / "render_commands"
    return sorted(
        path.name[:-len(".oracle.json")]
        for path in event_dir.glob("*.oracle.json"))


def _raw_flag(item: dict[str, Any], name: str) -> bool:
    flags = item.get("flags")
    return bool(flags.get(name)) if isinstance(flags, dict) else False


def _is_direct_item_with_undefined_build_clip(item: dict[str, Any]) -> bool:
    # libkrkr2.so sub_6C4E28 @ 0x6C5DBC only defines item+21 and
    # item+216..228 inside the item+19 branch. Direct top-level items with
    # item+19 == 0 are submitted later by sub_6C7440 from paintBox/viewport;
    # any Oracle bytes left in +21/+216..228 are native object-slot residue,
    # not stable build-flow semantics.
    if _raw_flag(item, "drawFlag19"):
        return False
    parent_ptr = item.get("parentItem264")
    parent_index = item.get("parentItemIndex")
    has_parent_ptr = isinstance(parent_ptr, str) and parent_ptr != "null"
    has_parent_index = isinstance(parent_index, int) and parent_index >= 0
    child_count = item.get("childItemCount")
    has_children = isinstance(child_count, int) and child_count > 0
    return not has_parent_ptr and not has_parent_index and not has_children


def _normalized_flags(item: dict[str, Any]) -> dict[str, Any] | Any:
    flags = item.get("flags")
    if not isinstance(flags, dict):
        return flags
    normalized = dict(flags)
    if _is_direct_item_with_undefined_build_clip(item):
        normalized["clipValid21"] = 0
    return normalized


def _normalized_item_value(item: dict[str, Any], field: str) -> Any:
    flags = _normalized_flags(item)
    clip_valid = (
        bool(flags.get("clipValid21"))
        if isinstance(flags, dict) else False
    )
    if field == "flags":
        return flags
    if field == "leafLayerVariantTag":
        value = item.get("leafLayerVariantTag")
        return item.get("leafLayerVariantTag320") if value is None else value
    if field == "composedLayerVariantTag":
        value = item.get("composedLayerVariantTag")
        return item.get("composedLayerVariantTag340") if value is None else value
    value = item.get(field)
    if field == "buildClipRect" and value is None:
        value = item.get("clipRect")
    if field in {"clipRect", "buildClipRect"} and not clip_valid:
        return [0, 0, 0, 0]
    if field == "viewportRect" and value == [1, 1, -1, -1]:
        return None
    if field in {"paintBox", "clipRect", "buildClipRect", "dirtyRect",
                 "viewportRect"} and isinstance(value, list):
        normalized = []
        for part in value:
            if isinstance(part, (int, float)):
                normalized.append(round(float(part), 3))
            else:
                normalized.append(part)
        return normalized
    return value


def _semantic_item(item: dict[str, Any]) -> dict[str, Any]:
    return {
        field: _normalized_item_value(item, field)
        for field in ITEM_FIELDS
    }


def _semantic_item_has_valid_drawable(item: dict[str, Any]) -> bool:
    flags = item.get("flags")
    if isinstance(flags, dict) and (
        bool(flags.get("flag16")) or bool(flags.get("flag17"))
    ):
        return False
    source_gate = item.get("sourceGate232")
    try:
        if int(source_gate) == 0:
            return False
    except (TypeError, ValueError):
        pass
    clip_valid = (
        bool(flags.get("clipValid21"))
        if isinstance(flags, dict) else False
    )
    rect = item.get("buildClipRect") if clip_valid else item.get("paintBox")
    if rect is None and clip_valid:
        rect = item.get("clipRect")
    if not clip_valid:
        viewport = item.get("viewportRect")
        if isinstance(rect, list) and len(rect) == 4 and \
                isinstance(viewport, list) and len(viewport) == 4:
            try:
                v_left, v_top, v_right, v_bottom = (
                    float(value) for value in viewport)
                left, top, right, bottom = (float(value) for value in rect)
            except (TypeError, ValueError):
                pass
            else:
                if v_right >= v_left and v_bottom >= v_top:
                    rect = [
                        max(left, math.floor(v_left)),
                        max(top, math.floor(v_top)),
                        min(right, math.ceil(v_right)),
                        min(bottom, math.ceil(v_bottom)),
                    ]
    if not isinstance(rect, list) or len(rect) != 4:
        return False
    try:
        left, top, right, bottom = (float(value) for value in rect)
    except (TypeError, ValueError):
        return False
    return left < right and top < bottom


def _build_flow(event: dict[str, Any]) -> dict[str, Any]:
    flow = event.get("buildFlow")
    if not isinstance(flow, dict):
        return {}
    legacy_items_raw = flow.get("items")
    if not isinstance(legacy_items_raw, list):
        legacy_items_raw = flow.get("commands")
    legacy_items = [
        _semantic_item(item)
        for item in legacy_items_raw if isinstance(item, dict)
    ] if isinstance(legacy_items_raw, list) else []
    main_items = flow.get("mainListSemanticItems")
    semantic_main_items = [
        _semantic_item(item)
        for item in main_items if isinstance(item, dict)
    ] if isinstance(main_items, list) else legacy_items
    aux_items = flow.get("auxListSemanticItems")
    semantic_aux_items = [
        _semantic_item(item)
        for item in aux_items if isinstance(item, dict)
    ] if isinstance(aux_items, list) else []
    summary = {field: flow.get(field) for field in BUILD_FLOW_FIELDS}
    if semantic_main_items:
        summary["validDrawableItemCount"] = sum(
            1 for item in semantic_main_items
            if _semantic_item_has_valid_drawable(item)
        )
    return {
        **summary,
        "mainListSemanticItems": semantic_main_items,
        "auxListSemanticItems": semantic_aux_items,
        "items": semantic_main_items,
    }


def _flow_digest(flow: dict[str, Any]) -> str:
    data = json.dumps(flow, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=True, allow_nan=False).encode("utf-8")
    return hashlib.sha256(data).hexdigest()


def build_flow_leaves(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [event for event in events
            if event.get("kind") == "build_commands_leave"]


def _kind_sequence_by_frame(
    events: list[dict[str, Any]],
) -> dict[int, list[str]]:
    out: dict[int, list[str]] = {}
    for event in events:
        frame_id = event.get("frameId")
        kind = event.get("kind")
        if isinstance(frame_id, int) and isinstance(kind, str):
            out.setdefault(frame_id, []).append(kind)
    return out


def _compare_stage_kind_sequences(
    oracle_events: list[dict[str, Any]],
    wasmtime_events: list[dict[str, Any]],
    expected: tuple[str, ...] | None = None,
) -> tuple[int, tuple[int, str, Any, Any] | None]:
    oracle_by_frame = _kind_sequence_by_frame(oracle_events)
    wasmtime_by_frame = _kind_sequence_by_frame(wasmtime_events)
    frame_ids = sorted(set(oracle_by_frame) | set(wasmtime_by_frame))
    mismatches = 0
    first_mismatch: tuple[int, str, Any, Any] | None = None
    expected_list = list(expected) if expected is not None else None
    for local_index, frame_id in enumerate(frame_ids):
        oracle_seq = oracle_by_frame.get(frame_id, [])
        wasmtime_seq = wasmtime_by_frame.get(frame_id, [])
        if expected_list is not None:
            matched = oracle_seq == expected_list and wasmtime_seq == expected_list
        else:
            matched = oracle_seq == wasmtime_seq
        if matched:
            continue
        mismatches += 1
        if first_mismatch is None:
            first_mismatch = (
                local_index,
                "kind_sequence",
                {"frameId": frame_id, "kinds": oracle_seq},
                {"frameId": frame_id, "kinds": wasmtime_seq},
            )
    return mismatches, first_mismatch


def execute_leaves(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [event for event in events if event.get("kind") == "execute_leave"]


def comparable_execute_events(
    events: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    return [
        event for event in events
        if event.get("kind") in COMPARABLE_EXECUTE_KINDS
    ]


def layer_save_images(events: list[dict[str, Any]]) -> dict[tuple[str, int], dict[str, Any]]:
    out: dict[tuple[str, int], dict[str, Any]] = {}
    for event in events:
        if event.get("kind") != "save_layer_image":
            continue
        phase = event.get("phase")
        frame = event.get("frame")
        if not isinstance(phase, str) or not isinstance(frame, int):
            continue
        out[(phase, frame)] = event
    return out


def decoded_image_hash(
    artifact_root: Path,
    image: dict[str, Any] | None,
    cache: dict[Path, str],
) -> str | None:
    if image is None:
        return None
    value = image_pixel_hash(image)
    if value is not None:
        return value
    rel = image.get("path")
    if not isinstance(rel, str) or not rel:
        return None
    path = artifact_root / rel
    cached = cache.get(path)
    if cached is not None:
        return cached
    cached = rgba_sha256_file(path)
    cache[path] = cached
    return cached


def _value_summary(value: Any) -> str:
    text = repr(value)
    if len(text) <= 220:
        return text
    return text[:217] + "..."


def _compare_item_lists(
    oracle_items: list[dict[str, Any]],
    wasmtime_items: list[dict[str, Any]],
) -> tuple[str, Any, Any] | None:
    if len(oracle_items) != len(wasmtime_items):
        return ("items.length", len(oracle_items), len(wasmtime_items))
    for index, (oracle_item, wasmtime_item) in enumerate(
        zip(oracle_items, wasmtime_items)):
        for field in ITEM_FIELDS:
            oracle_value = oracle_item.get(field)
            wasmtime_value = wasmtime_item.get(field)
            if oracle_value is None or wasmtime_value is None:
                continue
            if oracle_value != wasmtime_value:
                return (
                    f"items[{index}].{field}",
                    oracle_value,
                    wasmtime_value,
                )
    return None


def _compare_build_flow(
    oracle_flow: dict[str, Any],
    wasmtime_flow: dict[str, Any],
) -> tuple[str, Any, Any] | None:
    for field in BUILD_FLOW_FIELDS:
        oracle_value = oracle_flow.get(field)
        wasmtime_value = wasmtime_flow.get(field)
        if oracle_value is None or wasmtime_value is None:
            continue
        if oracle_value != wasmtime_value:
            return (field, oracle_value, wasmtime_value)
    main_diff = _compare_item_lists(
        oracle_flow.get("items") or [],
        wasmtime_flow.get("items") or [],
    )
    if main_diff is not None:
        return ("mainListSemanticItems." + main_diff[0],
                main_diff[1], main_diff[2])
    aux_diff = _compare_item_lists(
        oracle_flow.get("auxListSemanticItems") or [],
        wasmtime_flow.get("auxListSemanticItems") or [],
    )
    if aux_diff is not None:
        return ("auxListSemanticItems." + aux_diff[0],
                aux_diff[1], aux_diff[2])
    return None


def _compare_layer_save_images(
    oracle_root: Path,
    wasmtime_root: Path,
    oracle_images: dict[tuple[str, int], dict[str, Any]],
    wasmtime_images: dict[tuple[str, int], dict[str, Any]],
    oracle_cache: dict[Path, str],
    wasmtime_cache: dict[Path, str],
) -> tuple[int, tuple[str, Any, Any] | None]:
    mismatches = 0
    first_mismatch: tuple[str, Any, Any] | None = None
    phase_order = {"initial": 0, "post_draw": 1}
    keys = sorted(
        set(oracle_images) | set(wasmtime_images) | {("initial", 0)},
        key=lambda key: (phase_order.get(key[0], 99), key[1], key[0]),
    )
    for phase, frame in keys:
        if phase not in phase_order or (phase == "initial" and frame != 0):
            mismatches += 1
            if first_mismatch is None:
                first_mismatch = (
                    f"{phase}[{frame}].phase",
                    phase if (phase, frame) in oracle_images else None,
                    phase if (phase, frame) in wasmtime_images else None,
                )
            continue
        oracle_image = oracle_images.get((phase, frame))
        wasmtime_image = wasmtime_images.get((phase, frame))
        if oracle_image is None or wasmtime_image is None:
            mismatches += 1
            if first_mismatch is None:
                first_mismatch = (
                    f"{phase}[{frame}].present",
                    oracle_image is not None,
                    wasmtime_image is not None,
                )
            continue
        oracle_hash = decoded_image_hash(oracle_root, oracle_image, oracle_cache)
        wasmtime_hash = decoded_image_hash(
            wasmtime_root, wasmtime_image, wasmtime_cache)
        if oracle_hash != wasmtime_hash:
            mismatches += 1
            if first_mismatch is None:
                first_mismatch = (
                    f"{phase}[{frame}].rgbaSha256",
                    oracle_hash,
                    wasmtime_hash,
                )
    return mismatches, first_mismatch


def _event_frame_label(event: dict[str, Any], fallback: int) -> str:
    frame = event.get("frame")
    if isinstance(frame, int):
        return str(frame)
    frame_id = event.get("frameId")
    if isinstance(frame_id, int):
        return str(fallback)
    return str(fallback)


def compare_case(
    oracle_root: Path,
    wasmtime_root: Path,
    case_id: str,
    *,
    expected_frames: int,
    allow_render_flow_diagnostics: bool = False,
    ignore_layer_save: bool = False,
    semantic_only: bool = False,
) -> bool:
    oracle_prepare_events = load_events(
        oracle_root / "events" / "render_prepare" /
        f"{case_id}.oracle.json")
    wasmtime_prepare_events = load_events(
        wasmtime_root / "events" / "render_prepare" /
        f"{case_id}.wasmtime.json")
    oracle_command_events = load_events(
        oracle_root / "events" / "render_commands" /
        f"{case_id}.oracle.json")
    wasmtime_command_events = load_events(
        wasmtime_root / "events" / "render_commands" /
        f"{case_id}.wasmtime.json")
    oracle_build_events = build_flow_leaves(oracle_command_events)
    wasmtime_build_events = build_flow_leaves(wasmtime_command_events)
    oracle_execute_events = comparable_execute_events(load_events(
        oracle_root / "events" / "render_execute" /
        f"{case_id}.oracle.json"))
    wasmtime_execute_events = comparable_execute_events(load_events(
        wasmtime_root / "events" / "render_execute" /
        f"{case_id}.wasmtime.json"))
    oracle_execute = execute_leaves(oracle_execute_events)
    wasmtime_execute = execute_leaves(wasmtime_execute_events)
    if semantic_only:
        oracle_layer_save: dict[tuple[str, int], dict[str, Any]] = {}
        wasmtime_layer_save: dict[tuple[str, int], dict[str, Any]] = {}
    else:
        oracle_layer_save = layer_save_images(load_events(
            oracle_root / "events" / "layer_save" /
            f"{case_id}.oracle.json"))
        wasmtime_layer_save = layer_save_images(load_events(
            wasmtime_root / "events" / "layer_save" /
            f"{case_id}.wasmtime.json"))

    first_mismatch: tuple[int, str, str, Any, Any] | None = None
    boundary_sequences = {}
    for side, prepare, commands, execute in (
        ("oracle", oracle_prepare_events, oracle_command_events, oracle_execute_events),
        ("wasmtime", wasmtime_prepare_events, wasmtime_command_events, wasmtime_execute_events),
    ):
        try:
            boundary_sequences[side] = capture_boundary_sequences({
                "render_prepare": prepare, "render_commands": commands,
                "render_execute": execute,
            })
        except ValueError as exc:
            first_mismatch = (
                0, "sampling_contract", "capture_boundaries",
                str(exc) if side == "oracle" else None,
                str(exc) if side == "wasmtime" else None,
            )
    if first_mismatch is None:
        oracle_boundaries = boundary_sequences["oracle"]
        wasmtime_boundaries = boundary_sequences["wasmtime"]
        for index, frame_id in enumerate(sorted(set(oracle_boundaries) | set(wasmtime_boundaries))):
            if oracle_boundaries.get(frame_id) != wasmtime_boundaries.get(frame_id):
                first_mismatch = (
                    index, "sampling_contract", "interleaved_boundaries",
                    oracle_boundaries.get(frame_id), wasmtime_boundaries.get(frame_id),
                )
                break
    render_prepare_shape_mismatches = 0
    render_commands_shape_mismatches = 0
    render_execute_shape_mismatches = 0
    build_flow_mismatches = 0
    execute_pre_mismatches = 0
    execute_post_mismatches = 0
    update_layer_after_draw_pre_mismatches = 0
    update_layer_after_draw_post_mismatches = 0
    post_draw_mismatches = 0
    layer_save_mismatches = 0
    oracle_cache: dict[Path, str] = {}
    wasmtime_cache: dict[Path, str] = {}
    checkpoint_fields = (
        "executePreImage",
        "executePostImage",
        "updateLayerAfterDrawPreImage",
        "updateLayerAfterDrawPostImage",
        "postDrawImage",
    )
    post_draw_checkpoint_enabled = not semantic_only and (any(
        decoded_image_hash(oracle_root, event.get("postDrawImage"), oracle_cache)
        is not None
        for event in oracle_execute
    ) or any(
        decoded_image_hash(wasmtime_root, event.get("postDrawImage"),
                           wasmtime_cache)
        is not None
        for event in wasmtime_execute
    ))
    execute_checkpoint_enabled = not semantic_only and (any(
        decoded_image_hash(oracle_root, event.get(field), oracle_cache)
        is not None
        for event in oracle_execute
        for field in checkpoint_fields
    ) or any(
        decoded_image_hash(wasmtime_root, event.get(field), wasmtime_cache)
        is not None
        for event in wasmtime_execute
        for field in checkpoint_fields
    ))
    if not semantic_only and ignore_layer_save and not post_draw_checkpoint_enabled:
        first_mismatch = (
            0,
            "render_execute",
            "post_draw_checkpoint_images.present",
            True,
            False,
        )

    for stage, oracle_events, wasmtime_events in (
        ("render_prepare", oracle_prepare_events, wasmtime_prepare_events),
        ("render_commands", oracle_command_events, wasmtime_command_events),
        ("render_execute", oracle_execute_events, wasmtime_execute_events),
    ):
        mismatch_count, stage_first = _compare_stage_kind_sequences(
            oracle_events, wasmtime_events,
            EXPECTED_STAGE_KIND_SEQUENCES.get(stage))
        if stage == "render_prepare":
            render_prepare_shape_mismatches = mismatch_count
        elif stage == "render_commands":
            render_commands_shape_mismatches = mismatch_count
        else:
            render_execute_shape_mismatches = mismatch_count
        if stage_first is not None and first_mismatch is None:
            index, field, oracle_value, wasmtime_value = stage_first
            first_mismatch = (
                index, stage, field, oracle_value, wasmtime_value)

    frame_count = max(
        len(oracle_build_events),
        len(wasmtime_build_events),
        len(oracle_execute),
        len(wasmtime_execute),
    )
    if frame_count != expected_frames and first_mismatch is None:
        first_mismatch = (
            0,
            "fixture",
            "frame_count",
            expected_frames,
            frame_count,
        )
    for index in range(frame_count):
        if index >= len(oracle_build_events) or index >= len(wasmtime_build_events):
            build_flow_mismatches += 1
            if not allow_render_flow_diagnostics and first_mismatch is None:
                first_mismatch = (
                    index,
                    "build_flow",
                    "event_count",
                    index < len(oracle_build_events),
                    index < len(wasmtime_build_events),
                )
        else:
            oracle_flow = _build_flow(oracle_build_events[index])
            wasmtime_flow = _build_flow(wasmtime_build_events[index])
            field_diff = _compare_build_flow(oracle_flow, wasmtime_flow)
            if field_diff is not None:
                build_flow_mismatches += 1
                if not allow_render_flow_diagnostics and first_mismatch is None:
                    field, oracle_value, wasmtime_value = field_diff
                    first_mismatch = (
                        index,
                        "build_flow",
                        field,
                        {
                            "value": oracle_value,
                            "digest": _flow_digest(oracle_flow),
                        },
                        {
                            "value": wasmtime_value,
                            "digest": _flow_digest(wasmtime_flow),
                        },
                    )

        if index >= len(oracle_execute) or index >= len(wasmtime_execute):
            if execute_checkpoint_enabled:
                execute_pre_mismatches += 1
                execute_post_mismatches += 1
                update_layer_after_draw_pre_mismatches += 1
                update_layer_after_draw_post_mismatches += 1
                post_draw_mismatches += 1
            if (semantic_only or execute_checkpoint_enabled) and \
                    first_mismatch is None:
                first_mismatch = (
                    index,
                    "render_execute",
                    "execute_leave_count",
                    index < len(oracle_execute),
                    index < len(wasmtime_execute),
                )
            continue

        oracle_event = oracle_execute[index]
        wasmtime_event = wasmtime_execute[index]
        if execute_checkpoint_enabled:
            oracle_post_draw = decoded_image_hash(
                oracle_root, oracle_event.get("postDrawImage"), oracle_cache)
            wasmtime_post_draw = decoded_image_hash(
                wasmtime_root, wasmtime_event.get("postDrawImage"),
                wasmtime_cache)
            if oracle_post_draw != wasmtime_post_draw:
                post_draw_mismatches += 1
                if first_mismatch is None:
                    first_mismatch = (
                        index,
                        "render_execute",
                        "post_draw.rgbaSha256",
                        oracle_post_draw,
                        wasmtime_post_draw,
                    )

            oracle_pre = decoded_image_hash(
                oracle_root, oracle_event.get("executePreImage"), oracle_cache)
            wasmtime_pre = decoded_image_hash(
                wasmtime_root, wasmtime_event.get("executePreImage"),
                wasmtime_cache)
            if oracle_pre != wasmtime_pre:
                execute_pre_mismatches += 1
                if first_mismatch is None:
                    first_mismatch = (
                        index,
                        "render_execute",
                        "execute_pre.rgbaSha256",
                        oracle_pre,
                        wasmtime_pre,
                    )

            oracle_post = decoded_image_hash(
                oracle_root, oracle_event.get("executePostImage"), oracle_cache)
            wasmtime_post = decoded_image_hash(
                wasmtime_root, wasmtime_event.get("executePostImage"),
                wasmtime_cache)
            if oracle_post != wasmtime_post:
                execute_post_mismatches += 1
                if first_mismatch is None:
                    first_mismatch = (
                        index,
                        "render_execute",
                        "execute_post.rgbaSha256",
                        oracle_post,
                        wasmtime_post,
                    )

            oracle_update_pre = decoded_image_hash(
                oracle_root,
                oracle_event.get("updateLayerAfterDrawPreImage"),
                oracle_cache,
            )
            wasmtime_update_pre = decoded_image_hash(
                wasmtime_root,
                wasmtime_event.get("updateLayerAfterDrawPreImage"),
                wasmtime_cache,
            )
            if oracle_update_pre != wasmtime_update_pre:
                update_layer_after_draw_pre_mismatches += 1
                if first_mismatch is None:
                    first_mismatch = (
                        index,
                        "render_execute",
                        "updateLayerAfterDraw_pre.rgbaSha256",
                        oracle_update_pre,
                        wasmtime_update_pre,
                    )

            oracle_update_post = decoded_image_hash(
                oracle_root,
                oracle_event.get("updateLayerAfterDrawPostImage"),
                oracle_cache,
            )
            wasmtime_update_post = decoded_image_hash(
                wasmtime_root,
                wasmtime_event.get("updateLayerAfterDrawPostImage"),
                wasmtime_cache,
            )
            if oracle_update_post != wasmtime_update_post:
                update_layer_after_draw_post_mismatches += 1
                if first_mismatch is None:
                    first_mismatch = (
                        index,
                        "render_execute",
                        "updateLayerAfterDraw_post.rgbaSha256",
                        oracle_update_post,
                        wasmtime_update_post,
                    )

    if not semantic_only and not ignore_layer_save:
        layer_save_mismatches, layer_save_first = _compare_layer_save_images(
            oracle_root,
            wasmtime_root,
            oracle_layer_save,
            wasmtime_layer_save,
            oracle_cache,
            wasmtime_cache,
        )
        if layer_save_first is not None and first_mismatch is None:
            field, oracle_value, wasmtime_value = layer_save_first
            first_mismatch = (
                0,
                "layer_save",
                field,
                oracle_value,
                wasmtime_value,
            )

    ok = first_mismatch is None
    if ok:
        print(f"{case_id}: PASS frames={frame_count}")
        if allow_render_flow_diagnostics and build_flow_mismatches:
            print(
                f"{case_id}: diagnostics build_flow_mismatch="
                f"{build_flow_mismatches}")
    else:
        index, stage, field, oracle_value, wasmtime_value = first_mismatch
        label_event = (
            oracle_execute[index] if index < len(oracle_execute)
            else oracle_build_events[index] if index < len(oracle_build_events)
            else {}
        )
        print(
            f"{case_id}: FAIL first_mismatch localFrame="
            f"{_event_frame_label(label_event, index)} stage={stage} "
            f"field={field} oracle={_value_summary(oracle_value)} "
            f"wasmtime={_value_summary(wasmtime_value)}")

    layer_save_summary = (
        "semantic-only" if semantic_only
        else "ignored" if ignore_layer_save
        else str(layer_save_mismatches)
    )
    print(
        f"{case_id}: summary "
        f"render_prepare_shape_mismatch={render_prepare_shape_mismatches} "
        f"render_commands_shape_mismatch={render_commands_shape_mismatches} "
        f"render_execute_shape_mismatch={render_execute_shape_mismatches} "
        f"build_flow_mismatch={build_flow_mismatches} "
        f"execute_pre_mismatch={execute_pre_mismatches} "
        f"execute_post_mismatch={execute_post_mismatches} "
        "update_layer_after_draw_pre_mismatch="
        f"{update_layer_after_draw_pre_mismatches} "
        "update_layer_after_draw_post_mismatch="
        f"{update_layer_after_draw_post_mismatches} "
        f"post_draw_mismatch={post_draw_mismatches} "
        f"layer_save_mismatch={layer_save_summary}")
    return ok


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    specs = {
        str(spec["id"]): spec
        for spec in (
            json.loads(path.read_text(encoding="utf-8"))
            for path in sorted(args.spec_dir.glob("*.json"))
        )
    }
    if not specs:
        raise SystemExit(f"no motion_playback specs found in {args.spec_dir}")
    validate_simulation_contract(specs.values())

    for label, root in (("oracle", args.oracle_root),
                        ("wasmtime", args.wasmtime_root)):
        manifest_path = root / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        fixture = manifest.get("fixture")
        if not isinstance(fixture, dict):
            raise SystemExit(f"{label} manifest has no fixture object: "
                             f"{manifest_path}")
        fps = float(fixture.get("simulationFps", float("nan")))
        delta_ms = float(fixture.get("deltaMs", float("nan")))
        if fps != SIMULATION_FPS or not math.isclose(
            delta_ms, SIMULATION_DELTA_MS, rel_tol=0.0, abs_tol=1.0e-12
        ):
            raise SystemExit(
                f"{label} render manifest timing mismatch: "
                f"simulationFps={fps}, deltaMs={delta_ms}; expected "
                f"{SIMULATION_FPS:g} Hz / {SIMULATION_DELTA_MS} ms"
            )

    cases = args.case or case_ids(args.oracle_root)
    if not cases:
        raise SystemExit("no cases found")
    all_ok = True
    for case_id in cases:
        spec = specs.get(case_id)
        if spec is None:
            print(f"{case_id}: FAIL no matching motion_playback spec")
            all_ok = False
            continue
        if not compare_case(
            args.oracle_root,
            args.wasmtime_root,
            case_id,
            expected_frames=int(spec["frames"]),
            allow_render_flow_diagnostics=(
                args.allow_render_flow_diagnostics),
            ignore_layer_save=args.ignore_layer_save,
            semantic_only=args.semantic_only,
        ):
            all_ok = False
    return 0 if all_ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
