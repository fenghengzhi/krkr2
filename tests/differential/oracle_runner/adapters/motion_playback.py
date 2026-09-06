"""Adapter for the `motion_playback` differential family.

Two modes:
  * Live oracle (`record_all_oracles`): attach Frida to the APK harness,
    drop the 15 Hz-targeted oracle XP3 on the device, trigger
    `TVPMainScene::startupFrom` via the harness-RPC engine (pure
    scheduler call; doesn't touch GL thread state), and let the embedded
    `startup.tjs` play yuzulogo then m2logo on the cocos2d GL thread.
    The active 1.3.9 stage tracer snapshots each Player at the final phase-3
    helper return, before cleanup, inside the script-visible progress window.
    Values are copied on the GL thread; progress return only packages them.

    Rationale for the architecture split (harness-RPC for the boot
    call; Frida for the runtime observation): see the "分工原则"
    section in /Users/bytedance/.claude/plans/
    oracle-runner-panda-floofy-garden.md.

  * Disk oracle (`run_case`): compare supplied port trace frames
    against a locally recorded oracle JSON. No Android device is required.

Previous revisions of this file shipped a TJS snapshot script executed
via `engine.tjs_exec_str` from the harness-rpc pthread. That approach
crashed consistently: Motion.Player's `getLayerNames`/`draw` methods
iterate the node tree with GL-thread assumptions, and calling them
from our RPC worker SIGSEGV'd in `emutls_key_destructor`. Hooking
`Player_updateLayers` from its *natural* GL-thread caller side-steps
the whole problem.
"""

from __future__ import annotations

import json
import math
import re
import shlex
import subprocess
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path, PurePosixPath
from typing import Any

from oracle_runner.motion_capture_window import (
    FrameCaptureWindow,
    captured_case_ranges,
    frame_capture_window_from_args,
)
from oracle_runner.motion_timing import (
    DEFAULT_STARTUP_XP3_NAME,
    SIMULATION_DELTA_MS,
    SIMULATION_FPS,
    select_expected_segments,
)
from oracle_runner.png_artifacts import png_manifest_entry


# Schema fields, kept in sync with the port-side motion trace schema.
# The default golden diff intentionally compares Motion node state only.
# Strings/images and draw diagnostics are kept in JSON snapshots to aid
# investigation, but libkrkr2's Frida trace and the Wasmtime port trace do not
# need their diagnostic labels to match byte-for-byte for state parity.
LAYER_FIELDS_NUM = (
    "posX", "posY", "posZ", "angleDeg",
    "scaleX", "scaleY", "slantX", "slantY",
)
LAYER_FIELDS_INT = ("opacity", "blendMode", "nodeType", "index")
LAYER_FIELDS_BOOL = ("visible", "active", "flipX", "flipY")
LAYER_FIELDS_STR = ("label", "currentImage")
COMPARE_FIELDS_STR: tuple[str, ...] = ()


# Order that logo_test.xp3's startup.tjs plays motions. We partition the
# Frida trace by player pointer; the first segment is yuzulogo, second
# is m2logo. Adapter-level contract: spec ids must be one of these.
SEGMENT_ORDER: tuple[str, ...] = ("yuzulogo", "m2logo")
TRACE_FLATTEN_PROJECTION = "trace_flatten-semantic-v1"
TRACE_FLATTEN_SAMPLE_POINT = "progressCompat.phase3-end.pre-cleanup"
TRACE_FLATTEN_SAMPLE_ORDER = "phase3-return-order"
TRACE_FLATTEN_ABS_FLOAT_LIMIT = 1_000_000.0


def segment_order_for_specs(specs_or_by_id) -> tuple[str, ...]:
    specs_by_id = (
        {str(k): v for k, v in specs_or_by_id.items()}
        if isinstance(specs_or_by_id, dict)
        else {str(spec["id"]): spec for spec in specs_or_by_id}
    )
    unknown = [sid for sid in specs_by_id if sid not in SEGMENT_ORDER]
    if unknown:
        raise ValueError(
            f"unknown motion_playback spec id(s): {unknown}. Expected ids "
            f"are fixed by logo_test oracle fixtures: {SEGMENT_ORDER}."
        )
    return tuple(sid for sid in SEGMENT_ORDER if sid in specs_by_id)


# 15 Hz oracle-recording XP3. Its startup.tjs keeps the same
# KAGParser -> AffineLayer -> AffineSourceMotion -> onPaint() playback path
# as logo_test.xp3 and derives each motion interval from tick - lastTick. The
# Android display and Kirikiroid playback preferences pin the callback target
# to 15 Hz. Software rendering can miss that target, so the Frida oracle
# virtualizes only TVPContinuousEventDispatch's tick-source call edge at the
# same 15 Hz cadence as Wasmtime. The production script data flow remains
# tick - lastTick; host performance cannot turn one simulated frame into a
# several-hundred-ms jump. Small scripts are tracked beside the build script;
# large assets stay under reference/xp3/logo_test. Regenerate via
# `tests/differential/oracle_runner/fixtures/build_logo_test_oracle.sh`
# whenever the spec frame counts change.
_LOGO_TEST_XP3_REL = f"reference/xp3/{DEFAULT_STARTUP_XP3_NAME}"
_ORACLE_APK_PACKAGE = "org.tvp.kirikiri2_free_10309"
_REMOTE_APP_FILES_DIR = f"/sdcard/Android/data/{_ORACLE_APK_PACKAGE}/files"
_REMOTE_STARTUP_FILES_DIR = f"/data/user/0/{_ORACLE_APK_PACKAGE}/files"
ORACLE_RENDERER = "software"
ORACLE_RENDERER_SOURCE = "explicit-oracle-preference"
ORACLE_FPS_LIMIT = int(SIMULATION_FPS)
ORACLE_FPS_LIMIT_SOURCE = "explicit-oracle-preference"
_ORACLE_GLOBAL_PREFERENCE_PATH = (
    f"/data/user/0/{_ORACLE_APK_PACKAGE}/files/.preference/GlobalPreference.xml"
)
_ORACLE_GAME_PREFERENCE_FILE = "Kirikiroid2Preference.xml"
_ORACLE_TMP_PREFERENCE_PATH = (
    "/data/local/tmp/krkr2-motion-oracle-renderer-preference.xml"
)
_FRAMEBUFFER_SCHEMA = "motion-framebuffer-oracle-v1"
_FRAMEBUFFER_SOURCE = "android-libkrkr2-saveLayerImage"
_FRAMEBUFFER_CAPTURE_SURFACE = (
    "Layer main image immediately after Motion.Player.draw(base)"
)
_RENDER_STAGE_CAPTURE_SURFACES = ("initial", "post_draw")
_REFERENCE_RENDER_STAGE_CAPTURE_ROOT = (
    f"{_REMOTE_STARTUP_FILES_DIR}/savedata/motion_render_stage_capture"
)


# ---------------------------------------------------------------- device ops

def push_fixture(serial: str | None, local: Path, remote: str) -> None:
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += ["push", str(local), remote]
    subprocess.run(cmd, check=True, capture_output=True)


def _adb_shell(serial: str | None, cmdline: str) -> str:
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += ["shell", cmdline]
    out = subprocess.run(cmd, check=True, capture_output=True)
    return out.stdout.decode(errors="replace")


def _adb_shell_root(serial: str | None, args: list[str]) -> str:
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += ["shell", "su", "0"] + args
    out = subprocess.run(cmd, check=True, capture_output=True)
    return out.stdout.decode(errors="replace")


def _chown_to_app_files_owner(serial: str | None, remote_path: str) -> None:
    package_uid_lines = _adb_shell(
        serial,
        f"cmd package list packages -U {_ORACLE_APK_PACKAGE} 2>/dev/null "
        "| sed -n 's/.*uid://p' || true",
    ).strip().splitlines()
    if package_uid_lines:
        uid = package_uid_lines[-1].strip()
        if re.fullmatch(r"\d+", uid):
            _adb_shell(
                serial,
                f"chown -R {uid}:{uid} {shlex.quote(remote_path)} "
                "2>/dev/null || true",
            )
            return

    owner_lines = _adb_shell(
        serial,
        "stat -c '%u:%g' "
        f"{shlex.quote(_REMOTE_STARTUP_FILES_DIR)} 2>/dev/null || true",
    ).strip().splitlines()
    if not owner_lines:
        return
    uid_gid = owner_lines[-1].strip()
    if not re.fullmatch(r"\d+:\d+", uid_gid):
        return
    _adb_shell(
        serial,
        f"chown -R {uid_gid} {shlex.quote(remote_path)} 2>/dev/null || true",
    )


def _adb_pull(serial: str | None, remote: str, local: Path) -> None:
    cmd = ["adb"]
    if serial:
        cmd += ["-s", serial]
    cmd += ["pull", remote, str(local)]
    subprocess.run(cmd, check=True, capture_output=True)


def _subprocess_error_text(exc: subprocess.CalledProcessError) -> str:
    parts: list[str] = []
    for name in ("stdout", "stderr"):
        data = getattr(exc, name, None)
        if not data:
            continue
        if isinstance(data, bytes):
            text = data.decode(errors="replace").strip()
        else:
            text = str(data).strip()
        if text:
            parts.append(f"{name}: {text}")
    return "; ".join(parts) or str(exc)


def _renderer_preference_xml(renderer: str = ORACLE_RENDERER) -> str:
    return (
        "<?xml version=\"1.0\"?>\n"
        "<GlobalPreference>\n"
        f"    <Item key=\"renderer\" value=\"{renderer}\"/>\n"
        "    <Item key=\"ogl_accurate_render\" value=\"false\"/>\n"
        f"    <Item key=\"fps_limit\" value=\"{ORACLE_FPS_LIMIT}\"/>\n"
        "</GlobalPreference>\n"
    )


def _write_local_temp_text(text: str) -> Path:
    with tempfile.NamedTemporaryFile(
        "w", encoding="utf-8", delete=False,
        prefix="krkr2-oracle-renderer-", suffix=".xml",
    ) as fp:
        fp.write(text)
        return Path(fp.name)


def _write_remote_text(
    serial: str | None,
    remote_path: str,
    text: str,
    *,
    root: bool,
) -> None:
    local = _write_local_temp_text(text)
    try:
        if root:
            push_fixture(serial, local, _ORACLE_TMP_PREFERENCE_PATH)
            parent = str(PurePosixPath(remote_path).parent)
            _adb_shell_root(serial, ["mkdir", "-p", parent])
            _adb_shell_root(
                serial, ["cp", _ORACLE_TMP_PREFERENCE_PATH, remote_path])
            _adb_shell_root(serial, ["chmod", "644", remote_path])
            try:
                _adb_shell(
                    serial,
                    f"rm -f {shlex.quote(_ORACLE_TMP_PREFERENCE_PATH)}",
                )
            except subprocess.CalledProcessError:
                pass
        else:
            parent = str(PurePosixPath(remote_path).parent)
            _adb_shell(serial, f"mkdir -p {shlex.quote(parent)}")
            push_fixture(serial, local, remote_path)
    finally:
        local.unlink(missing_ok=True)


def _read_remote_text(
    serial: str | None,
    remote_path: str,
    *,
    root: bool,
) -> str:
    if root:
        return _adb_shell_root(serial, ["cat", remote_path])
    return _adb_shell(serial, f"cat {shlex.quote(remote_path)}")


def _game_preference_path(remote_game: str | None = None) -> str:
    if remote_game:
        return str(
            PurePosixPath(remote_game).parent / _ORACLE_GAME_PREFERENCE_FILE
        )
    return f"{_REMOTE_APP_FILES_DIR}/{_ORACLE_GAME_PREFERENCE_FILE}"


def oracle_renderer_metadata() -> dict[str, str]:
    return {
        "renderer": ORACLE_RENDERER,
        "rendererSource": ORACLE_RENDERER_SOURCE,
        "fpsLimit": str(ORACLE_FPS_LIMIT),
        "fpsLimitSource": ORACLE_FPS_LIMIT_SOURCE,
    }


def ensure_oracle_renderer_software(
    serial: str | None,
    *,
    remote_game: str | None = None,
    write_global: bool = True,
) -> None:
    """Force Android oracle playback to use software rendering at 15 Hz.

    The global preference must be written before HarnessActivity starts;
    the per-game preference is written again immediately before startupFrom
    so stale device state cannot switch the parity lane's renderer or FPS.
    """
    xml = _renderer_preference_xml()
    try:
        if write_global:
            _write_remote_text(
                serial, _ORACLE_GLOBAL_PREFERENCE_PATH, xml, root=True)
            global_text = _read_remote_text(
                serial, _ORACLE_GLOBAL_PREFERENCE_PATH, root=True)
            if global_text.strip() != xml.strip():
                raise RuntimeError(
                    "Android Oracle playback preference verification failed; "
                    "software renderer / 15 Hz cannot be guaranteed. "
                    "Global preference did not match: "
                    f"{_ORACLE_GLOBAL_PREFERENCE_PATH}")

        game_pref = _game_preference_path(remote_game)
        _write_remote_text(serial, game_pref, xml, root=True)
        game_text = _read_remote_text(serial, game_pref, root=True)
        if game_text.strip() != xml.strip():
            raise RuntimeError(
                "Android Oracle playback preference verification failed; "
                "software renderer / 15 Hz cannot be guaranteed. "
                "Per-game preference did not match: "
                f"{game_pref}")
    except subprocess.CalledProcessError as exc:
        raise RuntimeError(
            "failed to enforce Android Oracle playback preferences; "
            "software renderer / 15 Hz cannot be guaranteed. "
            f"ADB error: {_subprocess_error_text(exc)}"
        ) from exc


def _ensure_logo_test_xp3_pushed(
    serial: str | None,
    startup_xp3: Path | None = None,
) -> str:
    """Push the oracle bootstrap xp3 to a device startup path.

    `startup_xp3` lets callers run a single-motion fixture; the default is the
    combined tracked 15 Hz fixture.
    """
    repo_root = Path(__file__).resolve().parents[4]
    local = startup_xp3 or repo_root / _LOGO_TEST_XP3_REL
    if not local.is_absolute():
        local = repo_root / local
    if not local.exists():
        raise FileNotFoundError(
            f"oracle bootstrap xp3 missing: {local}. "
            f"Build it via tests/differential/oracle_runner/fixtures/"
            f"build_logo_test_oracle.sh (requires the xp3pack tool).")
    # Use the internal app files dir for startup assets/preferences. On API31
    # the external scoped-storage preference file can be root-owned after adb
    # setup and unreadable to the app under SELinux.
    remote_dir = _REMOTE_STARTUP_FILES_DIR
    _adb_shell_root(serial, ["mkdir", "-p", remote_dir])
    remote_name = local.name
    remote_path = f"{remote_dir}/{remote_name}"
    tmp_path = f"/data/local/tmp/krkr2-{remote_name}"
    push_fixture(serial, local, tmp_path)
    _adb_shell_root(serial, ["cp", tmp_path, remote_path])
    _adb_shell_root(serial, ["chmod", "644", remote_path])
    _chown_to_app_files_owner(serial, remote_path)
    return remote_path


def _prepare_framebuffer_capture(
    serial: str | None,
    specs_by_id: dict[str, dict],
    framebuffer_dir: Path,
    capture_window: FrameCaptureWindow | None = None,
    startup_xp3: Path | None = None,
) -> tuple[str, str]:
    remote_capture_root = _REFERENCE_RENDER_STAGE_CAPTURE_ROOT
    _adb_shell_root(serial, ["rm", "-rf", remote_capture_root])
    _adb_shell_root(serial, ["mkdir", "-p", remote_capture_root])
    total_frames = sum(int(spec["frames"]) for spec in specs_by_id.values())
    if capture_window is None:
        class _Args:
            record_only_frame = None
            record_first_frames = None
        capture_window = frame_capture_window_from_args(_Args(), total_frames)
    segment_order = segment_order_for_specs(specs_by_id)
    for case in captured_case_ranges(specs_by_id, segment_order,
                                     capture_window):
        spec_id = str(case["caseId"])
        for phase in _RENDER_STAGE_CAPTURE_SURFACES:
            _adb_shell_root(
                serial,
                ["mkdir", "-p", remote_capture_root + "/" + spec_id + "/" + phase],
            )
    _chown_to_app_files_owner(serial, remote_capture_root)

    remote_xp3 = _ensure_logo_test_xp3_pushed(serial, startup_xp3)
    return remote_xp3, remote_capture_root


def _prepare_render_stage_capture(
    serial: str | None,
    specs_by_id: dict[str, dict],
    artifact_dir: Path,
    capture_window: FrameCaptureWindow | None = None,
    startup_xp3: Path | None = None,
) -> tuple[str, str]:
    remote_capture_root = _REFERENCE_RENDER_STAGE_CAPTURE_ROOT
    _adb_shell_root(serial, ["rm", "-rf", remote_capture_root])
    _adb_shell_root(serial, ["mkdir", "-p", remote_capture_root])
    total_frames = sum(int(spec["frames"]) for spec in specs_by_id.values())
    if capture_window is None:
        class _Args:
            record_only_frame = None
            record_first_frames = None
        capture_window = frame_capture_window_from_args(_Args(), total_frames)
    segment_order = segment_order_for_specs(specs_by_id)
    for case in captured_case_ranges(specs_by_id, segment_order,
                                     capture_window):
        spec_id = str(case["caseId"])
        for phase in _RENDER_STAGE_CAPTURE_SURFACES:
            _adb_shell_root(
                serial,
                ["mkdir", "-p", remote_capture_root + "/" + spec_id + "/" + phase],
            )
    _chown_to_app_files_owner(serial, remote_capture_root)

    remote_xp3 = _ensure_logo_test_xp3_pushed(serial, startup_xp3)
    return remote_xp3, remote_capture_root


def _wait_for_remote_framebuffer_files(
    serial: str | None,
    remote_capture_root: str,
    expected_frames: int,
    *,
    timeout: float,
    settle_seconds: float = 2.0,
) -> None:
    deadline = time.time() + timeout
    stable_since: float | None = None
    last_count = -1
    quoted_root = shlex.quote(remote_capture_root)
    while time.time() < deadline:
        out = _adb_shell(
            serial,
            f"find {quoted_root} -type f -name 'frame_*.png' "
            "2>/dev/null | wc -l",
        )
        try:
            count = int(out.strip().splitlines()[-1])
        except (IndexError, ValueError):
            count = 0

        if count != last_count:
            stable_since = None
            last_count = count
        elif count >= expected_frames and stable_since is None:
            stable_since = time.time()

        if stable_since is not None and \
                time.time() - stable_since >= settle_seconds:
            return
        time.sleep(0.5)

    raise RuntimeError(
        f"framebuffer capture did not finish within {timeout:.1f}s "
        f"(remote files: {last_count}, expected: {expected_frames})"
    )


def _wait_for_remote_render_stage_files(
    serial: str | None,
    remote_capture_root: str,
    *,
    min_files: int,
    timeout: float,
    settle_seconds: float = 2.0,
) -> int:
    deadline = time.time() + timeout
    stable_since: float | None = None
    last_count = -1
    quoted_root = shlex.quote(remote_capture_root)
    while time.time() < deadline:
        out = _adb_shell(
            serial,
            f"find {quoted_root} -type f -name 'frame_*.png' "
            "2>/dev/null | wc -l",
        )
        try:
            count = int(out.strip().splitlines()[-1])
        except (IndexError, ValueError):
            count = 0

        if count != last_count:
            stable_since = None
            last_count = count
        elif count >= min_files and stable_since is None:
            stable_since = time.time()

        if stable_since is not None and \
                time.time() - stable_since >= settle_seconds:
            return count
        time.sleep(0.5)

    raise RuntimeError(
        f"render-stage capture did not settle within {timeout:.1f}s "
        f"(remote files: {last_count}, minimum expected: {min_files}, "
        f"remote root: {remote_capture_root})"
    )


def _png_frame_number(path: Path) -> int | None:
    match = re.fullmatch(r"frame_(\d+)\.png", path.name)
    if not match:
        return None
    return int(match.group(1))


def _write_framebuffer_manifest(
    framebuffer_dir: Path,
    specs_by_id: dict[str, dict],
    remote_capture_root: str,
    capture_window: FrameCaptureWindow | None = None,
    startup_xp3: Path | None = None,
) -> Path:
    total_spec_frames = sum(int(spec["frames"]) for spec in specs_by_id.values())
    if capture_window is None:
        class _Args:
            record_only_frame = None
            record_first_frames = None
        capture_window = frame_capture_window_from_args(
            _Args(), total_spec_frames)
    cases: list[dict[str, Any]] = []
    total_frames = 0
    segment_order = segment_order_for_specs(specs_by_id)
    for case in captured_case_ranges(specs_by_id, segment_order,
                                     capture_window):
        spec_id = str(case["caseId"])
        spec = case["spec"]
        expected = int(spec["frames"])
        case_dir = framebuffer_dir / spec_id
        images: list[dict[str, Any]] = []
        expected_frames = list(case["capturedLocalFrames"])
        expected_set = set(expected_frames)
        for frame in expected_frames:
            rel = Path(spec_id) / f"frame_{frame:04d}.png"
            path = framebuffer_dir / rel
            if not path.exists():
                raise RuntimeError(f"missing framebuffer PNG: {path}")
            images.append(png_manifest_entry(
                frame=frame,
                path=path,
                rel=rel,
            ))
        extras = [
            p for p in sorted(case_dir.glob("frame_*.png"))
            if _png_frame_number(p) not in expected_set
        ]
        if extras:
            raise RuntimeError(
                f"unexpected extra framebuffer PNG(s) for {spec_id}: "
                f"{[p.name for p in extras[:5]]}"
            )
        total_frames += len(images)
        cases.append({
            "caseId": spec_id,
            "mtnPath": spec.get("mtn_path"),
            "chara": spec.get("chara"),
            "label": spec.get("label"),
            "frames": expected,
            "fullFrameIdRange": case["fullFrameIdRange"],
            "capturedFrameIdRange": case["capturedFrameIdRange"],
            "capturedLocalFrames": expected_frames,
            "images": images,
        })

    manifest = {
        "schema": _FRAMEBUFFER_SCHEMA,
        "source": _FRAMEBUFFER_SOURCE,
        "captureSurface": _FRAMEBUFFER_CAPTURE_SURFACE,
        "generatedAt": datetime.now(timezone.utc)
        .replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "remoteCaptureRoot": remote_capture_root,
        "remoteCapturePhase": "post_draw",
        "localRoot": str(framebuffer_dir),
        "fixture": {
            "xp3": (
                Path(startup_xp3).name
                if startup_xp3 is not None else DEFAULT_STARTUP_XP3_NAME
            ),
            "window": {"width": 1920, "height": 1080},
            "simulationFps": SIMULATION_FPS,
            "deltaMs": SIMULATION_DELTA_MS,
            "segmentOrder": list(segment_order),
        },
        "summary": {
            "caseCount": len(cases),
            "frameCount": total_frames,
        },
        **capture_window.manifest_fields(),
        "cases": cases,
    }
    target = framebuffer_dir / "manifest.json"
    target.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True,
                   allow_nan=False) + "\n",
        encoding="utf-8",
    )
    return target


def _collect_framebuffer_capture(
    serial: str | None,
    specs_by_id: dict[str, dict],
    framebuffer_dir: Path,
    remote_capture_root: str,
    *,
    timeout: float,
    capture_window: FrameCaptureWindow | None = None,
    startup_xp3: Path | None = None,
) -> Path:
    total_frames = sum(int(s["frames"]) for s in specs_by_id.values())
    if capture_window is None:
        class _Args:
            record_only_frame = None
            record_first_frames = None
        capture_window = frame_capture_window_from_args(_Args(), total_frames)
    expected_frames = capture_window.count
    _wait_for_remote_framebuffer_files(
        serial, remote_capture_root, expected_frames,
        timeout=max(30.0, timeout))

    framebuffer_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = framebuffer_dir / "manifest.json"
    if manifest_path.exists():
        manifest_path.unlink()
    for spec_id in specs_by_id:
        old_case_dir = framebuffer_dir / str(spec_id)
        if old_case_dir.exists():
            import shutil

            shutil.rmtree(old_case_dir)
    segment_order = segment_order_for_specs(specs_by_id)
    for case in captured_case_ranges(specs_by_id, segment_order,
                                     capture_window):
        spec_id = str(case["caseId"])
        local_case_dir = framebuffer_dir / spec_id
        local_case_dir.mkdir(parents=True, exist_ok=True)
        for frame in case["capturedLocalFrames"]:
            name = f"frame_{int(frame):04d}.png"
            _adb_pull(
                serial,
                f"{remote_capture_root}/{spec_id}/post_draw/{name}",
                local_case_dir / name,
            )

    manifest = _write_framebuffer_manifest(
        framebuffer_dir, specs_by_id, remote_capture_root, capture_window,
        startup_xp3)
    _adb_shell(serial, f"rm -rf {shlex.quote(remote_capture_root)}")
    return manifest


def _collect_render_stage_capture(
    serial: str | None,
    specs_by_id: dict[str, dict],
    artifact_dir: Path,
    remote_capture_root: str,
    *,
    timeout: float,
    capture_window: FrameCaptureWindow | None = None,
) -> dict[str, Any]:
    total_frames = sum(int(s["frames"]) for s in specs_by_id.values())
    if capture_window is None:
        class _Args:
            record_only_frame = None
            record_first_frames = None
        capture_window = frame_capture_window_from_args(_Args(), total_frames)
    segment_order = segment_order_for_specs(specs_by_id)
    captured_cases = captured_case_ranges(specs_by_id, segment_order,
                                          capture_window)
    _wait_for_remote_render_stage_files(
        serial, remote_capture_root, min_files=len(captured_cases),
        timeout=max(30.0, timeout))

    artifact_dir.mkdir(parents=True, exist_ok=True)
    images_root = artifact_dir / "images"
    images_root.mkdir(parents=True, exist_ok=True)
    cases: list[dict[str, Any]] = []
    total_images = 0

    for case in captured_cases:
        spec_id = str(case["caseId"])
        spec = case["spec"]

        local_case_dir = images_root / spec_id
        if local_case_dir.exists():
            import shutil

            shutil.rmtree(local_case_dir)
        _adb_pull(
            serial,
            f"{remote_capture_root}/{spec_id}",
            local_case_dir,
        )
        _normalize_pulled_render_case_dir(local_case_dir, spec_id)

        expected = int(spec["frames"])
        requested_local_frames = list(case["capturedLocalFrames"])
        requested_set = set(requested_local_frames)
        captured_local_frames: list[int] | None = None
        phases: dict[str, list[dict[str, Any]]] = {}
        for phase in _RENDER_STAGE_CAPTURE_SURFACES:
            phase_dir = local_case_dir / phase
            if not phase_dir.is_dir():
                raise RuntimeError(
                    f"missing render stage image directory: {phase_dir}")
            present_frames = [
                frame for frame in (
                    _png_frame_number(p)
                    for p in sorted(phase_dir.glob("frame_*.png"))
                )
                if frame is not None
            ]
            if phase == "initial":
                expected_phase_frames = [0]
            else:
                extras = [
                    frame for frame in present_frames
                    if frame not in requested_set
                ]
                if extras:
                    raise RuntimeError(
                        f"unexpected extra render stage PNG frame(s) for "
                        f"{spec_id}/{phase}: {extras[:5]}"
                    )
                expected_phase_frames = [
                    frame for frame in present_frames
                    if frame in requested_set
                ]
                if not expected_phase_frames:
                    raise RuntimeError(
                        f"no render stage PNGs captured for {spec_id}/{phase}")
                captured_local_frames = expected_phase_frames
            expected_set = set(expected_phase_frames)
            images: list[dict[str, Any]] = []
            for frame in expected_phase_frames:
                rel = Path("images") / spec_id / phase / \
                    f"frame_{frame:04d}.png"
                path = artifact_dir / rel
                if not path.exists():
                    raise RuntimeError(
                        f"missing render stage PNG: {path}")
                images.append(png_manifest_entry(
                    frame=frame,
                    phase=phase,
                    path=path,
                    rel=rel,
                ))
            extras = [
                p for p in sorted(phase_dir.glob("frame_*.png"))
                if _png_frame_number(p) not in expected_set
            ]
            if extras:
                raise RuntimeError(
                    f"unexpected extra render stage PNG(s) for "
                    f"{spec_id}/{phase}: {[p.name for p in extras[:5]]}"
                )
            phases[phase] = images
            total_images += len(images)
        if captured_local_frames is None:
            captured_local_frames = []

        cases.append({
            "caseId": spec_id,
            "mtnPath": spec.get("mtn_path"),
            "chara": spec.get("chara"),
            "label": spec.get("label"),
            "frames": expected,
            "fullFrameIdRange": case["fullFrameIdRange"],
            "capturedFrameIdRange": case["capturedFrameIdRange"],
            "capturedLocalFrames": captured_local_frames,
            "requestedLocalFrames": requested_local_frames,
            "phases": phases,
        })

    _adb_shell(serial, f"rm -rf {shlex.quote(remote_capture_root)}")
    return {
        "remoteCaptureRoot": remote_capture_root,
        "captureSurfaces": list(_RENDER_STAGE_CAPTURE_SURFACES),
        "cases": cases,
        "summary": {
            "caseCount": len(cases),
            "imageCount": total_images,
        },
        **capture_window.manifest_fields(),
    }


def _normalize_pulled_case_dir(local_case_dir: Path, case_id: str) -> None:
    if (local_case_dir / "frame_0000.png").exists():
        return
    nested = local_case_dir / case_id
    if not nested.is_dir() or not (nested / "frame_0000.png").exists():
        return

    import shutil

    for child in nested.iterdir():
        shutil.move(str(child), str(local_case_dir / child.name))
    nested.rmdir()


def _normalize_pulled_render_case_dir(
    local_case_dir: Path,
    case_id: str,
) -> None:
    if all((local_case_dir / phase).is_dir()
           for phase in _RENDER_STAGE_CAPTURE_SURFACES):
        return
    nested = local_case_dir / case_id
    if not nested.is_dir():
        return
    if not all((nested / phase).is_dir()
               for phase in _RENDER_STAGE_CAPTURE_SURFACES):
        return

    import shutil

    for child in nested.iterdir():
        target = local_case_dir / child.name
        if target.exists():
            shutil.rmtree(target)
        shutil.move(str(child), str(target))
    nested.rmdir()


# ------------------------------------------------------------ startup boot

_startup_triggered = False


def trigger_startup(engine, game_path_on_device: str) -> None:
    """Kick libkrkr2's deferred startup chain for `game_path_on_device`.

    Idempotent per AdbHarnessEngine session: cocos2d only accepts one
    `scheduleOnce` per "startup" key; a second invocation is a no-op on
    our side but would raise from startupFrom. We guard with
    `_startup_triggered`.

    After this returns, the cocos2d GL thread will (asynchronously)
    pick up the scheduled `doStartup`, run StartApplication →
    TVPInitScriptEngine → plugin loader → startup.tjs. Playback begins
    at some point 1–3s later. This function *returns immediately* once
    the scheduler accepts the path; use the Frida tracer's event count
    to determine when actual Motion.Player frames start arriving.
    """
    global _startup_triggered
    if _startup_triggered:
        return
    deadline = time.time() + 10.0
    while True:
        try:
            ok = engine.startup_from(game_path_on_device)
            break
        except RuntimeError as exc:
            if ("TVPMainScene::GetInstance returned null" not in str(exc)
                    or time.time() >= deadline):
                raise
            time.sleep(0.2)
    if not ok:
        raise RuntimeError(
            f"TVPMainScene::startupFrom({game_path_on_device!r}) returned "
            f"false — TVPCheckStartupPath rejected the path. Check the "
            f"path is under the app's scoped-storage dir and exists.")
    _startup_triggered = True


# ------------------------------------------------------------ oracle recording

def normalize_frame(frame: dict, index: int) -> dict:
    """Canonicalise node values while retaining observed sampling evidence."""
    error = sampling_contract_error(frame)
    if error:
        raise RuntimeError(f"motion sampling contract at frame {index}: {error}")
    layers = []
    for layer in frame.get("layers", []):
        out = {k: layer.get(k) for k in (
            LAYER_FIELDS_NUM + LAYER_FIELDS_INT
            + LAYER_FIELDS_BOOL + LAYER_FIELDS_STR
        )}
        if out.get("blendMode") is None and "stencilType" in layer:
            out["blendMode"] = layer.get("stencilType")
        for key in LAYER_FIELDS_STR:
            if out.get(key) is None:
                out[key] = ""
        layers.append(out)
    return {
        "frame": index,
        "layers": layers,
        **{key: frame.get(key) for key in (
            "samplePoint", "sampleOrder", "deltaMs", "playerLayerCounts",
        )},
    }


def sampling_contract_error(frame: dict) -> str | None:
    if frame.get("samplePoint") != TRACE_FLATTEN_SAMPLE_POINT:
        return "missing or incompatible samplePoint; re-record this trace"
    if frame.get("sampleOrder") != TRACE_FLATTEN_SAMPLE_ORDER:
        return "missing or incompatible sampleOrder; re-record this trace"
    delta = frame.get("deltaMs")
    if isinstance(delta, bool) or not isinstance(delta, (int, float)) or \
            not math.isfinite(delta):
        return "missing or non-finite observed progress deltaMs"
    counts = frame.get("playerLayerCounts")
    if not isinstance(counts, list) or not counts or any(
        type(count) is not int or count <= 0 for count in counts
    ):
        return "missing or invalid playerLayerCounts"
    if sum(counts) != len(frame.get("layers", [])):
        return "playerLayerCounts does not cover the flattened snapshot"
    return None


def _trace_flatten_frames(events: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [
        ev for ev in events
        if ev.get("stage") == "trace_flatten" and ev.get("kind") == "frame"
    ]


def _segment_trace_flatten_frames(
    frames: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    segments: list[dict[str, Any]] = []
    for frame in frames:
        diagnostics = frame.get("diagnostics") or {}
        key = (
            diagnostics.get("objthis")
            or diagnostics.get("topPlayer")
            or frame.get("objthis")
            or frame.get("topPlayer")
        )
        if not segments or segments[-1]["player"] != key:
            segments.append({"player": key, "frames": []})
        segments[-1]["frames"].append(frame)
    return segments


def _trace_flatten_capture_summary(events: list[dict[str, Any]]) -> str:
    frames = _trace_flatten_frames(events)
    segments = _segment_trace_flatten_frames(frames)
    lines = [
        f"events={len(events)}",
        f"trace_flatten_frames={len(frames)}",
        f"segments={[len(s['frames']) for s in segments]}",
    ]
    if frames:
        last = frames[-1]
        diagnostics = last.get("diagnostics") or {}
        players = diagnostics.get("players") or []
        lines.append(
            "last_frame="
            f"frameId={last.get('frameId')} "
            f"topPlayer={diagnostics.get('topPlayer') or last.get('topPlayer')} "
            f"playerCount={last.get('playerCount')} "
            f"layerCount={len(last.get('layers') or [])} "
            f"diagnosticsError={diagnostics.get('error')!r} "
            f"playerLayouts={[p.get('layout') for p in players]}"
        )

    errors: list[str] = []
    for local_index, frame in enumerate(frames):
        diagnostics = frame.get("diagnostics") or {}
        diag_error = diagnostics.get("error")
        if diag_error:
            errors.append(
                f"frame[{local_index}] diagnostics.error={diag_error!r}"
            )
        for player in diagnostics.get("players") or []:
            player_error = player.get("error")
            if player_error:
                errors.append(
                    f"frame[{local_index}] player={player.get('ptr')} "
                    f"layout={player.get('layout')} error={player_error!r}"
                )
        if len(errors) >= 5:
            break
    if errors:
        lines.append("first_errors=" + " | ".join(errors[:5]))
    return "\n".join(lines)


def _stop_record_for_failure_summary(tracer) -> str:
    try:
        events = tracer.stop_record()
    except Exception as exc:
        return f"failed to collect partial trace: {exc!r}"
    return _trace_flatten_capture_summary(events)


def _strict_error(
    *,
    spec_id: str,
    frame: dict[str, Any] | None,
    local_frame: int,
    error: str,
) -> RuntimeError:
    frame = frame or {}
    diagnostics = frame.get("diagnostics") or {}
    players = diagnostics.get("players") or []
    first_player = players[0] if players else {}
    layers = frame.get("layers") or []
    return RuntimeError(
        "strict motion_playback oracle validation failed: "
        f"case={spec_id} "
        f"localFrame={local_frame} "
        f"frameId={frame.get('frameId')} "
        f"player={first_player.get('ptr') or diagnostics.get('topPlayer')} "
        f"layout={first_player.get('layout') or diagnostics.get('layout')} "
        f"layerCount={len(layers)} "
        f"error={error}"
    )


def _validate_trace_flatten_layer(
    *,
    spec_id: str,
    frame: dict[str, Any],
    local_frame: int,
    layer_index: int,
    layer: dict[str, Any],
) -> None:
    for key in LAYER_FIELDS_NUM:
        value = layer.get(key)
        if not isinstance(value, (int, float)) or \
                not math.isfinite(float(value)) or \
                abs(float(value)) > TRACE_FLATTEN_ABS_FLOAT_LIMIT:
            raise _strict_error(
                spec_id=spec_id, frame=frame, local_frame=local_frame,
                error=(
                    f"layer[{layer_index}].{key} invalid: "
                    f"{value!r}"))

    for key in ("index", "nodeType", "opacity", "stencilType"):
        value = layer.get(key)
        if not isinstance(value, int):
            raise _strict_error(
                spec_id=spec_id, frame=frame, local_frame=local_frame,
                error=(
                    f"layer[{layer_index}].{key} is not int: "
                    f"{value!r}"))

    for key in LAYER_FIELDS_BOOL:
        value = layer.get(key)
        if not isinstance(value, bool):
            raise _strict_error(
                spec_id=spec_id, frame=frame, local_frame=local_frame,
                error=(
                    f"layer[{layer_index}].{key} is not bool: "
                    f"{value!r}"))

    if layer.get("index") != layer_index:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=(
                f"layer[{layer_index}].index mismatch: "
                f"{layer.get('index')!r}"))
    opacity = int(layer["opacity"])
    if opacity < 0 or opacity > 255:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=f"layer[{layer_index}].opacity out of range: {opacity}")
    node_type = int(layer["nodeType"])
    if node_type < 0 or node_type > 32:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=f"layer[{layer_index}].nodeType out of range: {node_type}")
    stencil_type = int(layer["stencilType"])
    if abs(stencil_type) > 1024:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=(
                f"layer[{layer_index}].stencilType out of range: "
                f"{stencil_type}"))


def _validate_trace_flatten_frame(
    spec: dict,
    frame: dict[str, Any],
    local_frame: int,
) -> None:
    spec_id = str(spec["id"])
    sampling_error = sampling_contract_error(frame)
    if sampling_error:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=sampling_error)
    if frame.get("projection") != TRACE_FLATTEN_PROJECTION:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=f"unexpected projection: {frame.get('projection')!r}")
    if frame.get("samplePoint") != TRACE_FLATTEN_SAMPLE_POINT:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=f"unexpected samplePoint: {frame.get('samplePoint')!r}")

    diagnostics = frame.get("diagnostics")
    if not isinstance(diagnostics, dict):
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error="missing diagnostics")
    if diagnostics.get("error"):
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=f"diagnostics.error: {diagnostics.get('error')}")

    player_count = frame.get("playerCount")
    if not isinstance(player_count, int) or player_count <= 0:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=f"invalid playerCount: {player_count!r}")
    if len(frame["playerLayerCounts"]) != player_count:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error="playerLayerCounts length differs from playerCount")
    players = diagnostics.get("players")
    if not isinstance(players, list) or len(players) != player_count:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error=(
                f"diagnostics.players count mismatch: "
                f"{len(players) if isinstance(players, list) else None}"))
    for player in players:
        if player.get("layout") != "deque":
            raise _strict_error(
                spec_id=spec_id, frame=frame, local_frame=local_frame,
                error=f"player layout is not deque: {player.get('layout')!r}")
        if player.get("error"):
            raise _strict_error(
                spec_id=spec_id, frame=frame, local_frame=local_frame,
                error=f"player error: {player.get('error')}")

    layers = frame.get("layers")
    if not isinstance(layers, list):
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error="layers is not a list")
    if not layers:
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=local_frame,
            error="layers is empty")
    for layer_index, layer in enumerate(layers):
        if not isinstance(layer, dict):
            raise _strict_error(
                spec_id=spec_id, frame=frame, local_frame=local_frame,
                error=f"layer[{layer_index}] is not an object")
        _validate_trace_flatten_layer(
            spec_id=spec_id,
            frame=frame,
            local_frame=local_frame,
            layer_index=layer_index,
            layer=layer,
        )


def _validate_trace_flatten_segment(spec: dict, frames: list[dict]) -> None:
    wanted = int(spec["frames"])
    spec_id = str(spec["id"])
    if len(frames) != wanted:
        frame = frames[-1] if frames else None
        raise _strict_error(
            spec_id=spec_id, frame=frame, local_frame=len(frames) - 1,
            error=f"segment frame count mismatch: {len(frames)} != {wanted}")
    for local_frame, frame in enumerate(frames):
        _validate_trace_flatten_frame(spec, frame, local_frame)


def record_all_oracles(
    engine,
    specs: list[dict],
    *,
    serial: str | None = None,
    playback_timeout: float = 60.0,
    framebuffer_dir: Path | None = None,
    startup_xp3: Path | None = None,
) -> dict[str, list[dict]]:
    """Capture per-frame layer state for the requested specs."""
    global _startup_triggered
    _startup_triggered = False

    # Local import keeps disk-only compare paths free of the frida dependency.
    from oracle_runner.frida_motion_stage_tracer import FridaMotionStageTracer

    specs_by_id = {s["id"]: s for s in specs}
    segment_order = segment_order_for_specs(specs_by_id)

    framebuffer_remote_root: str | None = None
    if framebuffer_dir is not None:
        remote_game, framebuffer_remote_root = _prepare_framebuffer_capture(
            serial, specs_by_id, framebuffer_dir, startup_xp3=startup_xp3)
    else:
        remote_game = _ensure_logo_test_xp3_pushed(serial, startup_xp3)

    ensure_oracle_renderer_software(
        serial, remote_game=remote_game, write_global=False)
    with FridaMotionStageTracer(engine, device_id=serial) as tracer:
        tracer.start_record(
            ["trace_flatten"], simulation_fps=SIMULATION_FPS)
        trigger_startup(engine, remote_game)

        events = _wait_for_trace_flatten_segments(
            tracer, specs_by_id, timeout=playback_timeout,
            stabilise_seconds=5.0 if framebuffer_dir is not None else 2.0)
    segments = _segment_trace_flatten_frames(_trace_flatten_frames(events))
    ordered_specs = [specs_by_id[spec_id] for spec_id in segment_order]
    try:
        fixture_segments = select_expected_segments(segments, ordered_specs)
    except ValueError as exc:
        raise RuntimeError(
            f"failed to identify fixture trace_flatten segments: {exc}. "
            f"startup.tjs should produce {segment_order}; check logcat for "
            "Motion.Player creation or GL-surface failures."
        ) from exc

    results: dict[str, list[dict]] = {}
    for i, spec_id in enumerate(segment_order):
        spec = specs_by_id[spec_id]
        wanted = int(spec["frames"])
        frames = fixture_segments[i]["frames"]
        if len(frames) != wanted:
            raise RuntimeError(
                f"segment {i} ({spec_id}) has {len(frames)} frames; "
                f"spec requires exactly {wanted}. Check Motion.Player's "
                f"per-motion frame count and startup.tjs determinism.")
        _validate_trace_flatten_segment(spec, frames)
        results[spec_id] = [
            normalize_frame(fr, fi) for fi, fr in enumerate(frames)
        ]
    if framebuffer_dir is not None:
        assert framebuffer_remote_root is not None
        _collect_framebuffer_capture(
            serial, specs_by_id, framebuffer_dir, framebuffer_remote_root,
            timeout=playback_timeout, startup_xp3=startup_xp3)
    return results


def _wait_for_trace_flatten_segments(
    tracer,
    specs_by_id: dict[str, dict],
    *,
    timeout: float,
    poll_interval: float = 0.4,
    stabilise_seconds: float = 2.0,
) -> list[dict]:
    """Poll until the fixture segments match specs and capture stabilises.

    We can't peek at the buffer incrementally (rpc.exports round-trips
    freeze the whole array), so we only call stop_record() once the frame
    count has stabilised. Intermediate polls use `event_count` which is cheap
    (one integer over RPC).
    """
    needed_frames = sum(int(s["frames"]) for s in specs_by_id.values())
    ordered_specs = [
        specs_by_id[spec_id]
        for spec_id in segment_order_for_specs(specs_by_id)
    ]

    deadline = time.time() + timeout
    # A target-FPS preference is not a delivery guarantee. If playback has
    # naturally stopped below the contract (for example because a software
    # renderer exposed large wall-clock deltas), fail with the captured segment
    # lengths instead of waiting the full five-minute timeout. Ten seconds is
    # deliberately much longer than the expected 15 Hz cadence and the normal
    # per-frame CI render cost.
    shortfall_stabilise_seconds = max(10.0, stabilise_seconds)
    stable_since: float | None = None
    last_count = -1
    while time.time() < deadline:
        count = tracer.event_count()
        if count != last_count:
            stable_since = None
            last_count = count
        elif count > 0 and stable_since is None:
            stable_since = time.time()
        required_stability = (
            stabilise_seconds
            if count >= needed_frames else shortfall_stabilise_seconds
        )
        if stable_since is not None and \
                time.time() - stable_since >= required_stability:
            events = tracer.stop_record()
            segments = _segment_trace_flatten_frames(
                _trace_flatten_frames(events))
            try:
                select_expected_segments(segments, ordered_specs)
            except ValueError as exc:
                summary = _trace_flatten_capture_summary(events)
                raise RuntimeError(
                    f"trace_flatten frame count stabilised at "
                    f"{len(_trace_flatten_frames(events))}, but fixture "
                    f"segments did not match the spec contract: {exc}\n"
                    f"partial trace summary:\n{summary}"
                ) from exc
            else:
                return events
        time.sleep(poll_interval)
    summary = _stop_record_for_failure_summary(tracer)
    raise RuntimeError(
        f"motion playback did not stabilise within {timeout}s "
        f"(last event count: {last_count}, needed >= {needed_frames})\n"
        f"partial trace summary:\n{summary}")


# ------------------------------------------------------------ diff helpers

def _floats_close(a: float, b: float, *, rel: float, abs_: float) -> bool:
    if a == b:
        return True
    diff = abs(a - b)
    return diff <= max(abs_, rel * max(abs(a), abs(b)))


# Structural subset for focused tree/state debugging. This skips the
# accumulated transform fields while keeping the same string-diagnostic
# policy as the full diff.
STRUCTURAL_FIELDS_INT = ("index", "nodeType", "opacity", "blendMode")
STRUCTURAL_FIELDS_BOOL = ("visible", "active", "flipX", "flipY")
STRUCTURAL_FIELDS_STR: tuple[str, ...] = ()


def diff_frames(port_frames: list, oracle_frames: list, *,
                rel: float = 1e-6, abs_: float = 1e-6,
                structural_only: bool = False) -> list:
    if structural_only:
        int_fields = STRUCTURAL_FIELDS_INT
        bool_fields = STRUCTURAL_FIELDS_BOOL
        str_fields = STRUCTURAL_FIELDS_STR
        num_fields: tuple[str, ...] = ()
    else:
        int_fields = LAYER_FIELDS_INT
        bool_fields = LAYER_FIELDS_BOOL
        str_fields = COMPARE_FIELDS_STR
        num_fields = LAYER_FIELDS_NUM

    mismatches: list[dict[str, Any]] = []
    n = min(len(port_frames), len(oracle_frames))
    if len(port_frames) != len(oracle_frames):
        mismatches.append({
            "kind": "frame_count",
            "port": len(port_frames),
            "oracle": len(oracle_frames),
        })
    for f in range(n):
        pf = port_frames[f]
        of = oracle_frames[f]
        protocol_errors = {
            side: error for side, frame in (("port", pf), ("oracle", of))
            if (error := sampling_contract_error(frame)) is not None
        }
        for side, frame in (("port", pf), ("oracle", of)):
            if type(frame.get("frame")) is not int:
                protocol_errors[side] = "missing normalized frame identity"
        if protocol_errors:
            mismatches.append({
                "kind": "sampling_contract", "frame": f, **protocol_errors,
            })
            continue
        sampling_fields = ("frame", "samplePoint", "sampleOrder",
                           "deltaMs", "playerLayerCounts")
        boundary_mismatches = [
            {"kind": "sampling_contract", "frame": f, "field": key,
             "port": pf.get(key), "oracle": of.get(key)}
            for key in sampling_fields if pf.get(key) != of.get(key)
        ]
        if boundary_mismatches:
            mismatches.extend(boundary_mismatches)
            continue
        pl = pf.get("layers", [])
        ol = of.get("layers", [])
        if len(pl) != len(ol):
            mismatches.append({
                "kind": "layer_count",
                "frame": f,
                "port": len(pl),
                "oracle": len(ol),
            })
        for i in range(min(len(pl), len(ol))):
            pli = pl[i]
            oli = ol[i]
            for k in int_fields + bool_fields + str_fields:
                if pli.get(k) != oli.get(k):
                    mismatches.append({
                        "kind": "field",
                        "frame": f,
                        "layer_index": i,
                        "field": k,
                        "port": pli.get(k),
                        "oracle": oli.get(k),
                    })
            for k in num_fields:
                pv = pli.get(k)
                ov = oli.get(k)
                if pv is None or ov is None:
                    if pv != ov:
                        mismatches.append({
                            "kind": "field",
                            "frame": f,
                            "layer_index": i,
                            "field": k,
                            "port": pv,
                            "oracle": ov,
                        })
                    continue
                if not _floats_close(float(pv), float(ov),
                                     rel=rel, abs_=abs_):
                    mismatches.append({
                        "kind": "float",
                        "frame": f,
                        "layer_index": i,
                        "field": k,
                        "port": pv,
                        "oracle": ov,
                    })
    return mismatches


def run_case(engine, spec: dict, *, port_frames: list,
             oracle_frames: list | None = None,
             tracer=None,
             structural_only: bool = False) -> dict:
    """Compare port_frames against oracle_frames (live or cached)."""
    out: dict[str, Any] = {
        "case_id": spec["id"],
        "status": "ok",
        "mismatches": [],
    }
    if oracle_frames is None:
        out["status"] = "error"
        out["error"] = "no oracle frames provided"
        return out
    expected_frames = int(spec["frames"])
    contract_mismatches = []
    if len(port_frames) != expected_frames:
        contract_mismatches.append({
            "kind": "port_spec_frame_count",
            "port": len(port_frames),
            "spec": expected_frames,
        })
    if len(oracle_frames) != expected_frames:
        contract_mismatches.append({
            "kind": "oracle_spec_frame_count",
            "oracle": len(oracle_frames),
            "spec": expected_frames,
        })
    mismatches = diff_frames(port_frames, oracle_frames,
                             structural_only=structural_only)
    out["mismatches"] = contract_mismatches + mismatches
    if out["mismatches"]:
        out["status"] = "mismatch"
    return out
