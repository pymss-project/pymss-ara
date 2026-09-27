"""PyMSS ARA Plugin worker process.

Talks to the C++ plugin using a framed stdin/stdout control protocol and named
shared memory for separation audio. All human-readable logging goes to stderr;
only control frames go to stdout.

Frame layout (little-endian, identical in both directions)::

    uint32  header_len   # length of the JSON header in bytes
    uint32  body_len     # length of the binary body in bytes
    bytes   header_len   # UTF-8 JSON object
    bytes   body_len     # reserved; separation audio uses shared memory

Request header fields:
    id      int    request id (mirrored in responses/progress)
    cmd     str    "ping" | "check_pymss" | "list_models" | "model_info"
                   | "separate" | "cancel" | "shutdown"

Response / event header fields:
    id      int
    type    str    "result" | "progress" | "error"

The "separate" request and result describe planar float32 named mappings. The
mapping owner keeps each region alive until the peer has completed its access.
"""

from __future__ import annotations

import json
import math
import mmap
import os
import secrets
import struct
import sys
import threading
import traceback
from queue import Queue

import numpy as np


# -----------------------------------------------------------------------------
# Shared-memory protocol. Keep these values in sync with SharedMemoryRegion.h.
# -----------------------------------------------------------------------------

CONTROL_PROTOCOL_VERSION = 2
SHARED_MEMORY_TRANSPORT = "shared_memory_v1"
SHM_MAGIC = 0x4D48534D
SHM_PROTOCOL_VERSION = 1
SHM_HEADER_BYTES = 64
SHM_MAX_BYTES = 8 * 1024 * 1024 * 1024
SHM_MAX_RESULT_BYTES = 2 * 1024 * 1024 * 1024
SHM_MAX_CHANNELS = 64
SHM_MAX_STEMS = 64
SHM_ROLE_INPUT = 1
SHM_ROLE_OUTPUT = 2
SHM_STATE_WRITING = 1
SHM_STATE_READY = 2
_SHM_HEADER = struct.Struct("<IIIIQQQII16x")

_MAX_CONTROL_HEADER_BYTES = 8 * 1024 * 1024
_MAX_CONTROL_BODY_BYTES = 1024 * 1024


def _parse_nonnegative_int(value, field: str) -> int:
    if isinstance(value, bool):
        raise ValueError(f"{field} must be an integer")
    try:
        parsed = int(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{field} must be an integer") from exc
    if parsed < 0:
        raise ValueError(f"{field} must not be negative")
    return parsed


def _validate_mapping_name(name: str) -> str:
    if not isinstance(name, str) or not name.startswith("Local\\PyMSS_") or len(name) > 240:
        raise ValueError("invalid shared memory name")
    return name


def _validate_mapping_size(size: int) -> int:
    size = _parse_nonnegative_int(size, "mapping_bytes")
    if size < SHM_HEADER_BYTES or size > SHM_MAX_BYTES:
        raise ValueError("shared memory size is outside the supported range")
    return size


def _write_mapping_header(shared: mmap.mmap, *, role: int, request_id: int,
                          payload_bytes: int, frames: int, channels: int,
                          state: int) -> None:
    if len(shared) < SHM_HEADER_BYTES or payload_bytes < 0 or payload_bytes > len(shared) - SHM_HEADER_BYTES:
        raise ValueError("shared memory header does not fit the mapping")
    shared[:SHM_HEADER_BYTES] = b"\0" * SHM_HEADER_BYTES
    _SHM_HEADER.pack_into(
        shared,
        0,
        SHM_MAGIC,
        SHM_PROTOCOL_VERSION,
        SHM_HEADER_BYTES,
        role,
        request_id,
        payload_bytes,
        frames,
        channels,
        state,
    )


def _read_mapping_header(shared: mmap.mmap, *, expected_role: int,
                         expected_request_id: int) -> dict:
    if len(shared) < SHM_HEADER_BYTES:
        raise ValueError("shared memory mapping is smaller than its header")
    magic, version, header_bytes, role, request_id, payload_bytes, frames, channels, state = (
        _SHM_HEADER.unpack_from(shared, 0)
    )
    if magic != SHM_MAGIC or version != SHM_PROTOCOL_VERSION or header_bytes != SHM_HEADER_BYTES:
        raise ValueError("shared memory protocol header is invalid")
    if role != expected_role or request_id != expected_request_id:
        raise ValueError("shared memory mapping does not match the request")
    if state != SHM_STATE_READY:
        raise ValueError("shared memory mapping is not ready")
    if payload_bytes > len(shared) - SHM_HEADER_BYTES:
        raise ValueError("shared memory payload exceeds the mapping")
    return {
        "payload_bytes": payload_bytes,
        "frames": frames,
        "channels": channels,
    }


class SharedMemoryRegion:
    def __init__(self, name: str, size: int, access: int):
        self.name = _validate_mapping_name(name)
        self.size = _validate_mapping_size(size)
        self.mapping = mmap.mmap(-1, self.size, tagname=self.name, access=access)

    @classmethod
    def create(cls, name: str, size: int) -> "SharedMemoryRegion":
        return cls(name, size, mmap.ACCESS_WRITE)

    @classmethod
    def open_copy_on_write(cls, name: str, size: int) -> "SharedMemoryRegion":
        return cls(name, size, mmap.ACCESS_COPY)

    def close(self) -> None:
        if self.mapping is not None:
            self.mapping.close()
            self.mapping = None

    def __enter__(self) -> "SharedMemoryRegion":
        return self

    def __exit__(self, exc_type, exc_value, traceback_value) -> None:
        self.close()


def _make_mapping_name(request_id: int, direction: str) -> str:
    return f"Local\\PyMSS_{os.getpid()}_{request_id}_{secrets.token_hex(8)}_{direction}"


def _align_up(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def _open_shared_input(header: dict, request_id: int) -> tuple[SharedMemoryRegion, np.ndarray, int]:
    if int(header.get("protocol_version", 0)) != CONTROL_PROTOCOL_VERSION:
        raise ValueError("control protocol mismatch")
    if header.get("transport") != SHARED_MEMORY_TRANSPORT:
        raise ValueError("unsupported separation transport")

    metadata = header.get("input")
    if not isinstance(metadata, dict):
        raise ValueError("shared memory input metadata is missing")
    if metadata.get("format") != "float32_le" or metadata.get("layout") != "planar":
        raise ValueError("unsupported shared memory audio format")

    name = _validate_mapping_name(metadata.get("name"))
    mapping_bytes = _validate_mapping_size(metadata.get("mapping_bytes"))
    payload_offset = _parse_nonnegative_int(metadata.get("payload_offset"), "payload_offset")
    payload_bytes = _parse_nonnegative_int(metadata.get("payload_bytes"), "payload_bytes")
    frames = _parse_nonnegative_int(metadata.get("frames"), "frames")
    channels = _parse_nonnegative_int(metadata.get("channels"), "channels")
    sample_rate = _parse_nonnegative_int(metadata.get("sample_rate"), "sample_rate")

    if payload_offset != SHM_HEADER_BYTES or frames <= 0 or channels <= 0 or channels > SHM_MAX_CHANNELS:
        raise ValueError("invalid shared memory audio dimensions")
    if sample_rate <= 0 or sample_rate > 768000:
        raise ValueError("invalid input sample rate")
    expected_payload_bytes = frames * channels * np.dtype("<f4").itemsize
    if payload_bytes != expected_payload_bytes or mapping_bytes != SHM_HEADER_BYTES + payload_bytes:
        raise ValueError("shared memory input size does not match its dimensions")

    region = SharedMemoryRegion.open_copy_on_write(name, mapping_bytes)
    try:
        mapping_header = _read_mapping_header(
            region.mapping,
            expected_role=SHM_ROLE_INPUT,
            expected_request_id=request_id,
        )
        if (
            mapping_header["payload_bytes"] != payload_bytes
            or mapping_header["frames"] != frames
            or mapping_header["channels"] != channels
        ):
            raise ValueError("shared memory input header does not match its metadata")
        audio = np.ndarray(
            (channels, frames),
            dtype="<f4",
            buffer=region.mapping,
            offset=SHM_HEADER_BYTES,
            order="C",
        )
        return region, audio, sample_rate
    except Exception:
        region.close()
        raise


def _create_shared_output(request_id: int, sample_rate: int,
                          stem_names: list[str], stem_arrays: list[np.ndarray]) -> tuple[dict, SharedMemoryRegion]:
    if not stem_names or len(stem_names) != len(stem_arrays) or len(stem_names) > SHM_MAX_STEMS:
        raise ValueError("invalid stem output list")
    if sample_rate <= 0 or sample_rate > 768000:
        raise ValueError("invalid output sample rate")

    descriptors = []
    offset = 0
    for name, array in zip(stem_names, stem_arrays):
        if not isinstance(name, str) or not name or len(name) > 256:
            raise ValueError("stem name must not be empty")
        arr = np.asarray(array, dtype=np.float32)
        if arr.ndim == 1:
            arr = arr[:, np.newaxis]
        if arr.ndim != 2 or arr.shape[0] <= 0 or arr.shape[1] <= 0 or arr.shape[1] > SHM_MAX_CHANNELS:
            raise ValueError(f"invalid shape for stem {name!r}")

        frames, channels = arr.shape
        offset = _align_up(offset, 64)
        byte_count = int(frames) * int(channels) * np.dtype("<f4").itemsize
        if offset + byte_count > SHM_MAX_RESULT_BYTES:
            raise ValueError("shared memory output exceeds the supported size")
        descriptors.append({
            "name": name,
            "array": arr,
            "offset_bytes": offset,
            "frames": int(frames),
            "channels": int(channels),
        })
        offset += byte_count

    payload_bytes = offset
    mapping_bytes = SHM_HEADER_BYTES + payload_bytes
    mapping_name = _make_mapping_name(request_id, "output")
    region = SharedMemoryRegion.create(mapping_name, mapping_bytes)
    try:
        _write_mapping_header(
            region.mapping,
            role=SHM_ROLE_OUTPUT,
            request_id=request_id,
            payload_bytes=payload_bytes,
            frames=0,
            channels=0,
            state=SHM_STATE_WRITING,
        )

        stem_metadata = []
        for descriptor in descriptors:
            array = descriptor["array"]
            channel_bytes = descriptor["frames"] * np.dtype("<f4").itemsize
            for channel in range(descriptor["channels"]):
                channel_offset = SHM_HEADER_BYTES + descriptor["offset_bytes"] + channel * channel_bytes
                target = np.ndarray(
                    (descriptor["frames"],),
                    dtype="<f4",
                    buffer=region.mapping,
                    offset=channel_offset,
                )
                target[:] = array[:, channel]
                del target

            stem_metadata.append({
                "name": descriptor["name"],
                "offset_bytes": str(descriptor["offset_bytes"]),
                "frames": str(descriptor["frames"]),
                "channels": descriptor["channels"],
                "layout": "planar",
            })

        _write_mapping_header(
            region.mapping,
            role=SHM_ROLE_OUTPUT,
            request_id=request_id,
            payload_bytes=payload_bytes,
            frames=0,
            channels=0,
            state=SHM_STATE_READY,
        )

        response = {
            "id": request_id,
            "type": "result",
            "protocol_version": CONTROL_PROTOCOL_VERSION,
            "transport": SHARED_MEMORY_TRANSPORT,
            "sample_rate": sample_rate,
            "output": {
                "name": mapping_name,
                "mapping_bytes": str(mapping_bytes),
                "payload_offset": SHM_HEADER_BYTES,
                "payload_bytes": str(payload_bytes),
            },
            "stems": stem_metadata,
        }
        return response, region
    except Exception:
        region.close()
        raise


def resample_audio(audio: np.ndarray, orig_sr: int, target_sr: int) -> np.ndarray:
    """Resample channel-major float audio without external DSP runtimes.

    ARA hosts hand us whole media items, so conversion must work for buffers
    containing many minutes of audio.  More importantly, REAPER launches this
    worker after PyTorch/CUDA has loaded: importing/calling the native scipy
    resampler in that process can deadlock in its OpenMP runtime.  This small
    NumPy-only linear interpolator avoids scipy, librosa, and their native
    thread pools entirely.  Its quality is sufficient for model-rate
    conversion and the separator's own analysis filters.
    """
    if orig_sr == target_sr:
        return np.ascontiguousarray(audio, dtype=np.float32)

    source = np.ascontiguousarray(audio, dtype=np.float32)
    source_frames = source.shape[-1]
    # Match standard resamplers' convention: preserve the final partial output
    # interval instead of dropping it when the ratio is non-integral.
    output_frames = int(math.ceil(source_frames * target_sr / orig_sr))
    if source_frames == 0 or output_frames == 0:
        return np.empty((source.shape[0], 0), dtype=np.float32)

    # np.interp is executed in NumPy's core and is independent of scipy/OpenMP.
    # Keep a single position vector and handle each channel independently to
    # avoid a large temporary (channels x frames) allocation.
    source_positions = np.arange(output_frames, dtype=np.float64) * (orig_sr / target_sr)
    source_positions[-1] = min(source_positions[-1], source_frames - 1)
    source_indices = np.arange(source_frames, dtype=np.float64)
    output = np.empty((source.shape[0], output_frames), dtype=np.float32)
    for channel in range(source.shape[0]):
        output[channel] = np.interp(source_positions, source_indices, source[channel]).astype(np.float32)
    return output

# -----------------------------------------------------------------------------
# Framing helpers (binary, little-endian).
# -----------------------------------------------------------------------------

_FD_IN = 0   # stdin
_FD_OUT = 1  # stdout
_write_lock = threading.Lock()


def _read_exactly(fd: int, n: int) -> bytes:
    """Read exactly n bytes from fd, or raise EOFError on a closed stream."""
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = os.read(fd, remaining)
        if not chunk:
            raise EOFError("stdin closed")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def read_frame() -> tuple[dict, bytes]:
    """Read a single frame from stdin. Blocks until a full frame is available."""
    header_len = struct.unpack("<I", _read_exactly(_FD_IN, 4))[0]
    body_len = struct.unpack("<I", _read_exactly(_FD_IN, 4))[0]
    if header_len > _MAX_CONTROL_HEADER_BYTES or body_len > _MAX_CONTROL_BODY_BYTES:
        raise ValueError("control frame exceeds the protocol limit")
    header_bytes = _read_exactly(_FD_IN, header_len)
    body = _read_exactly(_FD_IN, body_len) if body_len else b""
    header = json.loads(header_bytes.decode("utf-8"))
    return header, body


def write_frame(header: dict, body: bytes = b"") -> None:
    """Write a single frame to stdout in a thread-safe way."""
    header_bytes = json.dumps(header, ensure_ascii=False).encode("utf-8")
    out = struct.pack("<II", len(header_bytes), len(body)) + header_bytes + body
    with _write_lock:
        os.write(_FD_OUT, out)


def log(msg: str) -> None:
    """Diagnostic logging that never interferes with the binary stdout stream."""
    sys.stderr.write(f"[pymss-worker] {msg}\n")
    sys.stderr.flush()


# -----------------------------------------------------------------------------
# Cancellation
# -----------------------------------------------------------------------------

class CancelledError(Exception):
    """Raised inside the pymss progress callback to abort inference."""


# -----------------------------------------------------------------------------

class Worker:
    """Dispatches commands and owns cancellation/progress state.

    Separators are intentionally short-lived: every separation request loads
    its model, runs inference, then leaves the separator context and releases
    model/GPU resources before the response is returned.
    """

    def __init__(self) -> None:
        self._cancel_lock = threading.Lock()
        self._cancelled_jobs: set[int] = set()
        self._job_id: int | None = None
        self._in_queue: Queue = Queue()
        self._output_regions: dict[int, SharedMemoryRegion] = {}

    # -- pymss availability ---------------------------------------------------

    def check_pymss(self) -> dict:
        try:
            import pymss  # noqa: F401
            from pymss import list_models  # noqa: F401
            version = getattr(pymss, "__version__", "unknown")
            return {"ok": True, "version": str(version), "message": "pymss is available"}
        except Exception as exc:  # noqa: BLE001
            return {"ok": False, "version": "", "message": f"{type(exc).__name__}: {exc}"}

    # -- model catalog --------------------------------------------------------

    def list_models(self, model_dir: str | None) -> dict:
        from pymss import list_models, resolve_model

        entries = list_models(supported=True)
        models = []
        for entry in entries:
            installed = False
            try:
                resolve_model(entry.name, model_dir=model_dir, require_supported=True, require_exists=True)
                installed = True
            except Exception:  # noqa: BLE001
                installed = False
            models.append({
                "name": entry.name,
                "stem": entry.stem,
                "architecture": entry.architecture,
                "category": entry.category_path,
                "target_stem": entry.target_stem,
                "installed": installed,
                "size_bytes": entry.size_bytes,
            })
        # Installed first, then by name.
        models.sort(key=lambda m: (not m["installed"], m["name"].lower()))
        return {"models": models, "model_dir": model_dir or ""}

    def model_info(self, model_name: str, model_dir: str | None) -> dict:
        from pymss import get_model_entry, resolve_model

        try:
            entry = get_model_entry(model_name)
        except Exception as exc:  # noqa: BLE001
            return {"found": False, "intro": f"Unknown model: {model_name} ({exc})"}

        installed = False
        local_paths: dict = {}
        try:
            resolved = resolve_model(entry.name, model_dir=model_dir, require_supported=True, require_exists=True)
            installed = True
            local_paths = {"model_path": resolved.get("model_path"), "config_path": resolved.get("config_path")}
        except Exception:  # noqa: BLE001
            installed = False

        pieces = []
        if entry.architecture:
            pieces.append(f"Architecture: {entry.architecture}")
        if entry.category_path:
            pieces.append(f"Category: {entry.category_path}")
        if entry.target_stem:
            pieces.append(f"Target stem: {entry.target_stem}")
        if entry.config_instruments:
            pieces.append(f"Instruments: {entry.config_instruments}")
        if entry.classification_basis:
            pieces.append(f"Notes: {entry.classification_basis}")
        if entry.size_bytes:
            pieces.append(f"Size: {entry.size_bytes / (1024 * 1024):.1f} MB")
        pieces.append(f"Supported: {'yes' if entry.supported else 'no'}")
        pieces.append(f"Installed locally: {'yes' if installed else 'no (will auto-download)'}")

        return {
            "found": True,
            "name": entry.name,
            "stem": entry.stem,
            "architecture": entry.architecture,
            "category": entry.category_path,
            "target_stem": entry.target_stem,
            "instruments": entry.config_instruments,
            "installed": installed,
            "intro": "\n".join(pieces),
            "local_paths": local_paths,
        }

    # -- model loading --------------------------------------------------------

    def _create_separator(self, model_name: str, model_dir: str | None, inference_params: dict):
        from pymss import MSSeparator

        log(f"Loading model {model_name!r} (model_dir={model_dir!r}, download=True)")
        return MSSeparator.from_model_name(
            model_name,
            model_dir=model_dir,
            download=True,
            progress_callback=self._progress_callback,
            inference_params=inference_params,
        )

    # -- progress + cancellation glue ----------------------------------------

    def _progress_callback(self, done, total, message):
        # Abort if the current job was cancelled.
        self._raise_if_cancelled(self._job_id)
        try:
            write_frame({
                "id": self._job_id,
                "type": "progress",
                "done": int(done),
                "total": int(total),
                "message": str(message or ""),
            })
        except Exception:  # noqa: BLE001
            pass

    # -- separation -----------------------------------------------------------

    def separate(self, request_id: int, header: dict, body: bytes) -> tuple[dict, bytes]:
        if body:
            raise ValueError("separation audio must use shared memory")
        if self._output_regions:
            log("Releasing an unacknowledged output buffer")
            self.release_all_outputs()

        model_name = header["model"]
        model_dir = header.get("model_dir") or None

        def _to_none_if_zero(v):
            return None if (v is None or int(v) <= 0) else int(v)

        inference_params = {
            "batch_size": _to_none_if_zero(header.get("batch_size", 0)),
            "overlap_size": _to_none_if_zero(header.get("overlap_size", 0)),
            "chunk_size": _to_none_if_zero(header.get("chunk_size", 0)),
            "normalize": bool(header.get("normalize", False)),
        }

        self._job_id = request_id
        self._discard_stale_cancellations(request_id)
        self._raise_if_cancelled(request_id)

        input_region, shared_mix, sample_rate = _open_shared_input(header, request_id)
        mix = shared_mix
        frames = int(shared_mix.shape[-1])
        channels = int(shared_mix.shape[0])
        try:
            # A fresh separator is used for every request. MSSeparator's context
            # manager calls close() on exit, including when inference raises.
            with self._create_separator(model_name, model_dir, inference_params) as separator:
                log("Model loaded")

                # Resample input to the model's design sample rate if needed.
                model_sr = self._model_sample_rate(separator)
                input_sr = sample_rate
                if model_sr and model_sr != sample_rate:
                    log(f"Resampling input {sample_rate} -> {model_sr} ({frames} frames)")
                    self._progress_callback(0, 1, "Resampling input for model...")
                    mix = resample_audio(mix, sample_rate, model_sr)
                    input_sr = model_sr

                log(f"Separating {mix.shape[-1]} frames @ {input_sr} Hz, channels={channels}")
                self._progress_callback(0, 1, "Running separation...")
                results = separator.separate(mix, pbar=False)
                self._raise_if_cancelled(request_id)
        finally:
            del mix
            del shared_mix
            input_region.close()

        log("Model released")

        # Order stems deterministically.
        stem_names = list(results.keys())

        # pymss returns stems sample-major as (frames, channels). Resample along
        # the time axis if we resampled the input, then publish planar buffers.
        out_sr = input_sr
        stem_arrays = []
        for name in stem_names:
            self._raise_if_cancelled(request_id)
            arr = np.asarray(results[name], dtype=np.float32)
            if arr.ndim == 1:
                arr = arr[:, np.newaxis]          # (frames, 1)
            # arr is (frames, channels)
            if out_sr != sample_rate:
                arr = resample_audio(arr.T, out_sr, sample_rate).T
            stem_arrays.append(arr)

        response_header, output_region = _create_shared_output(
            request_id,
            sample_rate,
            stem_names,
            stem_arrays,
        )
        if self.is_cancelled(request_id):
            output_region.close()
            raise CancelledError("separation cancelled")
        self.release_output(request_id)
        self._output_regions[request_id] = output_region
        return response_header, b""

    def _model_sample_rate(self, separator):
        try:
            cfg = separator.config
            audio = getattr(cfg, "audio", None)
            if audio is None:
                return None
            sr = audio.get("sample_rate") if hasattr(audio, "get") else getattr(audio, "sample_rate", None)
            return int(sr) if sr else None
        except Exception:  # noqa: BLE001
            return None

    # -- lifecycle -----------------------------------------------------------

    def cancel(self, request_id: int) -> None:
        if request_id <= 0:
            return
        with self._cancel_lock:
            self._cancelled_jobs.add(request_id)
        log(f"Cancel requested for job {request_id}")

    def is_cancelled(self, request_id: int | None) -> bool:
        if request_id is None:
            return False
        with self._cancel_lock:
            return request_id in self._cancelled_jobs

    def _raise_if_cancelled(self, request_id: int | None) -> None:
        if self.is_cancelled(request_id):
            raise CancelledError("separation cancelled")

    def _discard_stale_cancellations(self, request_id: int) -> None:
        with self._cancel_lock:
            self._cancelled_jobs = {
                cancelled_id for cancelled_id in self._cancelled_jobs
                if cancelled_id >= request_id
            }

    def clear_cancel(self, request_id: int) -> None:
        with self._cancel_lock:
            self._cancelled_jobs.discard(request_id)

    def release_output(self, request_id: int) -> None:
        region = self._output_regions.pop(request_id, None)
        if region is not None:
            region.close()

    def release_all_outputs(self) -> None:
        for region in self._output_regions.values():
            region.close()
        self._output_regions.clear()

    def shutdown(self) -> None:
        # Separators are scoped to individual separation requests, so there is
        # no model instance to release when the worker exits.
        self.release_all_outputs()


# -----------------------------------------------------------------------------
# Main loop: a reader thread feeds a queue; the main thread processes commands.
# Cancel commands are handled immediately by the reader thread so that they can
# interrupt an in-flight separation.
# -----------------------------------------------------------------------------

def main() -> int:
    log("worker starting")
    worker = Worker()

    def reader():
        try:
            while True:
                header, body = read_frame()
                cmd = header.get("cmd")
                if cmd == "cancel":
                    worker.cancel(int(header.get("id", 0)))
                    continue
                if cmd == "shutdown":
                    worker._in_queue.put(("shutdown", header, body))
                    return
                worker._in_queue.put((cmd, header, body))
        except EOFError:
            worker._in_queue.put(("shutdown", {}, b""))
        except Exception as exc:  # noqa: BLE001
            log(f"reader thread error: {exc}")
            worker._in_queue.put(("shutdown", {}, b""))

    threading.Thread(target=reader, name="stdin-reader", daemon=True).start()

    # Announce readiness and the pymss import state.
    pymss_state = worker.check_pymss()
    write_frame({
        "id": 0,
        "type": "ready",
        "protocol_version": CONTROL_PROTOCOL_VERSION,
        "capabilities": [SHARED_MEMORY_TRANSPORT],
        **pymss_state,
    })

    while True:
        try:
            cmd, header, body = worker._in_queue.get()
        except KeyboardInterrupt:
            break

        request_id = int(header.get("id", 0))

        if cmd == "shutdown":
            worker.shutdown()
            log("shutting down")
            return 0

        if cmd == "ping":
            write_frame({"id": request_id, "type": "result", "pong": True})
            continue

        if cmd == "check_pymss":
            write_frame({"id": request_id, "type": "result", **worker.check_pymss()})
            continue

        if cmd == "release_buffer":
            worker.release_output(request_id)
            continue

        if cmd == "list_models":
            try:
                result = worker.list_models(header.get("model_dir"))
                write_frame({"id": request_id, "type": "result", **result})
            except Exception as exc:  # noqa: BLE001
                write_frame({"id": request_id, "type": "error", "message": _format_exc(exc)})
            continue

        if cmd == "model_info":
            try:
                result = worker.model_info(header.get("model"), header.get("model_dir"))
            except Exception as exc:  # noqa: BLE001
                write_frame({"id": request_id, "type": "error", "message": _format_exc(exc)})
                continue
            write_frame({"id": request_id, "type": "result", **result})
            continue

        if cmd == "separate":
            try:
                resp_header, resp_body = worker.separate(request_id, header, body)
                write_frame(resp_header, resp_body)
            except CancelledError:
                worker.release_output(request_id)
                write_frame({"id": request_id, "type": "error", "error_type": "cancelled", "message": "cancelled"})
            except Exception as exc:  # noqa: BLE001
                worker.release_output(request_id)
                write_frame({"id": request_id, "type": "error", "error_type": "separation_failed", "message": _format_exc(exc)})
            finally:
                worker.clear_cancel(request_id)
                worker._job_id = None
            continue

        write_frame({"id": request_id, "type": "error", "message": f"unknown command: {cmd!r}"})


def _format_exc(exc: Exception) -> str:
    msg = f"{type(exc).__name__}: {exc}"
    tb = traceback.format_exc()
    sys.stderr.write(tb)
    sys.stderr.flush()
    return msg


if __name__ == "__main__":
    try:
        sys.exit(main())
    except EOFError:
        log("stdin closed, exiting")
        sys.exit(0)
