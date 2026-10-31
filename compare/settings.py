"""Requested against in force, for every system, with a hard failure on any difference.

A durability class is not proven until this passes. QuestDB is why: it accepts a bad
`cairo.commit.mode` without a word, echoes it back through SHOW PARAMETERS and runs nosync.
So an echo is never proof on its own, and QuestDB's commit mode has to pass four
independent checks here. The rules are plain functions over values, so
tests/compare/test_settings.py can inject one fault per rule, and the same functions run
against the real server.
"""

from dataclasses import dataclass

COMMIT_MODES = ("nosync", "async", "sync")
# `async` is a no-op flush on Linux, so it never counts as class B.
MODE_FOR_CLASS = {"A": "nosync", "B": "sync"}
PROVEN = "proven"
NOT_PROVEN = "NOT PROVEN"


class SettingsMismatch(Exception):
    """A setting in force is not the one requested. The run must stop."""


@dataclass(frozen=True)
class Setting:
    """One setting, with how its in-force value was read."""

    name: str
    requested: object
    in_force: object
    read_from: str

    def as_json(self):
        return {"name": self.name, "requested": self.requested, "in_force": self.in_force,
                "read_from": self.read_from}


def require_all_match(settings):
    """Fails on the first difference, and names every one of them."""
    wrong = [item for item in settings if item.requested != item.in_force]
    if wrong:
        lines = [f"{item.name}: requested {item.requested!r}, in force {item.in_force!r} "
                 f"(from {item.read_from})" for item in wrong]
        raise SettingsMismatch("; ".join(lines))
    return [item.as_json() for item in settings]


def parse_conf(text):
    """A server.conf as a dict. A key given twice is refused, since which wins is a guess."""
    values = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        key, separator, value = line.partition("=")
        if not separator:
            raise SettingsMismatch(f"not a key and value: {line!r}")
        if key in values:
            raise SettingsMismatch(f"{key} is given twice")
        values[key] = value
    return values


def check_commit_mode_requested(requested):
    if requested not in COMMIT_MODES:
        raise SettingsMismatch(f"cairo.commit.mode: {requested!r} is not one of {COMMIT_MODES}")


def check_commit_mode(requested, conf_text, echo, parsed_mode):
    """QuestDB's mode, from every source that can be read, against `requested`.

    conf_text    the server.conf the server was really started on
    echo         {"value", "value_source"} from SHOW PARAMETERS
    parsed_mode  what QuestDB's own PropServerConfiguration parsed from that conf, in the
                 same jar. This is the check that sees through a bad value, because the
                 parser falls back to nosync and the echo does not.

    Returns the Settings, or raises SettingsMismatch.
    """
    check_commit_mode_requested(requested)
    conf_value = parse_conf(conf_text).get("cairo.commit.mode")
    if conf_value not in COMMIT_MODES:
        raise SettingsMismatch(f"cairo.commit.mode: the conf holds {conf_value!r}, which is not "
                               f"one of {COMMIT_MODES}, and QuestDB would silently run nosync")
    if echo.get("value_source") != "conf":
        raise SettingsMismatch("cairo.commit.mode: SHOW PARAMETERS says it came from "
                               f"{echo.get('value_source')!r}, not the conf")
    settings = [
        Setting("cairo.commit.mode (conf file)", requested, conf_value, "server.conf as read back"),
        Setting("cairo.commit.mode (echo)", requested, echo.get("value"), "SHOW PARAMETERS"),
        Setting("cairo.commit.mode (parsed)", requested, parsed_mode,
                "QuestDB's own PropServerConfiguration, run on that conf from the same jar"),
    ]
    return require_all_match(settings)


def split_dirty(files, exempt):
    """Divides per-file dirty counts into those a commit should have cleaned and the rest.

    `files` is {relative path: dirty pages}. `exempt` holds compiled patterns for files that
    a system is known to leave dirty on commit. They stay in the result by name."""
    held = {path: pages for path, pages in files.items()
            if not any(pattern.search(path) for pattern in exempt)}
    excused = {path: pages for path, pages in files.items() if path not in held}
    return held, excused


def durability_verdict(system_class, flush, control_saw_dirty):
    """Whether a run's class is proven, from the dirty pages seen once a commit returned.

    flush              {"data_dirty_pages": int or None, "residual_dirty_files": {...}}
    control_saw_dirty  for class B: class A on this system and host left dirty pages through
                       the same detector, so that a 0 here means something.
    """
    dirty = flush.get("data_dirty_pages")
    if dirty is None:
        return {"status": NOT_PROVEN, "reason": "this host cannot count dirty pages"}
    if system_class == "A":
        if dirty > 0:
            return {"status": PROVEN,
                    "reason": f"{dirty} data pages were still dirty when the commit returned, "
                              "so no flush ran"}
        return {"status": NOT_PROVEN,
                "reason": "no data page was dirty after the commit, so class A is not shown to "
                          "skip the flush, and the detector has no control"}
    if dirty != 0:
        return {"status": NOT_PROVEN,
                "reason": f"{dirty} data pages were still dirty when the commit returned"}
    if not control_saw_dirty:
        return {"status": NOT_PROVEN,
                "reason": "0 dirty pages, but class A's control saw none either, so 0 proves "
                          "nothing"}
    residual = flush.get("residual_dirty_files") or {}
    note = (f"; {sum(residual.values())} pages stay dirty in files outside the flush, listed "
            "in residual_dirty_files" if residual else "")
    return {"status": PROVEN,
            "reason": "0 data pages dirty when the commit returned, and class A's control saw "
                      f"dirty pages through the same detector{note}"}
