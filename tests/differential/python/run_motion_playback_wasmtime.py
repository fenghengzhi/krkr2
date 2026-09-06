#!/usr/bin/env python3
"""Wasmtime port-side verifier for the motion_playback differential family."""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import posixpath
import shutil
import sys
import struct
import tempfile
import time
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "tests" / "differential"))
from oracle_runner.png_artifacts import (
    bgra_rgba_sha256_file,
    png_manifest_entry,
    write_bgra_png,
    write_rgba_png,
)
from oracle_runner.motion_capture_window import (
    FrameCaptureWindow,
    add_frame_capture_args,
    captured_case_ranges,
    frame_capture_window_from_bounds,
    frame_capture_window_from_args,
)
from oracle_runner.motion_timing import (
    DEFAULT_STARTUP_XP3_NAME,
    SIMULATION_DELTA_MS,
    SIMULATION_FPS,
    STARTUP_WARMUP_TICKS,
    select_expected_segments,
    validate_simulation_contract,
)

FRAMEBUFFER_SCHEMA = "motion-framebuffer-wasmtime-v1"
FRAMEBUFFER_SOURCE = "wasmtime-port-saveLayerImage"
FRAMEBUFFER_CAPTURE_SURFACE = (
    "Layer main image immediately after Motion.Player.draw(base)"
)
RENDER_SCHEMA = "motion-render-stage-wasmtime-v1"
RENDER_EVENT_SCHEMA = "motion-render-stage-wasmtime-v1-event"
RENDER_SOURCE = "wasmtime-port-render-stage"
RENDER_CAPTURE_GUEST_ROOT = (
    "/reference/xp3/savedata/motion_render_stage_capture"
)
RENDER_CHECKPOINT_GUEST_ROOT = "/render_stage_capture"
RENDER_CAPTURE_SURFACES: tuple[str, ...] = ("initial", "post_draw")
RENDER_STEP_CHECKPOINT_SURFACES: tuple[str, ...] = (
    "execute_pre",
    "execute_post",
    "updateLayerAfterDraw_pre",
    "updateLayerAfterDraw_post",
    "post_draw",
)
POST_DRAW_CANVAS_SIZE = (1920, 1080)
RENDER_STAGES: tuple[str, ...] = (
    "draw_dispatch",
    "render_prepare",
    "render_commands",
    "render_execute",
)


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="motion_playback Wasmtime differential runner")
    p.add_argument("--spec-dir",
                   default=str(REPO_ROOT / "tests" / "differential" /
                               "specs" / "motion_playback"),
                   help="Directory of spec JSON files")
    p.add_argument("--trace-dir",
                   default=str(REPO_ROOT / "tests" / "differential" /
                               "traces" / "motion_playback"),
                   help="Directory of cached oracle JSONs")
    p.add_argument("--wasm",
                   default=str(REPO_ROOT / "out" / "wasmtime" / "debug" /
                               "krkr2_wasmtime_guest.wasm"),
                   help="Path to the Wasmtime guest wasm")
    p.add_argument("--startup-xp3",
                   default=str(REPO_ROOT / "reference" / "xp3" /
                               DEFAULT_STARTUP_XP3_NAME),
                   help=f"Host path to {DEFAULT_STARTUP_XP3_NAME}")
    p.add_argument("--case", action="append", default=[],
                   help="Motion case id to run; repeat for multiple cases. "
                        "Defaults to all specs in --spec-dir")
    p.add_argument("--strict-missing-trace", action="store_true",
                   help="Fail when a disk golden is missing instead of "
                        "auto-skipping the case")
    p.add_argument("--only-structural", action="store_true",
                   help="Diff only structural Motion state fields")
    p.add_argument("--skip-golden-diff", action="store_true",
                   help="Only capture and optionally write Wasmtime port "
                        "traces; do not compare against cached oracle JSONs")
    p.add_argument("--write-port-traces", type=Path, default=None,
                   help="Write normalized Wasmtime port frames per spec as "
                        "<id>.port.json into this directory")
    p.add_argument("--record-framebuffer", action="store_true",
                   help="Save every Wasmtime motion_playback frame as PNG "
                        "artifacts and write a manifest.json")
    p.add_argument("--framebuffer-dir", type=Path, default=None,
                   help="Framebuffer artifact output directory. Default: "
                        "tests/differential/artifacts/"
                        "motion_playback_framebuffer_wasmtime/<run-id>")
    p.add_argument("--record-render-stages", action="store_true",
                   help="Save platform-independent Wasmtime render_path "
                        "semantic stage JSON. PNGs are collected only when "
                        "an explicit checkpoint/probe option is enabled")
    p.add_argument("--record-render-step-checkpoints", action="store_true",
                   help="With --record-render-stages, save execute_pre/"
                        "execute_post images around executeLayerRenderCommands "
                        "and updateLayerAfterDraw_pre/post images around "
                        "updateLayerAfterDraw, plus true post_draw after "
                        "startup.tjs onPaint")
    p.add_argument("--checkpoint-render-only", action="store_true",
                   help="With --record-render-step-checkpoints, build render "
                        "PNG artifacts only from guest Layer MainImage "
                        "checkpoints instead of fixture saveLayerImage PNGs")
    p.add_argument("--record-layer-raw-probes", action="store_true",
                   help="With --record-render-stages, capture raw Layer "
                        "MainImage probes at fillRect/saveLayerImage/"
                        "drawCompat/render execute/update boundaries")
    p.add_argument("--record-save-layer-visual-readback-probes",
                   action="store_true",
                   help="With --record-render-stages, capture saveLayerImage "
                        "visual readback row hashes")
    p.add_argument("--save-layer-visual-readback-frame-start", type=int,
                   default=0,
                   help="First global frame id for saveLayerImage visual "
                        "readback row probes")
    p.add_argument("--save-layer-visual-readback-frame-count", type=int,
                   default=1,
                   help="Number of global frames to capture visual readback "
                        "rows for; use -1 for all frames")
    p.add_argument("--render-artifact-dir", type=Path, default=None,
                   help="Render stage artifact output directory. Default: "
                        "tests/differential/artifacts/"
                        "motion_playback_render_stages_wasmtime/<run-id>")
    add_frame_capture_args(p)
    return p.parse_args(argv)


def default_framebuffer_dir() -> Path:
    run_id = time.strftime("%Y%m%d-%H%M%S")
    return (
        REPO_ROOT / "tests" / "differential" / "artifacts"
        / "motion_playback_framebuffer_wasmtime" / run_id
    )


def default_render_artifact_dir() -> Path:
    run_id = time.strftime("%Y%m%d-%H%M%S")
    return (
        REPO_ROOT / "tests" / "differential" / "artifacts"
        / "motion_playback_render_stages_wasmtime" / run_id
    )


def load_specs(spec_dir: Path) -> list[dict]:
    return [
        json.loads(path.read_text(encoding="utf-8"))
        for path in sorted(spec_dir.glob("*.json"))
    ]


def filter_specs(specs: list[dict], case_ids: list[str]) -> list[dict]:
    if not case_ids:
        return specs
    wanted = {str(case_id) for case_id in case_ids}
    selected = [spec for spec in specs if str(spec.get("id")) in wanted]
    found = {str(spec.get("id")) for spec in selected}
    missing = sorted(wanted - found)
    if missing:
        raise ValueError(f"unknown motion_playback case id(s): {missing}")
    return selected


def load_wasmtime():
    try:
        import wasmtime  # type: ignore
    except ImportError as exc:
        raise RuntimeError(
            "wasmtime is not installed; run "
            "'python3 -m pip install -r "
            "tests/differential/python/requirements-wasm.txt'"
        ) from exc
    return wasmtime


class WasmtimeEnvProvider:
    """Headless env::* provider for Emscripten imports."""

    def __init__(self, root: Path, gl_provider=None) -> None:
        self.root = root
        self.gl_provider = gl_provider
        self.canvas_width = 1920
        self.canvas_height = 1080
        self.css_width = 1920.0
        self.css_height = 1080.0
        self.main_loop: tuple[int, int, int, int] | None = None
        self.startup_xp3_path = ""
        self._next_al_id = 1
        self._next_fd = 100
        self._fds: dict[int, dict[str, Any]] = {}
        self.capture_swap_framebuffer = False
        self._swap_framebuffers: list[dict[str, Any]] = []
        # The full guest is stepped synchronously, so wall-clock time would
        # barely advance between mainLoop() calls. Keep the host's monotonic,
        # emscripten_get_now and RAF timestamp imports on one virtual clock;
        # startup.tjs can then use the production tick - lastTick data flow.
        monotonic_ns = time.monotonic_ns()
        self._simulation_monotonic_ns = (
            monotonic_ns - monotonic_ns % 1_000_000
        )
        self._simulation_subnanoseconds = 0.0

    @property
    def simulation_time_ms(self) -> float:
        return (
            self._simulation_monotonic_ns +
            self._simulation_subnanoseconds
        ) / 1_000_000.0

    def advance_simulation_time(self, delta_ms: float) -> None:
        if not math.isfinite(delta_ms) or delta_ms < 0.0:
            raise ValueError(f"invalid Wasmtime simulation delta: {delta_ms}")
        total_subnanoseconds = (
            self._simulation_subnanoseconds + delta_ms * 1_000_000.0
        )
        whole_nanoseconds = math.floor(total_subnanoseconds)
        self._simulation_monotonic_ns += int(whole_nanoseconds)
        self._simulation_subnanoseconds = (
            total_subnanoseconds - whole_nanoseconds
        )

    def begin_swap_framebuffer_capture(self) -> None:
        self._swap_framebuffers = []

    def last_swap_framebuffer(self) -> dict[str, Any] | None:
        if not self._swap_framebuffers:
            return None
        return self._swap_framebuffers[-1]

    def define_imports(self, linker: Any, module: Any) -> None:
        unknown: list[str] = []
        for imp in module.imports:
            if imp.module == "wasi_snapshot_preview1":
                callback = self._wasi_callback_for(imp.name, imp.type)
                if callback is None:
                    unknown.append(f"{imp.module}.{imp.name}")
                    continue
                linker.define_func(imp.module, imp.name, imp.type, callback,
                                   access_caller=True)
                continue
            if imp.module != "env":
                continue
            if imp.name == "memory":
                continue
            if imp.name.startswith("gl") or imp.name.startswith("emscripten_gl"):
                continue
            callback = self._callback_for(imp.name, imp.type)
            if callback is None:
                unknown.append(f"{imp.module}.{imp.name}")
                continue
            linker.define_func("env", imp.name, imp.type, callback,
                               access_caller=True)
        if unknown:
            raise RuntimeError(
                "unsupported import(s): " + ", ".join(sorted(unknown))
            )

    @staticmethod
    def _memory(caller: Any) -> Any:
        memory = caller.get("memory")
        if memory is None:
            raise RuntimeError("guest memory export is unavailable")
        return memory

    @staticmethod
    def _memory_base(caller: Any, memory: Any) -> int:
        try:
            ptr = memory.data_ptr(caller)
        except TypeError:
            ptr = memory.data_ptr()
        return ctypes.addressof(ptr.contents)

    @staticmethod
    def _memory_len(caller: Any, memory: Any) -> int:
        try:
            return int(memory.data_len(caller))
        except TypeError:
            return int(memory.data_len())

    def _read(self, caller: Any, ptr: int, size: int) -> bytes:
        if ptr == 0 or size <= 0:
            return b""
        memory = self._memory(caller)
        data_len = self._memory_len(caller, memory)
        if ptr < 0 or ptr + size > data_len:
            raise RuntimeError(
                f"guest memory read out of bounds: ptr={ptr} size={size}"
            )
        return ctypes.string_at(self._memory_base(caller, memory) + ptr, size)

    def _write(self, caller: Any, ptr: int, data: bytes) -> None:
        if not ptr:
            return
        memory = self._memory(caller)
        data_len = self._memory_len(caller, memory)
        if ptr < 0 or ptr + len(data) > data_len:
            raise RuntimeError(
                f"guest memory write out of bounds: ptr={ptr} size={len(data)}"
            )
        ctypes.memmove(self._memory_base(caller, memory) + ptr, data,
                       len(data))

    def _write_i32(self, caller: Any, ptr: int, value: int) -> None:
        self._write(caller, ptr, int(value).to_bytes(4, "little", signed=True))

    def _write_u32_array(self, caller: Any, ptr: int,
                         values: list[int]) -> None:
        for i, value in enumerate(values):
            self._write(caller, ptr + i * 4,
                        int(value & 0xffffffff).to_bytes(
                            4, "little", signed=False))

    def _write_f64(self, caller: Any, ptr: int, value: float) -> None:
        self._write(caller, ptr, struct.pack("<d", float(value)))

    def _write_i64(self, caller: Any, ptr: int, value: int) -> None:
        self._write(caller, ptr, int(value).to_bytes(8, "little",
                                                     signed=True))

    def _read_c_string(self, caller: Any, ptr: int) -> str:
        if ptr == 0:
            return ""
        memory = self._memory(caller)
        data_len = self._memory_len(caller, memory)
        if ptr < 0 or ptr >= data_len:
            raise RuntimeError(f"guest string pointer out of bounds: {ptr}")
        # Do not copy the entire remaining linear memory merely to find a NUL.
        # The debug guest currently grows to hundreds of MiB; doing so from a
        # hot EM_ASM callback turned every short string probe into a similarly
        # sized host allocation/copy.  Engine/ABI strings consumed here are
        # paths, symbol names, or inline diagnostic snippets, so 64 KiB is a
        # conservative corruption guard as well as the intended read bound.
        scan_len = min(data_len - ptr, 64 * 1024)
        data = ctypes.string_at(self._memory_base(caller, memory) + ptr,
                                scan_len)
        end = data.find(0)
        if end < 0:
            return bytes(data[:256]).decode("utf-8", errors="replace")
        return bytes(data[:end]).decode("utf-8", errors="replace")

    @staticmethod
    def _default_return(func_type: Any) -> Any:
        results = list(getattr(func_type, "results", []))
        if not results:
            return None
        text = str(results[0])
        if "f32" in text or "f64" in text:
            return 0.0
        return 0

    def _callback_for(self, name: str, func_type: Any) -> Any | None:
        if name.startswith("__syscall_"):
            return lambda _caller, *args: self._syscall(name, _caller, *args)
        if name.startswith("fsafs_"):
            return lambda _caller, *args: self._fsafs(name, _caller, *args)
        if (name.startswith("vlfs_js_")
                or name.startswith("__asyncjs__vlfs_js_")):
            return lambda _caller, *args: self._vlfs(name, _caller, *args)
        if name.startswith("egl"):
            return lambda caller, *args: self._egl(name, caller, *args)
        if name.startswith("al") or name.startswith("alc"):
            return lambda caller, *args: self._openal(name, caller, *args)
        if name.startswith("emscripten_set_"):
            return lambda caller, *args: self._emscripten_set(
                name, func_type, caller, *args)
        if name.startswith("emscripten_get_"):
            return lambda caller, *args: self._emscripten_get(
                name, func_type, caller, *args)
        exact = {
            "TVPCompleteWaveSoundContinuation":
                self._unsupported_web_wave_sound_continuation,
            "__asyncjs__TVPWaitWaveSoundContinuation":
                self._unsupported_web_wave_sound_continuation,
            "TVPWebAudioDecodeStartJS": self._web_audio_decode_start,
            "krkr2_wasmtime_logo_trace_enabled": self._logo_trace_enabled,
            "emscripten_asm_const_int": self._return_zero,
            "emscripten_asm_const_int_sync_on_main_thread":
                self._return_zero,
            "emscripten_asm_const_double": self._asm_const_double,
            "emscripten_asm_const_ptr_sync_on_main_thread": self._return_zero,
            "emscripten_notify_memory_growth": self._return_none,
            "emscripten_sample_gamepad_data": self._return_zero,
            "emscripten_has_asyncify": self._return_zero,
            "emscripten_sleep": self._return_none,
            "emscripten_exit_with_live_runtime": self._return_none,
            "emscripten_check_blocking_allowed": self._return_one,
            "emscripten_num_logical_cores": self._return_one,
            "js_decode_text": self._return_zero,
            "krkr2_get_startup_xp3_path": self._startup_xp3_path,
            "execve": self._return_minus_one,
            "flock": self._return_zero,
            "getgrnam": self._return_zero,
            "getpwnam": self._return_zero,
            "getpwnam_r": self._return_minus_one,
            "getpwuid_r": self._return_minus_one,
            "vfork": self._return_minus_one,
            "web_alert": self._return_zero,
            "web_confirm": self._return_zero,
            "_cc_canvas_render_text": self._return_zero,
            "emscripten_exit_fullscreen": self._return_zero,
            "emscripten_exit_pointerlock": self._return_zero,
            "emscripten_request_fullscreen_strategy": self._return_zero,
            "emscripten_request_pointerlock": self._return_zero,
            "_emscripten_thread_set_strongref": self._return_none,
            "_emscripten_init_main_thread_js": self._return_none,
            "_emscripten_thread_mailbox_await": self._return_none,
            "_emscripten_receive_on_main_thread_js": self._return_zero,
            "_emscripten_thread_cleanup": self._return_none,
            "_emscripten_notify_mailbox_postmessage": self._return_none,
            "__pthread_create_js": self._pthread_create,
        }
        callback = exact.get(name)
        if callback is None:
            return None
        return lambda caller, *args: callback(func_type, caller, *args)

    def _wasi_callback_for(self, name: str, func_type: Any) -> Any | None:
        exact = {
            "clock_time_get": self._wasi_clock_time_get,
            "environ_get": self._wasi_environ_get,
            "environ_sizes_get": self._wasi_environ_sizes_get,
            "fd_close": self._wasi_fd_close,
            "fd_fdstat_get": self._wasi_fd_fdstat_get,
            "fd_read": self._wasi_fd_read,
            "fd_seek": self._wasi_fd_seek,
            "fd_write": self._wasi_fd_write,
            "proc_exit": self._wasi_proc_exit,
            "random_get": self._wasi_random_get,
        }
        callback = exact.get(name)
        if callback is None:
            return None
        return lambda caller, *args: callback(func_type, caller, *args)

    def _return_none(self, _func_type: Any, _caller: Any, *args: Any) -> None:
        return None

    def _return_zero(self, _func_type: Any, _caller: Any, *args: Any) -> int:
        return 0

    def _return_one(self, _func_type: Any, _caller: Any, *args: Any) -> int:
        return 1

    def _return_minus_one(self, _func_type: Any, _caller: Any,
                          *args: Any) -> int:
        return -1

    def _unsupported_web_wave_sound_continuation(
            self, _func_type: Any, _caller: Any, *args: Any) -> None:
        del args
        # These imports suspend/resume a browser JSPI stack.  The motion
        # differential fixture does not exercise WaveSoundBuffer open/play,
        # but the shared core_sound_module still contributes the imports to
        # the full guest.  Make them linkable without pretending Wasmtime can
        # provide their asynchronous browser semantics; fail closed if the
        # fixture ever reaches either edge.
        raise RuntimeError(
            "Web WaveSound JSPI continuation reached in Wasmtime headless "
            "guest"
        )

    def _asm_const_double(self, _func_type: Any, _caller: Any,
                          *args: Any) -> float:
        return self.simulation_time_ms

    def _logo_trace_enabled(self, _func_type: Any, _caller: Any) -> int:
        return int(os.environ.get("KRKR2_WASMTIME_TRACE_LOGO_CHAIN") == "1")

    def _pthread_create(self, _func_type: Any, _caller: Any,
                        *args: Any) -> int:
        return 0

    def _web_audio_decode_start(self, _func_type: Any, caller: Any,
                                *args: Any) -> None:
        if len(args) < 4:
            raise RuntimeError(
                "TVPWebAudioDecodeStartJS received an invalid argument list"
            )
        opaque = int(args[2])
        callback_index = int(args[3])
        table = caller.get("__indirect_function_table")
        if table is None:
            raise RuntimeError("guest function table is unavailable")
        callback = table.get(caller, callback_index)
        if callback is None:
            raise RuntimeError(
                "TVPWebAudioDecodeStartJS completion is unavailable"
            )

        # The headless host has no OfflineAudioContext. Match the browser
        # bridge's unsupported-format path so the guest continues through its
        # existing WASM decoder creator chain.
        callback(caller, opaque, 0, 0, 0, 0, 0)

    def _startup_xp3_path(self, _func_type: Any, caller: Any,
                          *args: Any) -> int:
        del args
        if not self.startup_xp3_path:
            return 0
        malloc = caller.get("malloc")
        if malloc is None:
            return 0
        data = self.startup_xp3_path.encode("utf-8") + b"\0"
        ptr = int(malloc(caller, len(data)))
        if ptr == 0:
            return 0
        self._write(caller, ptr, data)
        return ptr

    def _syscall(self, name: str, caller: Any, *args: Any) -> int:
        if name == "__syscall_getcwd" and len(args) >= 2:
            buf, size = int(args[0]), int(args[1])
            if buf and size > 1:
                self._write(caller, buf, b"/\0")
                return buf
        if name in {"__syscall_stat64", "__syscall_lstat64"} and len(args) >= 2:
            host_path = self._resolve_guest_path(
                self._read_c_string(caller, int(args[0])))
            return self._write_stat_for_path(caller, host_path, int(args[1]))
        if name == "__syscall_newfstatat" and len(args) >= 4:
            host_path = self._resolve_guest_path(
                self._read_c_string(caller, int(args[1])))
            return self._write_stat_for_path(caller, host_path, int(args[2]))
        if name == "__syscall_fstat64" and len(args) >= 2:
            entry = self._fds.get(int(args[0]))
            if entry is None:
                return -8
            return self._write_stat_for_path(caller, entry["path"],
                                             int(args[1]))
        if name == "__syscall_faccessat" and len(args) >= 2:
            host_path = self._resolve_guest_path(
                self._read_c_string(caller, int(args[1])))
            return 0 if host_path.exists() else -44
        if name == "__syscall_openat" and len(args) >= 4:
            return self._open_guest_file(
                self._read_c_string(caller, int(args[1])), int(args[2]))
        if name == "__syscall_unlinkat" and len(args) >= 2:
            host_path = self._resolve_guest_path(
                self._read_c_string(caller, int(args[1])))
            try:
                host_path.unlink()
                return 0
            except FileNotFoundError:
                return -44
            except OSError:
                return -63
        if name == "__syscall_rmdir" and args:
            host_path = self._resolve_guest_path(
                self._read_c_string(caller, int(args[0])))
            try:
                host_path.rmdir()
                return 0
            except FileNotFoundError:
                return -44
            except OSError:
                return -63
        if name == "__syscall_getuid32":
            return 0
        if name in {"__syscall_umask", "__syscall_fchownat",
                    "__syscall_chmod", "__syscall_utimensat"}:
            return 0
        if name in {"__syscall_recvfrom", "__syscall_sendto",
                    "__syscall_wait4", "__syscall_linkat"}:
            return -52
        return -52

    def _resolve_guest_path(self, path: str) -> Path:
        if path.startswith("file://"):
            path = path[len("file://"):]
        if not path:
            path = "/"
        normalized = posixpath.normpath("/" + path.lstrip("/"))
        return self.root / normalized.lstrip("/")

    def _open_guest_file(self, guest_path: str, flags: int) -> int:
        host_path = self._resolve_guest_path(guest_path)
        write = bool(flags & 0x241)  # O_WRONLY | O_CREAT | O_TRUNC
        append = bool(flags & 0x400)
        if write:
            host_path.parent.mkdir(parents=True, exist_ok=True)
            if not host_path.exists():
                host_path.write_bytes(b"")
        if not host_path.exists():
            return -44
        if host_path.is_dir():
            data = b""
        else:
            data = host_path.read_bytes()
        fd = self._next_fd
        self._next_fd += 1
        self._fds[fd] = {
            "path": host_path,
            "data": bytearray(data),
            "pos": len(data) if append else 0,
            "write": write,
            "dir": host_path.is_dir(),
        }
        return fd

    def _write_stat_for_path(self, caller: Any, host_path: Path,
                             buf: int) -> int:
        try:
            st = host_path.stat()
        except FileNotFoundError:
            return -44
        mode = (0o040000 if host_path.is_dir() else 0o100000) | (
            st.st_mode & 0o777)
        size = 0 if host_path.is_dir() else st.st_size
        self._write(caller, buf, b"\0" * 96)
        self._write_i32(caller, buf + 0, 1)
        self._write_i32(caller, buf + 4, mode)
        self._write_i32(caller, buf + 8, 1)
        self._write_i32(caller, buf + 12, 0)
        self._write_i32(caller, buf + 16, 0)
        self._write_i32(caller, buf + 20, 0)
        self._write_i64(caller, buf + 24, size)
        self._write_i32(caller, buf + 32, 4096)
        self._write_i32(caller, buf + 36, (size + 511) // 512)
        sec = int(st.st_mtime)
        nsec = int((st.st_mtime - sec) * 1_000_000_000)
        for off in (40, 56, 72):
            self._write_i64(caller, buf + off, sec)
            self._write_i32(caller, buf + off + 8, nsec)
        self._write_i64(caller, buf + 88, int(st.st_ino) & 0x7fffffff)
        return 0

    def _wasi_clock_time_get(self, _func_type: Any, caller: Any,
                             *args: Any) -> int:
        if len(args) >= 3:
            clock_id = int(args[0])
            now_ns = (
                self._simulation_monotonic_ns
                if clock_id == 1 else time.time_ns()
            )
            self._write_i64(caller, int(args[2]), now_ns)
        return 0

    def _wasi_environ_sizes_get(self, _func_type: Any, caller: Any,
                                *args: Any) -> int:
        if len(args) >= 2:
            self._write_i32(caller, int(args[0]), 0)
            self._write_i32(caller, int(args[1]), 0)
        return 0

    def _wasi_environ_get(self, _func_type: Any, _caller: Any,
                          *args: Any) -> int:
        return 0

    def _wasi_fd_close(self, _func_type: Any, _caller: Any,
                       *args: Any) -> int:
        if args and int(args[0]) >= 3:
            entry = self._fds.pop(int(args[0]), None)
            if entry and entry.get("write"):
                entry["path"].write_bytes(bytes(entry["data"]))
        return 0

    def _wasi_fd_fdstat_get(self, _func_type: Any, caller: Any,
                            *args: Any) -> int:
        if len(args) >= 2:
            fd, buf = int(args[0]), int(args[1])
            entry = self._fds.get(fd)
            filetype = 2 if entry and entry.get("dir") else 4
            self._write(caller, buf, b"\0" * 24)
            self._write(caller, buf, bytes([filetype]))
        return 0

    def _wasi_fd_read(self, _func_type: Any, caller: Any,
                      *args: Any) -> int:
        if len(args) < 4:
            return 28
        fd, iovs, iovs_len, nread_ptr = map(int, args[:4])
        entry = self._fds.get(fd)
        if entry is None:
            self._write_i32(caller, nread_ptr, 0)
            return 8
        total = 0
        for i in range(iovs_len):
            ptr = int.from_bytes(self._read(caller, iovs + i * 8, 4),
                                 "little")
            length = int.from_bytes(self._read(caller, iovs + i * 8 + 4, 4),
                                    "little")
            pos = int(entry["pos"])
            chunk = bytes(entry["data"][pos:pos + length])
            self._write(caller, ptr, chunk)
            entry["pos"] = pos + len(chunk)
            total += len(chunk)
            if len(chunk) < length:
                break
        self._write_i32(caller, nread_ptr, total)
        return 0

    def _wasi_fd_write(self, _func_type: Any, caller: Any,
                       *args: Any) -> int:
        if len(args) < 4:
            return 28
        fd, iovs, iovs_len, nwritten_ptr = map(int, args[:4])
        chunks: list[bytes] = []
        total = 0
        for i in range(iovs_len):
            ptr = int.from_bytes(self._read(caller, iovs + i * 8, 4),
                                 "little")
            length = int.from_bytes(self._read(caller, iovs + i * 8 + 4, 4),
                                    "little")
            chunk = self._read(caller, ptr, length)
            chunks.append(chunk)
            total += len(chunk)
        if fd in (1, 2):
            stream = sys.stdout.buffer if fd == 1 else sys.stderr.buffer
            stream.write(b"".join(chunks))
            stream.flush()
        else:
            entry = self._fds.get(fd)
            if entry is None:
                return 8
            pos = int(entry["pos"])
            data = b"".join(chunks)
            buf = entry["data"]
            end = pos + len(data)
            if end > len(buf):
                buf.extend(b"\0" * (end - len(buf)))
            buf[pos:end] = data
            entry["pos"] = end
            entry["write"] = True
        self._write_i32(caller, nwritten_ptr, total)
        return 0

    def _wasi_fd_seek(self, _func_type: Any, caller: Any,
                      *args: Any) -> int:
        if len(args) < 4:
            return 28
        fd, offset, whence, newoffset_ptr = int(args[0]), int(args[1]), int(args[2]), int(args[3])
        entry = self._fds.get(fd)
        if entry is None:
            return 8
        base = 0
        if whence == 1:
            base = int(entry["pos"])
        elif whence == 2:
            base = len(entry["data"])
        new_pos = max(0, base + offset)
        entry["pos"] = new_pos
        self._write_i64(caller, newoffset_ptr, new_pos)
        return 0

    def _wasi_proc_exit(self, _func_type: Any, _caller: Any,
                        *args: Any) -> None:
        code = int(args[0]) if args else 0
        raise RuntimeError(f"guest called proc_exit({code})")

    def _wasi_random_get(self, _func_type: Any, caller: Any,
                         *args: Any) -> int:
        if len(args) >= 2:
            self._write(caller, int(args[0]), os.urandom(int(args[1])))
        return 0

    def _fsafs(self, name: str, caller: Any, *args: Any) -> Any:
        if name == "fsafs_is_host_stream":
            return 0
        if name == "fsafs_open_stream":
            return -1
        if name == "fsafs_get_stream_size":
            return 0.0
        if name == "fsafs_read_stream":
            return -1
        return None

    def _vlfs(self, name: str, _caller: Any, *args: Any) -> Any:
        # The headless guest has no window.VLFS provider. Answering 0 to
        # vlfs_js_enabled makes VLFS::Enabled() cache false, so the engine
        # keeps the MEMFS path; every other bridge is gated on Enabled()
        # and must never fire — fail loud if one does.
        if name == "vlfs_js_enabled":
            return 0
        raise RuntimeError(
            f"VLFS bridge called while disabled: {name}{args}"
        )

    def _emscripten_set(self, name: str, func_type: Any, caller: Any,
                        *args: Any) -> Any:
        if name == "emscripten_set_main_loop_arg":
            self.main_loop = tuple(int(v) for v in args[:4])
            return None
        if name == "emscripten_set_canvas_element_size" and len(args) >= 3:
            self.canvas_width = int(args[1])
            self.canvas_height = int(args[2])
            return 0
        if name == "emscripten_set_element_css_size" and len(args) >= 3:
            self.css_width = float(args[1])
            self.css_height = float(args[2])
            return 0
        if name == "emscripten_set_window_title":
            return None
        return self._default_return(func_type)

    def _emscripten_get(self, name: str, func_type: Any, caller: Any,
                        *args: Any) -> Any:
        if name == "emscripten_get_now":
            return self.simulation_time_ms
        if name == "emscripten_get_canvas_element_size" and len(args) >= 3:
            self._write_i32(caller, int(args[1]), self.canvas_width)
            self._write_i32(caller, int(args[2]), self.canvas_height)
            return 0
        if name == "emscripten_get_element_css_size" and len(args) >= 3:
            self._write_f64(caller, int(args[1]), self.css_width)
            self._write_f64(caller, int(args[2]), self.css_height)
            return 0
        if name == "emscripten_get_screen_size" and len(args) >= 2:
            self._write_i32(caller, int(args[0]), self.canvas_width)
            self._write_i32(caller, int(args[1]), self.canvas_height)
            return None
        if name == "emscripten_get_device_pixel_ratio":
            return 1.0
        if name == "emscripten_get_num_gamepads":
            return 0
        if name == "emscripten_get_gamepad_status":
            return -1
        return self._default_return(func_type)

    def _egl(self, name: str, caller: Any, *args: Any) -> Any:
        if name == "eglGetError":
            return 0x3000
        if name == "eglGetDisplay":
            return 1
        if name == "eglInitialize":
            if len(args) >= 3:
                self._write_i32(caller, int(args[1]), 1)
                self._write_i32(caller, int(args[2]), 4)
            return 1
        if name == "eglChooseConfig":
            if len(args) >= 5:
                configs, config_size, num_config = int(args[2]), int(args[3]), int(args[4])
                if configs and config_size > 0:
                    self._write_i32(caller, configs, 1)
                if num_config:
                    self._write_i32(caller, num_config, 1)
            return 1
        if name == "eglGetConfigAttrib":
            if len(args) >= 4:
                self._write_i32(caller, int(args[3]), 8)
            return 1
        if name in {"eglCreateContext", "eglCreateWindowSurface"}:
            return 1
        if name == "eglSwapBuffers":
            if self.capture_swap_framebuffer and self.gl_provider is not None:
                try:
                    data = self.gl_provider.read_default_framebuffer_bgra(
                        POST_DRAW_CANVAS_SIZE[0], POST_DRAW_CANVAS_SIZE[1])
                    self._swap_framebuffers.append({
                        "ok": True,
                        "data": data,
                        "width": POST_DRAW_CANVAS_SIZE[0],
                        "height": POST_DRAW_CANVAS_SIZE[1],
                        "pitch": POST_DRAW_CANVAS_SIZE[0] * 4,
                        "diagnostics": (
                            self.gl_provider
                            .last_default_framebuffer_read_diagnostics()),
                    })
                except Exception as exc:
                    self._swap_framebuffers.append({
                        "ok": False,
                        "error": str(exc),
                    })
            return 1
        if name in {"eglMakeCurrent", "eglBindAPI",
                    "eglSwapInterval", "eglWaitGL", "eglWaitNative",
                    "eglTerminate", "eglDestroyContext",
                    "eglDestroySurface"}:
            return 1
        if name == "eglQueryString":
            return 0
        return 0

    def _openal(self, name: str, caller: Any, *args: Any) -> Any:
        if name in {"alcOpenDevice", "alcCreateContext",
                    "alcGetCurrentContext"}:
            return 1
        if name in {"alcMakeContextCurrent", "alcCloseDevice",
                    "alcDestroyContext"}:
            return 1
        if name in {"alcGetString", "alcIsExtensionPresent"}:
            return 0
        if name == "alGetError":
            return 0
        if name in {"alGenSources", "alGenBuffers"} and len(args) >= 2:
            count, ptr = int(args[0]), int(args[1])
            ids = list(range(self._next_al_id, self._next_al_id + count))
            self._next_al_id += count
            self._write_u32_array(caller, ptr, ids)
            return None
        if name == "alGetSourcei" and len(args) >= 3:
            self._write_i32(caller, int(args[2]), 0)
            return None
        if name == "alGetSourcef" and len(args) >= 3:
            self._write(caller, int(args[2]), struct.pack("<f", 0.0))
            return None
        if name == "alGetSourcefv" and len(args) >= 3:
            self._write(caller, int(args[2]), struct.pack("<fff", 0.0, 0.0, 0.0))
            return None
        return None


def define_emscripten_imports(wasmtime, linker, module,
                              env_provider: WasmtimeEnvProvider) -> None:
    del wasmtime
    env_provider.define_imports(linker, module)


@dataclass(frozen=True)
class WasmtimeBootstrapInfo:
    root: Path
    preload_files: int
    font_guest_path: str
    xp3_guest_path: str

    def summary(self) -> str:
        return (
            "bootstrap: "
            f"guestRoot={self.root}, preloadFiles={self.preload_files}, "
            f"font={self.font_guest_path}, xp3={self.xp3_guest_path}"
        )


def _wasmtime_preference_xml(renderer: str) -> str:
    return (
        "<?xml version=\"1.0\"?>\n"
        "<GlobalPreference>\n"
        f"    <Item key=\"renderer\" value=\"{renderer}\"/>\n"
        f"    <Item key=\"fps_limit\" value=\"{SIMULATION_FPS:g}\"/>\n"
        "</GlobalPreference>\n"
    )


def prepare_wasmtime_bootstrap(root: Path,
                              startup_xp3: Path) -> WasmtimeBootstrapInfo:
    preload_src = REPO_ROOT / "ui" / "cocos-studio"
    if not preload_src.is_dir():
        raise FileNotFoundError(f"Wasmtime preload source missing: {preload_src}")

    for dirname in ("savedata", "save", "tmp", "reference/xp3"):
        (root / dirname).mkdir(parents=True, exist_ok=True)

    renderer = os.environ.get("KRKR2_WASMTIME_RENDERER") or "software"
    pref = root / "save" / "GlobalPreference.xml"
    pref.write_text(
        _wasmtime_preference_xml(renderer),
        encoding="utf-8",
    )

    preload_count = 0
    for src in preload_src.rglob("*"):
        if not src.is_file():
            continue
        rel = src.relative_to(preload_src)
        dst = root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        preload_count += 1

    font_path = root / "DroidSansFallback.ttf"
    if not font_path.is_file():
        raise FileNotFoundError(
            "Wasmtime preload did not provide /DroidSansFallback.ttf"
        )

    xp3_guest_rel = Path("reference/xp3") / startup_xp3.name
    xp3_dst = root / xp3_guest_rel
    xp3_dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(startup_xp3, xp3_dst)

    return WasmtimeBootstrapInfo(
        root=root,
        preload_files=preload_count,
        font_guest_path="/DroidSansFallback.ttf",
        xp3_guest_path="/" + xp3_guest_rel.as_posix(),
    )


def instantiate_module(wasmtime, wasm_path: Path, enable_gl: bool,
                       wasi_root: Path = REPO_ROOT):
    config = wasmtime.Config()
    config.debug_info = True
    config.cranelift_opt_level = "none"
    config.wasm_exceptions = True
    config.wasm_simd = True
    config.wasm_threads = True
    config.shared_memory = True
    # The debug guest is roughly 150 MiB.  Its content digest is stable across
    # the per-case direct guest runs, so reuse Wasmtime's compiled-module
    # cache instead of recompiling it for every trace lane.
    config.cache = True
    engine = wasmtime.Engine(config)
    module = wasmtime.Module.from_file(engine, str(wasm_path))
    store = wasmtime.Store(engine)

    wasi = wasmtime.WasiConfig()
    wasi.inherit_stdout()
    wasi.inherit_stderr()
    wasi.preopen_dir(str(wasi_root), "/")
    store.set_wasi(wasi)

    linker = wasmtime.Linker(engine)
    gl_provider = None
    if enable_gl:
        from wasmtime_gl_provider import WasmtimeGLProvider

        gl_provider = WasmtimeGLProvider()
    env_provider = WasmtimeEnvProvider(wasi_root, gl_provider)
    for imp in module.imports:
        if imp.module == "env" and imp.name == "memory":
            memory_type = imp.type
            if getattr(memory_type, "is_shared", False):
                import wasmtime._sharedmemory as sharedmemory  # type: ignore

                def _fixed_shared_memory_as_extern(self):
                    ffi = sharedmemory.ffi
                    union = ffi.wasmtime_extern_union(
                        sharedmemory=self.ptr())
                    return ffi.wasmtime_extern_t(
                        ffi.WASMTIME_EXTERN_SHAREDMEMORY, union)

                sharedmemory.SharedMemory._as_extern = (
                    _fixed_shared_memory_as_extern)
                memory = wasmtime.SharedMemory(engine, memory_type)
            else:
                memory = wasmtime.Memory(store, memory_type)
            linker.define(store, "env", "memory", memory)
    define_emscripten_imports(wasmtime, linker, module, env_provider)
    if enable_gl:
        gl_provider.define_imports(linker, module)
    else:
        gl_imports = [
            f"{imp.module}.{imp.name}" for imp in module.imports
            if imp.module == "env" and imp.name.startswith("gl")
        ]
        if gl_imports:
            raise RuntimeError(
                "wasm module has GL imports but GL provider is disabled: "
                f"imports: {', '.join(gl_imports)}"
            )
    instance = linker.instantiate(store, module)
    exports = instance.exports(store)

    initialize = None
    for init_name in ("__initialize", "_initialize"):
        try:
            initialize = exports[init_name]
            break
        except Exception:
            continue
    if initialize is not None:
        initialize(store)

    return store, exports, gl_provider, env_provider


def mem_base(store, memory) -> int:
    try:
        ptr = memory.data_ptr(store)
    except TypeError:
        ptr = memory.data_ptr()
    return ctypes.addressof(ptr.contents)


def write_bytes(store, memory, ptr: int, data: bytes) -> None:
    ctypes.memmove(mem_base(store, memory) + ptr, data, len(data))


def read_string(store, memory, ptr: int, length: int) -> str:
    if ptr == 0 or length <= 0:
        return ""
    buf = (ctypes.c_char * length).from_address(mem_base(store, memory) + ptr)
    return bytes(buf).decode("utf-8", errors="replace")


def _read_render_probe_events(store, exports, memory) -> list[dict[str, Any]]:
    try:
        get_ptr = exports["krkr2_wasm_get_render_probe_ptr"]
        get_len = exports["krkr2_wasm_get_render_probe_len"]
        clear = exports["krkr2_wasm_clear_render_probe"]
    except KeyError as exc:
        raise RuntimeError(
            "Wasmtime guest does not expose render probe buffer exports; "
            "rebuild krkr2_wasmtime_guest") from exc

    length = int(get_len(store))
    if length <= 0:
        clear(store)
        return []
    ptr = int(get_ptr(store))
    text = read_string(store, memory, ptr, length)
    clear(store)
    events: list[dict[str, Any]] = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            start = max(0, exc.pos - 80)
            end = min(len(line), exc.pos + 80)
            raise RuntimeError(
                f"render probe JSONL decode failed at line {lineno}: {exc}; "
                f"fragment={line[start:end]!r}"
            ) from exc
        if not isinstance(event, dict):
            raise RuntimeError(
                f"render probe JSONL line {lineno} is not an object: {event!r}")
        events.append(event)
    return events


def call_with_guest_bytes(store, memory, malloc, free, data: bytes, callback):
    ptr = malloc(store, len(data))
    if ptr == 0 and data:
        raise RuntimeError("guest malloc failed")
    try:
        write_bytes(store, memory, ptr, data)
        return callback(ptr, len(data))
    finally:
        free(store, ptr)


def _load_render_stage_events(path: Path | None) -> list[dict[str, Any]]:
    if path is None:
        raise RuntimeError("Wasmtime render collection requires render stage events")
    if not path.exists():
        raise RuntimeError(f"Wasmtime render stage event file missing: {path}")
    try:
        events = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"Wasmtime render stage event JSON decode failed: {exc}") from exc
    if not isinstance(events, list):
        raise RuntimeError(
            f"Wasmtime render stage event root is not a list: {type(events)}")
    return [
        event for event in events
        if isinstance(event, dict)
    ]


def _load_render_checkpoint_events(path: Path | None) -> list[dict[str, Any]]:
    return [
        event for event in _load_render_stage_events(path)
        if event.get("kind") == "execute_image_checkpoint"
    ]


def _capture_frame_id_enabled(frame_id: int, start: int, count: int) -> bool:
    if frame_id < max(0, int(start)):
        return False
    return int(count) < 0 or frame_id < max(0, int(start)) + int(count)


def _annotate_wasmtime_layer_raw_probe_events(
    bootstrap: WasmtimeBootstrapInfo,
    render_stage_events_path: Path | None,
) -> None:
    if render_stage_events_path is None or not render_stage_events_path.exists():
        return
    try:
        events = json.loads(render_stage_events_path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(
            f"Wasmtime render stage event JSON decode failed: {exc}") from exc
    if not isinstance(events, list):
        raise RuntimeError(
            f"Wasmtime render stage event root is not a list: {type(events)}")

    changed = False
    for event in events:
        if not isinstance(event, dict) or event.get("stage") != "layer_raw_probe":
            continue
        guest_path = event.get("guestPath")
        if not isinstance(guest_path, str):
            continue
        raw_path = bootstrap.root / guest_path.lstrip("/")
        if not raw_path.exists():
            event["ok"] = False
            event["error"] = f"raw probe BGRA file not found: {guest_path}"
            changed = True
            continue
        try:
            event["rgbaSha256"] = bgra_rgba_sha256_file(raw_path)
            event["bytes"] = raw_path.stat().st_size
        except RuntimeError as exc:
            event["ok"] = False
            event["error"] = str(exc)
        try:
            raw_path.unlink()
        except FileNotFoundError:
            pass
        changed = True

    if changed:
        render_stage_events_path.write_text(
            json.dumps(events, ensure_ascii=False, allow_nan=False) + "\n",
            encoding="utf-8",
        )


def _png_frame_number(path: Path) -> int | None:
    stem = path.stem
    if not stem.startswith("frame_"):
        return None
    try:
        return int(stem[len("frame_"):])
    except ValueError:
        return None


def _write_wasmtime_framebuffer_manifest(
    framebuffer_dir: Path,
    specs: list[dict],
    *,
    wasm_path: Path,
    startup_xp3: Path,
    manifest_startup_xp3: Path,
    bootstrap: WasmtimeBootstrapInfo,
    capture_window: FrameCaptureWindow,
) -> Path:
    from oracle_runner.adapters import motion_playback as mpb

    specs_by_id = {s["id"]: s for s in specs}
    segment_order = mpb.segment_order_for_specs(specs_by_id)
    cases: list[dict[str, Any]] = []
    total_frames = 0
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
                raise RuntimeError(f"missing Wasmtime framebuffer PNG: {path}")
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
                f"unexpected extra Wasmtime framebuffer PNG(s) for {spec_id}: "
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
        "schema": FRAMEBUFFER_SCHEMA,
        "source": FRAMEBUFFER_SOURCE,
        "captureSurface": FRAMEBUFFER_CAPTURE_SURFACE,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "localRoot": str(framebuffer_dir),
        "wasm": str(wasm_path),
        "startupXp3": str(manifest_startup_xp3),
        "captureXp3": str(startup_xp3),
        "fixture": {
            "guestRoot": str(bootstrap.root),
            "guestCaptureRoot": RENDER_CAPTURE_GUEST_ROOT,
            "guestCapturePhase": "post_draw",
            "xp3": bootstrap.xp3_guest_path,
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


def _collect_wasmtime_render_stage_capture(
    bootstrap: WasmtimeBootstrapInfo,
    render_artifact_dir: Path,
    specs: list[dict],
    *,
    record_render_step_checkpoints: bool = False,
    checkpoint_render_only: bool = False,
    render_stage_events_path: Path | None = None,
    capture_window: FrameCaptureWindow,
) -> dict[str, Any]:
    from oracle_runner.adapters import motion_playback as mpb

    capture_root = bootstrap.root / RENDER_CAPTURE_GUEST_ROOT.lstrip("/")
    if not capture_root.is_dir():
        raise RuntimeError(
            f"Wasmtime render stage capture directory missing: {capture_root}")
    _annotate_wasmtime_layer_raw_probe_events(
        bootstrap, render_stage_events_path)

    render_artifact_dir.mkdir(parents=True, exist_ok=True)
    images_root = render_artifact_dir / "images"
    images_root.mkdir(parents=True, exist_ok=True)

    specs_by_id = {s["id"]: s for s in specs}
    segment_order = mpb.segment_order_for_specs(specs_by_id)
    checkpoint_by_frame_phase: dict[tuple[int, str], dict[str, Any]] = {}
    if record_render_step_checkpoints:
        for checkpoint in _load_render_checkpoint_events(render_stage_events_path):
            frame_id = checkpoint.get("frameId")
            phase = checkpoint.get("phase")
            if isinstance(frame_id, int) and isinstance(phase, str):
                checkpoint_by_frame_phase[(frame_id, phase)] = checkpoint

    cases: list[dict[str, Any]] = []
    total_images = 0
    for case in captured_case_ranges(specs_by_id, segment_order,
                                     capture_window):
        spec_id = str(case["caseId"])
        spec = case["spec"]
        expected = int(spec["frames"])
        requested_local_frames = list(case["capturedLocalFrames"])
        requested_set = set(requested_local_frames)
        captured_local_frames: list[int] | None = None
        case_frame_id_base = int(case["fullFrameIdRange"][0])
        dst_case = images_root / spec_id
        if checkpoint_render_only:
            dst_case.mkdir(parents=True, exist_ok=True)
        else:
            src_case = capture_root / spec_id
            if not src_case.is_dir():
                raise RuntimeError(
                    f"Wasmtime render stage case directory missing: {src_case}")
            shutil.copytree(src_case, dst_case)

        if record_render_step_checkpoints:
            for phase in RENDER_STEP_CHECKPOINT_SURFACES:
                dst_phase = dst_case / phase
                if dst_phase.exists():
                    shutil.rmtree(dst_phase)
                dst_phase.mkdir(parents=True, exist_ok=True)
                for local_frame in requested_local_frames:
                    global_frame = case_frame_id_base + local_frame
                    checkpoint = checkpoint_by_frame_phase.get(
                        (global_frame, phase))
                    if checkpoint is None:
                        if phase == "post_draw":
                            continue
                        if checkpoint_render_only and phase != "post_draw":
                            continue
                        raise RuntimeError(
                            f"missing Wasmtime {phase} checkpoint for "
                            f"{spec_id} frame {local_frame} "
                            f"(frameId {global_frame})")
                    if not checkpoint.get("ok"):
                        raise RuntimeError(
                            f"Wasmtime {phase} checkpoint failed for "
                            f"{spec_id} frame {local_frame}: "
                            f"{checkpoint.get('error')}")
                    guest_path_value = checkpoint.get("guestPath")
                    if not isinstance(guest_path_value, str):
                        raise RuntimeError(
                            f"Wasmtime {phase} checkpoint has no guestPath for "
                            f"{spec_id} frame {local_frame}")
                    pixel_format = checkpoint.get("pixelFormat")
                    if pixel_format not in {"bgra32", "rgba32"}:
                        raise RuntimeError(
                            f"Wasmtime {phase} checkpoint has unsupported "
                            f"pixelFormat for {spec_id} frame {local_frame}: "
                            f"{pixel_format}")
                    if phase == "post_draw":
                        if checkpoint.get("source") != (
                                "wasmtime-port-render-stage"):
                            raise RuntimeError(
                                f"Wasmtime post_draw checkpoint for {spec_id} "
                                f"frame {local_frame} was not captured from "
                                "the guest render stage")
                        size = (
                            int(checkpoint["width"]),
                            int(checkpoint["height"]),
                        )
                        if size != POST_DRAW_CANVAS_SIZE:
                            raise RuntimeError(
                                f"Wasmtime post_draw checkpoint for {spec_id} "
                                f"frame {local_frame} captured {size[0]}x{size[1]}, "
                                f"expected {POST_DRAW_CANVAS_SIZE[0]}x"
                                f"{POST_DRAW_CANVAS_SIZE[1]} canvas")
                    raw_path = bootstrap.root / guest_path_value.lstrip("/")
                    if not raw_path.exists():
                        raise RuntimeError(
                            "missing Wasmtime render checkpoint raw pixels: "
                            f"{raw_path}")
                    rel = Path("images") / spec_id / phase / \
                        f"frame_{local_frame:04d}.png"
                    if pixel_format == "bgra32":
                        write_bgra_png(
                            raw_path=raw_path,
                            path=render_artifact_dir / rel,
                            width=int(checkpoint["width"]),
                            height=int(checkpoint["height"]),
                        )
                    else:
                        write_rgba_png(
                            raw_path=raw_path,
                            path=render_artifact_dir / rel,
                            width=int(checkpoint["width"]),
                            height=int(checkpoint["height"]),
                        )
                    try:
                        raw_path.unlink()
                    except FileNotFoundError:
                        pass

        phases: dict[str, list[dict[str, Any]]] = {}
        phases_to_collect = (
            [] if checkpoint_render_only else list(RENDER_CAPTURE_SURFACES)
        )
        if record_render_step_checkpoints:
            phases_to_collect.extend(RENDER_STEP_CHECKPOINT_SURFACES)
        for phase in phases_to_collect:
            phase_dir = dst_case / phase
            if not phase_dir.is_dir():
                raise RuntimeError(
                    f"missing Wasmtime render stage image directory: {phase_dir}")
            present_frames = [
                frame for frame in (
                    _png_frame_number(p)
                    for p in sorted(phase_dir.glob("frame_*.png"))
                )
                if frame is not None
            ]
            if phase == "initial":
                expected_phase_frames = [0]
            elif phase == "post_draw":
                extras = [
                    frame for frame in present_frames
                    if frame not in requested_set
                ]
                if extras:
                    raise RuntimeError(
                        f"unexpected extra Wasmtime render stage PNG frame(s) "
                        f"for {spec_id}/{phase}: {extras[:5]}"
                    )
                expected_phase_frames = [
                    frame for frame in present_frames
                    if frame in requested_set
                ]
                if not expected_phase_frames:
                    raise RuntimeError(
                        f"no Wasmtime render stage PNGs captured for "
                        f"{spec_id}/{phase}")
                captured_local_frames = expected_phase_frames
            else:
                if checkpoint_render_only:
                    expected_phase_frames = present_frames
                else:
                    expected_phase_frames = requested_local_frames
            expected_set = set(expected_phase_frames)
            images: list[dict[str, Any]] = []
            for frame in expected_phase_frames:
                rel = Path("images") / spec_id / phase / f"frame_{frame:04d}.png"
                path = render_artifact_dir / rel
                if not path.exists():
                    raise RuntimeError(
                        f"missing Wasmtime render stage PNG: {path}")
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
                    f"unexpected extra Wasmtime render stage PNG(s) for "
                    f"{spec_id}/{phase}: {[p.name for p in extras[:5]]}"
                )
            phases[phase] = images
            total_images += len(images)
        if captured_local_frames is None:
            captured_local_frames = (
                requested_local_frames if checkpoint_render_only else []
            )

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

    capture_surfaces = (
        [] if checkpoint_render_only else list(RENDER_CAPTURE_SURFACES)
    )
    if record_render_step_checkpoints:
        capture_surfaces.extend(RENDER_STEP_CHECKPOINT_SURFACES)
    image_manifest = {
        "guestCaptureRoot": (
            RENDER_CHECKPOINT_GUEST_ROOT if checkpoint_render_only
            else RENDER_CAPTURE_GUEST_ROOT
        ),
        "captureSurfaces": capture_surfaces,
        "cases": cases,
        "summary": {
            "caseCount": len(cases),
            "imageCount": total_images,
        },
        **capture_window.manifest_fields(),
    }
    image_manifest_path = render_artifact_dir / "image_manifest.json"
    image_manifest_path.write_text(
        json.dumps(image_manifest, indent=2, ensure_ascii=True,
                   allow_nan=False) + "\n",
        encoding="utf-8",
    )
    return image_manifest


def _write_wasmtime_semantic_image_manifest(
    render_artifact_dir: Path,
    specs: list[dict[str, Any]],
    *,
    capture_window: FrameCaptureWindow,
) -> dict[str, Any]:
    """Write the image-schema compatibility envelope for semantic-only runs."""
    from oracle_runner.adapters import motion_playback as mpb

    render_artifact_dir.mkdir(parents=True, exist_ok=True)
    specs_by_id = {str(spec["id"]): spec for spec in specs}
    segment_order = mpb.segment_order_for_specs(specs_by_id)
    cases: list[dict[str, Any]] = []
    for case in captured_case_ranges(
        specs_by_id, segment_order, capture_window,
    ):
        spec_id = str(case["caseId"])
        spec = case["spec"]
        captured_local_frames = list(case["capturedLocalFrames"])
        cases.append({
            "caseId": spec_id,
            "mtnPath": spec.get("mtn_path"),
            "chara": spec.get("chara"),
            "label": spec.get("label"),
            "frames": int(spec["frames"]),
            "fullFrameIdRange": case["fullFrameIdRange"],
            "capturedFrameIdRange": case["capturedFrameIdRange"],
            "capturedLocalFrames": captured_local_frames,
            "requestedLocalFrames": captured_local_frames,
            "phases": {},
        })

    image_manifest = {
        "guestCaptureRoot": None,
        "captureSurfaces": [],
        "semanticOnly": True,
        "cases": cases,
        "summary": {
            "caseCount": len(cases),
            "imageCount": 0,
        },
        **capture_window.manifest_fields(),
    }
    image_manifest_path = render_artifact_dir / "image_manifest.json"
    image_manifest_path.write_text(
        json.dumps(image_manifest, indent=2, ensure_ascii=True,
                   allow_nan=False) + "\n",
        encoding="utf-8",
    )
    return image_manifest


def _collect_wasmtime_framebuffer_capture(
    bootstrap: WasmtimeBootstrapInfo,
    framebuffer_dir: Path,
    specs: list[dict],
    *,
    wasm_path: Path,
    startup_xp3: Path,
    manifest_startup_xp3: Path,
    capture_window: FrameCaptureWindow,
) -> Path:
    from oracle_runner.adapters import motion_playback as mpb

    capture_root = bootstrap.root / RENDER_CAPTURE_GUEST_ROOT.lstrip("/")
    if not capture_root.is_dir():
        raise RuntimeError(
            f"Wasmtime framebuffer capture directory missing: {capture_root}")

    framebuffer_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = framebuffer_dir / "manifest.json"
    if manifest_path.exists():
        manifest_path.unlink()
    specs_by_id = {s["id"]: s for s in specs}
    segment_order = mpb.segment_order_for_specs(specs_by_id)
    for spec_id in specs_by_id:
        old_case = framebuffer_dir / str(spec_id)
        if old_case.exists():
            shutil.rmtree(old_case)

    for case in captured_case_ranges(specs_by_id, segment_order,
                                     capture_window):
        spec_id = str(case["caseId"])
        src_phase = capture_root / spec_id / "post_draw"
        if not src_phase.is_dir():
            raise RuntimeError(
                f"Wasmtime framebuffer source phase missing: {src_phase}")
        dst_case = framebuffer_dir / spec_id
        if dst_case.exists():
            shutil.rmtree(dst_case)
        dst_case.mkdir(parents=True, exist_ok=True)
        for frame in case["capturedLocalFrames"]:
            name = f"frame_{int(frame):04d}.png"
            src = src_phase / name
            if not src.exists():
                raise RuntimeError(
                    f"missing Wasmtime framebuffer source PNG: {src}")
            shutil.copy2(src, dst_case / name)

    return _write_wasmtime_framebuffer_manifest(
        framebuffer_dir, specs,
        wasm_path=wasm_path,
        startup_xp3=startup_xp3,
        manifest_startup_xp3=manifest_startup_xp3,
        bootstrap=bootstrap,
        capture_window=capture_window)


def drive_full_guest(wasm_path: Path, startup_xp3: Path,
                     frames: int, *,
                     motion_trace_out: Path | None = None,
                     framebuffer_dir: Path | None = None,
                     render_artifact_dir: Path | None = None,
                     record_render_step_checkpoints: bool = False,
                     checkpoint_render_only: bool = False,
                     record_layer_raw_probes: bool = False,
                     record_save_layer_visual_readback_probes: bool = False,
                     save_layer_visual_readback_frame_start: int = 0,
                     save_layer_visual_readback_frame_count: int = 1,
                     capture_window: FrameCaptureWindow,
                     render_stage_out: Path | None = None,
                     specs: list[dict] | None = None,
                     manifest_startup_xp3: Path | None = None) -> dict[str, Any]:
    if not wasm_path.exists():
        raise FileNotFoundError(
            f"wasm module not found: {wasm_path}. Build with "
            "`cmake --build out/wasmtime/debug --target krkr2_wasmtime_guest`."
        )
    if not startup_xp3.exists():
        raise FileNotFoundError(f"oracle bootstrap xp3 missing: {startup_xp3}")

    from oracle_runner.adapters import motion_playback as mpb

    wasmtime = load_wasmtime()
    with tempfile.TemporaryDirectory(prefix="krkr2-wasmtime-guestfs-") as tmp:
        bootstrap = prepare_wasmtime_bootstrap(Path(tmp), startup_xp3)
        try:
            if specs is None:
                raise RuntimeError(
                    "motion_playback Wasmtime fixture requires specs")
            capture_root = (
                bootstrap.root / RENDER_CAPTURE_GUEST_ROOT.lstrip("/")
            )
            segment_order = mpb.segment_order_for_specs(specs)
            for case in captured_case_ranges(specs, segment_order,
                                             capture_window):
                spec_id = str(case["caseId"])
                if checkpoint_render_only:
                    (capture_root / spec_id).mkdir(parents=True, exist_ok=True)
                else:
                    for phase in RENDER_CAPTURE_SURFACES:
                        (capture_root / spec_id / phase).mkdir(
                            parents=True, exist_ok=True)
            if render_artifact_dir is not None:
                if record_render_step_checkpoints:
                    checkpoint_root = (
                        bootstrap.root /
                        RENDER_CHECKPOINT_GUEST_ROOT.lstrip("/")
                    )
                    for phase in RENDER_STEP_CHECKPOINT_SURFACES:
                        (checkpoint_root / "_execute" / phase).mkdir(
                            parents=True, exist_ok=True)
            summary = _drive_full_guest_with_bootstrap(
                wasmtime, wasm_path, bootstrap, frames,
                record_render_step_checkpoints=(
                    record_render_step_checkpoints),
                record_layer_raw_probes=record_layer_raw_probes,
                record_save_layer_visual_readback_probes=(
                    record_save_layer_visual_readback_probes),
                save_layer_visual_readback_frame_start=(
                    save_layer_visual_readback_frame_start),
                save_layer_visual_readback_frame_count=(
                    save_layer_visual_readback_frame_count),
                capture_frame_start=capture_window.start,
                capture_frame_count=(
                    -1 if not capture_window.enabled
                    else capture_window.count),
                render_case_frame_bases=_render_case_frame_bases(
                    specs, mpb, capture_window),
                render_stage_out=render_stage_out,
                motion_trace_out=motion_trace_out)
            if framebuffer_dir is not None:
                manifest = _collect_wasmtime_framebuffer_capture(
                    bootstrap, framebuffer_dir, specs,
                    wasm_path=wasm_path,
                    startup_xp3=startup_xp3,
                    manifest_startup_xp3=(
                        manifest_startup_xp3 or startup_xp3),
                    capture_window=capture_window)
                summary["framebufferManifest"] = str(manifest)
            if render_artifact_dir is not None:
                semantic_only = not any((
                    record_render_step_checkpoints,
                    checkpoint_render_only,
                    record_layer_raw_probes,
                    record_save_layer_visual_readback_probes,
                ))
                if semantic_only:
                    image_manifest = _write_wasmtime_semantic_image_manifest(
                        render_artifact_dir,
                        specs,
                        capture_window=capture_window,
                    )
                else:
                    image_manifest = _collect_wasmtime_render_stage_capture(
                        bootstrap, render_artifact_dir, specs,
                        record_render_step_checkpoints=(
                            record_render_step_checkpoints),
                        checkpoint_render_only=checkpoint_render_only,
                        render_stage_events_path=render_stage_out,
                        capture_window=capture_window)
                summary["renderStageImageManifest"] = image_manifest["summary"]
            return summary
        except Exception as exc:
            raise RuntimeError(f"{exc}\n{bootstrap.summary()}") from exc


def _drive_full_guest_with_bootstrap(wasmtime, wasm_path: Path,
                                     bootstrap: WasmtimeBootstrapInfo,
                                     frames: int, *,
                                     record_render_step_checkpoints: bool = False,
                                     record_layer_raw_probes: bool = False,
                                     record_save_layer_visual_readback_probes: bool = False,
                                     save_layer_visual_readback_frame_start: int = 0,
                                     save_layer_visual_readback_frame_count: int = 1,
                                     capture_frame_start: int = 0,
                                     capture_frame_count: int = -1,
                                     render_case_frame_bases:
                                     dict[str, int] | None = None,
                                     render_stage_out: Path | None = None,
                                     motion_trace_out: Path | None = None
                                     ) -> dict[str, Any]:
    store, exports, gl_provider, env_provider = instantiate_module(
        wasmtime, wasm_path, enable_gl=True, wasi_root=bootstrap.root)
    memory = exports["memory"]
    malloc = exports["malloc"]
    free = exports["free"]
    init = exports["krkr2_wasm_init"]
    tick = exports["krkr2_wasm_tick"]
    try:
        set_record_layer_raw_probes = exports[
            "krkr2_wasm_set_record_layer_raw_probes"]
    except Exception:
        set_record_layer_raw_probes = None
    try:
        set_record_save_layer_visual_readback_probes = exports[
            "krkr2_wasm_set_record_save_layer_visual_readback_probes"]
    except Exception:
        set_record_save_layer_visual_readback_probes = None
    try:
        set_render_capture_frame_filter = exports[
            "krkr2_wasm_set_render_capture_frame_filter"]
    except Exception:
        set_render_capture_frame_filter = None
    try:
        set_render_case_frame_base = exports[
            "krkr2_wasm_set_render_case_frame_base"]
    except Exception:
        set_render_case_frame_base = None
    try:
        get_motion_trace_frame_count = exports[
            "krkr2_wasm_get_motion_trace_frame_count"]
    except Exception:
        get_motion_trace_frame_count = None
    try:
        get_motion_trace_ptr = exports["krkr2_wasm_get_motion_trace_ptr"]
        get_motion_trace_len = exports["krkr2_wasm_get_motion_trace_len"]
    except Exception:
        get_motion_trace_ptr = None
        get_motion_trace_len = None
    try:
        get_tick_count = exports["krkr2_wasm_get_tick_count"]
        get_tick_diag_mask = exports["krkr2_wasm_get_tick_diag_mask"]
    except Exception:
        get_tick_count = None
        get_tick_diag_mask = None
    diagnostic_export_names = (
        "krkr2_wasm_get_tick_diag_ever_mask",
        "krkr2_wasm_get_window_count",
        "krkr2_wasm_get_window_count_peak",
        "krkr2_wasm_get_continuous_event_hook_slot_count",
        "krkr2_wasm_get_continuous_event_hook_live_count",
        "krkr2_wasm_get_continuous_handler_slot_count",
        "krkr2_wasm_get_continuous_handler_live_count",
        "krkr2_wasm_get_continuous_handler_add_count",
        "krkr2_wasm_get_continuous_handler_remove_count",
        "krkr2_wasm_get_continuous_deliver_count",
        "krkr2_wasm_get_continuous_handler_invoke_count",
        "krkr2_wasm_get_continuous_handler_failure_count",
        "krkr2_wasm_get_event_disabled",
    )
    diagnostic_exports: dict[str, Any] = {}
    for export_name in diagnostic_export_names:
        try:
            diagnostic_exports[export_name] = exports[export_name]
        except Exception:
            pass
    try:
        get_continuous_tick_count = exports[
            "krkr2_wasm_get_continuous_tick_count"]
        get_continuous_tick_at = exports["krkr2_wasm_get_continuous_tick_at"]
    except Exception:
        get_continuous_tick_count = None
        get_continuous_tick_at = None

    guest_path = bootstrap.xp3_guest_path.encode("utf-8")
    config = json.dumps({
        "guestRoot": "/",
        "xp3": guest_path.decode("utf-8"),
        "headless": True,
        "bootstrap": {
            "preloadFiles": bootstrap.preload_files,
            "font": bootstrap.font_guest_path,
        },
    }).encode("utf-8")
    env_provider.startup_xp3_path = guest_path.decode("utf-8")

    init_ok = call_with_guest_bytes(
        store, memory, malloc, free, config,
        lambda ptr, length: init(store, ptr, length))
    if not init_ok:
        err = read_string(store, memory,
                          exports["krkr2_wasm_get_error_ptr"](store),
                          exports["krkr2_wasm_get_error_len"](store))
        raise RuntimeError(err or "krkr2_wasm_init returned false")

    if set_record_layer_raw_probes is not None:
        set_record_layer_raw_probes(
            store, 1 if record_layer_raw_probes else 0)
    if set_record_save_layer_visual_readback_probes is not None:
        set_record_save_layer_visual_readback_probes(
            store,
            1 if record_save_layer_visual_readback_probes else 0,
            int(save_layer_visual_readback_frame_start),
            int(save_layer_visual_readback_frame_count))
    if set_render_capture_frame_filter is not None:
        set_render_capture_frame_filter(
            store, int(capture_frame_start), int(capture_frame_count))
    if set_render_case_frame_base is not None and render_case_frame_bases:
        for case_id, frame_base in sorted(render_case_frame_bases.items()):
            encoded = str(case_id).encode("utf-8")
            call_with_guest_bytes(
                store, memory, malloc, free, encoded,
                lambda ptr, length, base=int(frame_base):
                    set_render_case_frame_base(store, ptr, length, base),
            )

    err = read_string(store, memory,
                      exports["krkr2_wasm_get_error_ptr"](store),
                      exports["krkr2_wasm_get_error_len"](store))
    if err:
        raise RuntimeError(err)

    render_events: list[dict[str, Any]] = []
    checkpoint_root = (
        bootstrap.root / RENDER_CHECKPOINT_GUEST_ROOT.lstrip("/") /
        "_execute" / "post_draw"
    )
    if record_render_step_checkpoints:
        checkpoint_root.mkdir(parents=True, exist_ok=True)

    max_ticks = int(frames)
    ticks_driven = 0
    for tick_index in range(max_ticks):
        env_provider.advance_simulation_time(SIMULATION_DELTA_MS)
        tick_ok = tick(store, SIMULATION_DELTA_MS)
        ticks_driven += 1
        if not tick_ok:
            err = read_string(store, memory,
                              exports["krkr2_wasm_get_error_ptr"](store),
                              exports["krkr2_wasm_get_error_len"](store))
            raise RuntimeError(err or "krkr2_wasm_tick returned false")
        tick_events = (
            _read_render_probe_events(store, exports, memory)
            if render_stage_out is not None else []
        )
        render_events.extend(tick_events)
        if tick_index + 1 >= frames:
            break

    render_probe_summary: dict[str, Any] | None = None
    if render_stage_out is not None:
        events = render_events + _read_render_probe_events(
            store, exports, memory)
        post_draw_marker_frames = {
            int(event["frameId"]) for event in events
            if event.get("kind") == "post_draw_marker"
            and isinstance(event.get("frameId"), int)
        }
        render_stage_out.parent.mkdir(parents=True, exist_ok=True)
        render_stage_out.write_text(
            json.dumps(events, ensure_ascii=False, allow_nan=False) + "\n",
            encoding="utf-8",
        )
        render_probe_summary = {
            "eventCount": len(events),
            "ticksDriven": ticks_driven,
            "postDrawCheckpointCount": sum(
                1 for event in events
                if event.get("kind") == "execute_image_checkpoint"
                and event.get("phase") == "post_draw"),
            "postDrawMarkerCount": len(post_draw_marker_frames),
        }
    motion_trace_frames = None
    if get_motion_trace_frame_count is not None:
        motion_trace_frames = int(get_motion_trace_frame_count(store))

    motion_trace_events: list[dict[str, Any]] = []
    if get_motion_trace_ptr is not None and get_motion_trace_len is not None:
        trace_len = int(get_motion_trace_len(store))
        trace_ptr = int(get_motion_trace_ptr(store)) if trace_len > 0 else 0
        trace_text = read_string(store, memory, trace_ptr, trace_len)
        for lineno, line in enumerate(trace_text.splitlines(), start=1):
            if not line.strip():
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError as exc:
                raise RuntimeError(
                    f"motion trace JSONL line {lineno} is invalid: {exc}"
                ) from exc
            if not isinstance(event, dict):
                raise RuntimeError(
                    f"motion trace JSONL line {lineno} is not an object")
            motion_trace_events.append(event)
    if motion_trace_out is not None:
        motion_trace_out.parent.mkdir(parents=True, exist_ok=True)
        motion_trace_out.write_text(
            json.dumps(
                motion_trace_events, ensure_ascii=False, allow_nan=False
            ) + "\n",
            encoding="utf-8",
        )

    guest_diagnostics = {
        name.removeprefix("krkr2_wasm_get_"): int(export(store))
        for name, export in diagnostic_exports.items()
    }
    if (get_continuous_tick_count is not None and
            get_continuous_tick_at is not None):
        tick_count = int(get_continuous_tick_count(store))
        retained_tick_count = min(tick_count, 512)
        guest_diagnostics["continuous_ticks_u32"] = [
            int(get_continuous_tick_at(store, index)) & 0xffffffff
            for index in range(retained_tick_count)
        ]

    return {
        "ok": True,
        "runner": "motion-playback-wasmtime-guest-trace",
        "framesDriven": ticks_driven,
        "motionTraceFrames": motion_trace_frames,
        "motionTraceEventCount": len(motion_trace_events),
        "guestTickCount": (
            int(get_tick_count(store)) if get_tick_count is not None else None
        ),
        "guestTickDiagMask": (
            int(get_tick_diag_mask(store))
            if get_tick_diag_mask is not None else None
        ),
        "guestDiagnostics": guest_diagnostics,
        "bootstrap": {
            "guestRoot": str(bootstrap.root),
            "preloadFiles": bootstrap.preload_files,
            "font": bootstrap.font_guest_path,
            "xp3": bootstrap.xp3_guest_path,
        },
        "renderProbe": render_probe_summary,
    }


def run_direct_guest_trace(wasm_path: Path, startup_xp3: Path, *,
                           specs: list[dict],
                           expected_frames: int,
                           framebuffer_dir: Path | None = None,
                           render_artifact_dir: Path | None = None,
                           record_render_step_checkpoints: bool = False,
                           checkpoint_render_only: bool = False,
                           record_layer_raw_probes: bool = False,
                           record_save_layer_visual_readback_probes: bool = False,
                           save_layer_visual_readback_frame_start: int = 0,
                           save_layer_visual_readback_frame_count: int = 1,
                           capture_window: FrameCaptureWindow,
                           manifest_startup_xp3: Path | None = None
                           ) -> tuple[list[dict], list[dict]]:
    """Drive the same full guest and consume its explicit trace exports.

    This is the architecture-neutral automated lane. Guest startup, virtual
    time, warmup, rendering, trace schema, image collection, and artifact
    normalization all execute inside the Wasm guest or through explicit host
    imports; no native JIT register inspection is involved.
    """
    with tempfile.TemporaryDirectory(
        prefix="krkr2-motion-wasmtime-direct-"
    ) as temp_dir:
        temp = Path(temp_dir)
        trace_path = temp / "trace.json"
        render_stage_path = temp / "render_stages.json"
        drive_full_guest(
            wasm_path,
            startup_xp3,
            max(expected_frames + STARTUP_WARMUP_TICKS, 1),
            motion_trace_out=trace_path,
            framebuffer_dir=framebuffer_dir,
            render_artifact_dir=render_artifact_dir,
            record_render_step_checkpoints=record_render_step_checkpoints,
            checkpoint_render_only=checkpoint_render_only,
            record_layer_raw_probes=record_layer_raw_probes,
            record_save_layer_visual_readback_probes=(
                record_save_layer_visual_readback_probes),
            save_layer_visual_readback_frame_start=(
                save_layer_visual_readback_frame_start),
            save_layer_visual_readback_frame_count=(
                save_layer_visual_readback_frame_count),
            capture_window=capture_window,
            render_stage_out=(
                render_stage_path if render_artifact_dir is not None else None),
            specs=specs,
            manifest_startup_xp3=manifest_startup_xp3,
        )
        events = json.loads(trace_path.read_text(encoding="utf-8"))
        render_events: list[dict] = []
        if render_artifact_dir is not None:
            render_events = json.loads(
                render_stage_path.read_text(encoding="utf-8"))
    if not isinstance(events, list):
        raise RuntimeError(
            f"Wasmtime direct trace root is not a list: {type(events)}")
    if not isinstance(render_events, list):
        raise RuntimeError(
            "Wasmtime direct render trace root is not a list: "
            f"{type(render_events)}")
    if expected_frames and len(events) < expected_frames:
        raise RuntimeError(
            f"Wasmtime direct trace captured only {len(events)} event(s); "
            f"expected at least {expected_frames}")
    return events, render_events


def _segment_events(events: list[dict]) -> list[dict]:
    segments: list[dict] = []
    for ev in events:
        key = ev.get("objthis") or ev.get("topPlayer")
        if not segments or segments[-1]["player"] != key:
            segments.append({"player": key, "frames": []})
        segments[-1]["frames"].append(ev)
    return segments


def partition_port_frames(
    events: list[dict],
    specs: list[dict],
    mpb,
    capture_window: FrameCaptureWindow | None = None,
) -> dict:
    specs_by_id = {s["id"]: s for s in specs}
    segment_order = mpb.segment_order_for_specs(specs_by_id)

    if capture_window is not None and capture_window.enabled:
        frames_by_id = {
            int(ev["frameId"]): ev for ev in events
            if isinstance(ev.get("frameId"), int)
        }
        results: dict[str, list[dict]] = {}
        for case in captured_case_ranges(
            specs_by_id, segment_order, capture_window):
            case_frames: list[dict] = []
            for frame_id, local_frame in zip(
                case["capturedFrameIds"], case["capturedLocalFrames"]):
                frame = frames_by_id.get(frame_id)
                if frame is None:
                    raise RuntimeError(
                        f"missing Wasmtime frameId {frame_id} for "
                        f"{case['caseId']}"
                    )
                case_frames.append(mpb.normalize_frame(frame, local_frame))
            results[str(case["caseId"])] = case_frames
        return results

    segments = _segment_events(events)
    ordered_specs = [specs_by_id[spec_id] for spec_id in segment_order]
    try:
        fixture_segments = select_expected_segments(segments, ordered_specs)
    except ValueError as exc:
        raise RuntimeError(
            f"failed to identify Wasmtime fixture segments: {exc}"
        ) from exc

    results: dict[str, list[dict]] = {}
    for i, spec_id in enumerate(segment_order):
        spec = specs_by_id[spec_id]
        wanted = int(spec["frames"])
        frames = fixture_segments[i]["frames"]
        if len(frames) != wanted:
            raise RuntimeError(
                f"Wasmtime segment {i} ({spec_id}) has "
                f"{len(frames)} frames; spec requires exactly {wanted}."
            )
        results[spec_id] = [
            mpb.normalize_frame(fr, fi)
            for fi, fr in enumerate(frames)
        ]
    return results


def render_case_segments(
    events: list[dict],
    specs: list[dict],
    mpb,
    capture_window: FrameCaptureWindow | None = None,
) -> list[dict]:
    specs_by_id = {s["id"]: s for s in specs}
    segment_order = mpb.segment_order_for_specs(specs_by_id)
    if capture_window is not None and capture_window.enabled:
        frames_by_id = {
            int(ev["frameId"]): ev for ev in events
            if isinstance(ev.get("frameId"), int)
        }
        out: list[dict[str, Any]] = []
        for case in captured_case_ranges(
            specs_by_id, segment_order, capture_window):
            selected = [
                frames_by_id[frame_id]
                for frame_id in case["capturedFrameIds"]
                if frame_id in frames_by_id
            ]
            if len(selected) != len(case["capturedFrameIds"]):
                missing = [
                    frame_id for frame_id in case["capturedFrameIds"]
                    if frame_id not in frames_by_id
                ]
                raise RuntimeError(
                    f"missing Wasmtime trace frame(s) for {case['caseId']}: "
                    f"{missing[:8]}"
                )
            out.append({
                "caseId": str(case["caseId"]),
                "player": selected[0].get("topPlayer") if selected else None,
                "firstFrameId": int(case["capturedFrameIdRange"][0]),
                "lastFrameId": int(case["capturedFrameIdRange"][1]) - 1,
                "caseFrameIdBase": int(case["fullFrameIdRange"][0]),
                "fullFrameIdRange": case["fullFrameIdRange"],
                "capturedFrameIdRange": case["capturedFrameIdRange"],
                "capturedLocalFrames": case["capturedLocalFrames"],
                "frames": selected,
            })
        return out

    segments = _segment_events(events)
    ordered_specs = [
        specs_by_id[spec_id]
        for spec_id in segment_order
        if spec_id in specs_by_id
    ]
    try:
        fixture_segments = select_expected_segments(segments, ordered_specs)
    except ValueError as exc:
        raise RuntimeError(
            f"failed to identify Wasmtime render fixture segments: {exc}"
        ) from exc
    out: list[dict[str, Any]] = []
    for i, spec_id in enumerate(segment_order):
        spec = specs_by_id.get(spec_id)
        if spec is None:
            continue
        wanted = int(spec["frames"])
        frames = fixture_segments[i]["frames"]
        if len(frames) != wanted:
            raise RuntimeError(
                f"Wasmtime segment {i} ({spec_id}) has {len(frames)} frame(s); "
                f"render stage capture requires exactly {wanted}.")
        selected = frames
        frame_ids = [int(frame["frameId"]) for frame in selected]
        out.append({
            "caseId": spec_id,
            "player": fixture_segments[i].get("player"),
            "firstFrameId": min(frame_ids),
            "lastFrameId": max(frame_ids),
            "caseFrameIdBase": min(frame_ids),
            "fullFrameIdRange": [min(frame_ids), max(frame_ids) + 1],
            "capturedFrameIdRange": [min(frame_ids), max(frame_ids) + 1],
            "capturedLocalFrames": list(range(len(selected))),
            "frames": selected,
        })
    return out


def _render_case_frame_bases(
    specs: list[dict],
    mpb,
    capture_window: FrameCaptureWindow,
) -> dict[str, int]:
    specs_by_id = {str(spec["id"]): spec for spec in specs}
    segment_order = mpb.segment_order_for_specs(specs_by_id)
    return {
        str(case["caseId"]): int(case["fullFrameIdRange"][0])
        for case in captured_case_ranges(
            specs_by_id, segment_order, capture_window)
    }


def _assign_render_case(ev: dict[str, Any],
                        case_segments: list[dict[str, Any]]) -> str | None:
    frame_id = ev.get("frameId")
    if not isinstance(frame_id, int):
        return None
    for segment in case_segments:
        if int(segment["firstFrameId"]) <= frame_id <= int(segment["lastFrameId"]):
            return str(segment["caseId"])
    return None


def _render_stage_summary(events: list[dict[str, Any]],
                          trace_frame_count: int) -> dict[str, Any]:
    frame_ids = [
        int(ev["frameId"]) for ev in events if isinstance(ev.get("frameId"), int)
    ]
    seqs = [int(ev["seq"]) for ev in events if isinstance(ev.get("seq"), int)]
    kinds = Counter(str(ev.get("kind")) for ev in events)
    summary: dict[str, Any] = {
        "eventCount": len(events),
        "kindCounts": dict(sorted(kinds.items())),
        "traceFrameCount": trace_frame_count,
        "framesWithEvents": len(set(frame_ids)),
    }
    if frame_ids:
        summary["eventFrameIdRange"] = [min(frame_ids), max(frame_ids)]
    if seqs:
        summary["eventSeqRange"] = [min(seqs), max(seqs)]
    return summary


def _merge_case_lists(
    existing: list[dict[str, Any]],
    incoming: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    order: list[str] = []
    by_id: dict[str, dict[str, Any]] = {}
    for item in [*existing, *incoming]:
        case_id = str(item.get("caseId"))
        if case_id not in by_id:
            order.append(case_id)
        by_id[case_id] = item
    return [by_id[case_id] for case_id in order]


def _count_manifest_images(cases: list[dict[str, Any]]) -> int:
    total = 0
    for case in cases:
        phases = case.get("phases", {})
        if not isinstance(phases, dict):
            continue
        for images in phases.values():
            if isinstance(images, list):
                total += len(images)
    return total


def _count_manifest_events(artifact_dir: Path,
                           cases: list[dict[str, Any]]) -> int:
    total = 0
    for case in cases:
        event_files = case.get("eventFiles", {})
        if not isinstance(event_files, dict):
            continue
        for rel in event_files.values():
            if not isinstance(rel, str):
                continue
            path = artifact_dir / rel
            if not path.exists():
                continue
            data = json.loads(path.read_text(encoding="utf-8"))
            events = data.get("events", [])
            if isinstance(events, list):
                total += len(events)
    return total


def _merge_unique(existing: list[Any], incoming: list[Any]) -> list[Any]:
    out: list[Any] = []
    for value in [*existing, *incoming]:
        if value is not None and value not in out:
            out.append(value)
    return out


def _merge_render_stage_manifest(
    artifact_dir: Path,
    manifest: dict[str, Any],
) -> dict[str, Any]:
    target = artifact_dir / "manifest.json"
    if not target.exists():
        return manifest

    existing = json.loads(target.read_text(encoding="utf-8"))
    merged = dict(existing)
    merged["generatedAt"] = manifest.get("generatedAt")
    merged["stages"] = _merge_unique(
        list(existing.get("stages", [])),
        list(manifest.get("stages", [])),
    )
    merged["startupXp3s"] = _merge_unique(
        list(existing.get("startupXp3s", [existing.get("startupXp3")])),
        [manifest.get("startupXp3")],
    )
    merged["captureXp3s"] = _merge_unique(
        list(existing.get("captureXp3s", [existing.get("captureXp3")])),
        [manifest.get("captureXp3")],
    )

    cases = _merge_case_lists(
        list(existing.get("cases", [])),
        list(manifest.get("cases", [])),
    )
    merged["cases"] = cases

    fixture = dict(existing.get("fixture", {}))
    incoming_fixture = manifest.get("fixture", {})
    if isinstance(incoming_fixture, dict):
        fixture["segmentOrder"] = _merge_unique(
            list(fixture.get("segmentOrder", [])),
            list(incoming_fixture.get("segmentOrder", [])),
        )
    merged["fixture"] = fixture

    existing_images = existing.get("images", {})
    incoming_images = manifest.get("images", {})
    if isinstance(existing_images, dict) and isinstance(incoming_images, dict):
        image_cases = _merge_case_lists(
            list(existing_images.get("cases", [])),
            list(incoming_images.get("cases", [])),
        )
        image_manifest = dict(existing_images)
        image_manifest["captureSurfaces"] = _merge_unique(
            list(existing_images.get("captureSurfaces", [])),
            list(incoming_images.get("captureSurfaces", [])),
        )
        image_manifest["cases"] = image_cases
        image_manifest["summary"] = {
            **dict(existing_images.get("summary", {})),
            "caseCount": len(image_cases),
            "imageCount": _count_manifest_images(image_cases),
        }
        merged["images"] = image_manifest

    merged["summary"] = {
        **dict(existing.get("summary", {})),
        "caseCount": len(cases),
        "traceFlattenFrameCount": sum(int(case.get("frames", 0))
                                      for case in cases),
        "renderEventCount": _count_manifest_events(artifact_dir, cases),
        "imageCount": (
            merged.get("images", {}).get("summary", {}).get("imageCount", 0)
            if isinstance(merged.get("images"), dict) else 0
        ),
    }
    return merged


def _split_render_events_by_stage_case(
    events: list[dict[str, Any]],
    case_segments: list[dict[str, Any]],
) -> dict[str, dict[str, list[dict[str, Any]]]]:
    out: dict[str, dict[str, list[dict[str, Any]]]] = {}
    for ev in events:
        stage = str(ev.get("stage") or "")
        if stage not in RENDER_STAGES or stage == "layer_save":
            continue
        case_id = _assign_render_case(ev, case_segments)
        if case_id is None:
            continue
        cloned = dict(ev)
        cloned["caseId"] = case_id
        out.setdefault(stage, {}).setdefault(case_id, []).append(cloned)
    return out


def _layer_save_events_for_case(case_images: dict[str, Any],
                                case_segment: dict[str, Any]) -> list[dict[str, Any]]:
    events: list[dict[str, Any]] = []
    case_id = str(case_images["caseId"])
    first_frame_id = int(case_segment.get(
        "caseFrameIdBase", case_segment["firstFrameId"]))
    seq = 0
    for phase in RENDER_CAPTURE_SURFACES:
        for image in case_images.get("phases", {}).get(phase, []):
            local_frame = int(image["frame"])
            events.append({
                "schema": RENDER_EVENT_SCHEMA,
                "source": RENDER_SOURCE,
                "stage": "layer_save",
                "kind": "save_layer_image",
                "samplePoint": f"startup.tjs.{phase}",
                "caseId": case_id,
                "frameId": first_frame_id + local_frame,
                "frame": local_frame,
                "seq": seq,
                "phase": phase,
            "path": image["path"],
            "width": image["width"],
            "height": image["height"],
            "bytes": image["bytes"],
            **({"rgbaSha256": image["rgbaSha256"]}
               if "rgbaSha256" in image else {}),
                "diagnostics": {
                    "synthetic": True,
                    "source": "wasmtime-render-png-manifest",
                },
            })
            seq += 1
    return events


def _phase_images_by_frame(
    case_images: dict[str, Any],
    phase: str,
) -> dict[int, dict[str, Any]]:
    out: dict[int, dict[str, Any]] = {}
    for image in case_images.get("phases", {}).get(phase, []):
        frame = image.get("frame")
        if isinstance(frame, int):
            out[frame] = dict(image)
    return out


def _add_image_manifest_error(
    event: dict[str, Any],
    message: str,
) -> None:
    diagnostics = dict(event.get("diagnostics") or {})
    existing = diagnostics.get("imageManifestError")
    if existing:
        diagnostics["imageManifestError"] = f"{existing}; {message}"
    else:
        diagnostics["imageManifestError"] = message
    event["diagnostics"] = diagnostics


def _enrich_draw_dispatch_events_for_case(
    events: list[dict[str, Any]],
    case_segment: dict[str, Any],
    case_images: dict[str, Any],
) -> list[dict[str, Any]]:
    first_frame_id = int(case_segment.get(
        "caseFrameIdBase", case_segment["firstFrameId"]))
    last_frame_id = int(case_segment["lastFrameId"])
    initial_image = _phase_images_by_frame(case_images, "initial").get(0)
    post_by_frame = _phase_images_by_frame(case_images, "post_draw")
    enriched: list[dict[str, Any]] = []

    for source_event in events:
        event = dict(source_event)
        frame_id = event.get("frameId")
        if not isinstance(frame_id, int):
            _add_image_manifest_error(event, "event has no integer frameId")
            enriched.append(event)
            continue
        if frame_id < first_frame_id or frame_id > last_frame_id:
            _add_image_manifest_error(
                event,
                f"frameId {frame_id} outside case segment "
                f"{first_frame_id}..{last_frame_id}",
            )
            enriched.append(event)
            continue

        local_frame = frame_id - first_frame_id
        post_draw = post_by_frame.get(local_frame)

        kind = str(event.get("kind") or "")
        if kind == "draw_leave":
            event["initialImage"] = initial_image
            event["postDrawImage"] = post_draw
            if initial_image is None:
                _add_image_manifest_error(
                    event, "missing initial image")
            if post_draw is None:
                _add_image_manifest_error(
                    event, f"missing post_draw image for frame {local_frame}")

        enriched.append(event)

    return enriched


def _semantic_render_item(item: dict[str, Any]) -> dict[str, Any]:
    flags = item.get("flags")
    clip_valid = (
        bool(flags.get("clipValid21"))
        if isinstance(flags, dict) else False
    )
    clip_rect = item.get("clipRect") if clip_valid else [0, 0, 0, 0]
    build_clip_rect = (
        item.get("buildClipRect", item.get("clipRect"))
        if clip_valid else [0, 0, 0, 0]
    )
    return {
        "index": item.get("index"),
        "nodeIndex": item.get("nodeIndex"),
        "sourceKey": item.get("sourceKey"),
        "flags": flags,
        "layerIds": item.get("layerIds"),
        "sortKey64": item.get("sortKey64"),
        "paintBox": item.get("paintBox"),
        "clipRect": clip_rect,
        "buildClipRect": build_clip_rect,
        "dirtyRect": item.get("dirtyRect"),
        "viewportRect": item.get("viewportRect"),
        "sourceGate232": item.get("sourceGate232"),
        "stencilType244": item.get("stencilType244"),
        "parentItemIndex": item.get("parentItemIndex"),
        "parentItem264": item.get("parentItem264"),
        "childItemCount": item.get("childItemCount"),
        "meshType280": item.get("meshType280"),
        "leafLayerVariantTag": item.get("leafLayerVariantTag"),
        "composedLayerVariantTag": item.get("composedLayerVariantTag"),
        "leafLayerVariantTag320": item.get("leafLayerVariantTag320"),
        "composedLayerVariantTag340": item.get("composedLayerVariantTag340"),
        "leafBuilt": item.get("leafBuilt"),
        "composedBuilt": item.get("composedBuilt"),
        "executedDirect": item.get("executedDirect"),
    }


def _has_valid_clip_rect(item: dict[str, Any]) -> bool:
    flags = item.get("flags")
    flag16 = bool(flags.get("flag16")) if isinstance(flags, dict) else False
    flag17 = bool(flags.get("flag17")) if isinstance(flags, dict) else False
    if flag16 or flag17:
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
    rect = (
        item.get("buildClipRect", item.get("clipRect"))
        if clip_valid else item.get("paintBox")
    )
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


def _build_flow_summary(event: dict[str, Any]) -> dict[str, Any]:
    if isinstance(event.get("buildFlow"), dict):
        return dict(event["buildFlow"])
    main_items = event.get("mainListSemanticItems")
    if isinstance(main_items, list):
        main_semantic_items = [
            _semantic_render_item(item)
            for item in main_items if isinstance(item, dict)
        ]
        aux_items = event.get("auxListSemanticItems")
        aux_semantic_items = [
            _semantic_render_item(item)
            for item in aux_items if isinstance(item, dict)
        ] if isinstance(aux_items, list) else []
        return {
            "inputItemCount": event.get("inputItemCount"),
            "builtItemCount": event.get("builtItemCount",
                                        len(main_semantic_items)),
            "validDrawableItemCount": event.get("validDrawableItemCount"),
            "leafBuiltCount": event.get("leafBuiltCount"),
            "composedBuiltCount": event.get("composedBuiltCount"),
            "mainListSemanticItems": main_semantic_items,
            "auxListSemanticItems": aux_semantic_items,
            "items": main_semantic_items,
        }
    render_lists = event.get("renderLists")
    main_list = (
        render_lists.get("mainList")
        if isinstance(render_lists, dict) else None
    )
    aux_list = (
        render_lists.get("auxList")
        if isinstance(render_lists, dict) else None
    )
    items = main_list.get("items") if isinstance(main_list, dict) else []
    aux_items = aux_list.get("items") if isinstance(aux_list, dict) else []
    semantic_items = [
        _semantic_render_item(item)
        for item in items if isinstance(item, dict)
    ]
    aux_semantic_items = [
        _semantic_render_item(item)
        for item in aux_items if isinstance(item, dict)
    ]
    return {
        "inputItemCount": main_list.get("count")
        if isinstance(main_list, dict) else None,
        "builtItemCount": len(semantic_items),
        "validDrawableItemCount": sum(
            1 for item in semantic_items if _has_valid_clip_rect(item)),
        "leafBuiltCount": None,
        "composedBuiltCount": None,
        "mainListSemanticItems": semantic_items,
        "auxListSemanticItems": aux_semantic_items,
        "items": semantic_items,
    }


def _enrich_render_commands_events_for_case(
    events: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    enriched: list[dict[str, Any]] = []
    for source_event in events:
        event = dict(source_event)
        if str(event.get("kind") or "").startswith("build_commands"):
            event["buildFlow"] = _build_flow_summary(event)
        enriched.append(event)
    return enriched


def _enrich_render_execute_events_for_case(
    events: list[dict[str, Any]],
    case_segment: dict[str, Any],
    case_images: dict[str, Any],
) -> list[dict[str, Any]]:
    first_frame_id = int(case_segment.get(
        "caseFrameIdBase", case_segment["firstFrameId"]))
    last_frame_id = int(case_segment["lastFrameId"])
    pre_by_frame = _phase_images_by_frame(case_images, "execute_pre")
    post_by_frame = _phase_images_by_frame(case_images, "execute_post")
    update_pre_by_frame = _phase_images_by_frame(
        case_images, "updateLayerAfterDraw_pre")
    update_post_by_frame = _phase_images_by_frame(
        case_images, "updateLayerAfterDraw_post")
    post_draw_by_frame = _phase_images_by_frame(case_images, "post_draw")
    enriched: list[dict[str, Any]] = []

    for source_event in events:
        event = dict(source_event)
        frame_id = event.get("frameId")
        if not isinstance(frame_id, int):
            _add_image_manifest_error(event, "event has no integer frameId")
            enriched.append(event)
            continue
        if frame_id < first_frame_id or frame_id > last_frame_id:
            _add_image_manifest_error(
                event,
                f"frameId {frame_id} outside case segment "
                f"{first_frame_id}..{last_frame_id}",
            )
            enriched.append(event)
            continue

        local_frame = frame_id - first_frame_id
        execute_pre = pre_by_frame.get(local_frame)
        execute_post = post_by_frame.get(local_frame)
        update_pre = update_pre_by_frame.get(local_frame)
        update_post = update_post_by_frame.get(local_frame)
        post_draw = post_draw_by_frame.get(local_frame)
        kind = str(event.get("kind") or "")
        if kind == "execute_enter":
            event["executePreImage"] = execute_pre
            if execute_pre is None:
                _add_image_manifest_error(
                    event,
                    f"missing execute_pre image for frame {local_frame}",
                )
        elif kind == "execute_leave":
            event["executePreImage"] = execute_pre
            event["executePostImage"] = execute_post
            event["updateLayerAfterDrawPreImage"] = update_pre
            event["updateLayerAfterDrawPostImage"] = update_post
            event["postDrawImage"] = post_draw
            if execute_pre is None:
                _add_image_manifest_error(
                    event,
                    f"missing execute_pre image for frame {local_frame}",
                )
            if execute_post is None:
                _add_image_manifest_error(
                    event,
                    f"missing execute_post image for frame {local_frame}",
                )
            if update_pre is None:
                _add_image_manifest_error(
                    event,
                    "missing updateLayerAfterDraw_pre image for frame "
                    f"{local_frame}",
                )
            if update_post is None:
                _add_image_manifest_error(
                    event,
                    "missing updateLayerAfterDraw_post image for frame "
                    f"{local_frame}",
                )
            if post_draw is None:
                _add_image_manifest_error(
                    event,
                    f"missing post_draw image for frame {local_frame}",
                )
        enriched.append(event)

    return enriched


def write_render_stage_artifacts(
    *,
    artifact_dir: Path,
    specs: list[dict[str, Any]],
    events: list[dict[str, Any]],
    case_segments: list[dict[str, Any]],
    image_manifest: dict[str, Any],
    wasm_path: Path,
    startup_xp3: Path,
    capture_xp3: Path,
    capture_window: FrameCaptureWindow,
) -> Path:
    events_by_stage_case = _split_render_events_by_stage_case(
        events, case_segments)
    case_segment_by_id = {
        str(segment["caseId"]): segment for segment in case_segments
    }
    image_case_by_id = {
        str(case["caseId"]): case for case in image_manifest.get("cases", [])
    }
    events_root = artifact_dir / "events"
    total_event_count = 0
    specs_by_id = {str(spec["id"]): spec for spec in specs}

    for stage in RENDER_STAGES:
        for spec_id in [sid for sid in case_segment_by_id if sid in specs_by_id]:
            case_segment = case_segment_by_id[spec_id]
            if stage == "layer_save":
                stage_events = _layer_save_events_for_case(
                    image_case_by_id.get(spec_id, {
                        "caseId": spec_id,
                        "phases": {},
                    }),
                    case_segment,
                )
            else:
                stage_events = (
                    events_by_stage_case.get(stage, {}).get(spec_id, [])
                )
                if stage == "draw_dispatch":
                    stage_events = _enrich_draw_dispatch_events_for_case(
                        stage_events,
                        case_segment,
                        image_case_by_id.get(spec_id, {
                            "caseId": spec_id,
                            "phases": {},
                        }),
                    )
                elif stage == "render_commands":
                    stage_events = _enrich_render_commands_events_for_case(
                        stage_events)
                elif stage == "render_execute":
                    stage_events = _enrich_render_execute_events_for_case(
                        stage_events,
                        case_segment,
                        image_case_by_id.get(spec_id, {
                            "caseId": spec_id,
                            "phases": {},
                        }),
                    )
            total_event_count += len(stage_events)
            payload = {
                "schema": RENDER_SCHEMA,
                "source": RENDER_SOURCE,
                "stage": stage,
                "caseId": spec_id,
                "events": stage_events,
                "summary": _render_stage_summary(
                    stage_events, len(case_segment["frames"])),
            }
            target = events_root / stage / f"{spec_id}.wasmtime.json"
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(
                json.dumps(payload, indent=2, ensure_ascii=True,
                           allow_nan=False) + "\n",
                encoding="utf-8",
            )

    manifest = {
        "schema": RENDER_SCHEMA,
        "source": RENDER_SOURCE,
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "localRoot": str(artifact_dir),
        "wasm": str(wasm_path),
        "startupXp3": str(startup_xp3),
        "captureXp3": str(capture_xp3),
        "fixture": {
            "guestCaptureRoot": image_manifest.get("guestCaptureRoot"),
            "window": {"width": 1920, "height": 1080},
            "simulationFps": SIMULATION_FPS,
            "deltaMs": SIMULATION_DELTA_MS,
            "segmentOrder": [str(segment["caseId"]) for segment in case_segments],
        },
        "stages": list(RENDER_STAGES),
        "eventsRoot": "events",
        "imagesRoot": "images",
        "images": image_manifest,
        "summary": {
            "caseCount": len(case_segments),
            "traceFlattenFrameCount": sum(
                len(segment["frames"]) for segment in case_segments),
            "renderEventCount": total_event_count,
            "imageCount": image_manifest.get("summary", {}).get("imageCount", 0),
        },
        **capture_window.manifest_fields(),
        "cases": [
            {
                "caseId": segment["caseId"],
                "frames": len(segment["frames"]),
                "frameIdRange": [
                    segment["firstFrameId"], segment["lastFrameId"]],
                "fullFrameIdRange": segment.get("fullFrameIdRange"),
                "capturedFrameIdRange": segment.get("capturedFrameIdRange"),
                "capturedLocalFrames": segment.get("capturedLocalFrames", []),
                "eventFiles": {
                    stage: str(
                        (Path("events") / stage /
                         f"{segment['caseId']}.wasmtime.json").as_posix()
                    )
                    for stage in RENDER_STAGES
                },
            }
            for segment in case_segments
        ],
    }
    target = artifact_dir / "manifest.json"
    manifest = _merge_render_stage_manifest(artifact_dir, manifest)
    target.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=True,
                   allow_nan=False) + "\n",
        encoding="utf-8",
    )
    return target


def write_port_traces(port_frames_by_id: dict[str, list],
                      output_dir: Path) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for spec_id, frames in sorted(port_frames_by_id.items()):
        target = output_dir / f"{spec_id}.port.json"
        target.write_text(
            json.dumps(frames, indent=2, sort_keys=True,
                       allow_nan=False) + "\n",
            encoding="utf-8",
        )
        print(f"[record] {spec_id}: wrote {len(frames)} Wasmtime frames "
              f"to {target}")


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    spec_dir = Path(args.spec_dir)
    trace_dir = Path(args.trace_dir)
    wasm_path = Path(args.wasm)
    startup_xp3 = Path(args.startup_xp3)
    framebuffer_dir = (
        Path(args.framebuffer_dir) if args.framebuffer_dir is not None
        else default_framebuffer_dir()
    ) if args.record_framebuffer else None
    render_artifact_dir = (
        Path(args.render_artifact_dir) if args.render_artifact_dir is not None
        else default_render_artifact_dir()
    ) if args.record_render_stages else None

    if framebuffer_dir is not None and render_artifact_dir is not None:
        print("--record-framebuffer and --record-render-stages currently use "
              "separate artifact collectors; run them separately",
              file=sys.stderr)
        return 2
    if args.record_render_step_checkpoints and render_artifact_dir is None:
        print("--record-render-step-checkpoints requires "
              "--record-render-stages", file=sys.stderr)
        return 2
    if args.checkpoint_render_only and not args.record_render_step_checkpoints:
        print("--checkpoint-render-only requires "
              "--record-render-step-checkpoints", file=sys.stderr)
        return 2
    if args.record_layer_raw_probes and render_artifact_dir is None:
        print("--record-layer-raw-probes requires --record-render-stages",
              file=sys.stderr)
        return 2
    if args.record_save_layer_visual_readback_probes and render_artifact_dir is None:
        print("--record-save-layer-visual-readback-probes requires "
              "--record-render-stages", file=sys.stderr)
        return 2

    if not spec_dir.exists():
        print(f"spec dir not found: {spec_dir}", file=sys.stderr)
        return 2

    try:
        specs = filter_specs(load_specs(spec_dir), args.case)
        validate_simulation_contract(specs)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2
    if not specs:
        print(f"no specs in {spec_dir}", file=sys.stderr)
        return 0

    from oracle_runner.adapters import motion_playback as mpb

    total_frames = sum(int(spec["frames"]) for spec in specs)
    try:
        capture_window = frame_capture_window_from_args(args, total_frames)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    try:
        expected_frames = capture_window.driven_frames
        trace_startup_xp3 = startup_xp3
        trace_kwargs = {
            "specs": specs,
            "expected_frames": expected_frames,
            "framebuffer_dir": framebuffer_dir,
            "render_artifact_dir": render_artifact_dir,
            "record_render_step_checkpoints": (
                args.record_render_step_checkpoints),
            "checkpoint_render_only": args.checkpoint_render_only,
            "record_layer_raw_probes": args.record_layer_raw_probes,
            "record_save_layer_visual_readback_probes": (
                args.record_save_layer_visual_readback_probes),
            "save_layer_visual_readback_frame_start": (
                args.save_layer_visual_readback_frame_start),
            "save_layer_visual_readback_frame_count": (
                args.save_layer_visual_readback_frame_count),
            "capture_window": capture_window,
            "manifest_startup_xp3": startup_xp3
            if (framebuffer_dir is not None or render_artifact_dir is not None)
            else None,
        }
        port_events, render_events = run_direct_guest_trace(
            wasm_path, trace_startup_xp3, **trace_kwargs)
        port_frames_by_id = partition_port_frames(
            port_events, specs, mpb, capture_window)
        segment_order = mpb.segment_order_for_specs(specs)
        captured_cases_by_id = {
            str(case["caseId"]): case for case in captured_case_ranges(
                {spec["id"]: spec for spec in specs},
                segment_order,
                capture_window,
            )
        }
        render_manifest: Path | None = None
        if render_artifact_dir is not None:
            image_manifest_path = render_artifact_dir / "image_manifest.json"
            if not image_manifest_path.exists():
                raise RuntimeError(
                    f"Wasmtime render image manifest missing: {image_manifest_path}")
            image_manifest = json.loads(
                image_manifest_path.read_text(encoding="utf-8"))
            render_manifest = write_render_stage_artifacts(
                artifact_dir=render_artifact_dir,
                specs=specs,
                events=render_events,
                case_segments=render_case_segments(
                    port_events, specs, mpb, capture_window),
                image_manifest=image_manifest,
                wasm_path=wasm_path,
                startup_xp3=startup_xp3,
                capture_xp3=trace_startup_xp3,
                capture_window=capture_window,
            )
            image_manifest_path.unlink(missing_ok=True)
        if args.write_port_traces is not None:
            write_port_traces(port_frames_by_id, args.write_port_traces)
    except Exception as exc:
        print(f"FAIL: Wasmtime guest trace error: {exc}", file=sys.stderr)
        return 1

    failures = 0
    if args.skip_golden_diff:
        for spec in specs:
            captured_case = captured_cases_by_id.get(str(spec["id"]))
            if capture_window.enabled and captured_case is None:
                print(f"SKIP: {spec['id']} outside capture window")
                continue
            port_frames = port_frames_by_id.get(spec["id"])
            if port_frames is None:
                print(f"FAIL: {spec['id']}: no Wasmtime frames captured",
                      file=sys.stderr)
                failures += 1
                continue
            print(f"TRACE: {spec['id']} ({len(port_frames)} frames)")
        return 1 if failures else 0

    for spec in specs:
        captured_case = captured_cases_by_id.get(str(spec["id"]))
        if capture_window.enabled and captured_case is None:
            print(f"SKIP: {spec['id']} outside capture window")
            continue
        oracle_path = trace_dir / f"{spec['id']}.oracle.json"
        if not oracle_path.exists():
            msg = f"no oracle for {spec['id']} at {oracle_path}"
            if args.strict_missing_trace:
                print(f"FAIL: {msg}", file=sys.stderr)
                failures += 1
            else:
                print(f"SKIP: {msg}")
            continue
        oracle_frames = json.loads(oracle_path.read_text(encoding="utf-8"))
        if captured_case is not None:
            oracle_frames = [
                oracle_frames[int(local_frame)]
                for local_frame in captured_case["capturedLocalFrames"]
            ]

        port_frames = port_frames_by_id.get(spec["id"])
        if port_frames is None:
            print(f"FAIL: {spec['id']}: no Wasmtime frames captured",
                  file=sys.stderr)
            failures += 1
            continue

        result = mpb.run_case(None, spec,
                              port_frames=port_frames,
                              oracle_frames=oracle_frames,
                              structural_only=args.only_structural)
        if result["status"] == "ok":
            print(f"PASS: {spec['id']} ({len(port_frames)} frames)")
        else:
            print(f"FAIL: {spec['id']}: {result['status']} "
                  f"({len(result['mismatches'])} mismatches)")
            for mismatch in result["mismatches"][:10]:
                print(f"  {mismatch}")
            if len(result["mismatches"]) > 10:
                print(f"  ... +{len(result['mismatches']) - 10} more")
            failures += 1
    if framebuffer_dir is not None:
        manifest = framebuffer_dir / "manifest.json"
        if manifest.exists():
            print(f"[record] framebuffer manifest: {manifest}")
    if render_artifact_dir is not None:
        manifest = render_artifact_dir / "manifest.json"
        if manifest.exists():
            print(f"[record] render stage manifest: {manifest}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
