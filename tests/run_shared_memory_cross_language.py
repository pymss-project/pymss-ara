from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import numpy as np


PROJECT_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PROJECT_ROOT / "python"))

import worker  # noqa: E402


def check_cpp_input(executable: str) -> None:
    process = subprocess.Popen(
        [executable, "produce-input"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        text=True,
    )
    try:
        name = process.stdout.readline().strip()
        mapping_bytes = int(process.stdout.readline().strip())
        frames = 5
        channels = 2
        payload_bytes = frames * channels * np.dtype("<f4").itemsize
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
        region, audio, sample_rate = worker._open_shared_input(header, 7001)
        try:
            expected = np.array(
                [[0.0, 0.25, 0.5, 0.75, 1.0], [-1.0, -0.75, -0.5, -0.25, 0.0]],
                dtype=np.float32,
            )
            if sample_rate != 48000:
                raise AssertionError("unexpected sample rate")
            np.testing.assert_allclose(audio, expected)
        finally:
            del audio
            region.close()
        process.stdin.write("release\n")
        process.stdin.flush()
        if process.wait(timeout=10) != 0:
            raise RuntimeError("C++ input producer failed")
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=10)
        for stream in (process.stdin, process.stdout):
            if stream is not None:
                stream.close()


def check_python_output(executable: str) -> None:
    stems = [
        np.array([[1.0, 10.0], [2.0, 20.0], [3.0, 30.0]], dtype=np.float32),
    ]
    response, region = worker._create_shared_output(7002, 48000, ["vocals"], stems)
    try:
        output = response["output"]
        subprocess.run(
            [
                executable,
                "consume-output",
                output["name"],
                output["mapping_bytes"],
                output["payload_bytes"],
            ],
            check=True,
        )
    finally:
        region.close()


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("expected the C++ smoke executable path")
    check_cpp_input(sys.argv[1])
    check_python_output(sys.argv[1])
    print("C++/Python shared-memory interoperability passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
