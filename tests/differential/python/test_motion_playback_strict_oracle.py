#!/usr/bin/env python3
"""Offline checks for strict motion_playback oracle validation."""

from __future__ import annotations

import copy
import json
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tests" / "differential"))

from oracle_runner.adapters import motion_playback as mpb  # noqa: E402


class StrictMotionPlaybackOracleTest(unittest.TestCase):
    def _spec(self, case_id: str) -> dict:
        path = (
            REPO_ROOT / "tests" / "differential" / "specs" /
            "motion_playback" / f"{case_id}.json"
        )
        return json.loads(path.read_text(encoding="utf-8"))

    def _strict_frames(self, case_id: str) -> list[dict]:
        spec = self._spec(case_id)
        strict_frames = []
        for index in range(int(spec["frames"])):
            layers = [{
                "index": 0,
                "nodeType": 0,
                "opacity": 255,
                "stencilType": 0,
                **{key: 0.0 for key in mpb.LAYER_FIELDS_NUM},
                **{key: False for key in mpb.LAYER_FIELDS_BOOL},
            }]
            player_count = 1
            strict_frames.append({
                "frameId": index,
                "projection": mpb.TRACE_FLATTEN_PROJECTION,
                "samplePoint": mpb.TRACE_FLATTEN_SAMPLE_POINT,
                "sampleOrder": mpb.TRACE_FLATTEN_SAMPLE_ORDER,
                "deltaMs": 0 if index == 0 else 67,
                "playerLayerCounts": [len(layers)],
                "playerCount": player_count,
                "diagnostics": {
                    "layout": "deque",
                    "topPlayer": "0x1",
                    "players": [
                        {"ptr": f"0x{player + 1:x}", "layout": "deque"}
                        for player in range(player_count)
                    ],
                },
                "layers": layers,
            })
        return strict_frames

    def test_specs_keep_15hz_frame_contract_without_behavior_goldens(self) -> None:
        for case_id in ("yuzulogo", "m2logo"):
            with self.subTest(case_id=case_id):
                spec = self._spec(case_id)
                self.assertGreater(int(spec["frames"]), 0)
                self.assertEqual(float(spec["simulation_fps"]), 15.0)
                self.assertNotIn("oracle_sanity", spec)

    def test_strict_validator_accepts_valid_trace_shape(self) -> None:
        for case_id in ("yuzulogo", "m2logo"):
            with self.subTest(case_id=case_id):
                spec = self._spec(case_id)
                frames = self._strict_frames(case_id)
                mpb._validate_trace_flatten_segment(spec, frames)

    def test_normalization_retains_sampling_evidence(self) -> None:
        raw = self._strict_frames("m2logo")[0]
        normalized = mpb.normalize_frame(raw, 0)
        for field in ("samplePoint", "sampleOrder", "deltaMs", "playerLayerCounts"):
            self.assertEqual(normalized[field], raw[field])
        self.assertEqual(mpb.diff_frames([normalized], [normalized]), [])

    def test_identical_layers_cannot_hide_different_sampling(self) -> None:
        frame = mpb.normalize_frame(self._strict_frames("m2logo")[0], 0)
        for field, value in (
            ("samplePoint", "progress.return"),
            ("sampleOrder", "entry-order"),
            ("deltaMs", 67),
            ("frame", 1),
            ("playerLayerCounts", []),
        ):
            with self.subTest(field=field):
                other = copy.deepcopy(frame)
                other[field] = value
                diffs = mpb.diff_frames([frame], [other])
                self.assertTrue(diffs)
                self.assertEqual(diffs[0]["kind"], "sampling_contract")

    def test_legacy_frames_require_recapture_even_when_both_match(self) -> None:
        legacy = {"frame": 0, "layers": self._strict_frames("m2logo")[0]["layers"]}
        self.assertEqual(
            mpb.diff_frames([legacy], [legacy])[0]["kind"], "sampling_contract")
        with self.assertRaisesRegex(RuntimeError, "re-record"):
            mpb.normalize_frame(legacy, 0)

    def test_invalid_trace_flatten_frames_fail_validation(self) -> None:
        spec = self._spec("yuzulogo")
        base_frames = self._strict_frames("yuzulogo")

        def mutate_root_only(frames: list[dict]) -> None:
            diag = frames[0]["diagnostics"]
            diag["layout"] = "root-only"
            diag["players"][0]["layout"] = "root-only"

        def mutate_deque_error(frames: list[dict]) -> None:
            diag = frames[0]["diagnostics"]
            diag["error"] = "deque-error: synthetic"
            diag["players"][0]["error"] = "deque-error: synthetic"

        def mutate_empty_layers(frames: list[dict]) -> None:
            frames[0]["layers"].clear()

        def mutate_missing_field(frames: list[dict]) -> None:
            del frames[0]["layers"][0]["posX"]

        def mutate_huge_float(frames: list[dict]) -> None:
            frames[0]["layers"][0]["posY"] = 1.0e74

        def mutate_non_finite_float(frames: list[dict]) -> None:
            frames[0]["layers"][0]["posZ"] = float("nan")

        def mutate_bad_opacity(frames: list[dict]) -> None:
            frames[0]["layers"][0]["opacity"] = 999

        cases = {
            "root-only": mutate_root_only,
            "deque-error": mutate_deque_error,
            "empty-layers": mutate_empty_layers,
            "missing-field": mutate_missing_field,
            "huge-float": mutate_huge_float,
            "non-finite-float": mutate_non_finite_float,
            "bad-opacity": mutate_bad_opacity,
        }
        for name, mutate in cases.items():
            with self.subTest(name=name):
                frames = copy.deepcopy(base_frames)
                mutate(frames)
                with self.assertRaisesRegex(
                    RuntimeError,
                    "strict motion_playback oracle validation failed",
                ):
                    mpb._validate_trace_flatten_segment(spec, frames)


if __name__ == "__main__":
    unittest.main()
