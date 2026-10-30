#!/usr/bin/env python3
"""Fetch and verify the pinned Nasdaq TotalView-ITCH 5.0 sample days.

    python3 -I scripts/fetch_data.py [--day 12302019|10302019|all] [--from PATH]
                                     [--data-dir data] [--verify-only] [--pins PATH]

The reference day is 12302019, the same file as the sibling project itch-orderbook,
so results on the two projects can be compared. 10302019 is the second day. Python
standard library only, 3.11 or newer, and nothing outside this file and its pins.

Trust. Nasdaq publishes no usable checksum: every `.md5sum` sidecar listed next to
these files answers 404, and the only value the publisher itself provides is the
gzip CRC32 and ISIZE inside the file. CRC32 detects accidents, not substitution,
and ISIZE only holds the size modulo 2^32. So the SHA-256 values in
`scripts/data_pins.json` were computed by downloading the files with this script.
They prove every later copy is the same bytes as that download. They do not prove
that download matched what Nasdaq originally wrote.

Verification. Each step stops the run with a clear message and its own exit code:

    1. compressed size against the pin                      exit 20
    2. compressed SHA-256 against the pin                   exit 21
    3. gzip stream: deflate data, CRC32, ISIZE, truncation,
       and no bytes after the trailer                       exit 22
    4. decompressed size against the pin (exact, unlike
       ISIZE, which wraps on files above 4 GiB)             exit 23
    5. decompressed SHA-256 against the pin                 exit 24

Other exit codes: 0 success; 2 bad command line; 10 download failed; 11 pins file
missing, malformed, or holding an empty hash without --compute-pins; 12 local file
missing or unwritable. A download failure is therefore always 10 and a failed check
always 20 to 24.

Files, under --data-dir:

    <name>.gz.part   download in progress. Resumed with an HTTP Range request.
    <name>.gz        renamed from .part only after every check passes.
    <name>.tmp       decompressed output in progress, deleted on any failure.
    <name>           renamed from .tmp only after every check passes.

An interrupted run is safe to repeat. A partial .part resumes, and a server that
ignores the Range request restarts it from zero. A resumed file that came out wrong
fails step 1, 2 or 3, and the .part is then discarded so the next run starts clean.

--from PATH verifies an existing copy of the .gz by exactly the same steps, then
makes `<name>.gz` a symlink to it, so the machine does not hold two copies.

--verify-only downloads nothing and writes nothing. It runs the steps on `<name>.gz`
or on the --from path, then checks `<name>` too if it exists.

--compute-pins fills empty hash fields in the pins file from the file at hand, after
every other check passes. Without it an empty hash is an error, so a pin that was
never computed cannot pass. It never replaces a hash that is already set.

`message_count` in the pins is a record, not a check: this script does not parse
messages. It comes from walking the 2-byte big-endian length prefixes of `<name>`.
"""

import argparse
import fcntl
import hashlib
import http.client
import json
import os
import re
import signal
import sys
import time
import urllib.error
import urllib.request
import zlib
from pathlib import Path

REFERENCE_DAY = "12302019"
DAYS = ("12302019", "10302019")

EXIT_OK = 0
EXIT_DOWNLOAD = 10
EXIT_PINS = 11
EXIT_LOCAL = 12
EXIT_GZ_SIZE = 20
EXIT_GZ_SHA256 = 21
EXIT_GZIP = 22
EXIT_SIZE = 23
EXIT_SHA256 = 24

CHUNK = 1 << 20
MAX_OUTPUT_PER_STEP = 16 * CHUNK
NETWORK_TIMEOUT_SECONDS = 60
MAX_FAILED_ATTEMPTS = 8
BACKOFF_SECONDS = float(os.environ.get("FETCH_DATA_BACKOFF", "2"))
PROGRESS_STEP = 1 << 30

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_PINS = REPO_ROOT / "scripts" / "data_pins.json"
DEFAULT_DATA_DIR = REPO_ROOT / "data"


class Failure(Exception):
    """A failure with its exit code. The message is what the user sees."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def log(message):
    print(message, file=sys.stderr, flush=True)


def load_pins(path):
    try:
        with open(path, encoding="utf-8") as f:
            pins = json.load(f)
    except (OSError, ValueError) as e:
        raise Failure(EXIT_PINS, f"cannot read pins file {path}: {e}") from e
    days = pins.get("days") if isinstance(pins, dict) else None
    if not isinstance(days, dict):
        raise Failure(EXIT_PINS, f"{path}: no 'days' object")
    for day, pin in days.items():
        check_pin(day, pin)
    return pins


def check_pin(day, pin):
    if not isinstance(pin, dict):
        raise Failure(EXIT_PINS, f"pin {day}: not an object")
    # The name becomes a path, so it must not be able to leave the data directory.
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", str(pin.get("name"))):
        raise Failure(EXIT_PINS, f"pin {day}: bad name {pin.get('name')!r}")
    if not isinstance(pin.get("url"), str):
        raise Failure(EXIT_PINS, f"pin {day}: no url")
    for field in ("gz_size", "decompressed_size"):
        value = pin.get(field)
        if not isinstance(value, int) or isinstance(value, bool) or value < 0:
            raise Failure(EXIT_PINS, f"pin {day}: bad {field} {value!r}")
    for field in ("gz_sha256", "decompressed_sha256"):
        value = pin.get(field)
        if value is not None and not re.fullmatch(r"[0-9a-f]{64}", str(value)):
            raise Failure(EXIT_PINS, f"pin {day}: bad {field} {value!r}")


def require_hashes(day, pin):
    empty = [f for f in ("gz_sha256", "decompressed_sha256") if pin.get(f) is None]
    if empty:
        raise Failure(EXIT_PINS, f"pin {day}: {', '.join(empty)} empty; "
                                 "an empty hash cannot pass (see --compute-pins)")


def write_pins(path, pins):
    tmp = Path(f"{path}.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(pins, f, indent=2)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def fsync_dir(path):
    fd = os.open(path, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def expected_range_start(content_range):
    match = re.fullmatch(r"bytes (\d+)-\d+/(\d+|\*)", (content_range or "").strip())
    return int(match.group(1)) if match else None


def declared_total(response):
    """The whole-file size the server claims, or None if it does not say."""
    content_range = response.headers.get("Content-Range")
    if content_range:
        match = re.search(r"/(\d+)\s*$", content_range)
        return int(match.group(1)) if match else None
    if response.status == 200:
        length = response.headers.get("Content-Length")
        return int(length) if length and length.isdigit() else None
    return None


def fetch_once(url, part, have, size):
    """Make one request and append what arrives. Network errors propagate."""
    request = urllib.request.Request(url, headers={
        "User-Agent": "tick-store-fetch-data/1",
        "Accept-Encoding": "identity",
    })
    if have:
        request.add_header("Range", f"bytes={have}-")
    with urllib.request.urlopen(request, timeout=NETWORK_TIMEOUT_SECONDS) as response:
        if have and response.status == 206:
            if expected_range_start(response.headers.get("Content-Range")) != have:
                log("server answered a different range; restarting from zero")
                part.unlink()
                return
        elif have:
            log(f"server ignored the Range request (HTTP {response.status}); "
                "restarting from zero")
            part.unlink()
            have = 0
        total = declared_total(response)
        if total is not None and total != size:
            raise Failure(EXIT_GZ_SIZE, f"compressed size: server says {total:,} bytes, "
                                        f"pin says {size:,}: {url}")
        next_report = (have // PROGRESS_STEP + 1) * PROGRESS_STEP
        started, start_have = time.monotonic(), have
        try:
            out = open(part, "ab")
        except OSError as e:
            raise Failure(EXIT_LOCAL, f"cannot write {part}: {e}") from e
        with out:
            # Reads one byte past the pin at most, which download() then rejects.
            while have <= size:
                chunk = response.read(min(CHUNK, size + 1 - have))
                if not chunk:
                    break
                try:
                    out.write(chunk)
                except OSError as e:
                    raise Failure(EXIT_LOCAL, f"cannot write {part}: {e}") from e
                have += len(chunk)
                if have >= next_report:
                    rate = (have - start_have) / max(time.monotonic() - started, 1e-9)
                    log(f"{part.name}: {have:,} of {size:,} bytes, {rate / 1e6:.1f} MB/s")
                    next_report += PROGRESS_STEP
            try:
                out.flush()
                os.fsync(out.fileno())
            except OSError as e:
                raise Failure(EXIT_LOCAL, f"cannot write {part}: {e}") from e


def download(url, part, size):
    """Bring `part` to `size` bytes, resuming across dropped connections."""
    failed_attempts = 0
    while True:
        have = part.stat().st_size if part.exists() else 0
        if have > size:
            log(f"{part.name} is larger than the pin; restarting from zero")
            part.unlink()
            have = 0
        if have == size:
            return
        reason = None
        try:
            fetch_once(url, part, have, size)
        except urllib.error.HTTPError as e:
            if e.code == 416:
                log("server rejected the resume offset; restarting from zero")
                part.unlink(missing_ok=True)
            elif e.code in (408, 429) or e.code >= 500:
                reason = f"HTTP {e.code}"
            else:
                raise Failure(EXIT_DOWNLOAD, f"download failed: HTTP {e.code}: {url}") from e
        except (OSError, http.client.HTTPException) as e:
            reason = str(e) or type(e).__name__
        now = part.stat().st_size if part.exists() else 0
        if now == size:
            return
        if now > size:
            part.unlink()
            raise Failure(EXIT_GZ_SIZE, f"compressed size: server sent more than the "
                                        f"pinned {size:,} bytes: {url}")
        if now > have:
            failed_attempts = 0
            reason = reason or "connection ended early"
        else:
            failed_attempts += 1
            reason = reason or "no bytes received"
        if failed_attempts >= MAX_FAILED_ATTEMPTS:
            raise Failure(EXIT_DOWNLOAD, f"download failed after {failed_attempts} attempts "
                                         f"without progress: {reason}: {url}")
        delay = min(BACKOFF_SECONDS * 2 ** failed_attempts, 60)
        log(f"{reason}; retrying from byte {now:,} in {delay:g} s")
        time.sleep(delay)


def sha256_of(path):
    with open(path, "rb") as f:
        return hashlib.file_digest(f, "sha256").hexdigest()


def compare_hash(label, measured, pinned, code, where):
    if pinned is not None and measured != pinned:
        raise Failure(code, f"{label} mismatch: {where} is {measured}, pin says {pinned}")


def check_compressed(path, pin):
    """Steps 1 and 2. Returns the measured SHA-256."""
    actual = path.stat().st_size
    if actual != pin["gz_size"]:
        raise Failure(EXIT_GZ_SIZE, f"compressed size mismatch: {path.name} is {actual:,} "
                                    f"bytes, pin says {pin['gz_size']:,}")
    digest = sha256_of(path)
    compare_hash("compressed SHA-256", digest, pin.get("gz_sha256"), EXIT_GZ_SHA256, path.name)
    return digest


def check_stream(path, pin, sink):
    """Steps 3 to 5: stream `path` through zlib, writing to `sink` if given.

    With wbits=31 zlib parses the gzip header and verifies the CRC32 and ISIZE when it
    reaches the trailer. It reports neither a missing trailer nor bytes after it, so
    both are checked here: `eof` stays false on a truncated file, and anything after
    the trailer lands in `unused_data`. Output per step is bounded, so a hostile file
    cannot exhaust memory, and stopping at the pinned size protects the disk.
    """
    limit = pin["decompressed_size"]
    inflater = zlib.decompressobj(wbits=31)
    digest = hashlib.sha256()
    written = 0
    with open(path, "rb") as source:
        while not inflater.eof:
            # zlib may hold output back even when all input is consumed, so an empty
            # read only means truncation once a step also produces no output.
            chunk = inflater.unconsumed_tail or source.read(CHUNK)
            try:
                data = inflater.decompress(chunk, MAX_OUTPUT_PER_STEP)
            except zlib.error as e:
                raise Failure(EXIT_GZIP, f"gzip check failed on {path.name}: {e}") from e
            if not data and not chunk:
                break
            written += len(data)
            if written > limit:
                raise Failure(EXIT_SIZE, f"decompressed size mismatch: {path.name} exceeds "
                                         f"the pinned {limit:,} bytes")
            digest.update(data)
            if sink is not None:
                try:
                    sink.write(data)
                except OSError as e:
                    raise Failure(EXIT_LOCAL, f"cannot write {sink.name}: {e}") from e
        if not inflater.eof:
            raise Failure(EXIT_GZIP, f"gzip check failed on {path.name}: truncated, the "
                                     f"stream ends after {written:,} decompressed bytes "
                                     "with no complete trailer")
        if inflater.unused_data or source.read(1):
            raise Failure(EXIT_GZIP, f"gzip check failed on {path.name}: "
                                     "data after the gzip trailer")
    if written != limit:
        raise Failure(EXIT_SIZE, f"decompressed size mismatch: {path.name} gives "
                                 f"{written:,} bytes, pin says {limit:,}")
    measured = digest.hexdigest()
    compare_hash("decompressed SHA-256", measured, pin.get("decompressed_sha256"),
                 EXIT_SHA256, path.name)
    return measured


def check_decompressed_file(path, pin):
    actual = path.stat().st_size
    if actual != pin["decompressed_size"]:
        raise Failure(EXIT_SIZE, f"decompressed size mismatch: {path.name} is {actual:,} "
                                 f"bytes, pin says {pin['decompressed_size']:,}")
    compare_hash("decompressed SHA-256", sha256_of(path), pin.get("decompressed_sha256"),
                 EXIT_SHA256, path.name)


def existing_output_is_good(out, pin):
    if not out.exists():
        return False
    if pin.get("decompressed_sha256") is None:
        return False
    try:
        check_decompressed_file(out, pin)
    except Failure as e:
        log(f"replacing existing {out.name}: {e}")
        out.unlink()
        return False
    return True


def gzip_trailer(path):
    with open(path, "rb") as f:
        f.seek(-8, os.SEEK_END)
        trailer = f.read(8)
    return trailer[:4][::-1].hex(), int.from_bytes(trailer[4:], "little")


def summarize(day, source, pin, gz_sha256, sha256):
    crc32, isize = gzip_trailer(source)
    print(f"ok {day} gz_size={pin['gz_size']} gz_sha256={gz_sha256} crc32={crc32} "
          f"isize={isize} decompressed_size={pin['decompressed_size']} "
          f"decompressed_sha256={sha256}", flush=True)


def verify_only(day, pin, gz, out, from_path):
    source = Path(from_path) if from_path else gz
    if not source.is_file():
        raise Failure(EXIT_LOCAL, f"nothing to verify: {source} does not exist")
    gz_sha256 = check_compressed(source, pin)
    sha256 = check_stream(source, pin, None)
    if out.exists():
        check_decompressed_file(out, pin)
    else:
        log(f"{out.name} is not present, so only the stream was checked")
    summarize(day, source, pin, gz_sha256, sha256)
    return gz_sha256, sha256


def link_copy(source, gz):
    if gz.exists() and os.path.samefile(source, gz):
        return
    link = Path(f"{gz}.link")
    link.unlink(missing_ok=True)
    os.symlink(source, link)
    os.replace(link, gz)


def fetch_day(day, pin, data_dir, from_path):
    gz = data_dir / f"{pin['name']}.gz"
    part = data_dir / f"{pin['name']}.gz.part"
    out = data_dir / pin["name"]
    tmp = data_dir / f"{pin['name']}.tmp"

    if from_path:
        candidate = Path(from_path).resolve()
        if not candidate.is_file():
            raise Failure(EXIT_LOCAL, f"--from file does not exist: {from_path}")
    elif gz.exists():
        candidate = gz
    else:
        download(pin["url"], part, pin["gz_size"])
        candidate = part

    keep_output = existing_output_is_good(out, pin)
    try:
        gz_sha256 = check_compressed(candidate, pin)
        if keep_output:
            sha256 = check_stream(candidate, pin, None)
        else:
            with open(tmp, "wb") as sink:
                sha256 = check_stream(candidate, pin, sink)
                sink.flush()
                os.fsync(sink.fileno())
            os.replace(tmp, out)
    except Failure as e:
        if candidate == part and e.code in (EXIT_GZ_SIZE, EXIT_GZ_SHA256, EXIT_GZIP):
            part.unlink(missing_ok=True)
            e.args = (f"{e} (discarded {part.name}; run again to download afresh)",)
        raise
    finally:
        tmp.unlink(missing_ok=True)

    if from_path:
        link_copy(candidate, gz)
    elif candidate == part:
        os.replace(part, gz)
    part.unlink(missing_ok=True)
    fsync_dir(data_dir)
    summarize(day, gz, pin, gz_sha256, sha256)
    return gz_sha256, sha256


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Fetch and verify the pinned Nasdaq ITCH days. Details and exit "
                    "codes are at the top of this script.")
    parser.add_argument("--day", choices=(*DAYS, "all"), default=REFERENCE_DAY,
                        help=f"day to fetch (default {REFERENCE_DAY})")
    parser.add_argument("--from", dest="from_path", metavar="PATH",
                        help="verify an existing .gz and symlink it instead of downloading")
    parser.add_argument("--data-dir", type=Path, default=DEFAULT_DATA_DIR,
                        help="data directory (default: data/ in this repo)")
    parser.add_argument("--verify-only", action="store_true",
                        help="verify what is on disk; download and write nothing")
    parser.add_argument("--pins", type=Path, default=DEFAULT_PINS,
                        help="pins file (default: scripts/data_pins.json)")
    parser.add_argument("--compute-pins", action="store_true",
                        help="fill empty hashes in the pins file from a passing file")
    args = parser.parse_args(argv)
    if args.from_path and args.day == "all":
        parser.error("--from names one file, so it needs one --day")
    if args.compute_pins and args.verify_only:
        parser.error("--compute-pins writes the pins file, so it cannot be --verify-only")
    return args


def run(args):
    pins = load_pins(args.pins)
    days = [d for d in DAYS if d in pins["days"]] if args.day == "all" else [args.day]
    if not days or any(d not in pins["days"] for d in days):
        raise Failure(EXIT_PINS, f"{args.pins}: no pin for {args.day}")
    if not args.compute_pins:
        for day in days:
            require_hashes(day, pins["days"][day])

    data_dir = args.data_dir
    if not args.verify_only:
        try:
            data_dir.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            raise Failure(EXIT_LOCAL, f"cannot create {data_dir}: {e}") from e
    lock = None
    try:
        if not args.verify_only:
            # Two writers appending to one .part would corrupt it.
            lock = os.open(data_dir, os.O_RDONLY)
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as e:
                raise Failure(EXIT_LOCAL, f"another fetch holds {data_dir}") from e
        for day in days:
            pin = pins["days"][day]
            try:
                if args.verify_only:
                    gz_sha256, sha256 = verify_only(day, pin, data_dir / f"{pin['name']}.gz",
                                                    data_dir / pin["name"], args.from_path)
                else:
                    gz_sha256, sha256 = fetch_day(day, pin, data_dir, args.from_path)
            except OSError as e:
                raise Failure(EXIT_LOCAL, f"{day}: {e}") from e
            if args.compute_pins:
                pin["gz_sha256"] = pin.get("gz_sha256") or gz_sha256
                pin["decompressed_sha256"] = pin.get("decompressed_sha256") or sha256
                write_pins(args.pins, pins)
    finally:
        if lock is not None:
            os.close(lock)


def main(argv=None):
    args = parse_args(argv)
    # The default action would skip the cleanup of .tmp.
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    try:
        run(args)
    except Failure as e:
        log(f"FAILED: {e}")
        return e.code
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
