"""Where everything lives, on either host, with no path written into the code.

A clean checkout keeps its data in data/, its downloads in vendor/ and its build in
build/. debian keeps these beside the exported tree, so each can be given separately.
Nothing here names a home directory, and every path stays out of the results.
"""

import os
import platform
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
SLICE_NAME = "12302019-open"
PINS_PATH = REPO_ROOT / "scripts" / "data_pins.json"


@dataclass(frozen=True)
class Layout:
    data: Path
    vendor: Path
    work: Path
    tools: Path

    @classmethod
    def default(cls):
        return cls(data=REPO_ROOT / "data", vendor=REPO_ROOT / "vendor",
                   work=REPO_ROOT / "build" / "compare",
                   tools=REPO_ROOT / "build" / "bench" / "tools")

    @classmethod
    def from_arguments(cls, args):
        default = cls.default()
        return cls(data=(args.data or default.data).resolve(),
                   vendor=(args.vendor or default.vendor).resolve(),
                   work=(args.work or default.work).resolve(),
                   tools=(args.tools or default.tools).resolve())

    def as_arguments(self):
        return ["--data", str(self.data), "--vendor", str(self.vendor),
                "--work", str(self.work), "--tools", str(self.tools)]

    def export_dir(self, name):
        return self.data / "export" / name

    @property
    def scratch(self):
        return self.work / "tmp"

    def tool(self, name):
        return self.tools / name

    def questdb_home(self):
        if platform.system() == "Linux":
            return self.vendor / "questdb" / "questdb-10.1.0-rt-linux-x86-64"
        return self.vendor / "questdb" / "questdb-10.1.0-no-jre-bin"

    def java(self):
        """The java that runs QuestDB: its bundled runtime on Linux, Temurin on macOS."""
        if platform.system() == "Linux":
            return self.questdb_home() / "bin" / "java"
        return self.jdk_home() / "bin" / "java"

    def jdk_home(self):
        """A full JDK, for compiling the probe. Linux needs one for that alone."""
        if platform.system() == "Linux":
            return self.vendor / "jdk" / "jdk-25.0.5+8"
        return self.vendor / "jdk" / "jdk-25.0.5+8" / "Contents" / "Home"

    def probe_classes(self):
        return self.vendor / "questdb-probe"


def child_environment(layout):
    """Everything a tool could write outside the folders is pointed back inside them."""
    scratch = layout.scratch
    for name in ("home", "xdg-cache", "xdg-config", "xdg-data", "xdg-state"):
        (scratch / name).mkdir(parents=True, exist_ok=True)
    environment = dict(os.environ)
    environment.update({
        "TMPDIR": str(scratch),
        "TMP": str(scratch),
        "TEMP": str(scratch),
        "HOME": str(scratch / "home"),
        "XDG_CACHE_HOME": str(scratch / "xdg-cache"),
        "XDG_CONFIG_HOME": str(scratch / "xdg-config"),
        "XDG_DATA_HOME": str(scratch / "xdg-data"),
        "XDG_STATE_HOME": str(scratch / "xdg-state"),
    })
    return environment
