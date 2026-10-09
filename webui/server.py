#!/usr/bin/env python3
"""KTPlace web console.

Run the placement engine from a browser and review what it wrote. The engine is
invoked directly -- this server and ktplace share one container -- every run
gets its own directory under the runs root, and the transcript streams to the
page as it is produced. The static gallery ktplace itself writes
(plots/index.html) is served out of the run directory, so the review half is the
engine's own output, not a second implementation of it.

Stdlib only: python3 is the only dependency beyond what the run image already
carries.

Endpoints
  GET  /                       the console page
  GET  /api/benchmarks         designs the console can run
  GET  /api/runs               run history, rebuilt from the runs root
  POST /api/runs               start a run: {"benchmark","algorithm","verbose"}
  GET  /api/runs/<id>          status, result summary and artifact links
  GET  /api/runs/<id>/log?offset=  next transcript chunk (poll this)
  GET  /runs/<id>/<path>       a file of a run (gallery, png, placed.pl, log)
"""

from __future__ import annotations

import datetime
import json
import os
import pathlib
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# ------------------------------------------------------------------ config
HOME = pathlib.Path(os.environ.get("KTPLACE_HOME", "/ktplace")).resolve()
BENCH_ROOT = pathlib.Path(os.environ.get("KTPLACE_BENCH_ROOT", str(HOME / "benchmark"))).resolve()
RUN_ROOT = pathlib.Path(os.environ.get("KTPLACE_RUN_ROOT", str(HOME / "runs"))).resolve()
HOST = os.environ.get("KTPLACE_WEB_HOST", "127.0.0.1")
PORT = int(os.environ.get("KTPLACE_WEB_PORT", "8080"))
KTPLACE_BIN = os.environ.get("KTPLACE_BIN", "ktplace")
MAX_RUNS = max(1, int(os.environ.get("KTPLACE_WEB_MAX_RUNS", "1")))
ALGORITHMS = ("simpl", "ntuplace1")  # what FlowMgr::runPlacement dispatches on
RUN_ID_RE = re.compile(r"^\d{8}-\d{6}-[0-9a-f]{8}$")

WEBUI_DIR = pathlib.Path(__file__).resolve().parent
STATIC_DIR = WEBUI_DIR / "static"

_MARKERS = (".def", ".def.gz", ".nodes", ".nodes.gz")


# ------------------------------------------------------------------- runs
class Run:
    """A single ktplace invocation and everything it produced."""

    def __init__(self, rid: str, meta: dict, work: pathlib.Path):
        self.id = rid
        self.meta = meta  # benchmark, algorithm, verbose, started
        self.work = work
        self.proc: subprocess.Popen | None = None
        self.status = "unknown"  # reset by launch / index_existing_runs
        self.exit_code: int | None = None
        self.started = meta.get("started", time.time())
        self.ended: float | None = None

    def elapsed(self) -> float:
        end = self.ended if self.ended is not None else time.time()
        return max(0.0, end - self.started)


_RUNS: dict[str, Run] = {}
_RUNS_LOCK = threading.Lock()


def new_id() -> str:
    stamp = time.strftime("%Y%m%d-%H%M%S")
    return f"{stamp}-{uuid.uuid4().hex[:8]}"


def launch(benchmark: str, target: str, algorithm: str, verbose: bool) -> Run:
    rid = new_id()
    work = RUN_ROOT / rid
    work.mkdir(parents=True, exist_ok=True)
    meta = {"benchmark": benchmark, "algorithm": algorithm, "verbose": verbose,
            "started": time.time()}
    try:
        (work / "request.json").write_text(
            json_dumps({**meta, "id": rid, "cmd": " ".join(
                [KTPLACE_BIN, benchmark, "-a", algorithm, "-w", str(work)]
                + (["-v"] if verbose else []))}))
    except OSError:
        pass
    args = [KTPLACE_BIN, target, "-a", algorithm, "-w", str(work)]
    if verbose:
        args.append("-v")
    # The engine logs to ktplace.log, which it flushes after every record; that
    # file is the stream the page follows, so the child's own stdio is thrown
    # away rather than drained through a pipe (the engine buffered it anyway).
    proc = subprocess.Popen(args, cwd=str(HOME), stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
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
            _RUNS[d.name] = run


def find_run(rid: str) -> Run | None:
    with _RUNS_LOCK:
        return _RUNS.get(rid)


def running_count() -> int:
    return sum(1 for r in _RUNS.values() if r.status == "running")


def read_log(run: Run, offset: int) -> tuple[str, int, bool]:
    """Return (text from offset, new offset, run finished)."""
    if offset < 0:
        offset = 0
    data = b""
    path = run.work / "ktplace.log"
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
    line just leaves that key absent, it must not fail the whole read."""
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
    return result


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
            if parts[1] == "log":
                qs = urllib.parse.parse_qs(parsed.query)
                offset = int(qs.get("offset", ["0"])[0])
                text, new_off, done = read_log(run, offset)
                return self._json(200, {"offset": new_off, "text": text, "done": done})
            return self._json(404, {"error": "unknown endpoint"})
        if parsed.path.startswith("/runs/"):  # files of a run
            return self._serve_run_file(parsed.path[len("/runs/"):])
        return self._json(404, {"error": "not found"})

    def do_POST(self):
        if self.path != "/api/runs":
            return self._json(404, {"error": "not found"})
        body = self._read_json_body()
        if not isinstance(body, dict):
            return self._json(400, {"error": "expected a JSON body"})
        benchmark = str(body.get("benchmark", "")).strip()
        algorithm = str(body.get("algorithm", "simpl")).strip()
        verbose = bool(body.get("verbose", False))
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
            run = launch(benchmark, str(target), algorithm, verbose)
        except OSError as e:
            return self._json(500, {"error": f"could not start ktplace: {e}"})
        return self._json(201, self._run_view(run))

    # -- bodies ------------------------------------------------------
    def _run_view(self, run: Run, detail=False) -> dict:
        view = {
            "id": run.id,
            "benchmark": run.meta.get("benchmark") or run.id,
            "algorithm": run.meta.get("algorithm") or "simpl",
            "verbose": bool(run.meta.get("verbose")),
            "status": run.status, "started": run.started,
            "elapsed": round(run.elapsed(), 2), "exitCode": run.exit_code,
        }
        if detail:
            view["summary"] = summary(run)
            view["artifacts"] = artifacts(run)
            view["workDir"] = str(run.work)
        return view

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
          f"(benchmarks: {BENCH_ROOT}, runs: {RUN_ROOT}, ktplace: {KTPLACE_BIN})",
          flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()