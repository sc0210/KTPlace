#!/usr/bin/env python3
"""Unit tests for the KTPlace web console (webui/server.py).

Stdlib unittest only, no third-party anything -- the server itself is stdlib
only, and its tests stay that way too. A live server runs in a thread against
a temporary HOME (fixture benchmarks, an empty runs root, a fake `ktplace`
binary), so these are the real HTTP endpoints, not the functions under them.

Run from the repository root:

    python3 webui/test/test_server.py
    python3 -m unittest discover -s webui/test
"""

import http.client
import http.server
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import zipfile
from pathlib import Path

TEST_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_DIR.parent))

TMP = Path(tempfile.mkdtemp(prefix="ktplace-webui-test-"))
HOME = TMP / "home"
BENCH = HOME / "benchmark"
RUNS = HOME / "runs"
BINDIR = TMP / "bin"
BINDIR.mkdir(parents=True)

# One fixture design (a directory with a Bookshelf marker file) and one
# directory that is not a design.
(BENCH / "toy" / "tiny").mkdir(parents=True)
(BENCH / "toy" / "tiny" / "tiny.nodes").write_text("UCLA nodes 1.0\n")
(BENCH / "empty").mkdir(parents=True)

# A fake engine: writes what a finished run would, understands -w, sleeps when
# the run asks it to (KTPLACE_SLOW_RUN seconds) so concurrency tests have a
# live run to trip over.
FAKE_BIN = BINDIR / "ktplace"
FAKE_BIN.write_text(
    "#!/bin/sh\n"
    'work=""; prev=""\n'
    'for a in "$@"; do [ "$prev" = "-w" ] && work="$a"; prev="$a"; done\n'
    'sleep "${KTPLACE_SLOW_RUN:-0}"\n'
    'printf "2026-01-01 00:00:00.000 [echo] | loaded: fake |\\n" > "$work/ktplace.log"\n'
    'printf "2026-01-01 00:00:00.000 [echo] | running fake global placement |\\n" >> "$work/ktplace.log"\n'
    'printf "2026-01-01 00:00:00.000 [echo] | verdict | PASS |\\n" >> "$work/ktplace.log"\n'
    'printf "fake trace\\n" > "$work/ktplace_trace.log"\n'
    'printf "p1 0 0 : N\\n" > "$work/placed.pl"\n'
    "exit 0\n"
)
FAKE_BIN.chmod(0o755)

os.environ.update({
    "KTPLACE_HOME": str(HOME),
    "KTPLACE_BIN": str(FAKE_BIN),
    "KTPLACE_WEB_ALLOW_EXEC": "1",
})

import server as console  # noqa: E402  (env above must win)


def wait_until(predicate, timeout=10.0, step=0.05):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(step)
    return False


class FakeDockerHandler(http.server.BaseHTTPRequestHandler):
    """Just enough of the Engine API for the console's build path."""

    def log_message(self, *args):
        pass

    def _read_body(self):
        n = int(self.headers.get("Content-Length", "0") or 0)
        return self.rfile.read(n) if n else b""

    def _send_json(self, code, obj):
        data = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_POST(self):
        body = self._read_body()
        if self.path.endswith("/exec"):
            try:
                cmd = json.loads(body.decode() or "{}").get("Cmd", [])
            except ValueError:
                cmd = []
            if self.server.fail_create:
                return self._send_json(404, {"message": "no such container"})
            if any("pkill" in c for c in cmd):
                self.server.stops.append(cmd)
                return self._send_json(201, {"Id": "stop-1"})
            return self._send_json(201, {"Id": "build-1"})
        if self.path == "/exec/build-1/start":
            time.sleep(self.server.start_delay)
            frames = b"".join(
                struct.pack(">BxxxI", 1, len(chunk)) + chunk
                for chunk in self.server.chunks)
            self.send_response(200)
            self.send_header("Content-Type",
                             "application/vnd.docker.multiplexed-stream")
            self.send_header("Content-Length", str(len(frames)))
            self.end_headers()
            self.wfile.write(frames)
            return
        if self.path == "/exec/stop-1/start":
            self.send_response(204)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        return self._send_json(404, {"message": "unknown"})

    def do_GET(self):
        if self.path == "/exec/build-1/json":
            return self._send_json(200, {"ExitCode": self.server.exit_code})
        return self._send_json(404, {"message": "unknown"})


class FakeDockerServer(http.server.HTTPServer):
    address_family = socket.AF_UNIX

    def __init__(self, path):
        try:
            os.unlink(path)
        except OSError:
            pass
        self.stops = []
        self.chunks = [b"building...\n"]
        self.exit_code = 0
        self.start_delay = 0.0
        self.fail_create = False
        super().__init__(str(path), FakeDockerHandler)


class ConsoleCase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        RUNS.mkdir(parents=True, exist_ok=True)
        cls.server = console.ThreadingHTTPServer(("127.0.0.1", 0), console.Handler)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.docker_path = str(TMP / "docker.sock")
        cls.docker = FakeDockerServer(cls.docker_path)
        cls.docker_thread = threading.Thread(target=cls.docker.serve_forever,
                                             daemon=True)
        cls.docker_thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.docker.shutdown()
        cls.docker.server_close()
        try:
            os.unlink(cls.docker_path)
        except OSError:
            pass
        shutil.rmtree(TMP, ignore_errors=True)

    def setUp(self):
        self._saved_socket = console.DOCKER_SOCKET
        self._saved_allow = console.ALLOW_EXEC
        self._saved_resolve = console.resolve_bin
        console.DOCKER_SOCKET = self.docker_path
        self.docker.fail_create = False
        self.docker.start_delay = 0.0
        self.docker.exit_code = 0
        self.docker.stops.clear()
        self._runs = []
        self._execs = []

    def tearDown(self):
        console.DOCKER_SOCKET = self._saved_socket
        console.ALLOW_EXEC = self._saved_allow
        console.resolve_bin = self._saved_resolve
        for rid in self._runs:
            try:
                self.api("DELETE", f"/api/runs/{rid}")
            except OSError:
                pass
            with console._RUNS_LOCK:
                console._RUNS.pop(rid, None)
            shutil.rmtree(RUNS / rid, ignore_errors=True)
        for eid in self._execs:
            try:
                self.api("DELETE", f"/api/exec/{eid}")
            except OSError:
                pass

    # -- plumbing ----------------------------------------------------
    def api(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        data, hdrs = None, dict(headers or {})
        if body is not None:
            data = json.dumps(body).encode()
            hdrs.setdefault("Content-Type", "application/json")
        conn.request(method, path, body=data, headers=hdrs)
        resp = conn.getresponse()
        payload = resp.read()
        ctype = resp.getheader("Content-Type", "")
        if ctype.startswith("application/json"):
            payload = json.loads(payload.decode())
        return resp.status, dict(resp.getheaders()), payload

    def raw(self, request: bytes) -> bytes:
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=15)
        try:
            sock.sendall(request)
            out = b""
            while True:
                chunk = sock.recv(1 << 16)
                if not chunk:
                    break
                out += chunk
                if len(out) > (1 << 20):
                    break
            return out
        finally:
            sock.close()

    def start_run(self, **kw):
        body = {"benchmark": "toy/tiny", "algorithm": "simpl",
                "verbose": False, "env": {}}
        body.update(kw)
        status, _, run = self.api("POST", "/api/runs", body)
        self.assertEqual(status, 201, run)
        self._runs.append(run["id"])
        return run

    def wait_run(self, rid, timeout=15.0):
        self.assertTrue(
            wait_until(lambda: console.find_run(rid) is not None
                       and console.find_run(rid).status != "running", timeout),
            f"run {rid} never finished")

    def make_fixture_run(self, rid, log="", extra=None):
        """A run directory on disk, adopted without a process."""
        work = RUNS / rid
        work.mkdir(parents=True, exist_ok=True)
        meta = {"benchmark": "toy/tiny", "algorithm": "simpl", "verbose": False,
                "env": {}, "started": time.time()}
        (work / "request.json").write_text(console.json_dumps({**meta, "id": rid}))
        (work / "ktplace.log").write_text(log)
        for name, text in (extra or {}).items():
            (work / name).write_text(text)
        console.index_existing_runs()
        self._runs.append(rid)
        return rid

    # -- benchmarks / system ------------------------------------------
    def test_benchmarks_lists_fixture_design(self):
        status, _, body = self.api("GET", "/api/benchmarks")
        self.assertEqual(status, 200)
        self.assertEqual(body["designs"], ["toy/tiny"])
        self.assertTrue(body["root"].endswith("benchmark"))

    def test_system_shape(self):
        status, _, body = self.api("GET", "/api/system")
        self.assertEqual(status, 200)
        self.assertIn("cpu", body)
        self.assertIn("mem", body)

    # -- trust gates ---------------------------------------------------
    def test_foreign_host_refused(self):
        out = self.raw(b"GET /api/runs HTTP/1.1\r\nHost: evil.com\r\n"
                       b"Connection: close\r\n\r\n")
        self.assertIn(b"403", out.split(b"\r\n", 1)[0])

    def test_post_without_json_content_type_refused(self):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        conn.request("POST", "/api/runs", body=b"{}",
                     headers={"Content-Type": "text/plain"})
        self.assertEqual(conn.getresponse().status, 415)

    def test_cross_origin_post_refused(self):
        status, _, body = self.api("POST", "/api/runs", {},
                                   headers={"Origin": "http://evil.com/"})
        self.assertEqual(status, 403)

    # -- run validation -------------------------------------------------
    def test_run_rejects_unknown_benchmark(self):
        status, _, body = self.api("POST", "/api/runs",
                                   {"benchmark": "nope", "algorithm": "simpl"})
        self.assertEqual(status, 400)

    def test_run_rejects_escape_from_benchmark_root(self):
        for bad in ("../x", "/etc", "toy/../../etc"):
            status, _, _ = self.api("POST", "/api/runs",
                                    {"benchmark": bad, "algorithm": "simpl"})
            self.assertEqual(status, 400, bad)

    def test_run_rejects_unknown_algorithm(self):
        status, _, _ = self.api("POST", "/api/runs",
                                {"benchmark": "toy/tiny", "algorithm": "nope"})
        self.assertEqual(status, 400)

    def test_run_rejects_non_dict_body(self):
        status, _, _ = self.api("POST", "/api/runs", ["x"])
        self.assertEqual(status, 400)

    def test_run_reports_engine_not_built(self):
        console.resolve_bin = lambda: None
        status, _, body = self.api("POST", "/api/runs",
                                   {"benchmark": "toy/tiny", "algorithm": "simpl"})
        self.assertEqual(status, 503)
        self.assertIn("built", body["error"])

    def test_run_rejects_second_concurrent_run(self):
        run = self.start_run(env={"KTPLACE_SLOW_RUN": "5"})
        try:
            status, _, body = self.api("POST", "/api/runs",
                                       {"benchmark": "toy/tiny",
                                        "algorithm": "simpl"})
            self.assertEqual(status, 409)
        finally:
            self.api("DELETE", f"/api/runs/{run['id']}")

    # -- run lifecycle ---------------------------------------------------
    def test_run_lifecycle(self):
        run = self.start_run()
        rid = run["id"]
        self.assertEqual(run["status"], "running")
        self.wait_run(rid)
        status, _, detail = self.api("GET", f"/api/runs/{rid}")
        self.assertEqual(status, 200)
        self.assertEqual(detail["status"], "done")
        self.assertEqual(detail["exitCode"], 0)
        self.assertEqual(detail["summary"].get("verdict"), "PASS")
        self.assertTrue(detail["artifacts"])
        self.assertIn("downloads", detail)

    def test_run_log_streams_transcript(self):
        rid = self.start_run()["id"]
        self.wait_run(rid)
        status, _, body = self.api("GET", f"/api/runs/{rid}/log?offset=0")
        self.assertEqual(status, 200)
        self.assertIn("loaded: fake", body["text"])
        self.assertTrue(body["done"])
        # reading past the end hands nothing out and stays done
        status, _, again = self.api(
            "GET", f"/api/runs/{rid}/log?offset={body['offset']}")
        self.assertEqual((status, again["text"], again["done"]), (200, "", True))

    def test_run_label_round_trip(self):
        rid = self.start_run()["id"]
        self.wait_run(rid)
        status, _, updated = self.api("POST", f"/api/runs/{rid}/label",
                                      {"label": "hello"})
        self.assertEqual((status, updated["label"]), (200, "hello"))
        stored = json.loads((RUNS / rid / "request.json").read_text())
        self.assertEqual(stored["label"], "hello")
        status, _, cleared = self.api("POST", f"/api/runs/{rid}/label",
                                      {"label": "  "})
        self.assertEqual((status, cleared["label"]), (200, ""))

    def test_run_delete_removes_directory(self):
        rid = self.start_run()["id"]
        self.wait_run(rid)
        self.assertTrue((RUNS / rid).is_dir())
        status, _, body = self.api("DELETE", f"/api/runs/{rid}")
        self.assertEqual((status, body["deleted"]), (200, rid))
        self._runs.remove(rid)
        self.assertFalse((RUNS / rid).exists())
        status, _, _ = self.api("GET", f"/api/runs/{rid}")
        self.assertEqual(status, 404)

    def test_unknown_run_is_404(self):
        for method, path in (("GET", "/api/runs/20260101-000000-deadbeef"),
                             ("POST", "/api/runs/20260101-000000-deadbeef/label"),
                             ("DELETE", "/api/runs/20260101-000000-deadbeef"),
                             ("GET", "/api/runs/20260101-000000-deadbeef/log?"
                              "offset=0")):
            status, _, _ = self.api(method, path, {} if method == "POST" else None)
            self.assertEqual(status, 404, f"{method} {path}")

    # -- summary / artifacts / downloads ----------------------------------
    LOG = (
        "2026-01-01 00:00:00.000 [echo] | running simpl global placement |\n"
        "2026-01-01 00:00:01.000 [echo] | HPWL after | 9.99 |\n"
        "2026-01-01 00:00:02.000 [echo] | running fastdp detailed placement (FastDP) |\n"
        "2026-01-01 00:00:03.000 [echo] | HPWL after | 1.23 |\n"
        "2026-01-01 00:00:04.000 [echo] | overlapping pairs | 0 |\n"
        "2026-01-01 00:00:05.000 [echo] | cells out of rows | 2 |\n"
        "2026-01-01 00:00:06.000 [echo] | verdict | PASS |\n"
    )

    def test_summary_reads_final_hpwl_not_stages(self):
        rid = self.make_fixture_run("20260101-000000-aa000001", log=self.LOG)
        status, _, detail = self.api("GET", f"/api/runs/{rid}")
        self.assertEqual(status, 200)
        summary = detail["summary"]
        self.assertEqual(summary["hpwl"], "1.23")  # the detail table's, not 9.99
        self.assertEqual(summary["verdict"], "PASS")
        self.assertEqual(summary["overlaps"], "0")
        self.assertEqual(summary["offRow"], "2")

    def test_summary_reads_legacy_hpwl_line(self):
        rid = self.make_fixture_run(
            "20260101-000000-aa000002",
            log="2026-01-01 00:00:00.000 [echo] | HPWL detailed | 4.5 |\n")
        _, _, detail = self.api("GET", f"/api/runs/{rid}")
        self.assertEqual(detail["summary"].get("hpwl"), "4.5")

    def test_downloads_offer_only_existing_files(self):
        # the reported bug: every button rendered for every run, and a missing
        # placed.pl downloaded a 404 JSON as a file.
        rid = self.make_fixture_run("20260101-000000-aa000003", log="x\n")
        _, _, detail = self.api("GET", f"/api/runs/{rid}")
        self.assertEqual(detail["downloads"],
                         [["transcript", "log"], ["request.json", "request"],
                          ["everything (.zip)", "all"]])
        rid2 = self.make_fixture_run(
            "20260101-000000-aa000004", log="x\n",
            extra={"ktplace_trace.log": "t\n", "placed.pl": "p\n"})
        _, _, detail2 = self.api("GET", f"/api/runs/{rid2}")
        self.assertEqual(detail2["downloads"],
                         [["transcript", "log"], ["trace", "trace"],
                          ["placed.pl", "placed"], ["request.json", "request"],
                          ["everything (.zip)", "all"]])

    def test_download_each_file(self):
        rid = self.make_fixture_run(
            "20260101-000000-aa000005", log="log-bytes\n",
            extra={"ktplace_trace.log": "trace-bytes\n", "placed.pl": "placed-bytes\n"})
        for what, fragment in (("log", b"log-bytes"), ("trace", b"trace-bytes"),
                               ("placed", b"placed-bytes")):
            status, headers, payload = self.api(
                "GET", f"/api/runs/{rid}/download?what={what}")
            self.assertEqual(status, 200, what)
            self.assertIn("attachment", headers.get("Content-Disposition", ""))
            self.assertIn(rid, headers.get("Content-Disposition", ""))
            self.assertIn(fragment, payload)
            self.assertTrue(
                int(headers["Content-Length"]) >= len(fragment), what)
        status, _, body = self.api("GET", f"/api/runs/{rid}/download?what=request")
        self.assertEqual(status, 200)
        self.assertEqual(body["benchmark"], "toy/tiny")

    def test_download_missing_file_is_404_json(self):
        rid = self.make_fixture_run("20260101-000000-aa000006", log="x\n")
        status, headers, body = self.api("GET", f"/api/runs/{rid}/download?what=placed")
        self.assertEqual(status, 404)
        self.assertIn("placed.pl", body["error"])
        self.assertNotIn("Content-Disposition", headers)

    def test_download_rejects_unknown_what(self):
        rid = self.make_fixture_run("20260101-000000-aa000007", log="x\n")
        status, _, body = self.api("GET", f"/api/runs/{rid}/download?what=bogus")
        self.assertEqual(status, 404)

    def test_download_all_is_a_valid_zip(self):
        rid = self.make_fixture_run(
            "20260101-000000-aa000008", log=self.LOG,
            extra={"placed.pl": "p\n", "ktplace_trace.log": "t\n"})
        status, headers, payload = self.api("GET", f"/api/runs/{rid}/download?what=all")
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Content-Type"), "application/zip")
        path = TMP / "all.zip"
        path.write_bytes(payload)
        with zipfile.ZipFile(path) as archive:
            members = set(archive.namelist())
            self.assertIn("summary.json", members)
            self.assertIn("placed.pl", members)
            summary = json.loads(archive.read("summary.json"))
        self.assertEqual(summary["summary"]["verdict"], "PASS")

    def test_download_all_works_with_barely_anything(self):
        rid = self.make_fixture_run("20260101-000000-aa000009", log="")
        (RUNS / rid / "ktplace.log").unlink()
        status, _, payload = self.api("GET", f"/api/runs/{rid}/download?what=all")
        self.assertEqual(status, 200)
        path = TMP / "bare.zip"
        path.write_bytes(payload)
        with zipfile.ZipFile(path) as archive:
            self.assertEqual(archive.namelist(),
                             ["request.json", "summary.json"])

    # -- run files ----------------------------------------------------------
    def test_run_file_served(self):
        rid = self.make_fixture_run("20260101-000000-aa000010", log="hello-log\n")
        status, headers, payload = self.api("GET", f"/runs/{rid}/ktplace.log")
        self.assertEqual(status, 200)
        self.assertIn(b"hello-log", payload)

    def test_run_file_escape_refused(self):
        rid = self.make_fixture_run("20260101-000000-aa000011", log="x\n")
        status, _, _ = self.api("GET", f"/runs/{rid}/../../x")
        self.assertEqual(status, 403)

    def test_run_file_missing_is_404(self):
        rid = self.make_fixture_run("20260101-000000-aa000012", log="x\n")
        status, _, _ = self.api("GET", f"/runs/{rid}/nope.txt")
        self.assertEqual(status, 404)
        status, _, _ = self.api("GET", "/runs/not-an-id/ktplace.log")
        self.assertEqual(status, 404)

    # -- command runner ------------------------------------------------------
    def test_exec_round_trip(self):
        status, _, ex = self.api("POST", "/api/exec", {"cmd": "echo hi"})
        self.assertEqual(status, 201)
        self._execs.append(ex["id"])
        self.assertTrue(wait_until(
            lambda: self.api("GET", f"/api/exec/{ex['id']}?offset=0")[2]["done"]))
        _, _, view = self.api("GET", f"/api/exec/{ex['id']}?offset=0")
        self.assertEqual(view["status"], "done")
        self.assertEqual(view["exitCode"], 0)
        self.assertIn("hi", view["text"])

    def test_exec_validation(self):
        for body, want in (({}, 400), ({"cmd": "  "}, 400),
                           ({"cmd": "x" * 2001}, 400), (["x"], 400)):
            status, _, _ = self.api("POST", "/api/exec", body)
            self.assertEqual(status, want, body)

    def test_exec_disabled(self):
        console.ALLOW_EXEC = False
        status, _, body = self.api("POST", "/api/exec", {"cmd": "echo hi"})
        self.assertEqual(status, 403)

    def test_exec_stop(self):
        _, _, ex = self.api("POST", "/api/exec", {"cmd": "sleep 30"})
        self._execs.append(ex["id"])
        status, _, body = self.api("DELETE", f"/api/exec/{ex['id']}")
        self.assertEqual((status, body["stopped"]), (200, ex["id"]))
        self.assertTrue(wait_until(
            lambda: self.api("GET", f"/api/exec/{ex['id']}?offset=0")[2]["done"]))

    def test_exec_unknown_id_is_404(self):
        for method, path in (("GET", "/api/exec/nope?offset=0"),
                             ("DELETE", "/api/exec/nope")):
            status, _, _ = self.api(method, path)
            self.assertEqual(status, 404)

    def test_exec_fourth_concurrent_refused(self):
        ids = []
        try:
            for _ in range(3):
                _, _, ex = self.api("POST", "/api/exec", {"cmd": "sleep 30"})
                ids.append(ex["id"])
            status, _, _ = self.api("POST", "/api/exec", {"cmd": "echo late"})
            self.assertEqual(status, 429)
        finally:
            for eid in ids:
                self._execs.append(eid)

    # -- engine build ----------------------------------------------------------
    def test_build_without_socket_is_503(self):
        console.DOCKER_SOCKET = str(TMP / "no-such-socket")
        status, _, body = self.api("POST", "/api/build", {})
        self.assertEqual(status, 503)
        self.assertIn("socket", body["error"])

    def test_build_start_poll_done(self):
        self.docker.chunks = [b"Building...\n", b"Build complete\n"]
        status, _, build = self.api("POST", "/api/build", {})
        self.assertEqual(status, 201)
        self.assertEqual(build["status"], "running")
        self.assertTrue(wait_until(
            lambda: self.api("GET", "/api/build?offset=0")[2]["done"],
            timeout=15.0))
        _, _, view = self.api("GET", "/api/build?offset=0")
        self.assertEqual(view["status"], "done")
        self.assertEqual(view["exitCode"], 0)
        self.assertIn("Build complete", view["text"])

    def test_build_second_post_is_409_while_running(self):
        self.docker.start_delay = 2.0
        self.api("POST", "/api/build", {})
        try:
            status, _, _ = self.api("POST", "/api/build", {})
            self.assertEqual(status, 409)
        finally:
            self.assertTrue(wait_until(
                lambda: self.api("GET", "/api/build?offset=0")[2]["done"],
                timeout=15.0))

    def test_build_stop_interrupts(self):
        self.docker.start_delay = 5.0
        self.docker.exit_code = 130
        self.api("POST", "/api/build", {})
        try:
            self.assertTrue(wait_until(
                lambda: self.api("GET", "/api/build?offset=0")[2]["status"]
                == "running", timeout=5.0))
            status, _, body = self.api("DELETE", "/api/build")
            self.assertEqual((status, body["stopped"]), (200, True))
            self.assertTrue(wait_until(
                lambda: self.api("GET", "/api/build?offset=0")[2]["done"],
                timeout=15.0))
            _, _, view = self.api("GET", "/api/build?offset=0")
            self.assertEqual(view["status"], "failed")
            self.assertIn("stopped", view["note"])
            self.assertTrue(self.docker.stops, "no pkill exec reached the daemon")
        finally:
            self.docker.start_delay = 0.0
            self.docker.exit_code = 0

    def test_build_missing_container_is_503(self):
        self.docker.fail_create = True
        status, _, body = self.api("POST", "/api/build", {})
        self.assertEqual(status, 503)
        self.assertIn("ktplace-dev", body["error"])

    def test_build_status_shape(self):
        status, _, view = self.api("GET", "/api/build?offset=0")
        self.assertEqual(status, 200)
        for key in ("status", "exitCode", "elapsed", "note", "cmd",
                    "offset", "text", "done"):
            self.assertIn(key, view)


if __name__ == "__main__":
    unittest.main(verbosity=2)
