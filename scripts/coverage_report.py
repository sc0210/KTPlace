#!/usr/bin/env python3
"""Line-coverage report for KTPlace, built on gcov alone.

Why gcov and not lcov
---------------------
gcov ships with gcc. A coverage gate that needs a package installed first is a
gate that silently does not run on the machine that needed it, and "no report"
reads the same as "full coverage". So this script drives gcov itself, reads its
JSON output, and prints the table.

What is counted
---------------
Executable lines in the engine's own translation units, grouped by file and by
component directory. Two things are excluded on purpose:

  * third_party / vendored headers (CImg.h), which are not our code and would
    swamp the denominator;
  * system headers, which gcov also reports for a few translation units.

The "lines" column is gcov's own notion of an executable line, so it is not the
same as a naive count of non-blank source lines. A function that is defined but
never called is not missing coverage as far as gcov is concerned; its body lines
are reported as uncovered, which is the useful signal.

Usage
-----
    coverage_report.py --clean --build --min 60
    coverage_report.py --build --min 60 --component legalizer
"""

from __future__ import annotations

import argparse
import glob
import gzip
import json
import os
import re
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src"
COV_BUILD = REPO / "build-cov"

# Vendored code is not ours and would dominate the denominator.
EXCLUDED = ("CImg.h",)

# Files in these directories are third-party or generated.
EXCLUDED_DIR_PARTS = ("third_party", "build", "build-cov", "test", "tests")


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=REPO, text=True, capture_output=True, **kw)


def sh(cmd: list[str]) -> None:
    """Run a command, aborting with its output if it fails."""
    proc = run(cmd)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit(f"command failed: {' '.join(cmd)}")


def clean() -> None:
    """Remove the instrumented build and any stale counters.

    A stale .gcda is worse than none: gcov merges it into the new run, so a
    function deleted since the last build keeps reporting as covered and the
    number drifts upward every run.
    """
    shutil.rmtree(COV_BUILD, ignore_errors=True)
    for stale in REPO.glob("build/**/*.gcda"):
        stale.unlink(missing_ok=True)
    # The per-binary gcov payloads are written into the repository root by gcov
    # and are not cleaned up by the build, so they pile up as untracked files.
    for stale in REPO.glob("*.gcov.json.gz"):
        stale.unlink(missing_ok=True)


def build() -> None:
    # COVERAGE=1 switches the build system to the instrumented object tree, and
    # test-build links the test binaries without running them: the driver runs
    # them below, so that the .gcda counters are all produced by one process per
    # binary, in a known order.
    sh(["make", "-C", str(SRC), "-f", "Master.make", "-j",
        str(os.cpu_count() or 4), "COVERAGE=1", "test-build"])


def run_tests() -> list[str]:
    """Run every test binary, returning their names.

    Each binary is run even if an earlier one fails, because a partial run still
    produces a coverage number and the point of a local run is to see where the
    gaps are, not to gate.
    """
    # A --coverage link drops a .gcno next to each binary, so globbing "test_*"
    # also matches those. Only real executables are test programs.
    bins = sorted(p for p in glob.glob(str(COV_BUILD / "bin" / "test_*"))
                  if ".gcno" not in p and os.access(p, os.X_OK))
    if not bins:
        raise SystemExit("no test binaries built; run with --build")

    # Rasterising is -O0 with a counter on every basic block, and both picture
    # outputs are large by default: the final still is 6144x6144, and the
    # animation deflates a frame per placement change. Between them they were the
    # entire cost of this step, and neither buys any coverage -- test_viz already
    # drives the PNG writer, the GIF writer and the raster path at sizes chosen
    # for a test.
    #
    # Measured on this suite: 1524s at the defaults, and the same coverage with
    # the picture sizes cut. The animation stays ON -- turning it off is faster
    # still, but it drops the GIF writer out of the report and the gate is set
    # from the total, so the saving is not worth the regression. What is cut is
    # how much is drawn: 40 frames at 768x768 rather than 1200 at twice that.
    #
    # setdefault, not assignment: a developer chasing something can still ask for
    # the full-size picture.
    #
    # KTPLACE_FINAL_ZOOM=1 is 768x768 rather than 0 or off, so the final-image
    # code still runs and stays covered -- off would have saved the same time and
    # quietly dropped it from the report.
    #
    # Drawing is off for the suite runs, and the one path that needs it is replayed
    # afterwards on a single case.
    #
    # It is not the frame count, the frame size or the frame cadence that costs.
    # In test_flow, turning the animation on goes from 0.6s to 190s, and 40 frames,
    # 8 frames, 1 frame, 768px and 77px all land within a second of each other --
    # because a per-conjugate-gradient-iterate frame is recorded as "mandatory",
    # and a mandatory frame is exempt from both the stage budget and the thinning
    # stride. So the budget does not bound it: the last iterate of every solve is
    # always recorded whatever KTPLACE_SIMPL_CG_EVERY says, and that is enough
    # frames, each one an SVG written plus a frame rasterised, to cost 190s.
    #
    # So the coverage run takes the animation off -- which drops the solver's
    # per-iterate recording out of the report -- and then puts back exactly the one
    # case that draws on purpose. .gcda counters merge across runs, so the replayed
    # case lands in the same report.
    env = dict(os.environ)
    env.setdefault("KTPLACE_ANIM", "0")
    env.setdefault("KTPLACE_ANIM_MAX_FRAMES", "40")
    env.setdefault("KTPLACE_ANIM_ZOOM", "1")
    env.setdefault("KTPLACE_FINAL_ZOOM", "1")

    for b in bins:
        proc = subprocess.run([b, "--log_level=message"], cwd=REPO,
                              text=True, capture_output=True, env=env)
        name = Path(b).name
        if proc.returncode != 0:
            print(f"  {name}: FAILED", file=sys.stderr)
        else:
            print(f"  {name}: ok", file=sys.stderr)

    # The case that renders on purpose, with the animation on, once.
    animated = REPO / "build-cov" / "bin" / "test_flow"
    if os.access(animated, os.X_OK):
        anim_env = dict(env)
        anim_env["KTPLACE_ANIM"] = "1"
        subprocess.run([str(animated), "--run_test=flow_writes_visualization_output_when_asked",
                        "--log_level=message"], cwd=REPO, text=True, capture_output=True,
                       env=anim_env)
    return [Path(b).name for b in bins]


def gcov_json() -> list[dict]:
    """Ask gcov for machine-readable coverage of every instrumented object.

    gcov's -o names the directory holding the .gcno/.gcda pair, and the
    positional argument names the object *inside* that directory. The engine
    objects are spread over build-cov/obj/<component>/, so they have to be
    visited one directory at a time; passing a whole-directory -o with a
    path-qualified object name makes gcov treat the path as a source file and
    report "No executable lines".

    gcov writes its .gcov.json.gz into the current directory, so each call runs
    in the same scratch directory and the results are collected from there.
    """
    gcda = sorted(glob.glob(str(COV_BUILD / "**" / "*.gcda"), recursive=True))
    if not gcda:
        raise SystemExit("no .gcda files: the tests did not run under the coverage build")

    scratch = COV_BUILD / "gcov"
    shutil.rmtree(scratch, ignore_errors=True)
    scratch.mkdir(parents=True)

    failures: list[str] = []
    for path in gcda:
        obj_dir = str(Path(path).parent)
        obj_name = Path(path).name[: -len(".gcda")]
        proc = subprocess.run(
            ["gcov", "--json-format", "-o", obj_dir, obj_name],
            cwd=scratch, text=True, capture_output=True)
        if proc.returncode != 0:
            # One bad object should not throw away every other component's
            # numbers; the gap is reported in the table anyway.
            failures.append(f"{obj_name}: {proc.stderr.strip()}")

    if failures:
        print("gcov could not read some objects:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)

    out: list[dict] = []
    for path in glob.glob(str(scratch / "*.gcov.json.gz")):
        with gzip.open(path, "rt") as fh:
            doc = json.load(fh)
        out.extend(doc.get("files", []))
    if not out:
        raise SystemExit("gcov produced no coverage data")
    return out


def should_report(name: str) -> bool:
    if any(part in EXCLUDED_DIR_PARTS for part in Path(name).parts):
        return False
    return not any(name.endswith(e) for e in EXCLUDED)


def component_of(name: str) -> str:
    """The component directory a source file belongs to, or 'other'."""
    parts = Path(name).parts
    if "src" in parts:
        parts = parts[parts.index("src") + 1:]
    return parts[0] if len(parts) > 1 else "src"


class Totals:
    __slots__ = ("hit", "found")

    def __init__(self) -> None:
        self.hit = 0
        self.found = 0

    def add(self, hit: int, found: int) -> None:
        self.hit += hit
        self.found += found

    @property
    def pct(self) -> float:
        return 100.0 * self.hit / self.found if self.found else 0.0


def collect() -> tuple[dict[str, Totals], dict[str, Totals], Totals]:
    per_file: dict[str, Totals] = defaultdict(Totals)
    per_component: dict[str, Totals] = defaultdict(Totals)
    total = Totals()

    for entry in gcov_json():
        name = entry.get("file", "")
        if not should_report(name):
            continue
        # gcov also reports the standard headers a translation unit included
        # (e.g. /usr/include/c++/13/tuple). Those are not ours, and they are
        # the bulk of the line count, so they are dropped by path, not by name.
        try:
            rel = str(Path(name).resolve().relative_to(REPO))
        except ValueError:
            continue
        t = Totals()
        for line in entry.get("lines", []):
            # count is 0 for a non-executable line, which must not be counted at
            # all: including it would deflate the percentage for no reason.
            if line.get("count", 0) > 0:
                t.add(1, 1)
            else:
                t.add(0, 1)
        if t.found == 0:
            continue
        # add(), not assignment: several files share a component, and assigning
        # would leave only the last one counted.
        per_file[rel].add(t.hit, t.found)
        per_component[component_of(rel)].add(t.hit, t.found)
        total.add(t.hit, t.found)
    return dict(per_file), dict(per_component), total


def bar(pct: float, width: int = 24) -> str:
    filled = int(round(pct / 100.0 * width))
    return "#" * filled + "." * (width - filled)


def report(per_file: dict[str, Totals], per_component: dict[str, Totals],
           total: Totals) -> None:
    print()
    print("Line coverage by component")
    print("-" * 66)
    print(f"{'component':<26}{'lines':>9}{'covered':>9}{'pct':>7}  ")
    for comp in sorted(per_component):
        t = per_component[comp]
        print(f"{comp:<26}{t.found:>9}{t.hit:>9}{t.pct:>6.1f}%  {bar(t.pct)}")
    print("-" * 66)
    print(f"{'TOTAL':<26}{total.found:>9}{total.hit:>9}{total.pct:>6.1f}%  {bar(total.pct)}")

    print()
    print("Line coverage by file (worst first)")
    print("-" * 78)
    print(f"{'file':<52}{'lines':>7}{'pct':>7}  ")
    for path in sorted(per_file, key=lambda p: (per_file[p].pct, p)):
        t = per_file[path]
        print(f"{path:<52}{t.found:>7}{t.pct:>6.1f}%")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--clean", action="store_true", help="remove the instrumented build first")
    ap.add_argument("--build", action="store_true", help="build the instrumented binaries")
    ap.add_argument("--no-test", action="store_true", help="skip running the tests")
    ap.add_argument("--min", type=float, default=0.0,
                    help="fail if total line coverage is below this percentage")
    ap.add_argument("--component", help="only report this component")
    args = ap.parse_args()

    if args.clean:
        clean()
    if args.build or args.clean:
        build()
    if not args.no_test:
        run_tests()

    per_file, per_component, total = collect()
    if args.component:
        keep = {k: v for k, v in per_file.items() if component_of(k) == args.component}
        if not keep:
            print(f"no coverage data for component {args.component!r}", file=sys.stderr)
            return 2
        total = Totals()
        for t in keep.values():
            total.add(t.hit, t.found)
        per_file = keep
        per_component = {args.component: total}

    report(per_file, per_component, total)

    if total.pct + 1e-9 < args.min:
        print(f"\nFAIL: total line coverage {total.pct:.1f}% is below the {args.min:.1f}% gate",
              file=sys.stderr)
        return 1
    if args.min > 0:
        print(f"\ncoverage gate satisfied: {total.pct:.1f}% >= {args.min:.1f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
