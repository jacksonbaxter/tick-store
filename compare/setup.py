#!/usr/bin/env python3
"""Builds the comparison environment from a clean checkout, on either host.

    python3 -I compare/setup.py [--vendor DIR] [--venv DIR] [--uv PATH]

Makes a virtual environment from the tracked compare/requirements.lock, by hash, then fetches
QuestDB and a JDK from their official releases, checks each against its pin here, and unpacks
them under vendor/, which is gitignored. Linux's QuestDB bundles its own runtime and needs a
JDK only to compile the commit-mode probe. An archive is untrusted until its hash matches, so
each goes into a new empty directory and nothing in it is run until then. Nothing is written
outside the checkout's vendor/ and .venv/ unless a directory is given.

Standard library only.
"""

import argparse
import hashlib
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
LOCK = REPO_ROOT / "compare" / "requirements.lock"
PROBE = REPO_ROOT / "compare" / "java" / "CommitModeProbe.java"
PYTHON_VERSION = "3.13"
QUESTDB_RELEASES = "https://github.com/questdb/questdb/releases/download/10.1.0"
TEMURIN_RELEASES = ("https://github.com/adoptium/temurin25-binaries/releases/download/"
                    "jdk-25.0.5%2B8")
JAVA_RELEASE = "25"

ARCHIVES = {
    "Darwin": {
        "questdb": (f"{QUESTDB_RELEASES}/questdb-10.1.0-no-jre-bin.tar.gz",
                    "5d84ca94bb46eacf53fc0d27b30e50d82fa13324069e073a67a60c0cf0de5cff",
                    "questdb-10.1.0-no-jre-bin"),
        "jdk": (f"{TEMURIN_RELEASES}/OpenJDK25U-jdk_aarch64_mac_hotspot_25.0.5_8.tar.gz",
                "36dc14d797aaff6d89aab302cd053fdba6207dfdcf772052fa7b023a94b0e857",
                "jdk-25.0.5+8"),
    },
    "Linux": {
        "questdb": (f"{QUESTDB_RELEASES}/questdb-10.1.0-rt-linux-x86-64.tar.gz",
                    "567994861a032bd939588ed207e4f074db975c74ea5386cd2e01c8d68136b244",
                    "questdb-10.1.0-rt-linux-x86-64"),
        "jdk": (f"{TEMURIN_RELEASES}/OpenJDK25U-jdk_x64_linux_hotspot_25.0.5_8.tar.gz",
                "68982529869257d0ee6b824ec5aa270ff8b118bb438670da0b97ebdab178d7fb",
                "jdk-25.0.5+8"),
    },
}


class SetupError(Exception):
    pass


def sha256_of(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        while chunk := source.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(url, expected, directory):
    """The archive, in a directory of its own, once its hash matches."""
    directory.mkdir(parents=True, exist_ok=True)
    path = directory / url.rsplit("/", 1)[1]
    if not path.exists() or sha256_of(path) != expected:
        with urllib.request.urlopen(url, timeout=600) as response, open(path, "wb") as out:
            shutil.copyfileobj(response, out)
    actual = sha256_of(path)
    if actual != expected:
        path.unlink()
        raise SetupError(f"{path.name}: sha256 {actual}, pinned {expected}")
    return path


def unpack(archive, destination, expected_top):
    """Unpacks with tarfile's data filter, which refuses paths that leave the directory."""
    if (destination / expected_top).exists():
        return
    destination.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive) as tar:
        tar.extractall(destination, filter="data")
    if not (destination / expected_top).exists():
        raise SetupError(f"{archive.name} did not unpack to {expected_top}")


def child_environment(vendor):
    cache = vendor / "cache"
    environment = dict(os.environ)
    environment.update({
        "UV_CACHE_DIR": str(cache / "uv"), "UV_PYTHON_INSTALL_DIR": str(vendor / "uv-python"),
        "UV_PYTHON_BIN_DIR": str(vendor / "uv-python-bin"), "UV_NO_MODIFY_PATH": "1",
        "XDG_CACHE_HOME": str(cache / "xdg"), "XDG_DATA_HOME": str(cache / "xdg-data"),
        "XDG_CONFIG_HOME": str(cache / "xdg-config"), "TMPDIR": str(cache / "tmp"),
    })
    (cache / "tmp").mkdir(parents=True, exist_ok=True)
    return environment


def make_venv(uv, venv, environment):
    if not (venv / "bin" / "python").exists():
        subprocess.run([uv, "venv", "--python", PYTHON_VERSION, str(venv)], check=True,
                       env=environment)
    subprocess.run([uv, "pip", "sync", "--python", str(venv / "bin" / "python"),
                    "--require-hashes", str(LOCK)], check=True, env=environment)


def compile_probe(vendor):
    home = vendor / "jdk" / "jdk-25.0.5+8"
    if platform.system() == "Darwin":
        home = home / "Contents" / "Home"
    classes = vendor / "questdb-probe"
    classes.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(home / "bin" / "javac"), "--release", JAVA_RELEASE, "-d", str(classes),
                    str(PROBE)], check=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--vendor", type=Path, default=REPO_ROOT / "vendor")
    parser.add_argument("--venv", type=Path, default=REPO_ROOT / ".venv")
    parser.add_argument("--uv", default=shutil.which("uv"))
    parser.add_argument("--skip-venv", action="store_true")
    args = parser.parse_args(argv)
    vendor = args.vendor.resolve()
    archives = ARCHIVES.get(platform.system())
    if archives is None:
        print(f"setup: no archives for {platform.system()}", file=sys.stderr)
        return 2
    environment = child_environment(vendor)
    try:
        if not args.skip_venv:
            if args.uv is None:
                raise SetupError("no uv: give --uv")
            make_venv(args.uv, args.venv.resolve(), environment)
        for name, (url, sha256, top) in archives.items():
            archive = fetch(url, sha256, vendor / "downloads" / name)
            unpack(archive, vendor / name, top)
            print(f"{name}: {top}, sha256 {sha256}")
        compile_probe(vendor)
    except (SetupError, OSError, subprocess.CalledProcessError) as error:
        print(f"setup: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
