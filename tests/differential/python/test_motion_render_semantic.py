from __future__ import annotations

import importlib.util
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
DIFFERENTIAL_ROOT = REPO_ROOT / "tests" / "differential"
sys.path.insert(0, str(DIFFERENTIAL_ROOT))


def load_script(name: str):
    path = DIFFERENTIAL_ROOT / "python" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


render_compare = load_script("compare_motion_render_steps")
stage_oracle = load_script("run_motion_stage_oracle")
artifact_validator = load_script("validate_motion_render_artifact")


class MotionRenderSemanticTests(unittest.TestCase):
    def test_render_path_selects_only_rebased_semantic_stages(self) -> None:
        self.assertEqual(
            stage_oracle.selected_stages("render_path"),
            [
                "draw_dispatch",
                "render_prepare",
                "render_commands",
                "render_execute",
            ],
        )

    def test_libgame_render_runner_uses_startup_from_without_tjs_init(
        self,
    ) -> None:
        source = (
            DIFFERENTIAL_ROOT / "python" / "run_motion_stage_oracle.py"
        ).read_text(encoding="utf-8")
        self.assertIn("mpb.trigger_startup(engine, remote_game)", source)
        self.assertNotIn("engine.tjs_init()", source)

    def test_android_render_events_receive_canonical_source(self) -> None:
        split = stage_oracle.split_render_events_by_stage_and_case(
            [{
                "schema": "motion-render-stage-oracle-v1-event",
                "stage": "draw_dispatch",
                "kind": "draw_enter",
                "frameId": 0,
                "seq": 1,
            }],
            [{
                "caseId": "case",
                "firstFrameId": 0,
                "lastFrameId": 0,
                "firstSeq": 0,
                "lastSeq": 2,
            }],
        )
        event = split["draw_dispatch"]["case"][0]
        self.assertEqual(event["caseId"], "case")
        self.assertEqual(event["source"], stage_oracle.RENDER_SOURCE)

    def test_android_agent_uses_139_render_entrypoints(self) -> None:
        source = (
            DIFFERENTIAL_ROOT
            / "oracle_runner"
            / "frida_motion_stage_agent.js"
        ).read_text(encoding="utf-8")
        expected = {
            "PLAYER_DRAW_COMPAT_OFF": "0x6D3398",
            "PLAYER_DRAW_D3D_OFF": "0x6D2F70",
            "PLAYER_DRAW_SLA_OFF": "0x6D2A38",
            "PLAYER_RENDER_PREPARE_OFF": "0x6D2544",
            "PLAYER_APPLY_PREPARED_PROJECTION_OFF": "0x6D2644",
            "PLAYER_BUILD_ITEMS_OFF": "0x6BF714",
            "PLAYER_BUILD_COMMANDS_OFF": "0x6C2208",
            "PLAYER_ACCURATE_SLA_RENDER_OFF": "0x6C7088",
            "PLAYER_RENDER_EXECUTE_OFF": "0x6C4820",
            "PLAYER_UPDATE_LAYER_AFTER_DRAW_OFF": "0x6CBBB8",
            "DEBUG_MESSAGE_OFF": "0xA178BC",
        }
        for name, value in expected.items():
            with self.subTest(name=name):
                self.assertRegex(
                    source,
                    rf"const\s+{re.escape(name)}\s*=\s*{value};",
                )
        self.assertIn("capability: 'motion-render-semantic-v1'", source)
        self.assertIn("enabledStages.add(STAGE_TRACE_FLATTEN);", source)

    @staticmethod
    def _write_events(
        root: Path,
        source_suffix: str,
        stage: str,
        events: list[dict],
    ) -> None:
        path = root / "events" / stage / f"case.{source_suffix}.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(
            json.dumps({"events": events}) + "\n",
            encoding="utf-8",
        )

    def _artifact_pair(self, *, drop_wasmtime_execute: bool = False):
        temp = tempfile.TemporaryDirectory()
        root = Path(temp.name)
        oracle = root / "oracle"
        wasmtime = root / "wasmtime"
        prepare = [
            {"frameId": 0, "kind": kind}
            for kind in (
                "prepare_enter",
                "prepare_leave",
                "apply_projection_enter",
                "apply_projection_leave",
            )
        ]
        commands = [
            {"frameId": 0, "kind": "build_items_enter"},
            {"frameId": 0, "kind": "build_items_leave"},
            {
                "frameId": 0,
                "kind": "build_commands_enter",
                "buildFlow": {},
            },
            {
                "frameId": 0,
                "kind": "build_commands_leave",
                "buildFlow": {},
            },
        ]
        execute = [
            {"frameId": 0, "kind": "execute_enter"},
            {"frameId": 0, "kind": "execute_leave"},
        ]
        # Real envelope: prepared-item construction belongs inside prepare;
        # command construction belongs inside execute.
        ordered = [prepare[0], *commands[:2], *prepare[1:],
                   execute[0], *commands[2:], execute[1]]
        for seq, event in enumerate(ordered):
            event["seq"] = seq
            event["captureContract"] = render_compare.RENDER_CAPTURE_CONTRACT
        for event in execute:
            event["executeBoundary"] = "Player.renderToCanvas"
            event["samplePoint"] = "Player.renderToCanvas." + (
                "enter" if event["kind"] == "execute_enter" else "leave")
        for artifact, suffix in ((oracle, "oracle"),
                                 (wasmtime, "wasmtime")):
            self._write_events(
                artifact, suffix, "render_prepare", prepare)
            self._write_events(
                artifact, suffix, "render_commands", commands)
            self._write_events(
                artifact,
                suffix,
                "render_execute",
                [] if drop_wasmtime_execute and suffix == "wasmtime"
                else execute,
            )
        return temp, oracle, wasmtime

    def test_semantic_compare_needs_no_image_artifacts(self) -> None:
        temp, oracle, wasmtime = self._artifact_pair()
        with temp:
            self.assertTrue(render_compare.compare_case(
                oracle,
                wasmtime,
                "case",
                expected_frames=1,
                semantic_only=True,
            ))

    def test_semantic_compare_fails_closed_without_execute_envelope(self) -> None:
        temp, oracle, wasmtime = self._artifact_pair(
            drop_wasmtime_execute=True)
        with temp:
            self.assertFalse(render_compare.compare_case(
                oracle,
                wasmtime,
                "case",
                expected_frames=1,
                semantic_only=True,
            ))

    def test_command_build_crossing_execute_boundary_fails(self) -> None:
        temp, oracle, wasmtime = self._artifact_pair()
        with temp:
            path = wasmtime / "events/render_execute/case.wasmtime.json"
            payload = json.loads(path.read_text())
            # Keep both per-stage kind sequences intact; move only enter to
            # after command construction, reproducing the old inner scope.
            payload["events"][0]["seq"] = 100
            payload["events"][1]["seq"] = 101
            path.write_text(json.dumps(payload))
            self.assertFalse(render_compare.compare_case(
                oracle, wasmtime, "case", expected_frames=1,
                allow_render_flow_diagnostics=True, semantic_only=True))

    def test_sequence_numbers_can_restart_in_a_later_frame(self) -> None:
        events = [{
            "frameId": frame, "seq": 0, "kind": "prepare_enter",
            "captureContract": render_compare.RENDER_CAPTURE_CONTRACT,
        } for frame in (0, 1)]
        sequences = render_compare.capture_boundary_sequences({
            "render_prepare": events,
        })
        self.assertEqual(sequences[0], sequences[1])
        with self.assertRaisesRegex(ValueError, "duplicate sequence"):
            render_compare.capture_boundary_sequences({
                "render_prepare": [events[0], events[0]],
            })

    def test_missing_boundary_protocol_requires_recapture(self) -> None:
        temp, oracle, wasmtime = self._artifact_pair()
        with temp:
            for root, suffix in ((oracle, "oracle"), (wasmtime, "wasmtime")):
                path = root / f"events/render_execute/case.{suffix}.json"
                payload = json.loads(path.read_text())
                del payload["events"][0]["captureContract"]
                path.write_text(json.dumps(payload))
            self.assertFalse(render_compare.compare_case(
                oracle, wasmtime, "case", expected_frames=1, semantic_only=True))

    def test_command_construction_requires_the_outer_execute_envelope(self) -> None:
        with self.assertRaisesRegex(ValueError, "outside execute envelope"):
            render_compare.capture_boundary_sequences({
                "render_commands": [{
                    "frameId": 0, "seq": 0, "kind": "build_commands_enter",
                    "captureContract": render_compare.RENDER_CAPTURE_CONTRACT,
                }],
            })

    def test_validator_accepts_explicit_empty_semantic_image_envelope(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = {
                "images": {
                    "semanticOnly": True,
                    "captureSurfaces": [],
                    "cases": [{
                        "caseId": "case",
                        "frames": 1,
                        "capturedLocalFrames": [0],
                        "requestedLocalFrames": [0],
                        "phases": {},
                    }],
                },
                "summary": {"imageCount": 0},
            }
            entries, case_count = artifact_validator.validate_images(
                root, manifest)
            self.assertEqual(entries, {})
            self.assertEqual(case_count, 1)

    def test_validator_rejects_images_in_semantic_only_envelope(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            manifest = {
                "images": {
                    "semanticOnly": True,
                    "captureSurfaces": [],
                    "cases": [{
                        "caseId": "case",
                        "frames": 1,
                        "capturedLocalFrames": [0],
                        "requestedLocalFrames": [0],
                        "phases": {"post_draw": []},
                    }],
                },
                "summary": {"imageCount": 0},
            }
            with self.assertRaisesRegex(
                ValueError, "semantic-only artifact contains image phases",
            ):
                artifact_validator.validate_images(root, manifest)


if __name__ == "__main__":
    unittest.main()
