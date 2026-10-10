#!/usr/bin/env python3
"""KTPlace web console.

Run the placement engine from a browser and review what it wrote. The engine is
invoked directly -- no second service, and no copy of the binary in this image
either: compose gives the server the dev container's build volume, so the
console runs the tree `make` just built (resolve_bin says where from). Every
run gets its own directory under the runs root, and the transcript streams to
the page as it is produced. The static gallery ktplace itself writes
(plots/index.html) is served out of the run directory, so the review half is the
engine's own output, not a second implementation of it.

Stdlib only: python3 is the only dependency beyond what the runtime image
already carries.

Endpoints
  GET    /                       the console page
  GET    /api/benchmarks         designs the console can run
  GET    /api/system             machine CPU and memory, for the resource panel
  GET    /api/runs               run history with headline metrics
  POST   /api/runs               start a run: {"benchmark","algorithm","verbose","env"}
  GET    /api/runs/<id>          status, progress, result summary, artifact links
  POST   /api/runs/<id>/label    rename a run: {"label"}
  DELETE /api/runs/<id>          stop a run (if live) and delete its directory
  GET    /api/runs/<id>/log?offset=&stream=  log/trace chunk + progress + liveness
  GET    /api/runs/<id>/download?what=  log, trace, placed, request or all (zip)
  POST   /api/exec               run a command in the container: {"cmd"}
  GET    /api/exec/<id>?offset=  its output so far
  DELETE /api/exec/<id>          stop it
  GET    /runs/<id>/<path>       a file of a run (gallery, png, placed.pl, log)
"""

from __future__ import annotations

import datetime
import io
import json
import os
import pathlib
import re
import shlex
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.parse
import uuid
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ------------------------------------------------------------------ config
HOME = pathlib.Path(os.environ.get("KTPLACE_HOME", "/ktplace")).resolve()
BENCH_ROOT = pathlib.Path(os.environ.get("KTPLACE_BENCH_ROOT", str(HOME / "benchmark"))).resolve()
RUN_ROOT = pathlib.Path(os.environ.get("KTPLACE_RUN_ROOT", str(HOME / "runs"))).resolve()
HOST = os.environ.get("KTPLACE_WEB_HOST", "127.0.0.1")
PORT = int(os.environ.get("KTPLACE_WEB_PORT", "8080"))
# Set to say which engine to run; when unset it is resolved per run, see
# resolve_bin(). The image ships none -- the console runs the tree the dev
# container compiles.
KTPLACE_BIN = os.environ.get("KTPLACE_BIN")
MAX_RUNS = max(1, int(os.environ.get("KTPLACE_WEB_MAX_RUNS", "1")))
ALGORITHMS = ("simpl", "ntuplace1")  # what FlowMgr::runPlacement dispatches on
RUN_ID_RE = re.compile(r"^\d{8}-\d{6}-[0-9a-f]{8}$")

# The command runner is a shell in the console container, so it is off unless it
# is asked for. compose.yaml turns it on for the loopback development setup.
ALLOW_EXEC = os.environ.get("KTPLACE_WEB_ALLOW_EXEC", "").lower() not in ("", "0", "false", "no")
EXEC_ROOT = pathlib.Path(os.environ.get("KTPLACE_EXEC_ROOT", str(HOME / "exec"))).resolve()
MAX_EXECS = max(1, int(os.environ.get("KTPLACE_WEB_MAX_EXECS", "12")))

# The engine reads its tuning from the environment (kt_option/flowMgr take no
# flags for it), so an override is a KEY=VALUE pair rather than an argument. The
# allow-list is deliberately: KTPLACE_-namespaced keys with a value of shell- and
# environment-safe characters, so what launch() hands to Popen cannot smuggle a
# second command or a new variable into the child.
ENV_KEY_RE = re.compile(r"^KTPLACE_[A-Z0-9_]{1,48}$")
ENV_VALUE_RE = re.compile(r"^[A-Za-z0-9_.,:/+%-]{0,64}$")

WEBUI_DIR = pathlib.Path(__file__).resolve().parent
STATIC_DIR = WEBUI_DIR / "static"

_MARKERS = (".def", ".def.gz", ".nodes", ".nodes.gz")


def resolve_bin() -> str | None:
    """Where the engine comes from. Decided on every run, not once at startup:
    the console is up before the first compile finishes, and a build while it
    runs should be picked up without a restart.

    The order says what the design is: an explicit KTPLACE_BIN, then the build
    volume compose mounts at HOME/build -- the tree the dev container compiles,
    and the one this container is meant to run -- then the repository's own
    build/ for a bare-metal server, and only then PATH.
    """
    if KTPLACE_BIN:
        return KTPLACE_BIN
    for candidate in (HOME / "build" / "bin" / "ktplace",
                      WEBUI_DIR.parent / "build" / "bin" / "ktplace"):
        if os.access(candidate, os.X_OK):
            return str(candidate)
    return shutil.which("ktplace")


# ---------------------------------------------------------------- progress
# What the transcript says about where a run is, and how much of the bar each
# phase is worth. The engine marks *phases*, not iterations, so these are the
# milestones a non-verbose run reports; the weights are what the phases cost on
# a typical design (loading and the solver dominate), not a measurement. The
# precise "is it still working" comes from the heartbeat, which is separate:
# the engine streams one SVG per conjugate-gradient iteration, and /proc says
# how much CPU the child has burned, so a stage that reports nothing for a
# while is still visibly alive.
_PROGRESS_MARKERS = (
    ("loaded:", 0.08, "load", "design loaded"),
    ("running simpl global placement", 0.10, "place", "global placement (SimPL)"),
    ("running ntuplace1 global placement", 0.10, "place", "global placement (NTUPlace1)"),
    ("running abacus legalization", 0.82, "legalize", "legalization (Abacus)"),
    ("running fastdp detailed placement", 0.90, "detail", "detailed placement (FastDP)"),
    ("final high-resolution image:", 0.97, "write", "writing output and images"),
    ("placement completed successfully", 1.0, "done", "done"),
)

_FRAME_DIRS = ("plots/simpl", "plots/legalize", "plots/detailplace")


def scan_progress(run: "Run") -> None:
    """Advance the stage bar with whatever the transcript has gained.

    Reads only the bytes added since the last call, and the markers are
    monotonic (a later phase never lowers the fraction), so this is cheap and
    safe to call on every poll.
    """
    path = run.work / "ktplace.log"
    try:
        size = path.stat().st_size
    except OSError:
        return
    if size < run.scan:  # truncated or rewritten: start over
        run.scan = 0
    if size == run.scan:  # nothing new
        return
    try:
        with open(path, "rb") as fh:
            fh.seek(run.scan)
            text = fh.read().decode("utf-8", "replace")
    except OSError:
        return
    run.scan = size
    low = text.lower()
    for needle, fraction, stage, label in _PROGRESS_MARKERS:
        if needle in low and fraction > run.progress["fraction"]:
            run.progress = {"fraction": fraction, "stage": stage, "label": label,
                            "updated": time.time()}


# --------------------------------------------------------------- heartbeat
def _mtime(path: pathlib.Path) -> float:
    try:
        return path.stat().st_mtime
    except OSError:
        return 0.0


def count_frames(work: pathlib.Path) -> tuple[int, float]:
    """(frames written, newest of them) under a run's plot directories.

    A frame lands on disk once per conjugate-gradient iteration, which is the
    finest-grained sign of life the engine produces on its own: it is what makes
    "the log has been quiet for a minute" distinguishable from "nothing is
    moving anywhere".
    """
    count = 0
    newest = 0.0
    for rel in _FRAME_DIRS:
        try:
            with os.scandir(work / rel) as entries:
                for entry in entries:
                    if not entry.name.endswith(".svg"):
                        continue
                    count += 1
                    try:
                        newest = max(newest, entry.stat().st_mtime)
                    except OSError:
                        pass
        except OSError:
            continue
    return count, newest


try:
    _CLK_TCK = os.sysconf("SC_CLK_TCK") or 100
except (ValueError, OSError):
    _CLK_TCK = 100
try:
    _PAGE_SIZE = os.sysconf("SC_PAGE_SIZE") or 4096
except (ValueError, OSError):
    _PAGE_SIZE = 4096


def proc_usage(pid: int) -> tuple[float, float] | None:
    """(cpu seconds, rss bytes) of a live process, or None once it is gone.

    /proc/<pid>/stat starts with the command in parentheses, which may contain
    spaces or parentheses of its own, so the fields are counted from the last
    ')' rather than from the start of the line.
    """
    try:
        with open(f"/proc/{pid}/stat", "rb") as fh:
            raw = fh.read().decode("ascii", "replace")
        fields = raw[raw.rfind(")") + 2:].split()
        utime, stime = int(fields[11]), int(fields[12])  # overall fields 14, 15
    except (OSError, ValueError, IndexError):
        return None
    try:
        with open(f"/proc/{pid}/statm", "rb") as fh:
            rss = int(fh.read().split()[1]) * _PAGE_SIZE
    except (OSError, ValueError, IndexError):
        rss = 0
    return (utime + stime) / _CLK_TCK, float(rss)


def heartbeat(run: "Run") -> dict:
    """What moved since the last call, and how hard the engine is working.

    Called on every poll, so it keeps the previous sample on the run and reports
    rates rather than totals: frames per second, CPU percent of one core, and
    how long the transcript has been silent. `state` is the heuristic on top:
    output recently, or CPU still burning, is working; neither for a while is
    the shape a hang has.
    """
    now = time.time()
    frames, newest = count_frames(run.work)
    log_mtime = _mtime(run.work / "ktplace.log")
    usage = proc_usage(run.proc.pid) if run.proc is not None else None

    previous = run.hb
    dt = now - previous["t"] if previous else 0.0
    frame_rate = cpu_percent = None
    if previous and dt > 0.05:
        frame_rate = max(0.0, (frames - previous["frames"]) / dt)
        if usage and previous.get("cpu") is not None:
            cpu_percent = round(100.0 * (usage[0] - previous["cpu"]) / dt, 1)
    run.hb = {"t": now, "frames": frames, "cpu": usage[0] if usage else None}

    if run.status != "running":
        state = run.status
    elif (log_mtime and now - log_mtime < 3.0) or (frame_rate or 0) > 0:
        state = "working"
    elif (cpu_percent or 0) >= 5:
        state = "working"  # computing between outputs, e.g. a long linear solve
    elif log_mtime and now - log_mtime > 20.0:
        state = "stalled"
    else:
        state = "quiet"

    return {
        "state": state,
        "logAge": None if not log_mtime else round(now - log_mtime, 1),
        "frames": frames,
        "frameAge": None if not newest else round(now - newest, 1),
        "framesPer": None if frame_rate is None else round(frame_rate, 2),
        "cpuSeconds": None if not usage else round(usage[0], 1),
        "cpuPercent": cpu_percent,
        "rssMB": None if not usage else round(usage[1] / 1e6, 1),
    }


# ----------------------------------------------------------------- machine
# The Linux the containers run in -- on macOS and Windows that is Docker's VM,
# which is the only machine the engine can actually use. All of it is read from
# /proc and the cgroup, so nothing has to be installed and it works in the
# container as it stands.
_CPU_LOCK = threading.Lock()
_CPU_SAMPLE: tuple[float, float, float] | None = None  # (when, idle, total)


def _cpu_usage() -> float | None:
    """Busy percent since the previous call, machine-wide."""
    global _CPU_SAMPLE
    try:
        with open("/proc/stat") as fh:
            fields = fh.readline().split()[1:]
    except OSError:
        return None
    try:
        values = [float(v) for v in fields]
    except ValueError:
        return None
    idle = values[3] + (values[4] if len(values) > 4 else 0.0)  # idle + iowait
    total = sum(values)
    with _CPU_LOCK:
        previous, _CPU_SAMPLE = _CPU_SAMPLE, (time.time(), idle, total)
    if previous is None or total <= previous[2] or idle < previous[1]:
        return None
    return round(100.0 * (1.0 - (idle - previous[1]) / (total - previous[2])), 1)


def _meminfo() -> dict:
    out: dict[str, int] = {}
    try:
        with open("/proc/meminfo") as fh:
            for line in fh:
                key, _, rest = line.partition(":")
                value = rest.strip().split()
                if value and value[0].isdigit():
                    out[key] = int(value[0]) * 1024
    except OSError:
        return out
    return out


def _cgroup_memory() -> tuple[int | None, int | None]:
    """(limit, current) for this container, when the cgroup v2 files are there."""
    def read(name: str) -> int | None:
        try:
            text = pathlib.Path("/sys/fs/cgroup", name).read_text().strip()
        except OSError:
            return None
        return None if text in ("", "max") else int(text)
    current = read("memory.current")
    return read("memory.max"), current


def system_view() -> dict:
    """Machine CPU and memory, in bytes, for the panel beside the runs."""
    mem = _meminfo()
    total = mem.get("MemTotal", 0)
    available = mem.get("MemAvailable", 0)
    limit, used = _cgroup_memory()
    swap_total, swap_free = mem.get("SwapTotal", 0), mem.get("SwapFree", 0)
    load = None
    try:
        load = [float(v) for v in pathlib.Path("/proc/loadavg").read_text().split()[:3]]
    except (OSError, ValueError):
        pass
    return {
        "cpu": {
            "cores": os.cpu_count() or 1,
            "load": load,
            "usagePercent": _cpu_usage(),
        },
        "mem": {
            "totalBytes": total,
            "availableBytes": available,
            "usedBytes": max(0, total - available),
            "limitBytes": limit,
            "limitUsedBytes": used,
            "swapTotalBytes": swap_total,
            "swapUsedBytes": max(0, swap_total - swap_free),
        },
    }


# ------------------------------------------------------------------- runs
class Run:
    """A single ktplace invocation and everything it produced."""

    def __init__(self, rid: str, meta: dict, work: pathlib.Path):
        self.id = rid
        self.meta = meta  # benchmark, algorithm, verbose, env, started, label
        self.work = work
        self.proc: subprocess.Popen | None = None
        self.status = "unknown"  # reset by launch / index_existing_runs
        self.exit_code: int | None = None
        self.started = meta.get("started", time.time())
        self.ended: float | None = None
        self.env: dict = meta.get("env") or {}
        self.label: str = meta.get("label") or ""
        self._summary: dict | None = None  # cache of the parsed headline numbers
        # Live progress: the stage bar is scanned out of the transcript, while
        # the heartbeat is measured from /proc and the frames the engine writes.
        # scan is how many bytes of ktplace.log have already been read for it.
        self.progress = {"fraction": 0.0, "stage": "load", "label": "starting",
                         "updated": 0.0}
        self.scan = 0
        self.hb: dict | None = None  # previous heartbeat sample, for the rates

    def elapsed(self) -> float:
        end = self.ended if self.ended is not None else time.time()
        return max(0.0, end - self.started)


_RUNS: dict[str, Run] = {}
_RUNS_LOCK = threading.Lock()


def new_id() -> str:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return f"{stamp}-{uuid.uuid4().hex[:8]}"


class EngineNotBuilt(RuntimeError):
    """The console is up and the engine is not yet -- the state of the seconds
    between `docker compose up` and the first `make` finishing. Reported as
    such (503, retried by the user) rather than as a fault in the server."""


def clean_env(raw) -> dict:
    """Keep the KTPLACE_ overrides that pass the allow-lists, drop the rest.

    A rejected pair is dropped rather than fatal: a typo in one line should not
    refuse a run that the remaining lines describe.
    """
    if not isinstance(raw, dict):
        return {}
    out = {}
    for key, value in raw.items():
        key, value = str(key).strip(), str(value).strip()
        if ENV_KEY_RE.match(key) and ENV_VALUE_RE.match(value):
            out[key] = value
    return out


def launch(benchmark: str, target: str, algorithm: str, verbose: bool,
           env: dict | None = None) -> Run:
    # Resolved first, so a console that is up but whose engine is not built
    # yet reports that instead of leaving an empty run directory behind.
    binary = resolve_bin()
    if not binary:
        raise EngineNotBuilt(
            "ktplace is not built yet -- run scripts/devenv.sh up, or `make` "
            "in the dev container, then start the run again")
    env = clean_env(env)
    rid = new_id()
    work = RUN_ROOT / rid
    work.mkdir(parents=True, exist_ok=True)
    meta = {"benchmark": benchmark, "algorithm": algorithm, "verbose": verbose,
            "env": env, "started": time.time()}
    try:
        (work / "request.json").write_text(
            json_dumps({**meta, "id": rid, "cmd": " ".join(
                [binary, benchmark, "-a", algorithm, "-w", str(work)]
                + (["-v"] if verbose else []))}))
    except OSError:
        pass
    args = [binary, target, "-a", algorithm, "-w", str(work)]
    if verbose:
        args.append("-v")
    # The engine logs to ktplace.log, which it flushes after every record; that
    # file is the stream the page follows, so the child's own stdio is thrown
    # away rather than drained through a pipe (the engine buffered it anyway).
    # The overrides are merged over the server's own environment, not replacing
    # it: the engine still needs PATH and HOME like any other process.
    proc_env = {**os.environ, **env} if env else None
    proc = subprocess.Popen(args, cwd=str(HOME), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, env=proc_env)
    run = Run(rid, meta, work)
    run.proc = proc
    run.status = "running"
    with _RUNS_LOCK:
        _RUNS[rid] = run
    threading.Thread(target=_reap, args=(run,), daemon=True).start()
    return run


def _reap(run: Run) -> None:
    """Wait for the engine to exit and record the outcome."""
    run.exit_code = run.proc.wait()
    run.status = "done" if run.exit_code == 0 else "failed"
    run.ended = time.time()


def index_existing_runs() -> None:
    """Adopt runs already on disk (e.g. from a previous server lifetime)."""
    with _RUNS_LOCK:
        for d in RUN_ROOT.iterdir():
            if not d.is_dir() or not RUN_ID_RE.match(d.name):
                continue
            try:
                meta = json_loads((d / "request.json").read_text()) or {}
            except OSError:
                meta = {}
            run = Run(d.name, meta, d)
            transcript = d / "ktplace.log"
            if transcript.is_file():
                try:
                    text = transcript.read_text(errors="replace")
                except OSError:
                    text = ""
                run.status = "done" if "Placement completed successfully" in text else "failed"
            else:
                run.status = "failed"
            st = meta.get("started")
            if isinstance(st, (int, float)):
                run.started = float(st)
            # Back-done runs have no exit code to read; estimate the finish time
            # from the last artifact written (the placed file).
            last = d / "placed.pl"
            if not last.is_file():
                last = transcript
            try:
                run.ended = float(last.stat().st_mtime)
            except OSError:
                run.ended = run.started
            scan_progress(run)  # so an adopted finished run shows a full bar
            _RUNS[d.name] = run


def find_run(rid: str) -> Run | None:
    with _RUNS_LOCK:
        return _RUNS.get(rid)


def running_count() -> int:
    return sum(1 for r in _RUNS.values() if r.status == "running")


_TRANSCRIPTS = {"log": "ktplace.log", "trace": "ktplace_trace.log"}


def read_log(run: Run, offset: int, stream: str = "log") -> tuple[str, int, bool]:
    """Return (text from offset, new offset, run finished) for one stream.

    The engine writes two files: the transcript (ktplace.log), which carries the
    phase echoes, and the trace file (ktplace_trace.log), which carries the
    per-iteration diagnostics that only `-v` would also print. Both are plain
    appends, so one offset arithmetic serves either -- the page picks with
    ?stream=trace. _TRANSCRIPTS is the allow-list; anything else is the
    transcript, so the file name never comes from the request.
    """
    if stream not in _TRANSCRIPTS:
        stream = "log"
    if offset < 0:
        offset = 0
    data = b""
    path = run.work / _TRANSCRIPTS[stream]
    try:
        with open(path, "rb") as fh:
            fh.seek(offset)
            data = fh.read()
    except OSError:
        data = b""
    return data.decode("utf-8", "replace"), offset + len(data), run.status != "running"


# ------------------------------------------------------------- benchmarks
def looks_like_design_dir(path: pathlib.Path) -> bool:
    try:
        names = [f.name.lower() for f in path.iterdir() if f.is_file()]
    except OSError:
        return False
    return any(n.endswith(_MARKERS) for n in names)


def scan_benchmarks() -> list[str]:
    """Every design directory under the benchmark root, relative paths."""
    found: list[str] = []
    if BENCH_ROOT.is_dir():
        for root, dirs, files in os.walk(BENCH_ROOT):
            dirs.sort()
            p = pathlib.Path(root)
            if looks_like_design_dir(p):
                found.append(str(p.relative_to(BENCH_ROOT)) if p != BENCH_ROOT else "")
        found = [f for f in found if f]
    return sorted(found)


# --------------------------------------------------------------- artifacts
def artifacts(run: Run) -> list[dict]:
    """Key files the run wrote, with browser-sensible titles."""
    want = [
        ("placement gallery", run.work / "plots" / "index.html", "Open the SVG frame gallery and HPWL curve"),
        ("final placement image", run.work / "plots" / "final" / "final.png",
         "High-resolution still of the finished placement"),
        ("placed cells", run.work / "placed.pl", "Bookshelf .pl of the result"),
        ("transcript", run.work / "ktplace.log", "Plain-text run log, as it streamed live"),
        ("trace log", run.work / "ktplace_trace.log",
         "Per-iteration diagnostics the engine writes alongside the transcript"),
        ("animation", run.work / "plots" / "anim" / "placement.gif", "Animated run, when enabled"),
    ]
    out = []
    for title, path, note in want:
        if path.is_file():
            out.append({"title": title, "rel": str(path.relative_to(run.work)),
                        "note": note})
    return out


def summary(run: Run) -> dict:
    """Pick the run's headline numbers out of the transcript. Tolerant: a missing
    line just leaves that key absent, it must not fail the whole read.

    Cached once the run has stopped, so the history table can ask for every run's
    numbers without re-reading every transcript on each poll.
    """
    if run._summary is not None:
        return run._summary
    log = run.work / "ktplace.log"
    if not log.is_file():
        return {}
    try:
        lines = log.read_text(errors="replace").splitlines()
    except OSError:
        return {}
    pairs = (
        ("verdict", "verdict"),
        ("overlaps", "overlap"),
        ("hpwl", "HPWL detailed"),
        ("unplaced", "unplaced"),
        ("offRow", "cells out of rows"),
    )
    result: dict = {}
    for key, needle in pairs:
        vals = []
        for ln in lines:
            if needle.lower() in ln.lower():
                vals.append(ln)
        if not vals:
            continue
        line = vals[-1]
        fields = [f.strip() for f in line.split("|") if f.strip()]
        # Transcript: "<ts> [echo] | key ... | value |" -- the value is the last
        # pipe-delimited field; strip the timestamp/level prefix first.
        fields = [f for f in fields if not re.match(r"^\d{4}-\d\d-\d\d", f)
                  and not f.startswith("[")]
        if len(fields) >= 2:
            result[key] = fields[-1]
    if run.status != "running":
        run._summary = result
    return result


def stop_run(run: Run) -> None:
    """Ask a live run to stop; the reaper thread records the outcome."""
    if run.proc is not None and run.status == "running":
        try:
            run.proc.terminate()
        except OSError:
            pass


def delete_run(run: Run) -> None:
    """Stop the run if it is live, then remove what it wrote."""
    stop_run(run)
    if run.proc is not None and run.status == "running":
        try:
            run.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                run.proc.kill()
            except OSError:
                pass
    with _RUNS_LOCK:
        _RUNS.pop(run.id, None)
    try:
        shutil.rmtree(run.work, ignore_errors=True)
    except OSError:
        pass


def export_zip(run: Run) -> bytes:
    """Everything textual the run wrote, plus the final image, as one archive.

    Deliberately not the frame gallery: a full run's SVG frames run to hundreds
    of megabytes, and the gallery is served from the run directory anyway.
    """
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, path in (
            ("ktplace.log", run.work / "ktplace.log"),
            ("ktplace_trace.log", run.work / "ktplace_trace.log"),
            ("placed.pl", run.work / "placed.pl"),
            ("request.json", run.work / "request.json"),
            ("final.png", run.work / "plots" / "final" / "final.png"),
        ):
            if path.is_file():
                z.write(path, arcname=name)
        z.writestr("summary.json", json_dumps({
            "id": run.id, "benchmark": run.meta.get("benchmark"),
            "algorithm": run.meta.get("algorithm"), "status": run.status,
            "elapsed": round(run.elapsed(), 2), "env": run.env,
            "summary": summary(run),
        }))
    return buf.getvalue()


# ------------------------------------------------------------ command runner
# A small one-shot shell in the console container, off unless KTPLACE_WEB_ALLOW_
# EXEC is set. Output goes to a file, so the page polls it the same way it polls
# a run's transcript -- no websockets, no PTY. It runs as the server's own user
# in the web image, which has python3 and the engine but deliberately no
# toolchain: a build still belongs in the dev container.
class Exec:
    def __init__(self, eid: str, cmd: str, path: pathlib.Path):
        self.id = eid
        self.cmd = cmd
        self.path = path
        self.proc: subprocess.Popen | None = None
        self.status = "running"
        self.exit_code: int | None = None
        self.started = time.time()
        self.ended: float | None = None

    def elapsed(self) -> float:
        end = self.ended if self.ended is not None else time.time()
        return max(0.0, end - self.started)


_EXECS: dict[str, Exec] = {}
_EXECS_LOCK = threading.Lock()


def start_exec(cmd: str) -> Exec:
    EXEC_ROOT.mkdir(parents=True, exist_ok=True)
    eid = new_id()
    path = EXEC_ROOT / f"{eid}.out"
    handle = open(path, "wb")
    try:
        # Its own session, so stop_exec can signal the whole pipeline rather
        # than just the shell that `sh -c` put in front of it.
        proc = subprocess.Popen(cmd, shell=True, cwd=str(HOME),
                                stdin=subprocess.DEVNULL, stdout=handle,
                                stderr=subprocess.STDOUT, start_new_session=True)
    finally:
        handle.close()
    ex = Exec(eid, cmd, path)
    ex.proc = proc
    with _EXECS_LOCK:
        _EXECS[eid] = ex
        if len(_EXECS) > MAX_EXECS:  # drop the oldest finished ones
            done = sorted((e for e in _EXECS.values() if e.status != "running"),
                          key=lambda e: e.started)
            for old in done[:len(_EXECS) - MAX_EXECS]:
                _EXECS.pop(old.id, None)
                try:
                    old.path.unlink()
                except OSError:
                    pass
    threading.Thread(target=_reap_exec, args=(ex,), daemon=True).start()
    return ex


def _reap_exec(ex: Exec) -> None:
    ex.exit_code = ex.proc.wait()
    ex.status = "done" if ex.exit_code == 0 else "failed"
    ex.ended = time.time()


def find_exec(eid: str) -> Exec | None:
    with _EXECS_LOCK:
        return _EXECS.get(eid)


def read_exec(ex: Exec, offset: int) -> tuple[str, int, bool]:
    if offset < 0:
        offset = 0
    data = b""
    try:
        with open(ex.path, "rb") as fh:
            fh.seek(offset)
            data = fh.read()
    except OSError:
        data = b""
    return data.decode("utf-8", "replace"), offset + len(data), ex.status != "running"


def stop_exec(ex: Exec) -> None:
    if ex.proc is None or ex.status != "running":
        return
    try:
        os.killpg(os.getpgid(ex.proc.pid), signal.SIGTERM)
    except OSError:
        pass


def json_dumps(obj) -> str:
    return json.dumps(obj, indent=2, default=_json_default)


def _json_default(o):
    if isinstance(o, (datetime.datetime, datetime.date)):
        return o.isoformat()
    raise TypeError(f"not serialisable: {o!r}")


def json_loads(text: str):
    if not text:
        return None
    return json.loads(text)


# -------------------------------------------------------------------- http
_MIME = {
    ".html": "text/html; charset=utf-8", ".htm": "text/html; charset=utf-8",
    ".css": "text/css; charset=utf-8", ".js": "application/javascript; charset=utf-8",
    ".svg": "image/svg+xml", ".png": "image/png", ".gif": "image/gif",
    ".jpg": "image/jpeg", ".csv": "text/csv; charset=utf-8",
    ".json": "application/json; charset=utf-8", ".log": "text/plain; charset=utf-8",
    ".pl": "text/plain; charset=utf-8", ".txt": "text/plain; charset=utf-8",
    ".gz": "application/gzip",
}


class Handler(BaseHTTPRequestHandler):
    server_version = "KTPlaceWeb/0.1"

    # -- plumbing ----------------------------------------------------
    def log_message(self, fmt, *args):  # quieter access log
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def _send(self, code, body: bytes = b"", ctype="text/plain; charset=utf-8",
              headers=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _json(self, code, obj):
        self._send(code, json_dumps(obj).encode(), "application/json; charset=utf-8")

    def _read_json_body(self) -> dict | None:
        try:
            n = int(self.headers.get("Content-Length", "0"))
            if n <= 0 or n > 1 << 20:
                return None
            return json_loads(self.rfile.read(n).decode("utf-8", "replace"))
        except Exception:
            return None

    # -- routing -----------------------------------------------------
    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if self.path == "/" or self.path == "/index.html":
            return self._serve_file(STATIC_DIR / "index.html")
        if parsed.path.startswith("/api/benchmarks"):
            return self._json(200, {"root": str(BENCH_ROOT), "designs": scan_benchmarks()})
        if parsed.path == "/api/system":
            return self._json(200, system_view())
        if parsed.path.startswith("/api/exec/"):
            parts = parsed.path[len("/api/exec/"):].split("/")
            ex = find_exec(urllib.parse.unquote(parts[0]))
            if ex is None:
                return self._json(404, {"error": "no such command"})
            qs = urllib.parse.parse_qs(parsed.query)
            offset = int(qs.get("offset", ["0"])[0])
            text, new_off, done = read_exec(ex, offset)
            return self._json(200, {
                "offset": new_off, "text": text, "done": done,
                "status": ex.status, "exitCode": ex.exit_code,
                "cmd": ex.cmd, "elapsed": round(ex.elapsed(), 2),
            })
        if parsed.path == "/api/runs":
            runs = []
            for r in sorted(_RUNS.values(), key=lambda r: r.started, reverse=True):
                runs.append(self._run_view(r))
            return self._json(200, {"runs": runs, "maxConcurrent": MAX_RUNS,
                                    "running": running_count()})
        if parsed.path.startswith("/api/runs/"):
            parts = parsed.path[len("/api/runs/"):].split("/")
            rid = urllib.parse.unquote(parts[0])
            run = find_run(rid)
            if run is None:
                return self._json(404, {"error": "no such run"})
            if len(parts) == 1:
                return self._json(200, self._run_view(run, detail=True))
            if parts[1] == "download":
                return self._download_run(run, urllib.parse.parse_qs(parsed.query))
            if parts[1] == "log":
                qs = urllib.parse.parse_qs(parsed.query)
                offset = int(qs.get("offset", ["0"])[0])
                stream = qs.get("stream", ["log"])[0]
                text, new_off, done = read_log(run, offset, stream)
                # The log poll is also the heartbeat: the page already calls it
                # every tick, so progress and liveness ride along instead of
                # costing a second request.
                scan_progress(run)
                return self._json(200, {
                    "offset": new_off, "text": text, "done": done, "stream": stream,
                    "elapsed": round(run.elapsed(), 2),
                    "progress": dict(run.progress),
                    "heartbeat": heartbeat(run),
                })
            return self._json(404, {"error": "unknown endpoint"})
        if parsed.path.startswith("/runs/"):  # files of a run
            return self._serve_run_file(parsed.path[len("/runs/"):])
        return self._json(404, {"error": "not found"})

    def do_POST(self):
        path = urllib.parse.urlparse(self.path).path
        if path == "/api/runs":
            return self._post_run()
        if path == "/api/exec":
            return self._post_exec()
        if path.startswith("/api/runs/"):
            parts = path[len("/api/runs/"):].split("/")
            run = find_run(urllib.parse.unquote(parts[0]))
            if run is None:
                return self._json(404, {"error": "no such run"})
            if len(parts) == 2 and parts[1] == "label":
                body = self._read_json_body() or {}
                run.label = str(body.get("label", "")).strip()[:80]
                run.meta["label"] = run.label
                try:
                    (run.work / "request.json").write_text(
                        json_dumps({**run.meta, "id": run.id, "env": run.env}))
                except OSError:
                    pass
                return self._json(200, self._run_view(run, detail=True))
        return self._json(404, {"error": "not found"})

    def _post_run(self):
        body = self._read_json_body()
        if not isinstance(body, dict):
            return self._json(400, {"error": "expected a JSON body"})
        benchmark = str(body.get("benchmark", "")).strip()
        algorithm = str(body.get("algorithm", "simpl")).strip()
        verbose = bool(body.get("verbose", False))
        env = clean_env(body.get("env"))
        if algorithm not in ALGORITHMS:
            return self._json(400, {"error": f"algorithm must be one of {ALGORITHMS}"})
        target = (BENCH_ROOT / benchmark).resolve()
        try:
            target.relative_to(BENCH_ROOT)
        except ValueError:
            return self._json(400, {"error": "benchmark must live under the benchmark root"})
        if not target.is_dir() or not looks_like_design_dir(target):
            return self._json(400, {"error": f"no design found at {benchmark}"})
        if running_count() >= MAX_RUNS:
            return self._json(409, {"error": "a run is already in progress"})
        try:
            run = launch(benchmark, str(target), algorithm, verbose, env)
        except EngineNotBuilt as e:
            return self._json(503, {"error": str(e)})
        except OSError as e:
            return self._json(500, {"error": f"could not start ktplace: {e}"})
        return self._json(201, self._run_view(run))

    def _post_exec(self):
        if not ALLOW_EXEC:
            return self._json(403, {"error":
                "the command runner is off; start the console with "
                "KTPLACE_WEB_ALLOW_EXEC=1 to enable it"})
        body = self._read_json_body()
        if not isinstance(body, dict):
            return self._json(400, {"error": "expected a JSON body"})
        cmd = str(body.get("cmd", "")).strip()
        if not cmd:
            return self._json(400, {"error": "cmd is required"})
        if len(cmd) > 2000:
            return self._json(400, {"error": "cmd is too long (2000 characters)"})
        with _EXECS_LOCK:
            live = sum(1 for e in _EXECS.values() if e.status == "running")
        if live >= 3:
            return self._json(429, {"error": "three commands are already running"})
        try:
            ex = start_exec(cmd)
        except OSError as e:
            return self._json(500, {"error": f"could not start the command: {e}"})
        return self._json(201, self._exec_view(ex))

    def do_DELETE(self):
        path = urllib.parse.urlparse(self.path).path
        if path.startswith("/api/runs/"):
            rid = urllib.parse.unquote(path[len("/api/runs/"):].split("/")[0])
            run = find_run(rid)
            if run is None:
                return self._json(404, {"error": "no such run"})
            delete_run(run)
            return self._json(200, {"deleted": rid})
        if path.startswith("/api/exec/"):
            eid = urllib.parse.unquote(path[len("/api/exec/"):].split("/")[0])
            ex = find_exec(eid)
            if ex is None:
                return self._json(404, {"error": "no such command"})
            stop_exec(ex)
            return self._json(200, {"stopped": eid})
        return self._json(404, {"error": "not found"})

    # -- bodies ------------------------------------------------------
    def _exec_view(self, ex: Exec) -> dict:
        return {"id": ex.id, "cmd": ex.cmd, "status": ex.status,
                "started": ex.started, "elapsed": round(ex.elapsed(), 2),
                "exitCode": ex.exit_code}

    def _run_view(self, run: Run, detail=False) -> dict:
        view = {
            "id": run.id,
            "benchmark": run.meta.get("benchmark") or run.id,
            "algorithm": run.meta.get("algorithm") or "simpl",
            "label": run.label,
            "verbose": bool(run.meta.get("verbose")),
            "env": run.env,
            "status": run.status, "started": run.started,
            "elapsed": round(run.elapsed(), 2), "exitCode": run.exit_code,
            # The headline numbers ride on every view: the history table compares
            # runs by them, and summary() is cached once a run has stopped. The
            # live heartbeat does not: it walks the frame directories, and is
            # only worth that for the run actually being watched.
            "summary": summary(run),
            "progress": round(run.progress["fraction"], 3),
        }
        if detail:
            scan_progress(run)
            view["artifacts"] = artifacts(run)
            view["workDir"] = str(run.work)
            view["progress"] = dict(run.progress)
            view["heartbeat"] = heartbeat(run)
        return view

    def _download_run(self, run: Run, qs) -> None:
        what = (qs.get("what", ["log"])[0] or "log").lower()
        if what == "all":
            try:
                body = export_zip(run)
            except OSError as e:
                return self._json(500, {"error": f"could not build the archive: {e}"})
            return self._send_download(f"ktplace-{run.id}.zip", body, "application/zip")
        files = {
            "log": ("ktplace.log", "text/plain; charset=utf-8"),
            "trace": ("ktplace_trace.log", "text/plain; charset=utf-8"),
            "placed": ("placed.pl", "text/plain; charset=utf-8"),
            "request": ("request.json", "application/json; charset=utf-8"),
        }
        if what not in files:
            return self._json(404, {"error": "what must be log, trace, placed, request or all"})
        name, ctype = files[what]
        return self._stream_download(run.work / name, f"{run.id}-{name}", ctype)

    def _send_download(self, filename: str, body: bytes, ctype: str) -> None:
        self._send(200, body, ctype,
                   {"Content-Disposition": f'attachment; filename="{filename}"'})

    def _stream_download(self, path: pathlib.Path, filename: str, ctype: str) -> None:
        try:
            size = path.stat().st_size
        except OSError:
            return self._json(404, {"error": f"no such file: {path.name}"})
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(size))
        self.send_header("Content-Disposition", f'attachment; filename="{filename}"')
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if self.command == "HEAD":
            return
        try:
            with open(path, "rb") as fh:  # streamed: a trace log can be large
                shutil.copyfileobj(fh, self.wfile, length=1 << 16)
        except OSError:
            pass

    def _serve_run_file(self, rel: str) -> None:
        parts = rel.split("/")
        rid = urllib.parse.unquote(parts[0])
        if not RUN_ID_RE.match(rid):
            return self._json(404, {"error": "not found"})
        run = find_run(rid)
        base = (RUN_ROOT / rid).resolve()
        rest = "/".join(urllib.parse.unquote(p) for p in parts[1:])
        target = (base / rest).resolve()
        try:
            target.relative_to(base)
        except ValueError:
            return self._json(403, {"error": "outside the run directory"})
        return self._serve_file(target)

    def _serve_file(self, path: pathlib.Path) -> None:
        if not path.is_file():
            return self._json(404, {"error": f"no such file: {path.name}"})
        suffix = path.suffix.lower()
        ctype = _MIME.get(suffix, "application/octet-stream")
        try:
            with open(path, "rb") as fh:
                body = fh.read()
        except OSError:
            return self._json(500, {"error": "could not read file"})
        self._send(200, body, ctype)


def main() -> None:
    RUN_ROOT.mkdir(parents=True, exist_ok=True)
    index_existing_runs()
    server = ThreadingHTTPServer((HOST, PORT), Handler)
    print(f"KTPlace web console on http://{HOST}:{PORT}  "
          f"(benchmarks: {BENCH_ROOT}, runs: {RUN_ROOT}, "
          f"ktplace: {resolve_bin() or 'not built yet'}, "
          f"command runner: {'on' if ALLOW_EXEC else 'off'})",
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()