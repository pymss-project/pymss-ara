"""PyMSS ARA Plugin worker process.

Talks to the C++ plugin over stdin/stdout using a binary framed protocol.
All human-readable logging goes to stderr; only frames go to stdout.

Frame layout (little-endian, identical in both directions)::

    uint32  header_len   # length of the JSON header in bytes
    uint32  body_len     # length of the binary body in bytes
    bytes   header_len   # UTF-8 JSON object
    bytes   body_len     # raw binary payload

Request header fields:
    id      int    request id (mirrored in responses/progress)
    cmd     str    "ping" | "check_pymss" | "list_models" | "model_info"
                   | "separate" | "cancel" | "shutdown"

Response / event header fields:
    id      int
    type    str    "result" | "progress" | "error"

The "separate" request carries interleaved float32 PCM in its body and returns
the stems as concatenated interleaved float32 PCM (one stem after another).
"""

from __future__ import annotations

import json
import os
import struct
import sys
import threading
import traceback
from queue import Queue

import numpy as np

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
    """Holds long-lived state (loaded model) and dispatches commands."""

    def __init__(self) -> None:
        self._separator = None
        self._loaded_key: tuple | None = None  # (model_name, model_dir, inference_params_hash)
        self._cancel_event = threading.Event()
        self._job_id: int | None = None
        self._in_queue: Queue = Queue()

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

    def _hash_params(self, params: dict) -> int:
        return hash(tuple(sorted((k, str(v)) for k, v in (params or {}).items())))

    def _ensure_model(self, model_name: str, model_dir: str | None, inference_params: dict):
        from pymss import MSSeparator

        key = (model_name, model_dir or "", self._hash_params(inference_params))
        if self._separator is not None and self._loaded_key == key:
            return self._separator

        # Release the previous model before loading a new one.
        if self._separator is not None:
            try:
                self._separator.close()
            except Exception:  # noqa: BLE001
                pass
            self._separator = None

        log(f"Loading model {model_name!r} (model_dir={model_dir!r}, download=True)")
        separator = MSSeparator.from_model_name(
            model_name,
            model_dir=model_dir,
            download=True,
            progress_callback=self._progress_callback,
            inference_params=inference_params,
        )
        separator.load_model()
        self._separator = separator
        self._loaded_key = key
        log("Model loaded")
        return separator

    # -- progress + cancellation glue ----------------------------------------

    def _progress_callback(self, done, total, message):
        # Abort if the current job was cancelled.
        if self._cancel_event.is_set():
            raise CancelledError("separation cancelled")
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
        model_name = header["model"]
        model_dir = header.get("model_dir") or None
        sample_rate = int(header["sample_rate"])
        channels = int(header.get("channels", 2))

        def _to_none_if_zero(v):
            return None if (v is None or int(v) <= 0) else int(v)

        inference_params = {
            "batch_size": _to_none_if_zero(header.get("batch_size", 0)),
            "overlap_size": _to_none_if_zero(header.get("overlap_size", 0)),
            "chunk_size": _to_none_if_zero(header.get("chunk_size", 0)),
            "normalize": bool(header.get("normalize", False)),
        }

        self._cancel_event.clear()
        self._job_id = request_id

        separator = self._ensure_model(model_name, model_dir, inference_params)

        # Decode interleaved float32 body -> (channels, frames).
        raw = np.frombuffer(body, dtype="<f4")
        frames = raw.size // max(1, channels)
        raw = raw[: frames * channels]
        mix = np.ascontiguousarray(raw.reshape(frames, channels).T.astype(np.float32))

        # Resample input to the model's design sample rate if needed.
        model_sr = self._model_sample_rate(separator)
        input_sr = sample_rate
        if model_sr and model_sr != sample_rate:
            import librosa
            log(f"Resampling input {sample_rate} -> {model_sr}")
            mix = np.ascontiguousarray(
                librosa.resample(mix, orig_sr=sample_rate, target_sr=model_sr).astype(np.float32)
            )
            input_sr = model_sr

        log(f"Separating {frames} frames @ {input_sr} Hz, channels={channels}")
        results = separator.separate(mix, pbar=False)

        # Order stems deterministically.
        stem_names = list(results.keys())

        # pymss returns stems sample-major as (frames, channels). Resample along
        # the time axis if we resampled the input, then pack interleaved.
        out_sr = input_sr
        stem_arrays = []
        for name in stem_names:
            arr = np.asarray(results[name], dtype=np.float32)
            if arr.ndim == 1:
                arr = arr[:, np.newaxis]          # (frames, 1)
            # arr is (frames, channels)
            if out_sr != sample_rate:
                import librosa
                arr = np.ascontiguousarray(
                    librosa.resample(np.ascontiguousarray(arr.T), orig_sr=out_sr, target_sr=sample_rate)
                    .T.astype(np.float32)
                )
            stem_arrays.append(arr)

        # Pack stems as concatenated interleaved float32 (f0c0, f0c1, f1c0, ...).
        body_chunks = []
        stem_meta = []
        for name, arr in zip(stem_names, stem_arrays):
            n_fr, n_ch = arr.shape                # (frames, channels)
            interleaved = np.ascontiguousarray(arr, dtype="<f4").reshape(-1)
            body_chunks.append(interleaved.tobytes())
            stem_meta.append({"name": name, "channels": int(n_ch), "frames": int(n_fr)})

        response_header = {
            "id": request_id,
            "type": "result",
            "sample_rate": sample_rate,
            "stems": stem_meta,
        }
        return response_header, b"".join(body_chunks)

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

    def cancel(self) -> None:
        self._cancel_event.set()
        log("Cancel requested")

    def shutdown(self) -> None:
        if self._separator is not None:
            try:
                self._separator.close()
            except Exception:  # noqa: BLE001
                pass
        self._separator = None


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
                    worker.cancel()
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
    write_frame({"id": 0, "type": "ready", **pymss_state})

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
                write_frame({"id": request_id, "type": "error", "error_type": "cancelled", "message": "cancelled"})
            except Exception as exc:  # noqa: BLE001
                write_frame({"id": request_id, "type": "error", "error_type": "separation_failed", "message": _format_exc(exc)})
            finally:
                worker._cancel_event.clear()
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
