"""QuestDB, as a server the harness starts and always stops.

The server runs from `java` directly, with the options bin/questdb.sh passes, so that the
harness holds its process and nothing runs in the background of a wrapper. It binds to
127.0.0.1 only, telemetry is off, and its root is inside the work folder. A single class of
failure is fatal here: a commit mode that the harness cannot prove. See compare/settings.py.
"""

import json
import os
import platform
import re
import resource
import signal
import socket
import subprocess
import time
import urllib.parse
import urllib.request
from pathlib import Path

from compare.layout import child_environment
from compare.schema import TABLE_COLUMNS
from compare.settings import (MODE_FOR_CLASS, Setting, SettingsMismatch, check_commit_mode,
                              check_commit_mode_requested, require_all_match)
from compare.systems.base import System

HOST = "127.0.0.1"
HTTP_PORT = 9000
PORTS = (HTTP_PORT, 9003, 8812, 9009)
TABLE = "ticks"
PROBE_TABLE = "flush_probe"
HEAP = "1g"
DEFAULT_HEAP = "default"  # No -Xmx at all. Only a full-day run may ask for this.
START_TIMEOUT_SECONDS = 60
STREAM_CHUNK_ROWS = 1 << 22
STOP_TIMEOUT_SECONDS = 60
WAL_PURGE_TIMEOUT_SECONDS = 150
EVIDENCE_PLAN = "Interval forward scan"
PROBE_CLASS = "CommitModeProbe"
LAUNCHER_TAG = "tick-store"
WAL_POLL_SECONDS = 2
LINUX_JEMALLOC = "libjemalloc.so"
# Beyond bin/questdb.sh's options: the JVM prints its own properties to the log as it starts.
SHOW_PROPERTIES = "-XshowSettings:properties"
PROPERTY_BLOCK = "Property settings:"
PROPERTY_LINE = re.compile(r"^\s+([\w.]+) = (.*)$")

COLUMN_TYPES = {"seq": "LONG", "ts": "TIMESTAMP_NS", "msg_type": "SHORT", "locate": "INT",
                "tracking": "INT", "order_ref": "LONG", "new_ref": "LONG", "side": "BYTE",
                "shares": "INT", "price": "INT", "match": "LONG", "printable": "BYTE",
                "stock": "SYMBOL", "mpid": "SYMBOL", "rest": "VARCHAR"}

JAVA_OPTIONS = (
    "-Dcontainerized=false", "-ea", "-Dnoebug", "-XX:+UnlockExperimentalVMOptions",
    "-XX:+AlwaysPreTouch", "-XX:+UseParallelGC", "--sun-misc-unsafe-memory-access=allow",
    "--enable-native-access=io.questdb", "--add-opens=java.base/java.lang=io.questdb",
    "--add-opens=java.base/java.lang.reflect=io.questdb",
    "--add-opens=java.base/java.nio=io.questdb",
    "--add-opens=java.base/java.time.zone=io.questdb",
    "--add-exports=java.base/jdk.internal.vm=io.questdb",
)
PROBE_OPTIONS = tuple(option for option in JAVA_OPTIONS if option.startswith("--"))

BASE_CONF = {
    "telemetry.enabled": "false",
    "telemetry.disable.completely": "true",
    "posthog.enabled": "false",
    "http.net.bind.to": f"{HOST}:{HTTP_PORT}",
    "http.bind.to": f"{HOST}:{HTTP_PORT}",
    "http.min.net.bind.to": f"{HOST}:9003",
    "pg.net.bind.to": f"{HOST}:8812",
    "line.tcp.net.bind.to": f"{HOST}:9009",
    "line.udp.bind.to": f"{HOST}:9009",
    "line.udp.enabled": "false",
    "qwp.udp.bind.to": f"{HOST}:9007",
    "qwp.udp.enabled": "false",
}
ECHOED_CONF_KEYS = ("telemetry.enabled", "line.udp.enabled", "http.net.bind.to",
                    "pg.net.bind.to", "line.tcp.net.bind.to")
OPEN_FILE_TARGET = 1048576  # QuestDB's capacity docs, which also ask this of vm.max_map_count.
WORKER_SETTINGS = ("shared.network.worker.count", "shared.query.worker.count",
                   "shared.write.worker.count")


class ServerFailure(Exception):
    """The server would not start, stop, or stay on its loopback address."""


def heap_flags(heap=HEAP):
    """Both bounds, or none for the JVM's own default of a quarter of RAM."""
    return [] if heap == DEFAULT_HEAP else [f"-Xms{heap}", f"-Xmx{heap}"]


def linux_death_signal():
    """A child made with this dies with its parent, on Linux."""
    if platform.system() != "Linux":
        return None
    import ctypes

    def request():
        ctypes.CDLL(None, use_errno=True).prctl(1, signal.SIGKILL)  # PR_SET_PDEATHSIG

    return request


def named_loopback(value):
    """No results file holds an address, even this one."""
    return value.replace(HOST, "loopback") if isinstance(value, str) else value


def raise_open_file_limit():
    """The soft limit up to what QuestDB's docs ask, as far as the hard limit allows."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    wanted = OPEN_FILE_TARGET if hard == resource.RLIM_INFINITY else min(hard, OPEN_FILE_TARGET)
    if soft == resource.RLIM_INFINITY or soft >= wanted:
        return
    try:
        resource.setrlimit(resource.RLIMIT_NOFILE, (wanted, hard))
    except (OSError, ValueError):
        pass  # The value in force goes into the results either way.


def host_limits():
    """What the docs ask of the host, against what it has. Neither can change without root."""
    limits = {"docs_ask_open_files": OPEN_FILE_TARGET,
              "open_files_soft_limit": resource.getrlimit(resource.RLIMIT_NOFILE)[0]}
    if platform.system() == "Linux":
        limits["docs_ask_max_map_count"] = OPEN_FILE_TARGET
        limits["vm_max_map_count"] = int(
            Path("/proc/sys/vm/max_map_count").read_text(encoding="ascii"))
    return limits


def jvm_properties(log_text):
    """The last set of properties a JVM printed in a log. A log holds one per server start."""
    _, found, block = log_text.rpartition(PROPERTY_BLOCK)
    if not found:
        raise ServerFailure("the server's log has no properties from its JVM")
    matches = (PROPERTY_LINE.match(line) for line in block.splitlines())
    return dict(match.groups() for match in matches if match)


def library_names(text, darwin):
    """File names from `lsof -Fn` on macOS, or /proc's maps on Linux."""
    if darwin:
        paths = (line[1:] for line in text.splitlines() if line.startswith("n"))
    else:
        paths = (line.split()[-1] for line in text.splitlines() if "/" in line)
    return {Path(path).name for path in paths}


def loaded_libraries(pid):
    """What is mapped into a running process, asked of the process and not of how it started."""
    if platform.system() == "Darwin":
        text = subprocess.run(["lsof", "-nP", "-p", str(pid), "-Fn"], check=False,
                              capture_output=True, text=True).stdout
        return library_names(text, darwin=True)
    return library_names(Path(f"/proc/{pid}/maps").read_text(encoding="utf-8", errors="replace"),
                         darwin=False)


def runtime_reading(properties, libraries):
    """QuestDB's runtime as read, for the results header. A process with no JVM mapped was not
    really read, and that must not pass for "no jemalloc"."""
    if not any(name.startswith("libjvm") for name in libraries):
        raise ServerFailure("the server's libraries could not be read")
    return {"jvm_vendor": properties["java.vendor"],
            "jvm_vendor_version": properties.get("java.vendor.version", "not stated"),
            "jvm_version": properties["java.runtime.version"],
            "jvm_read_from": f"the running JVM's own properties, printed by {SHOW_PROPERTIES}",
            "jemalloc_preloaded": any("jemalloc" in name for name in libraries),
            "jemalloc_read_from": "the libraries mapped into the running server"}


def port_in_use(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.settimeout(0.2)
        return probe.connect_ex((HOST, port)) == 0


def require_ports_free():
    taken = [port for port in PORTS if port_in_use(port)]
    if taken:
        raise ServerFailure(f"ports in use: {taken}")


def listening_addresses(pid):
    """Every TCP address the server listens on, from the host's own socket tools."""
    if platform.system() == "Darwin":
        text = subprocess.run(["lsof", "-nP", "-a", "-p", str(pid), "-iTCP", "-sTCP:LISTEN"],
                              check=False, capture_output=True, text=True).stdout
        return sorted({line.split()[8] for line in text.splitlines()[1:]})
    text = subprocess.run(["ss", "-ltnpH"], check=True, capture_output=True, text=True).stdout
    return sorted({line.split()[3] for line in text.splitlines() if f"pid={pid}," in line})


def http_get(path, timeout=120):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(f"http://{HOST}:{HTTP_PORT}{path}", timeout=timeout) as response:
        return json.load(response)


def sql(text, timeout=600):
    """One statement over HTTP, as a list of row dicts."""
    answer = http_get("/exec?" + urllib.parse.urlencode({"query": text}), timeout)
    if "error" in answer:
        raise ServerFailure(f"{text}: {answer['error']}")
    if "columns" not in answer:
        return []
    names = [column["name"] for column in answer["columns"]]
    return [dict(zip(names, row, strict=True)) for row in answer["dataset"]]


def nanos_text(day, nanos):
    seconds, fraction = divmod(nanos, 1_000_000_000)
    hours, rest = divmod(seconds, 3600)
    minutes, seconds = divmod(rest, 60)
    return f"{day.iso}T{hours:02d}:{minutes:02d}:{seconds:02d}.{fraction:09d}Z"


class QuestDBServer:
    """One server process. `stop` is safe to call at any time, and more than once."""

    def __init__(self, layout, root, threads, extra_conf, heap=HEAP, gc_log=None):
        self.layout = layout
        self.root = Path(root)
        self.threads = threads
        self.conf = {**BASE_CONF, **extra_conf}
        self.heap = heap
        self.gc_log = gc_log
        self.process = None
        self.peak_rss_bytes = 0

    @property
    def pid(self):
        return self.process.pid

    @property
    def heap_options(self):
        return heap_flags(self.heap)

    @property
    def heap_flag(self):
        return None if self.heap == DEFAULT_HEAP else f"-Xmx{self.heap}"

    @property
    def initial_heap_flag(self):
        return None if self.heap == DEFAULT_HEAP else f"-Xms{self.heap}"

    @property
    def conf_path(self):
        return self.root / "conf" / "server.conf"

    def write_conf(self):
        self.conf_path.parent.mkdir(parents=True, exist_ok=True)
        (self.root / "tmp").mkdir(exist_ok=True)
        (self.root / "log").mkdir(exist_ok=True)
        lines = [f"{key}={value}" for key, value in self.conf.items()]
        self.conf_path.write_text("\n".join(lines) + "\n", encoding="ascii")

    def command(self):
        layout = self.layout
        common = [str(layout.java()), "-DQuestDB-Runtime-tick-store", *JAVA_OPTIONS,
                  SHOW_PROPERTIES, *self.heap_options, f"-XX:ActiveProcessorCount={self.threads}",
                  "-XX:-UsePerfData", f"-Djava.io.tmpdir={self.root / 'tmp'}",
                  f"-XX:ErrorFile={self.root / 'log'}/hs_err_pid%p.log"]
        if platform.system() == "Darwin":
            common.append("-XX:-MaxFDLimit")
            module = ["-p", str(layout.questdb_home() / "questdb.jar")]
        else:
            common.append(f"-Dquestdb.libs.dir={layout.questdb_home() / 'lib'}")
            module = []
        return [*common, *module, "-m", "io.questdb/io.questdb.ServerMain", "-d", str(self.root)]

    def environment(self):
        environment = child_environment(self.layout)
        if platform.system() == "Linux":
            environment["LD_PRELOAD"] = str(self.layout.questdb_home() / "bin" / LINUX_JEMALLOC)
        return environment

    def start(self):
        require_ports_free()
        raise_open_file_limit()
        with open(self.root / "log" / "stdout.txt", "ab") as log:
            self.process = subprocess.Popen(
                self.command(), stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                env=self.environment(), preexec_fn=linux_death_signal())
        deadline = time.monotonic() + START_TIMEOUT_SECONDS
        while True:
            if self.process.poll() is not None:
                raise ServerFailure(f"the server exited with {self.process.returncode}")
            try:
                sql("SELECT 1", timeout=5)
                break
            except (OSError, ValueError, ServerFailure):
                if time.monotonic() > deadline:
                    raise ServerFailure("the server never answered") from None
                time.sleep(0.05)
        listening = listening_addresses(self.process.pid)
        foreign = [address for address in listening if not address.startswith(f"{HOST}:")]
        if foreign or not listening:
            raise ServerFailure(f"not on loopback alone: {listening}")
        return listening

    def stop(self):
        """Stops the server, and fails if it will not go or leaves a port taken."""
        process = self.process
        self.process = None
        if process is not None and process.returncode is None:
            process.send_signal(signal.SIGTERM)
            deadline = time.monotonic() + STOP_TIMEOUT_SECONDS
            forced = False
            while True:
                pid, status, usage = os.wait4(process.pid, os.WNOHANG)
                if pid == process.pid:
                    process.returncode = os.waitstatus_to_exitcode(status)
                    break
                if not forced and time.monotonic() > deadline:
                    process.kill()
                    forced = True
                time.sleep(0.02)
            scale = 1 if platform.system() == "Darwin" else 1024
            self.peak_rss_bytes = max(self.peak_rss_bytes, usage.ru_maxrss * scale)
            if forced:
                raise ServerFailure("the server ignored SIGTERM and was killed")
        self.wait_for_ports()

    def wait_for_ports(self):
        deadline = time.monotonic() + 10
        while any(port_in_use(port) for port in PORTS):
            if time.monotonic() > deadline:
                raise ServerFailure("a port is still taken after the server stopped")
            time.sleep(0.05)

    def runtime(self):
        log = (self.root / "log" / "stdout.txt").read_text(encoding="utf-8", errors="replace")
        return runtime_reading(jvm_properties(log), loaded_libraries(self.pid))

    def parsed_commit_mode(self):
        """What the same jar's own parser makes of the conf. Needs a started root."""
        layout = self.layout
        command = [str(layout.java()), "-XX:-UsePerfData",
                   f"-Djava.io.tmpdir={self.root / 'tmp'}", *PROBE_OPTIONS]
        if platform.system() == "Darwin":
            command += ["-p", str(layout.questdb_home() / "questdb.jar")]
        else:
            command.append(f"-Dquestdb.libs.dir={layout.questdb_home() / 'lib'}")
        command += ["--add-modules", "io.questdb", "-cp", str(layout.probe_classes()), PROBE_CLASS,
                    str(self.root)]
        run = subprocess.run(command, check=False, capture_output=True, text=True,
                             env=self.environment())
        match = re.search(r"^commit_mode=(\w+) value=(\d+)$", run.stdout, re.MULTILINE)
        if match is None:
            raise SettingsMismatch(f"the parser probe gave no answer: exit {run.returncode}")
        return match.group(1)


class LaunchedServer(QuestDBServer):
    """The same server through bin/questdb.sh and its JVM_PREPEND hook, for a full-day run.

    The hook carries what `command` passes and the launcher does not, and a GC log. The
    launcher forks java and leaves, so this holds a pid, not a child: it reads the peak from
    /proc before the stop, and compare/guard.py stops the java if this process dies first."""

    def __init__(self, *arguments, **options):
        super().__init__(*arguments, **options)
        self.java_pid = None

    @property
    def pid(self):
        return self.java_pid

    def prepended(self):
        log = self.root / "log"
        options = [*self.heap_options, f"-XX:ActiveProcessorCount={self.threads}",
                   "-XX:-UsePerfData", f"-Djava.io.tmpdir={self.root / 'tmp'}",
                   f"-XX:ErrorFile={log}/hs_err_pid%p.log"]
        if self.gc_log is not None:
            options.append(f"-Xlog:gc*:file={self.gc_log}:time,uptime,level,tags:filesize=0")
        return options

    def runtime(self):
        return {"status": "not read", "reason": "the launcher keeps the JVM's start-up output"}

    def launcher_environment(self):
        return {**child_environment(self.layout), "JVM_PREPEND": " ".join(self.prepended())}

    def find_java(self):
        found = subprocess.run(["pgrep", "-f", "--", f"-DQuestDB-Runtime-{LAUNCHER_TAG}"],
                               check=False, capture_output=True, text=True).stdout.split()
        return int(found[0]) if len(found) == 1 else None

    def start(self):
        require_ports_free()
        raise_open_file_limit()
        launcher = self.layout.questdb_home() / "bin" / "questdb.sh"
        with open(self.root / "log" / "launcher.txt", "ab") as log:
            subprocess.run([str(launcher), "start", "-d", str(self.root), "-t", LAUNCHER_TAG],
                           stdin=subprocess.DEVNULL, stdout=log, stderr=log, check=True,
                           cwd=self.root / "tmp", env=self.launcher_environment())
        deadline = time.monotonic() + START_TIMEOUT_SECONDS
        while self.java_pid is None or not self.answers():
            if self.java_pid is None:
                self.java_pid = self.find_java()
            if time.monotonic() > deadline:
                raise ServerFailure("the server never answered")
            time.sleep(0.05)
        listening = listening_addresses(self.java_pid)
        foreign = [address for address in listening if not address.startswith(f"{HOST}:")]
        if foreign or not listening:
            raise ServerFailure(f"not on loopback alone: {listening}")
        return listening

    @staticmethod
    def answers():
        try:
            sql("SELECT 1", timeout=5)
        except (OSError, ValueError, ServerFailure):
            return False
        return True

    def stop(self):
        pid = self.java_pid
        self.java_pid = None
        if pid is not None and process_alive(pid):
            self.peak_rss_bytes = max(self.peak_rss_bytes, proc_status_bytes(pid, "VmHWM"))
            os.kill(pid, signal.SIGTERM)
            deadline = time.monotonic() + STOP_TIMEOUT_SECONDS
            forced = False
            while process_alive(pid):
                if not forced and time.monotonic() > deadline:
                    os.kill(pid, signal.SIGKILL)
                    forced = True
                time.sleep(0.02)
            if forced:
                raise ServerFailure("the server ignored SIGTERM and was killed")
        self.wait_for_ports()


def process_alive(pid):
    try:
        state = Path(f"/proc/{pid}/stat").read_text(encoding="ascii").rsplit(")", 1)[1].split()[0]
    except FileNotFoundError:
        return False
    return state != "Z"


def proc_status_bytes(pid, field):
    for line in Path(f"/proc/{pid}/status").read_text(encoding="ascii").splitlines():
        if line.startswith(f"{field}:"):
            return int(line.split()[1]) * 1024
    return 0


def commit_mode_echo():
    rows = sql("SELECT property_path, value, value_source FROM (SHOW PARAMETERS) "
               "WHERE property_path = 'cairo.commit.mode'")
    if len(rows) != 1:
        raise SettingsMismatch("SHOW PARAMETERS has no cairo.commit.mode")
    return rows[0]


def prove_commit_mode(server, requested):
    """The full check on a running server, against its real conf file."""
    check_commit_mode_requested(requested)
    conf_text = server.conf_path.read_text(encoding="ascii")
    return check_commit_mode(requested, conf_text, commit_mode_echo(), server.parsed_commit_mode())


def _variant(locate_index):
    return {"partition_by": "DAY", "wal": True, "locate_symbol_index": locate_index}


class QuestDBSystem(System):
    name = "questdb"
    headline_variant = "plain"
    variants = {"plain": _variant(False), "locate-index": _variant(True)}
    # What a commit under sync must flush: the WAL, the sequencer's log and _txn. Everything
    # else is outside that and is listed in each result. This is how the real server behaves:
    # under sync it leaves the table's symbol maps and each segment's _meta dirty on a commit,
    # keeps the schema files CREATE TABLE wrote dirty, and after a large apply leaves partition
    # column files dirty for several seconds. See "Comparison harness" in NOTES.md, and
    # like_for_like below.
    flush_exempt = (re.compile(r"^\d{4}-\d{2}-\d{2}"),
                    re.compile(r"^(?:stock|mpid|locate)\.[kovc](?:\.\d+)?$"),
                    re.compile(r"^(?:wal\d+/\d+/|txn_seq/)?_meta$"))

    def __init__(self, context):
        super().__init__(context)
        self.mode = MODE_FOR_CLASS[context.durability]
        server_class = LaunchedServer if context.full_day else QuestDBServer
        self.server = server_class(context.layout, self.root, context.threads,
                                   {"cairo.commit.mode": self.mode}, heap=context.heap,
                                   gc_log=context.gc_log)
        self.client = None
        self.listening = []

    # Runner side.

    def prepare(self):
        super().prepare()
        self.server.write_conf()

    def start_service(self):
        self.listening = self.server.start()

    def requested(self):
        return {"server_conf": {key: named_loopback(value)
                                for key, value in self.server.conf.items()},
                "java_options": [*JAVA_OPTIONS, SHOW_PROPERTIES, *self.server.heap_options,
                                 f"-XX:ActiveProcessorCount={self.context.threads}",
                                 "-XX:-UsePerfData"],
                "table": self._column_definitions(TABLE),
                "ingest": "QWP over WebSocket, Python client, in time order",
                "symbol_capacity": "not set, as the docs conflict",
                "o3_settings": "the defaults"}

    def stop_service(self):
        self.server.stop()
        return {"peak_rss_bytes": self.server.peak_rss_bytes,
                "heap": " ".join(self.server.heap_options) or "the JVM's default"}

    def service_settings(self):
        """Run with the server up. This is what makes a class proven, or stops the run."""
        settings = prove_commit_mode(self.server, self.mode)
        rows = sql("SELECT property_path, value FROM (SHOW PARAMETERS) WHERE property_path IN "
                   f"({', '.join(repr(name) for name in WORKER_SETTINGS)})")
        counts = {row["property_path"]: row["value"] for row in rows}
        command = self._running_command()
        checks = [Setting(name, str(self.context.threads), counts.get(name), "SHOW PARAMETERS")
                  for name in WORKER_SETTINGS]
        echoed = {row["property_path"]: row["value"]
                  for row in sql("SELECT property_path, value FROM (SHOW PARAMETERS)")}
        checks += [Setting(key, named_loopback(value), named_loopback(echoed.get(key)),
                           "SHOW PARAMETERS")
                   for key, value in self.server.conf.items() if key in ECHOED_CONF_KEYS]
        checks += [
            Setting("java heap", self.server.heap_flag, _flag(command, "-Xmx"),
                    "the running command"),
            Setting("java initial heap", self.server.initial_heap_flag, _flag(command, "-Xms"),
                    "the running command"),
            Setting("java processors", f"-XX:ActiveProcessorCount={self.context.threads}",
                    _flag(command, "-XX:ActiveProcessorCount="), "the running command"),
            Setting("listening on loopback only", True,
                    all(item.startswith(f"{HOST}:") for item in self.listening) and
                    bool(self.listening), "the host's socket table"),
        ]
        return settings + require_all_match(checks)

    def _running_command(self):
        return subprocess.run(["ps", "-ww", "-o", "command=", "-p", str(self.server.pid)],
                              check=True, capture_output=True, text=True).stdout

    def settle(self):
        """Lets QuestDB drop applied WAL segments, so that size at rest is the table's.

        A segment stays until its writer goes, and the purge job then runs on an interval. So
        this restarts the server to release the writers and waits for the job, then reports
        whatever is left rather than deleting it."""
        self.server.stop()
        self.start_service()
        deadline = time.monotonic() + WAL_PURGE_TIMEOUT_SECONDS
        started = time.monotonic()
        while time.monotonic() < deadline:
            if not self.wal_directories():
                break
            time.sleep(0.5)
        return {"wal_purge_seconds": round(time.monotonic() - started, 3),
                "wal_directories_left": len(self.wal_directories())}

    def wal_directories(self):
        return [path for directory in self.data_paths() for path in directory.glob("wal*")
                if path.is_dir()]

    def service_facts(self):
        version = subprocess.run([str(self.context.layout.java()), "-XX:-UsePerfData", "-version"],
                                 check=True,
                                 capture_output=True, text=True,
                                 env=child_environment(self.context.layout)).stderr
        return {"java": version.strip().splitlines(),
                "server_build": sql("SELECT build")[0]["build"],
                "runtime": self.server.runtime(),
                "allocator": (LINUX_JEMALLOC + " by LD_PRELOAD, as bin/questdb.sh does"
                              if platform.system() == "Linux" else "the system allocator"),
                "heap": (" ".join(self.server.heap_options) or "the JVM's default") +
                        " with AlwaysPreTouch, so that much is resident always",
                "listening_ports": sorted(int(item.rsplit(":", 1)[1]) for item in self.listening),
                "bind": "loopback only, read from the host's socket table",
                "telemetry": "off",
                "host_limits": host_limits(),
                "commit_mode_conf": self.mode}

    # Worker side.

    def open(self):
        import pyarrow as pa
        import questdb

        pa.set_cpu_count(self.context.threads)
        pa.set_io_thread_count(self.context.threads)
        self.client = questdb.connect(f"ws::addr={HOST}:{HTTP_PORT};")

    def close(self):
        if self.client is not None:
            self.client.close()
            self.client = None

    def settings_in_force(self):
        return []

    def _column_definitions(self, table):
        parts = []
        for name in TABLE_COLUMNS:
            column_type = COLUMN_TYPES[name]
            if name == "locate" and self.tuning["locate_symbol_index"]:
                column_type = "SYMBOL INDEX"
            parts.append(f"{name} {column_type}")
        return (f"CREATE TABLE {table} ({', '.join(parts)}) TIMESTAMP(ts) "
                f"PARTITION BY {self.tuning['partition_by']} WAL")

    def _prepare(self, table):
        import pyarrow as pa

        columns = {column: table.column(column) for column in table.column_names}
        symbols = ["stock", "mpid"]
        if self.tuning["locate_symbol_index"]:
            columns["locate"] = columns["locate"].cast(pa.string())
            symbols.append("locate")
        for symbol in symbols:
            columns[symbol] = columns[symbol].dictionary_encode()
        return pa.table(columns), symbols

    def _send(self, name, table):
        started = time.perf_counter()
        prepared, symbols = self._prepare(table)
        ready = time.perf_counter()
        self.client.dataframe(prepared, table_name=name, symbols=symbols, at="ts")
        sent = time.perf_counter()
        sql(f"SELECT wait_wal_table('{name}')")
        visible = time.perf_counter()
        return {"seconds": visible - started, "python_seconds": ready - started,
                "engine_seconds": visible - ready, "sent_seconds": sent - ready,
                "apply_wait_seconds": visible - sent,
                "python_note": "python_seconds builds the dictionaries the client needs for "
                               "SYMBOL columns. engine_seconds covers the client and server"}

    def _require_applied(self, timings, rows):
        state = sql(f"SELECT writerTxn, sequencerTxn FROM wal_tables() WHERE name = '{TABLE}'")
        if len(state) != 1 or state[0]["writerTxn"] != state[0]["sequencerTxn"]:
            raise ServerFailure(f"the WAL is not fully applied: {state}")
        count = sql(f"SELECT count(*) AS n FROM {TABLE}")[0]["n"]
        if count != rows:
            raise ServerFailure(f"{count} rows visible, {rows} sent")
        timings["clock_stops"] = "after wait_wal_table, with writerTxn = sequencerTxn checked"
        return timings

    def ingest(self, columns, table):
        sql(self._column_definitions(TABLE))
        timings = self._send(TABLE, table)
        return self._require_applied(timings, table.num_rows)

    def stream_chunk_rows(self):
        return STREAM_CHUNK_ROWS

    def _wal_state(self):
        """The table's row in wal_tables(), which holds its lag and memory pressure."""
        state = sql(f"SELECT * FROM wal_tables() WHERE name = '{TABLE}'")[0]
        if state.get("suspended"):
            raise ServerFailure(f"the table is suspended: {state}")
        return state

    def ingest_stream(self, chunks):
        """Every dataframe call commits. Each state also records the client's own memory, since
        the chunks are bounded and whatever it holds beyond them is its own. The clock stops as
        in `ingest`, but a day's backlog can outlast one call's query timeout, so the WAL is
        polled down before the last call."""
        import pyarrow as pa

        sql(self._column_definitions(TABLE))
        started = time.perf_counter()
        rows = 0
        wal_states = []
        for table in chunks:
            prepared, symbols = self._prepare(table)
            self.client.dataframe(prepared, table_name=TABLE, symbols=symbols, at="ts")
            rows += table.num_rows
            wal_states.append({"rows_sent": rows, "seconds": time.perf_counter() - started,
                               "arrow_allocated_bytes": pa.total_allocated_bytes(),
                               "client_rss_anon_bytes": proc_status_bytes(os.getpid(), "RssAnon"),
                               **self._wal_state()})
            print(f"questdb: {wal_states[-1]}", flush=True)
        sent = time.perf_counter()
        while (state := self._wal_state())["writerTxn"] != state["sequencerTxn"]:
            wal_states.append({"rows_sent": rows, "seconds": time.perf_counter() - started,
                               **state})
            print(f"questdb: {wal_states[-1]}", flush=True)
            time.sleep(WAL_POLL_SECONDS)
        sql(f"SELECT wait_wal_table('{TABLE}')")
        visible = time.perf_counter()
        return self._require_applied(
            {"seconds": visible - started, "sent_seconds": sent - started,
             "apply_wait_seconds": visible - sent, "wal_states": wal_states}, rows)

    def probe_flush(self, table):
        sql(self._column_definitions(PROBE_TABLE))
        self._send(PROBE_TABLE, table)

    def dirty_paths(self):
        return self._table_directories(PROBE_TABLE)

    def _select_list(self, columns):
        if self.tuning["locate_symbol_index"]:
            return ", ".join("CAST(locate AS INT) AS locate" if name == "locate" else name
                             for name in columns)
        return ", ".join(columns)

    def _rows(self, text):
        return self.client.query(text).to_arrow()

    def read_all(self):
        return self._rows(f"SELECT {self._select_list(TABLE_COLUMNS)} FROM {TABLE} ORDER BY seq")

    def _locate_filter(self, locate):
        if locate < 0:
            return ""
        literal = f"'{locate}'" if self.tuning["locate_symbol_index"] else str(locate)
        return f" AND locate = {literal}"

    def _window(self, start_ns, end_ns):
        day = self.context.day
        return (f"ts >= '{nanos_text(day, start_ns)}' AND ts < '{nanos_text(day, end_ns)}'")

    def _rows_text(self, start_ns, end_ns, locate, columns):
        return (f"SELECT {self._select_list(columns)} FROM {TABLE} WHERE "
                f"{self._window(start_ns, end_ns)}{self._locate_filter(locate)}")

    def query_rows(self, start_ns, end_ns, locate, columns):
        return self._rows(self._rows_text(start_ns, end_ns, locate, columns))

    def _minutes_text(self, start_ns, end_ns, locate, msg_types):
        types = ", ".join(str(value) for value in msg_types)
        return (f"SELECT ts, count() AS n, sum(shares) AS s, min(price) AS lo, max(price) AS hi "
                f"FROM {TABLE} WHERE {self._window(start_ns, end_ns)}"
                f"{self._locate_filter(locate)} AND msg_type IN ({types}) "
                f"SAMPLE BY 1m ALIGN TO CALENDAR")

    def query_minute_aggregate(self, start_ns, end_ns, locate, msg_types):
        import pyarrow as pa

        table = self._rows(self._minutes_text(start_ns, end_ns, locate, msg_types))
        rows = zip(table.column("ts").cast(pa.timestamp("ns")).cast(pa.int64()).to_pylist(),
                   table.column("n").to_pylist(), table.column("s").to_pylist(),
                   table.column("lo").to_pylist(), table.column("hi").to_pylist(), strict=True)
        return [[int(value) for value in row] for row in rows]

    def query_column_sum(self, column):
        table = self._rows(f"SELECT count() AS n, sum({column}) AS s FROM {TABLE}")
        return [[table.column("n")[0].as_py(), table.column("s")[0].as_py()]]

    def scan_facts(self, query):
        from compare.queries import ADD_ORDER_TYPES, ROW_COLUMNS

        if query.kind == "rows":
            text = self._rows_text(query.start_ns, query.end_ns, query.locate, ROW_COLUMNS)
        elif query.kind == "minute_aggregate":
            text = self._minutes_text(query.start_ns, query.end_ns, query.locate, ADD_ORDER_TYPES)
        else:
            text = f"SELECT count() AS n, sum(shares) AS s FROM {TABLE}"
        plan = [row["QUERY PLAN"] for row in sql(f"EXPLAIN {text}")]
        return {"rows_decoded": {"status": "unavailable",
                                 "reason": "QuestDB does not report rows or bytes it decoded"},
                "bytes_decoded": {"status": "unavailable",
                                  "reason": "QuestDB does not report rows or bytes it decoded"},
                "plan": plan, "interval_scan": any(EVIDENCE_PLAN in line for line in plan)}

    def stored_settings(self):
        columns = {row["column"]: row for row in sql(f"SHOW COLUMNS FROM {TABLE}")}
        table = sql("SELECT designatedTimestamp, partitionBy, walEnabled FROM tables() "
                    f"WHERE table_name = '{TABLE}'")[0]
        created = sql(f"SHOW CREATE TABLE {TABLE}")[0]["ddl"]
        checks = [Setting("column list", list(TABLE_COLUMNS), list(columns), "SHOW COLUMNS")]
        for name, column_type in COLUMN_TYPES.items():
            if name == "locate" and self.tuning["locate_symbol_index"]:
                column_type = "SYMBOL"
            checks.append(Setting(f"column {name}", column_type,
                                  columns.get(name, {}).get("type"), "SHOW COLUMNS"))
        checks += [
            Setting("designated timestamp", "ts", table["designatedTimestamp"], "tables()"),
            Setting("partition by", self.tuning["partition_by"], table["partitionBy"], "tables()"),
            Setting("WAL enabled", True, table["walEnabled"], "tables()"),
            Setting("partition in DDL", True, f"PARTITION BY {self.tuning['partition_by']}"
                    in created, "SHOW CREATE TABLE"),
            Setting("locate indexed", self.tuning["locate_symbol_index"],
                    columns["locate"]["indexed"], "SHOW COLUMNS"),
        ]
        checked = require_all_match(checks)
        return {"checks_passed": len(checked), "ddl": created,
                "ingest_path": "QWP over WebSocket, from the Python client",
                "partitions": sql(f"SELECT name, numRows, diskSize FROM table_partitions('{TABLE}')")}

    def after_queries(self):
        """QuestDB's own Parquet partitions. The docs say to report size after this too."""
        day = self.context.day.iso
        try:
            sql(f"ALTER TABLE {TABLE} CONVERT PARTITION TO PARQUET LIST '{day}'")
        except ServerFailure as failure:
            return {"status": "refused", "reason": str(failure)}
        sql(f"SELECT wait_wal_table('{TABLE}')")
        partitions = sql(f"SELECT name, numRows, isParquet, parquetFileSize "
                         f"FROM table_partitions('{TABLE}')")
        return {"status": "converted" if partitions[0]["isParquet"] else "accepted, not converted",
                "partitions": partitions, "codec": "QuestDB's default for this conversion",
                "read_back": "not repeated on the converted form"}

    # Either side.

    def durability_facts(self):
        if not self.class_b:
            return {"sync_primitive": "none: cairo.commit.mode=nosync",
                    "flush_granularity": "none", "like_for_like": True,
                    "note": "QuestDB's default"}
        macos = platform.system() == "Darwin"
        return {"sync_primitive": "msync(MS_SYNC) and plain fsync",
                "flush_granularity": "per commit: the client commits about every 100 batches, "
                                     "and each WAL commit is then applied to the table",
                "like_for_like": not macos,
                "note": ("not like-for-like on macOS: QuestDB's plain fsync does not reach the "
                         "platters' cache, and LMDB and the harness use F_FULLFSYNC, about 80x "
                         "slower at p50" if macos else "both reach the device on Linux")}

    def flush_findings(self, facts, flush):
        """A commit that leaves table pages dirty is not like an fsync of everything."""
        table_pages = {name: pages for name, pages in flush["residual_dirty_files"].items()
                       if not name.endswith("_meta")}
        if self.class_b and table_pages:
            return {**facts, "like_for_like": False,
                    "note": f"{facts['note']}. Under sync the WAL is flushed at commit, and "
                            f"{sum(table_pages.values())} pages of the table's own files were "
                            "still dirty once the rows were readable"}
        return facts

    def _table_directories(self, name):
        return sorted((self.root / "db").glob(f"{name}~*"))

    def data_paths(self):
        return self._table_directories(TABLE)

    @staticmethod
    def versions():
        import pyarrow as pa
        import questdb

        return {"questdb_server": "10.1.0", "questdb_client": questdb.__version__,
                "pyarrow": pa.__version__}


def _flag(command, prefix):
    for token in command.split():
        if token.startswith(prefix):
            return token
    return None


