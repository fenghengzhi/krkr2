#!/usr/bin/env python3
"""Fail-closed validator for motion render-stage artifact directories."""

from __future__ import annotations

import argparse
import json
import sys
from collections import Counter
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tests" / "differential"))
from oracle_runner.png_artifacts import png_rgba_info  # noqa: E402

DEFAULT_REQUIRED_STAGES = (
    "draw_dispatch",
    "render_prepare",
    "render_commands",
    "render_execute",
)


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate a motion render-stage artifact root")
    parser.add_argument("--artifact-root", type=Path, required=True)
    parser.add_argument(
        "--require-stage", action="append", default=[],
        help="Stage that must contain events for every case; repeatable. "
             "Defaults to draw/prepare/commands/execute.")
    return parser.parse_args(argv)


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError(f"cannot load JSON {path}: {exc}") from exc


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def validate_images(
    root: Path,
    manifest: dict[str, Any],
) -> tuple[dict[tuple[str, str, int], dict[str, Any]], int]:
    images = manifest.get("images")
    require(isinstance(images, dict), "manifest.images is not an object")
    cases = images.get("cases")
    require(isinstance(cases, list) and cases, "manifest.images.cases is empty")
    semantic_only = images.get("semanticOnly") is True
    if semantic_only:
        require(images.get("captureSurfaces") == [],
                "semantic-only artifact declares image capture surfaces")

    entries: dict[tuple[str, str, int], dict[str, Any]] = {}
    referenced_paths: set[Path] = set()
    for case in cases:
        require(isinstance(case, dict), "image case is not an object")
        case_id = case.get("caseId")
        frames = case.get("frames")
        phases = case.get("phases")
        require(isinstance(case_id, str) and case_id, "image case has no caseId")
        require(isinstance(frames, int) and frames >= 0,
                f"{case_id}: invalid frames")
        require(isinstance(phases, dict), f"{case_id}: phases is not an object")
        if semantic_only:
            require(not phases,
                    f"{case_id}: semantic-only artifact contains image phases")
        expected_frames = list(range(frames))
        for field in ("capturedLocalFrames", "requestedLocalFrames"):
            value = case.get(field)
            if value is not None:
                require(value == expected_frames,
                        f"{case_id}: {field} is not 0..{frames - 1}")

        for phase, phase_entries in phases.items():
            require(isinstance(phase, str) and isinstance(phase_entries, list),
                    f"{case_id}: invalid phase entry")
            frame_numbers: list[int] = []
            for entry in phase_entries:
                require(isinstance(entry, dict),
                        f"{case_id}/{phase}: image entry is not an object")
                frame = entry.get("frame")
                rel = entry.get("path")
                require(isinstance(frame, int) and 0 <= frame < frames,
                        f"{case_id}/{phase}: invalid frame {frame!r}")
                require(isinstance(rel, str) and rel,
                        f"{case_id}/{phase}/{frame}: missing path")
                key = (case_id, phase, frame)
                require(key not in entries, f"duplicate image key {key}")
                path = root / rel
                require(path.is_file(), f"missing image {path}")
                require(path not in referenced_paths,
                        f"image path referenced more than once: {path}")
                referenced_paths.add(path)
                width, height, rgba_sha256 = png_rgba_info(path)
                require(entry.get("width") == width,
                        f"{path}: width mismatch")
                require(entry.get("height") == height,
                        f"{path}: height mismatch")
                require(entry.get("bytes") == path.stat().st_size,
                        f"{path}: byte-size mismatch")
                require(entry.get("rgbaSha256") == rgba_sha256,
                        f"{path}: decoded RGBA SHA-256 mismatch")
                entries[key] = entry
                frame_numbers.append(frame)
            require(frame_numbers == sorted(frame_numbers),
                    f"{case_id}/{phase}: image frames are not ordered")
            require(len(frame_numbers) == len(set(frame_numbers)),
                    f"{case_id}/{phase}: duplicate frame")

    disk_paths = set(root.rglob("*.png"))
    require(disk_paths == referenced_paths,
            "PNG file set differs from manifest references: "
            f"extra={sorted(disk_paths - referenced_paths)[:5]} "
            f"missing={sorted(referenced_paths - disk_paths)[:5]}")
    expected_count = manifest.get("summary", {}).get("imageCount")
    require(expected_count == len(entries),
            f"manifest imageCount={expected_count!r}, decoded={len(entries)}")
    if semantic_only:
        require(not entries, "semantic-only artifact contains image entries")
    return entries, len(cases)


def validate_events(
    root: Path,
    manifest: dict[str, Any],
    image_entries: dict[tuple[str, str, int], dict[str, Any]],
    required_stages: set[str],
) -> int:
    case_frames = {
        case["caseId"]: int(case["frames"])
        for case in manifest["images"]["cases"]
    }
    event_paths = sorted((root / "events").rglob("*.json"))
    require(event_paths, "artifact has no event JSON files")
    seen_stage_cases: set[tuple[str, str]] = set()
    total_events = 0

    for path in event_paths:
        envelope = load_json(path)
        require(isinstance(envelope, dict), f"{path}: root is not an object")
        stage = envelope.get("stage")
        case_id = envelope.get("caseId")
        source = envelope.get("source")
        events = envelope.get("events")
        summary = envelope.get("summary")
        require(isinstance(stage, str) and stage == path.parent.name,
                f"{path}: stage/path mismatch")
        require(isinstance(case_id, str) and case_id in case_frames,
                f"{path}: unknown caseId {case_id!r}")
        require(isinstance(source, str) and source,
                f"{path}: missing source")
        require(isinstance(events, list), f"{path}: events is not a list")
        require(isinstance(summary, dict), f"{path}: summary is not an object")
        seen_stage_cases.add((stage, case_id))
        if stage in required_stages:
            require(events, f"{path}: required stage is empty")

        frame_ids: set[int] = set()
        kinds: Counter[str] = Counter()
        for index, event in enumerate(events):
            require(isinstance(event, dict),
                    f"{path}[{index}]: event is not an object")
            require(event.get("stage") == stage,
                    f"{path}[{index}]: event stage mismatch")
            require(event.get("caseId") == case_id,
                    f"{path}[{index}]: event caseId mismatch")
            require(event.get("source") == source,
                    f"{path}[{index}]: event source mismatch")
            require(isinstance(event.get("schema"), str),
                    f"{path}[{index}]: event schema missing")
            if stage in DEFAULT_REQUIRED_STAGES:
                require(event.get("captureContract") == "motion-render-boundaries-v1",
                        f"{path}[{index}]: missing capture contract; re-record artifact")
            frame_id = event.get("frameId")
            require(isinstance(frame_id, int) and
                    0 <= frame_id < case_frames[case_id],
                    f"{path}[{index}]: invalid frameId {frame_id!r}")
            kind = event.get("kind")
            require(isinstance(kind, str) and kind,
                    f"{path}[{index}]: missing kind")
            frame_ids.add(frame_id)
            kinds[kind] += 1

            if stage == "layer_save" and kind == "save_layer_image":
                key = (case_id, event.get("phase"), event.get("frame"))
                image = image_entries.get(key)
                require(image is not None,
                        f"{path}[{index}]: no manifest image for {key}")
                for field in ("path", "width", "height", "bytes",
                              "rgbaSha256"):
                    require(event.get(field) == image.get(field),
                            f"{path}[{index}]: {field} differs from image manifest")

        require(summary.get("eventCount") == len(events),
                f"{path}: eventCount summary mismatch")
        require(summary.get("framesWithEvents") == len(frame_ids),
                f"{path}: framesWithEvents summary mismatch")
        require(summary.get("kindCounts") == dict(kinds),
                f"{path}: kindCounts summary mismatch")
        total_events += len(events)

    for case_id in case_frames:
        for stage in required_stages:
            require((stage, case_id) in seen_stage_cases,
                    f"missing required stage file: {case_id}/{stage}")
    manifest_summary = manifest.get("summary", {})
    expected_count = manifest_summary.get(
        "renderEventCount", manifest_summary.get("eventCount"))
    require(expected_count == total_events,
            f"manifest renderEventCount={expected_count!r}, actual={total_events}")
    return total_events


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    root = args.artifact_root.resolve()
    required_stages = set(args.require_stage or DEFAULT_REQUIRED_STAGES)
    try:
        require(root.is_dir(), f"artifact root is not a directory: {root}")
        manifest = load_json(root / "manifest.json")
        require(isinstance(manifest, dict), "manifest root is not an object")
        images, case_count = validate_images(root, manifest)
        event_count = validate_events(
            root, manifest, images, required_stages)
        trace_count = sum(
            int(case["frames"]) for case in manifest["images"]["cases"])
        require(manifest.get("summary", {}).get("caseCount") == case_count,
                "manifest caseCount mismatch")
        require(manifest.get("summary", {}).get("traceFlattenFrameCount") ==
                trace_count, "manifest traceFlattenFrameCount mismatch")
    except ValueError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    print(
        f"PASS: {root} cases={case_count} frames={trace_count} "
        f"events={event_count} images={len(images)} "
        f"requiredStages={','.join(sorted(required_stages))}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
