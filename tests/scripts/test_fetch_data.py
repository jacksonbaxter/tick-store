#!/usr/bin/env python3
"""Tests for scripts/fetch_data.py. Run with:

    python3 -I tests/scripts/test_fetch_data.py

The script runs as a subprocess, as a user would run it, against tiny gzip fixtures
generated under the repo's gitignored build/ directory, and a local HTTP server.

A check that cannot fail proves nothing, so each one is shown failing on an injected
fault. The chain stops at its first failure, so a fault aimed at a later step repins
the earlier steps to the damaged file: otherwise an earlier check would catch it and
the later check would never run.
"""

import fcntl
import gzip
import hashlib
import http.server
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT = REPO_ROOT / "scripts" / "fetch_data.py"
SCRATCH_ROOT = REPO_ROOT / "build" / "test_fetch_data"

DAY = "12302019"
NAME = "12302019.NASDAQ_ITCH50"

EXIT_DOWNLOAD = 10
EXIT_PINS = 11
EXIT_LOCAL = 12
EXIT_GZ_SIZE = 20
EXIT_GZ_SHA256 = 21
EXIT_GZIP = 22
EXIT_SIZE = 23
EXIT_SHA256 = 24


def framed_messages():
    """Deterministic bytes shaped like ITCH: length-prefixed, partly compressible."""
    rng = random.Random(20191230)
    out = bytearray()
    for i in range(3000):
        body = bytes([65 + i % 26]) + rng.randbytes(11) + b"AAPL    " * 2
        out += len(body).to_bytes(2, "big") + body
    return bytes(out)


PAYLOAD = framed_messages()
GOOD_GZ = gzip.compress(PAYLOAD, mtime=0)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def flipped(data, offset):
    damaged = bytearray(data)
    damaged[offset] ^= 0x01
    return bytes(damaged)


class Server:
    """A local file server with switchable bad behaviour."""

    def __init__(self, body, honour_range=True, drop_after=None, status=None,
                 send_length=True):
        self.body = body
        self.send_length = send_length
        self.honour_range = honour_range
        self.drop_after = drop_after
        self.status = status
        self.range_headers = []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                outer.range_headers.append(self.headers.get("Range"))
                if outer.status is not None:
                    self.send_error(outer.status)
                    return
                start = 0
                header = self.headers.get("Range")
                if header and outer.honour_range:
                    start = int(header.removeprefix("bytes=").removesuffix("-"))
                remaining = outer.body[start:]
                if header and outer.honour_range:
                    self.send_response(206)
                    self.send_header("Content-Range", f"bytes {start}-{len(outer.body) - 1}"
                                                      f"/{len(outer.body)}")
                else:
                    self.send_response(200)
                if outer.send_length:
                    self.send_header("Content-Length", str(len(remaining)))
                self.end_headers()
                if outer.drop_after is not None and len(remaining) > outer.drop_after:
                    self.wfile.write(remaining[:outer.drop_after])
                    outer.drop_after = None
                    self.close_connection = True
                    return
                self.wfile.write(remaining)
                self.close_connection = True

            def log_message(self, *args):
                pass

        self.httpd = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}/{NAME}.gz"
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *exc):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.thread.join()


class FixtureCase(unittest.TestCase):
    def setUp(self):
        SCRATCH_ROOT.mkdir(parents=True, exist_ok=True)
        self.root = Path(tempfile.mkdtemp(dir=SCRATCH_ROOT))
        self.addCleanup(shutil.rmtree, self.root, ignore_errors=True)
        self.data = self.root / "data"
        self.gz = self.data / f"{NAME}.gz"
        self.part = self.data / f"{NAME}.gz.part"
        self.out = self.data / NAME
        self.tmp = self.data / f"{NAME}.tmp"
        self.source = self.root / "copy" / f"{NAME}.gz"
        self.source.parent.mkdir()
        self.pins_path = self.root / "pins.json"

    def write_pins(self, **overrides):
        pin = {
            "name": NAME,
            "url": "http://127.0.0.1:9/unused",
            "gz_size": len(GOOD_GZ),
            "gz_sha256": sha256(GOOD_GZ),
            "decompressed_size": len(PAYLOAD),
            "decompressed_sha256": sha256(PAYLOAD),
        }
        pin.update(overrides)
        self.pins_path.write_text(json.dumps({"days": {DAY: pin}}))

    def run_script(self, *args, data_dir=True):
        command = [sys.executable, "-I", str(SCRIPT), "--pins", str(self.pins_path)]
        if data_dir:
            command += ["--data-dir", str(self.data)]
        env = dict(os.environ, FETCH_DATA_BACKOFF="0", TMPDIR=str(self.root))
        result = subprocess.run([*command, *args], capture_output=True, text=True,
                                env=env, timeout=120, cwd=self.root)
        result.message = result.stderr
        return result

    def verify_copy(self, content, *args):
        """Run the normal --from path on `content`."""
        self.source.write_bytes(content)
        return self.run_script("--from", str(self.source), *args)

    def assert_failed(self, result, code, text):
        self.assertEqual(result.returncode, code, result.stderr)
        self.assertIn(text, result.stderr)
        self.assertIn("FAILED", result.stderr)

    def assert_nothing_published(self):
        self.assertFalse(self.out.exists(), "decompressed file left behind")
        self.assertFalse(self.tmp.exists(), "temporary output left behind")
        self.assertFalse(os.path.lexists(self.gz), ".gz published despite the failure")


class GoodFixture(FixtureCase):
    def test_pass_through_from(self):
        self.write_pins()
        result = self.verify_copy(GOOD_GZ)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.out.read_bytes(), PAYLOAD)
        self.assertTrue(self.gz.is_symlink())
        self.assertEqual(os.path.realpath(self.gz), os.path.realpath(self.source))
        self.assertFalse(self.tmp.exists())
        self.assertIn(f"ok {DAY}", result.stdout)

    def test_pass_through_download(self):
        with Server(GOOD_GZ) as server:
            self.write_pins(url=server.url)
            result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.gz.read_bytes(), GOOD_GZ)
        self.assertFalse(self.gz.is_symlink())
        self.assertEqual(self.out.read_bytes(), PAYLOAD)
        self.assertFalse(self.part.exists())

    def test_default_day_is_the_reference_day(self):
        with Server(GOOD_GZ) as server:
            self.write_pins(url=server.url)
            self.assertEqual(self.run_script("--day", DAY).returncode, 0)

    def test_rerun_keeps_a_good_output_and_rebuilds_a_bad_one(self):
        self.write_pins()
        self.assertEqual(self.verify_copy(GOOD_GZ).returncode, 0)
        before = self.out.stat().st_mtime_ns
        self.assertEqual(self.run_script("--from", str(self.source)).returncode, 0)
        self.assertEqual(self.out.stat().st_mtime_ns, before)
        self.out.write_bytes(flipped(PAYLOAD, 7))
        self.assertEqual(self.run_script("--from", str(self.source)).returncode, 0)
        self.assertEqual(self.out.read_bytes(), PAYLOAD)


class CompressedSize(FixtureCase):
    def test_pin_disagrees_with_file(self):
        self.write_pins(gz_size=len(GOOD_GZ) + 1)
        self.assert_failed(self.verify_copy(GOOD_GZ), EXIT_GZ_SIZE, "compressed size mismatch")
        self.assert_nothing_published()

    def test_truncated_gz(self):
        self.write_pins()
        self.assert_failed(self.verify_copy(GOOD_GZ[:-100]), EXIT_GZ_SIZE,
                           "compressed size mismatch")
        self.assert_nothing_published()

    def test_server_declares_another_size(self):
        with Server(GOOD_GZ + b"x") as server:
            self.write_pins(url=server.url)
            self.assert_failed(self.run_script(), EXIT_GZ_SIZE, "server says")
        self.assert_nothing_published()
        self.assertFalse(self.part.exists())


    def test_server_sends_too_much_without_declaring_a_size(self):
        with Server(GOOD_GZ + b"x" * 10, send_length=False) as server:
            self.write_pins(url=server.url)
            self.assert_failed(self.run_script(), EXIT_GZ_SIZE, "more than the pinned")
        self.assert_nothing_published()
        self.assertFalse(self.part.exists())


class CompressedHash(FixtureCase):
    def test_one_flipped_byte(self):
        self.write_pins()
        damaged = flipped(GOOD_GZ, len(GOOD_GZ) // 2)
        self.assert_failed(self.verify_copy(damaged), EXIT_GZ_SHA256,
                           "compressed SHA-256 mismatch")
        self.assert_nothing_published()

    def test_flipped_byte_in_a_download_discards_the_part(self):
        damaged = flipped(GOOD_GZ, len(GOOD_GZ) // 2)
        with Server(damaged) as server:
            self.write_pins(url=server.url)
            self.assert_failed(self.run_script(), EXIT_GZ_SHA256,
                               "compressed SHA-256 mismatch")
        self.assert_nothing_published()
        self.assertFalse(self.part.exists(), "a bad .part would block every later run")


class GzipStream(FixtureCase):
    """Steps 1 and 2 are repinned to the damaged file, so only gzip can catch these."""

    def repinned(self, damaged):
        self.write_pins(gz_size=len(damaged), gz_sha256=sha256(damaged))
        return self.verify_copy(damaged)

    def test_flipped_byte_in_deflate_data(self):
        damaged = flipped(GOOD_GZ, len(GOOD_GZ) // 2)
        self.assert_failed(self.repinned(damaged), EXIT_GZIP, "gzip check failed")
        self.assert_nothing_published()

    def test_flipped_byte_in_crc32(self):
        damaged = flipped(GOOD_GZ, len(GOOD_GZ) - 8)
        self.assert_failed(self.repinned(damaged), EXIT_GZIP, "incorrect data check")
        self.assert_nothing_published()

    def test_flipped_byte_in_isize(self):
        damaged = flipped(GOOD_GZ, len(GOOD_GZ) - 4)
        self.assert_failed(self.repinned(damaged), EXIT_GZIP, "incorrect length check")
        self.assert_nothing_published()

    def test_truncated_at_each_tail_length(self):
        for cut in (1, 4, 8, 9, 100, len(GOOD_GZ) - 11):
            with self.subTest(cut=cut):
                self.assert_failed(self.repinned(GOOD_GZ[:-cut]), EXIT_GZIP, "truncated")
                self.assert_nothing_published()

    def test_bytes_after_the_trailer(self):
        damaged = GOOD_GZ + b"\0"
        self.assert_failed(self.repinned(damaged), EXIT_GZIP, "data after the gzip trailer")
        self.assert_nothing_published()

    def test_not_gzip_at_all(self):
        damaged = b"<html>not found</html>"
        self.assert_failed(self.repinned(damaged), EXIT_GZIP, "gzip check failed")
        self.assert_nothing_published()


class DecompressedSize(FixtureCase):
    def test_pin_too_large(self):
        self.write_pins(decompressed_size=len(PAYLOAD) + 1)
        self.assert_failed(self.verify_copy(GOOD_GZ), EXIT_SIZE, "decompressed size mismatch")
        self.assert_nothing_published()

    def test_pin_too_small_stops_writing(self):
        self.write_pins(decompressed_size=len(PAYLOAD) - 1)
        self.assert_failed(self.verify_copy(GOOD_GZ), EXIT_SIZE, "exceeds the pinned")
        self.assert_nothing_published()

    def test_only_size_differs_by_4_gib(self):
        # ISIZE matches modulo 2^32, so this is the case the exact check exists for.
        self.write_pins(decompressed_size=len(PAYLOAD) + 2 ** 32)
        self.assert_failed(self.verify_copy(GOOD_GZ), EXIT_SIZE, "decompressed size mismatch")
        self.assert_nothing_published()


class DecompressedHash(FixtureCase):
    def test_pin_disagrees_with_file(self):
        self.write_pins(decompressed_sha256=sha256(b"something else"))
        self.assert_failed(self.verify_copy(GOOD_GZ), EXIT_SHA256,
                           "decompressed SHA-256 mismatch")
        self.assert_nothing_published()

    def test_valid_gzip_of_other_content(self):
        other = gzip.compress(flipped(PAYLOAD, 100), mtime=0)
        self.write_pins(gz_size=len(other), gz_sha256=sha256(other))
        self.assert_failed(self.verify_copy(other), EXIT_SHA256,
                           "decompressed SHA-256 mismatch")
        self.assert_nothing_published()

    def test_failure_on_a_download_keeps_a_part_that_passed_gzip(self):
        # The compressed file passed, so the pin is what needs attention.
        with Server(GOOD_GZ) as server:
            self.write_pins(url=server.url, decompressed_sha256=sha256(b"other"))
            self.assert_failed(self.run_script(), EXIT_SHA256, "decompressed SHA-256 mismatch")
        self.assert_nothing_published()
        self.assertEqual(self.part.read_bytes(), GOOD_GZ)


class Download(FixtureCase):
    def test_http_404_is_a_download_failure(self):
        with Server(GOOD_GZ, status=404) as server:
            self.write_pins(url=server.url)
            self.assert_failed(self.run_script(), EXIT_DOWNLOAD, "HTTP 404")
        self.assert_nothing_published()

    def test_refused_connection_is_a_download_failure(self):
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            port = s.getsockname()[1]
        self.write_pins(url=f"http://127.0.0.1:{port}/x.gz")
        self.assert_failed(self.run_script(), EXIT_DOWNLOAD, "download failed")
        self.assert_nothing_published()

    def test_http_500_retries_then_fails_as_a_download_failure(self):
        with Server(GOOD_GZ, status=500) as server:
            self.write_pins(url=server.url)
            self.assert_failed(self.run_script(), EXIT_DOWNLOAD, "HTTP 500")
            self.assertGreater(len(server.range_headers), 1)

    def test_resume_requests_only_the_missing_bytes(self):
        half = len(GOOD_GZ) // 2
        self.data.mkdir()
        self.part.write_bytes(GOOD_GZ[:half])
        with Server(GOOD_GZ) as server:
            self.write_pins(url=server.url)
            result = self.run_script()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(server.range_headers, [f"bytes={half}-"])
        self.assertEqual(self.gz.read_bytes(), GOOD_GZ)
        self.assertFalse(self.part.exists())

    def test_complete_part_is_verified_without_a_request(self):
        self.data.mkdir()
        self.part.write_bytes(GOOD_GZ)
        with Server(GOOD_GZ) as server:
            self.write_pins(url=server.url)
            self.assertEqual(self.run_script().returncode, 0)
            self.assertEqual(server.range_headers, [])

    def test_dropped_connection_resumes(self):
        with Server(GOOD_GZ, drop_after=1000) as server:
            self.write_pins(url=server.url)
            result = self.run_script()
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(server.range_headers, [None, "bytes=1000-"])
        self.assertEqual(self.gz.read_bytes(), GOOD_GZ)

    def test_server_ignoring_range_restarts_cleanly(self):
        half = len(GOOD_GZ) // 2
        self.data.mkdir()
        self.part.write_bytes(GOOD_GZ[:half])
        with Server(GOOD_GZ, honour_range=False) as server:
            self.write_pins(url=server.url)
            result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.gz.read_bytes(), GOOD_GZ)

    def test_oversized_part_restarts(self):
        self.data.mkdir()
        self.part.write_bytes(GOOD_GZ + b"junk")
        with Server(GOOD_GZ) as server:
            self.write_pins(url=server.url)
            result = self.run_script()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(self.gz.read_bytes(), GOOD_GZ)

    def test_corrupt_part_fails_then_the_next_run_recovers(self):
        half = len(GOOD_GZ) // 2
        self.data.mkdir()
        self.part.write_bytes(flipped(GOOD_GZ[:half], 50))
        with Server(GOOD_GZ) as server:
            self.write_pins(url=server.url)
            self.assert_failed(self.run_script(), EXIT_GZ_SHA256, "compressed SHA-256 mismatch")
            self.assertFalse(self.part.exists())
            self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(self.gz.read_bytes(), GOOD_GZ)

    def test_second_writer_is_refused(self):
        self.data.mkdir()
        self.write_pins()
        lock = os.open(self.data, os.O_RDONLY)
        self.addCleanup(os.close, lock)
        fcntl.flock(lock, fcntl.LOCK_EX)
        self.assert_failed(self.run_script(), EXIT_LOCAL, "another fetch holds")

    def test_existing_gz_is_not_downloaded_again(self):
        self.data.mkdir()
        self.gz.write_bytes(GOOD_GZ)
        with Server(GOOD_GZ) as server:
            self.write_pins(url=server.url)
            self.assertEqual(self.run_script().returncode, 0)
            self.assertEqual(server.range_headers, [])
        self.assertEqual(self.out.read_bytes(), PAYLOAD)

    def test_existing_bad_gz_fails_and_is_left_for_the_user(self):
        self.data.mkdir()
        damaged = flipped(GOOD_GZ, 40)
        self.gz.write_bytes(damaged)
        self.write_pins()
        self.assert_failed(self.run_script(), EXIT_GZ_SHA256, "compressed SHA-256 mismatch")
        self.assertEqual(self.gz.read_bytes(), damaged)
        self.assertFalse(self.out.exists())


class FromPath(FixtureCase):
    def test_source_is_not_changed_or_moved(self):
        self.write_pins()
        self.assertEqual(self.verify_copy(GOOD_GZ).returncode, 0)
        self.assertEqual(self.source.read_bytes(), GOOD_GZ)

    def test_bad_source_gets_no_symlink(self):
        self.write_pins()
        self.assert_failed(self.verify_copy(flipped(GOOD_GZ, 30)), EXIT_GZ_SHA256,
                           "compressed SHA-256 mismatch")
        self.assert_nothing_published()

    def test_missing_source(self):
        self.write_pins()
        self.assert_failed(self.run_script("--from", str(self.root / "absent.gz")),
                           EXIT_LOCAL, "does not exist")
        self.assert_nothing_published()

    def test_source_already_in_the_data_dir_stays_a_regular_file(self):
        self.write_pins()
        self.data.mkdir()
        self.gz.write_bytes(GOOD_GZ)
        self.assertEqual(self.run_script("--from", str(self.gz)).returncode, 0)
        self.assertFalse(self.gz.is_symlink())

    def test_all_days_is_rejected(self):
        self.write_pins()
        result = self.run_script("--from", str(self.source), "--day", "all")
        self.assertEqual(result.returncode, 2)


class VerifyOnly(FixtureCase):
    def test_passes_on_good_files(self):
        self.write_pins()
        self.assertEqual(self.verify_copy(GOOD_GZ).returncode, 0)
        self.assertEqual(self.run_script("--verify-only").returncode, 0)

    def test_writes_nothing(self):
        self.write_pins()
        self.data.mkdir()
        self.gz.write_bytes(GOOD_GZ)
        self.assertEqual(self.run_script("--verify-only").returncode, 0)
        self.assertEqual(os.listdir(self.data), [self.gz.name])

    def test_from_does_not_link(self):
        self.write_pins()
        self.source.write_bytes(GOOD_GZ)
        result = self.run_script("--verify-only", "--from", str(self.source))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(self.data.exists())

    def test_never_downloads(self):
        with Server(GOOD_GZ) as server:
            self.write_pins(url=server.url)
            self.assert_failed(self.run_script("--verify-only"), EXIT_LOCAL, "nothing to verify")
            self.assertEqual(server.range_headers, [])

    def test_catches_a_damaged_gz(self):
        self.write_pins()
        self.data.mkdir()
        self.gz.write_bytes(flipped(GOOD_GZ, 60))
        self.assert_failed(self.run_script("--verify-only"), EXIT_GZ_SHA256,
                           "compressed SHA-256 mismatch")

    def test_catches_a_damaged_output_and_leaves_it(self):
        self.write_pins()
        self.assertEqual(self.verify_copy(GOOD_GZ).returncode, 0)
        self.out.write_bytes(flipped(PAYLOAD, 9))
        self.assert_failed(self.run_script("--verify-only"), EXIT_SHA256,
                           "decompressed SHA-256 mismatch")
        self.assertTrue(self.out.exists())

    def test_catches_a_short_output(self):
        self.write_pins()
        self.assertEqual(self.verify_copy(GOOD_GZ).returncode, 0)
        self.out.write_bytes(PAYLOAD[:-1])
        self.assert_failed(self.run_script("--verify-only"), EXIT_SIZE,
                           "decompressed size mismatch")


class Pins(FixtureCase):
    def test_missing_file(self):
        self.assert_failed(self.run_script(), EXIT_PINS, "cannot read pins file")

    def test_malformed_json(self):
        self.pins_path.write_text("{")
        self.assert_failed(self.run_script(), EXIT_PINS, "cannot read pins file")

    def test_day_not_pinned(self):
        self.write_pins()
        self.assert_failed(self.run_script("--day", "10302019"), EXIT_PINS, "no pin for")

    def test_empty_hash_cannot_pass(self):
        for field in ("gz_sha256", "decompressed_sha256"):
            with self.subTest(field=field):
                self.write_pins(**{field: None})
                self.assert_failed(self.verify_copy(GOOD_GZ), EXIT_PINS, "cannot pass")
                self.assert_nothing_published()

    def test_name_cannot_leave_the_data_dir(self):
        self.write_pins(name="../escaped")
        self.assert_failed(self.verify_copy(GOOD_GZ), EXIT_PINS, "bad name")

    def test_compute_pins_fills_only_empty_hashes(self):
        self.write_pins(gz_sha256=None, decompressed_sha256=None)
        result = self.verify_copy(GOOD_GZ, "--compute-pins")
        self.assertEqual(result.returncode, 0, result.stderr)
        pin = json.loads(self.pins_path.read_text())["days"][DAY]
        self.assertEqual(pin["gz_sha256"], sha256(GOOD_GZ))
        self.assertEqual(pin["decompressed_sha256"], sha256(PAYLOAD))
        self.assertEqual(self.run_script("--verify-only").returncode, 0)

    def test_compute_pins_cannot_override_a_set_hash(self):
        self.write_pins(gz_sha256=sha256(b"other"), decompressed_sha256=None)
        self.assert_failed(self.verify_copy(GOOD_GZ, "--compute-pins"), EXIT_GZ_SHA256,
                           "compressed SHA-256 mismatch")

    def test_compute_pins_still_checks_sizes_and_gzip(self):
        for label, content, overrides, code in (
            ("gz size", GOOD_GZ, {"gz_size": 1}, EXIT_GZ_SIZE),
            ("gzip", flipped(GOOD_GZ, len(GOOD_GZ) - 8), {}, EXIT_GZIP),
            ("size", GOOD_GZ, {"decompressed_size": 1}, EXIT_SIZE),
        ):
            with self.subTest(label=label):
                self.write_pins(gz_sha256=None, decompressed_sha256=None, **overrides)
                result = self.verify_copy(content, "--compute-pins")
                self.assertEqual(result.returncode, code, result.stderr)
                pin = json.loads(self.pins_path.read_text())["days"][DAY]
                self.assertIsNone(pin["gz_sha256"])


if __name__ == "__main__":
    unittest.main()
