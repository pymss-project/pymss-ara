from __future__ import annotations

import json
import mmap
import struct
import subprocess
import sys
import unittest
from pathlib import Path

import numpy as np


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "python"))

import worker  # noqa: E402


class WorkerLogicTests(unittest.TestCase):
    def test_pymss_identity_is_embedded_in_shared_memory_header(self):
        self.assertEqual(worker.CONTROL_PROTOCOL_VERSION, 4)
        self.assertEqual(worker.SHARED_MEMORY_TRANSPORT, "pymss.shared_memory.audio.v2")
        self.assertEqual(worker.SHM_MAGIC_BYTES, b"PYMS")
        self.assertEqual(worker.SHM_IDENTITY.rstrip(b"\0"), b"PYMSS::SHM::V2")
        self.assertEqual(worker._SHM_HEADER.size, worker.SHM_HEADER_BYTES)

        header = bytearray(worker.SHM_HEADER_BYTES)
        worker._write_mapping_header(
            header,
            role=worker.SHM_ROLE_INPUT,
            request_id=9001,
            payload_bytes=0,
            frames=0,
            channels=0,
            state=worker.SHM_STATE_READY,
        )
        self.assertEqual(bytes(header[:4]), worker.SHM_MAGIC_BYTES)
        self.assertEqual(bytes(header[48:64]), worker.SHM_IDENTITY)
        worker._read_mapping_header(
            header,
            expected_role=worker.SHM_ROLE_INPUT,
            expected_request_id=9001,
        )

        header[48] ^= 0x01
        with self.assertRaisesRegex(ValueError, "protocol header is invalid"):
            worker._read_mapping_header(
                header,
                expected_role=worker.SHM_ROLE_INPUT,
                expected_request_id=9001,
            )

    def test_model_config_defaults_are_extracted_without_legacy_normalize(self):
        config = {
            "audio": {"chunk_size": 1000},
            "inference": {
                "batch_size": 3,
                "num_overlap": 4,
                "normalize": True,
            },
        }
        defaults = worker.Worker._extract_default_params(config)
        self.assertEqual(defaults["batch_size"], 3)
        self.assertEqual(defaults["chunk_size"], 1000)
        self.assertEqual(defaults["overlap_size"], 750)
        self.assertTrue(defaults["standardize"])
        self.assertFalse(defaults["normalize"])

        config["inference"]["chunk_size"] = 800
        compatible = worker.Worker._extract_default_params(config)
        self.assertEqual(compatible["chunk_size"], 800)
        self.assertEqual(compatible["overlap_size"], 600)

        registered_chunk = worker.Worker._extract_default_params(
            {"audio": {"chunk_size": 1000}, "inference": {"num_overlap": 4}},
            {"chunk_size": 800},
        )
        self.assertEqual(registered_chunk["chunk_size"], 800)
        self.assertEqual(registered_chunk["overlap_size"], 600)

        overridden = worker.Worker._extract_default_params(
            config,
            {"overlap_size": 125, "normalize": True},
        )
        self.assertEqual(overridden["overlap_size"], 125)
        self.assertTrue(overridden["normalize"])

    def test_vr_model_defaults_and_overrides_are_extracted(self):
        defaults = worker.Worker._extract_default_params(None, architecture="vr")
        self.assertEqual(defaults["batch_size"], 2)
        self.assertEqual(defaults["window_size"], 512)
        self.assertEqual(defaults["aggression"], 5)
        self.assertEqual(defaults["post_process_threshold"], 0.2)
        self.assertFalse(defaults["enable_tta"])
        self.assertFalse(defaults["high_end_process"])
        self.assertFalse(defaults["enable_post_process"])

        overridden = worker.Worker._extract_default_params(
            None,
            {
                "batch_size": 4,
                "window_size": 1024,
                "aggression": 0,
                "post_process_threshold": 0.35,
                "enable_tta": True,
                "high_end_process": True,
                "enable_post_process": True,
                "normalize": True,
            },
            architecture="vr",
        )
        self.assertEqual(overridden["batch_size"], 4)
        self.assertEqual(overridden["window_size"], 1024)
        self.assertEqual(overridden["aggression"], 0)
        self.assertEqual(overridden["post_process_threshold"], 0.35)
        self.assertTrue(overridden["enable_tta"])
        self.assertTrue(overridden["high_end_process"])
        self.assertTrue(overridden["enable_post_process"])
        self.assertTrue(overridden["normalize"])

    def test_model_download_reports_progress_and_returns_info(self):
        import pymss

        events = []
        instance = worker.Worker()
        original_download = pymss.download_model
        original_write_frame = worker.write_frame
        original_model_info = instance.model_info

        def fake_download(model_name, model_dir=None, progress_callback=None, **kwargs):
            self.assertEqual(model_name, "fixture.ckpt")
            self.assertEqual(model_dir, "models")
            progress_callback(25, 100, "Downloading fixture.ckpt")
            progress_callback(100, 100, "Downloaded fixture.ckpt")

        pymss.download_model = fake_download
        worker.write_frame = lambda header, body=b"": events.append((header, body))
        instance.model_info = lambda name, model_dir: {
            "name": name,
            "installed": True,
            "default_params": {"batch_size": 2},
        }
        try:
            info = instance.download_model(501, "fixture.ckpt", "models")
        finally:
            pymss.download_model = original_download
            worker.write_frame = original_write_frame
            instance.model_info = original_model_info

        self.assertTrue(info["installed"])
        self.assertEqual([event[0]["done"] for event in events], [25, 100])
        self.assertTrue(all(event[0]["type"] == "download_progress" for event in events))

    def test_uninstalled_catalog_model_reports_zero_defaults(self):
        info = worker.Worker().model_info("logic_bs_roformer.ckpt", None)
        self.assertTrue(info["found"])
        if not info["installed"]:
            self.assertEqual(
                info["default_params"],
                {
                    "batch_size": 0,
                    "overlap_size": 0,
                    "chunk_size": 0,
                    "window_size": 0,
                    "aggression": 0,
                    "post_process_threshold": 0.0,
                    "enable_tta": False,
                    "standardize": False,
                    "high_end_process": False,
                    "enable_post_process": False,
                    "normalize": False,
                },
            )


@unittest.skipUnless(sys.platform == "win32", "Windows named mappings are required")
class SharedMemoryProtocolTests(unittest.TestCase):
    def create_input(self, request_id: int = 41):
        channels = 2
        frames = 5
        payload_bytes = channels * frames * np.dtype("<f4").itemsize
        mapping_bytes = worker.SHM_HEADER_BYTES + payload_bytes
        name = worker._make_mapping_name(request_id, "input")
        owner = mmap.mmap(-1, mapping_bytes, tagname=name, access=mmap.ACCESS_WRITE)
        worker._write_mapping_header(
            owner,
            role=worker.SHM_ROLE_INPUT,
            request_id=request_id,
            payload_bytes=payload_bytes,
            frames=frames,
            channels=channels,
            state=worker.SHM_STATE_READY,
        )
        samples = np.ndarray(
            (channels, frames),
            dtype="<f4",
            buffer=owner,
            offset=worker.SHM_HEADER_BYTES,
        )
        samples[:] = np.array(
            [[0.0, 0.25, 0.5, 0.75, 1.0], [-1.0, -0.75, -0.5, -0.25, 0.0]],
            dtype=np.float32,
        )
        header = {
            "protocol_version": worker.CONTROL_PROTOCOL_VERSION,
            "transport": worker.SHARED_MEMORY_TRANSPORT,
            "input": {
                "name": name,
                "mapping_bytes": str(mapping_bytes),
                "payload_offset": worker.SHM_HEADER_BYTES,
                "payload_bytes": str(payload_bytes),
                "format": "float32_le",
                "layout": "planar",
                "frames": str(frames),
                "channels": channels,
                "sample_rate": 48000,
            },
        }
        return owner, samples, header

    def test_planar_input_is_opened_copy_on_write(self):
        owner, owner_samples, header = self.create_input()
        region = None
        shared_samples = None
        try:
            region, shared_samples, sample_rate = worker._open_shared_input(header, 41)
            self.assertEqual(sample_rate, 48000)
            np.testing.assert_allclose(shared_samples, owner_samples)
            shared_samples[0, 0] = 123.0
            self.assertEqual(float(owner_samples[0, 0]), 0.0)
        finally:
            del shared_samples
            if region is not None:
                region.close()
            del owner_samples
            owner.close()

    def test_output_mapping_is_planar_and_self_describing(self):
        stems = [
            np.array([[1.0, 10.0], [2.0, 20.0], [3.0, 30.0]], dtype=np.float32),
            np.array([[4.0], [5.0], [6.0], [7.0]], dtype=np.float32),
        ]
        response, region = worker._create_shared_output(72, 44100, ["vocals", "other"], stems)
        reader = None
        try:
            output = response["output"]
            reader = mmap.mmap(
                -1,
                int(output["mapping_bytes"]),
                tagname=output["name"],
                access=mmap.ACCESS_READ,
            )
            mapping_header = worker._read_mapping_header(
                reader,
                expected_role=worker.SHM_ROLE_OUTPUT,
                expected_request_id=72,
            )
            self.assertEqual(mapping_header["payload_bytes"], int(output["payload_bytes"]))

            for metadata, expected in zip(response["stems"], stems):
                frames = int(metadata["frames"])
                channels = int(metadata["channels"])
                offset = worker.SHM_HEADER_BYTES + int(metadata["offset_bytes"])
                actual = np.ndarray(
                    (channels, frames),
                    dtype="<f4",
                    buffer=reader,
                    offset=offset,
                )
                np.testing.assert_allclose(actual, expected.T)
                del actual
        finally:
            if reader is not None:
                reader.close()
            region.close()

    def test_output_payload_limit_is_enforced_before_mapping(self):
        original_limit = worker.SHM_MAX_RESULT_BYTES
        worker.SHM_MAX_RESULT_BYTES = 8
        try:
            with self.assertRaisesRegex(ValueError, "exceeds the supported size"):
                worker._create_shared_output(
                    73,
                    44100,
                    ["vocals"],
                    [np.ones((3, 2), dtype=np.float32)],
                )
        finally:
            worker.SHM_MAX_RESULT_BYTES = original_limit

    def test_header_request_mismatch_is_rejected(self):
        owner, owner_samples, header = self.create_input(request_id=9)
        try:
            with self.assertRaisesRegex(ValueError, "does not match"):
                worker._open_shared_input(header, 10)
        finally:
            del owner_samples
            owner.close()

    def test_cancel_before_job_start_is_preserved(self):
        owner, owner_samples, header = self.create_input(request_id=106)
        header.update({
            "model": "transport-fixture",
            "model_dir": "",
            "batch_size": 0,
            "overlap_size": 0,
            "chunk_size": 0,
            "normalize": False,
        })
        separator_created = False
        instance = worker.Worker()

        def create_separator(*args, **kwargs):
            nonlocal separator_created
            separator_created = True
            raise AssertionError("cancelled job must not create a separator")

        instance._create_separator = create_separator
        original_log = worker.log
        worker.log = lambda *args, **kwargs: None
        try:
            instance.cancel(106)
            with self.assertRaises(worker.CancelledError):
                instance.separate(106, header, b"")
            self.assertFalse(separator_created)
        finally:
            worker.log = original_log
            instance.clear_cancel(106)
            del owner_samples
            owner.close()

    def test_mapping_can_be_opened_by_another_process(self):
        owner, owner_samples, header = self.create_input(request_id=88)
        try:
            script = """
import json
import sys
sys.path.insert(0, sys.argv[1])
import worker
header = json.loads(sys.argv[2])
region, audio, sample_rate = worker._open_shared_input(header, 88)
try:
    print(f"{sample_rate}:{float(audio.sum()):.2f}")
finally:
    del audio
    region.close()
"""
            completed = subprocess.run(
                [
                    sys.executable,
                    "-c",
                    script,
                    str(PROJECT_ROOT / "python"),
                    json.dumps(header),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            self.assertEqual(completed.stdout.strip(), "48000:0.00")
        finally:
            del owner_samples
            owner.close()

    def test_worker_control_handshake_reports_protocol_v4(self):
        process = subprocess.Popen(
            [sys.executable, str(PROJECT_ROOT / "python" / "worker.py")],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        def read_exactly(count: int) -> bytes:
            result = bytearray()
            while len(result) < count:
                chunk = process.stdout.read(count - len(result))
                if not chunk:
                    raise EOFError("worker control stream ended")
                result.extend(chunk)
            return bytes(result)

        def read_frame() -> tuple[dict, bytes]:
            header_length, body_length = struct.unpack("<II", read_exactly(8))
            header = json.loads(read_exactly(header_length).decode("utf-8"))
            body = read_exactly(body_length) if body_length else b""
            return header, body

        def write_frame(header: dict) -> None:
            encoded = json.dumps(header).encode("utf-8")
            process.stdin.write(struct.pack("<II", len(encoded), 0) + encoded)
            process.stdin.flush()

        try:
            ready, body = read_frame()
            self.assertEqual(body, b"")
            self.assertEqual(ready["type"], "ready")
            self.assertEqual(ready["protocol_version"], worker.CONTROL_PROTOCOL_VERSION)
            self.assertIn(worker.SHARED_MEMORY_TRANSPORT, ready["capabilities"])

            write_frame({"id": 301, "cmd": "ping"})
            pong, body = read_frame()
            self.assertEqual(body, b"")
            self.assertEqual(pong, {"id": 301, "type": "result", "pong": True})

            write_frame({"id": 302, "cmd": "shutdown"})
            self.assertEqual(process.wait(timeout=10), 0)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream is not None:
                    stream.close()

    def test_worker_separation_uses_shared_memory_end_to_end(self):
        owner, owner_samples, header = self.create_input(request_id=105)
        header.update({
            "model": "transport-fixture",
            "model_dir": "",
            "model_architecture": "vr",
            "batch_size": 2,
            "window_size": 1024,
            "aggression": 0,
            "post_process_threshold": 0.35,
            "enable_tta": True,
            "high_end_process": True,
            "enable_post_process": True,
            "normalize": True,
        })

        class Separator:
            class Config:
                audio = {"sample_rate": 48000}

            config = Config()

            def __enter__(self):
                return self

            def __exit__(self, exc_type, exc_value, traceback_value):
                return False

            def separate(self, audio, pbar=False):
                self.test_case.assertFalse(pbar)
                return {
                    "vocals": audio.T.copy(),
                    "other": (audio * 0.5).T.copy(),
                }

        separator = Separator()
        separator.test_case = self
        instance = worker.Worker()
        captured_params = {}

        def create_separator(model_name, model_dir, inference_params):
            self.assertEqual(model_name, "transport-fixture")
            self.assertIsNone(model_dir)
            captured_params.update(inference_params)
            return separator

        instance._create_separator = create_separator
        original_write_frame = worker.write_frame
        original_log = worker.log
        worker.write_frame = lambda *args, **kwargs: None
        worker.log = lambda *args, **kwargs: None
        reader = None
        try:
            response, body = instance.separate(105, header, b"")
            self.assertEqual(body, b"")
            self.assertEqual(response["transport"], worker.SHARED_MEMORY_TRANSPORT)
            self.assertIn(105, instance._output_regions)
            self.assertEqual(
                captured_params,
                {
                    "batch_size": 2,
                    "enable_tta": True,
                    "normalize": True,
                    "window_size": 1024,
                    "aggression": 0,
                    "high_end_process": True,
                    "enable_post_process": True,
                    "post_process_threshold": 0.35,
                },
            )

            output = response["output"]
            reader = mmap.mmap(
                -1,
                int(output["mapping_bytes"]),
                tagname=output["name"],
                access=mmap.ACCESS_READ,
            )
            metadata = response["stems"][0]
            actual = np.ndarray(
                (int(metadata["channels"]), int(metadata["frames"])),
                dtype="<f4",
                buffer=reader,
                offset=worker.SHM_HEADER_BYTES + int(metadata["offset_bytes"]),
            )
            np.testing.assert_allclose(actual, owner_samples)
            del actual
        finally:
            worker.write_frame = original_write_frame
            worker.log = original_log
            if reader is not None:
                reader.close()
            instance.shutdown()
            del owner_samples
            owner.close()


if __name__ == "__main__":
    unittest.main()
