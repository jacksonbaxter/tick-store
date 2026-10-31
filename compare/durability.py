"""The two durability classes, and the sync primitive each host offers.

Class A returns once data is in the OS page cache. Class B returns after a flush to the
device. BENCHMARKS.md defines them. The two are never compared with each other.
"""

import fcntl
import os
import platform
from pathlib import Path

CLASS_A = "A"
CLASS_B = "B"
CLASSES = (CLASS_A, CLASS_B)


def harness_sync_primitive():
    return "F_FULLFSYNC" if platform.system() == "Darwin" else "fsync"


def sync_descriptor(descriptor):
    if platform.system() == "Darwin":
        fcntl.fcntl(descriptor, fcntl.F_FULLFSYNC)
    else:
        os.fsync(descriptor)


def sync_path(path):
    """Flushes one file or directory to the device, with the host's strongest primitive."""
    descriptor = os.open(path, os.O_RDONLY)
    try:
        sync_descriptor(descriptor)
    finally:
        os.close(descriptor)


def sync_tree(root):
    """Flushes every file under `root`, then every directory, deepest first."""
    root = Path(root)
    directories = [root]
    for path in root.rglob("*"):
        if path.is_symlink():
            continue
        if path.is_dir():
            directories.append(path)
        elif path.is_file():
            sync_path(path)
    for directory in sorted(directories, key=lambda item: len(item.parts), reverse=True):
        sync_path(directory)
