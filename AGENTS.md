# AGENTS.md

KTPlace is a VLSI placement engine: C++23 (`g++`), a custom recursive Make build, Boost.Test
unit tests, and a stdlib-only Python web console. Read `README.md` for the full picture; this
file only records what is easy to get wrong.

## Platform: build in Docker, not on this host

The build targets **Linux** (`g++` 13+, oneTBB, Boost.Iostreams, libfmt, `libboost-test-dev`,
zlib). Native `make` on macOS/Windows is not supported; this checkout's `g++` is Apple clang and
there is no `build/`. Use the Docker dev container, which pins the same toolchain as CI:

```sh
scripts/devenv.sh up        # start dev + web containers, wait for SSH, build the binary
scripts/devenv.sh build     # recompile (extra args go to make)
scripts/devenv.sh ssh 'make -j"$(nproc)" && make -j"$(nproc)" test'   # one command over SSH
# without a bash shell:  docker compose up -d --build
```

`ssh -p 2222 dev@127.0.0.1` (or `docker exec -it -u dev ktplace-dev bash -l`) enters the
container, where the repo is at `/workspace` and `ktplace` is on `PATH`. The container's `build/`
and `build-cov/` are Docker volumes, so Linux artifacts never mix with the host's.

## Commands

```sh
make -j"$(nproc)"        # build build/bin/ktplace
make -j"$(nproc)" test   # build + run every Boost.Test suite (alias: make check)
make format              # clang-format -i on tracked src (skips vendored CImg.h)
make format-check        # dry-run; non-zero if anything is unformatted
python3 scripts/coverage_report.py --clean --build --min 80   # instrumented build + tests + gcov report
make compile-commands    # regenerate compile_commands.json for clangd (gitignored)
make hooks               # point core.hooksPath at .githooks (pre-commit)
```

`source ktplace.sh` (generated at the repo root by a successful build; templates in `scripts/`)
puts `ktplace` on `PATH`. Run a design with:

```sh
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1
```

### Run a single test

Test binaries land in `build/bin/test_*` (`test_datamodel`, `test_adaptor`, `test_flow`,
`test_util`, `test_constraint`, `test_option`, `test_viz`, `test_detail`, `test_ntuplace1`).
They are Boost.Test binaries, so take the usual flags:

```sh
build/bin/test_flow --run_test=flow_writes_visualization_output_when_asked --log_level=message
```

Each suite writes its own synthetic design into a scratch dir, so no benchmark is needed.
Tests of paths that end the process via `ktlog::fatal` use `fork(2)`; Boost.Test 1.83 has no
death-test macros.

## Architecture

- `src/Master.make` is the source of truth for the build. `SUBDIRS = datamodel adaptor
  constraint placer legalizer detailPlacer visualization util`; each component has its own
  `Master.make`. Add a component here, not just a directory.
- The README's `Layout` block is **stale** (it omits `constraint`, `legalizer`, `detailPlacer`
  and the `placer/ntuplace1` sub-placer). Trust `src/Master.make`.
- Entry point `src/kt_place.cc` -> `src/kt_flowMgr.{h,cc}` (load -> place -> write); CLI in
  `src/kt_option.{h,cc}`.
- Algorithms: `src/placer/simpl/` (default, the SimPL paper) and `src/placer/ntuplace1/`
  (`-a ntuplace1`). Legalization is `src/legalizer/` (Abacus, plus `MultiRowLegalizer` for cells
  taller than a row); detailed placement is `src/detailPlacer/` (FastDP).
- `INCLUDES := -I.` runs from `src/`, so **every quoted include is relative to `src/`**, e.g.
  `#include "datamodel/kt_dm.h"`. There is no `CMakeLists.txt`.
- Web console is `webui/server.py` (Python stdlib only); it runs the binary the dev container
  built and serves the run's own `plots/index.html`.

## Conventions and gotchas

- **stdout is never written to.** All output goes through `src/util/kt_log.h` to
  `ktplace.log` + stderr; `ktlog.trace` goes to a separate `ktplace_trace.log`. New code must not
  print to stdout. `ktReportTable` renders aligned tables as a single log record.
- **Behavior is tuned by `KTPLACE_*` environment variables, not flags.** Visualization frames
  dominate runtime on large designs; turn them off when only the placement matters:
  `KTPLACE_ANIM=0 KTPLACE_SIMPL_CG_EVERY=0 KTPLACE_SIMPL_TRACE_EVERY=0`.
- **clang-format version matters** — a different major lays files out differently. CI/Docker use
  18; this host has 20. Run `make format` in the container. `src/visualization/CImg.h` is vendored
  and exempt from formatting and from the coverage denominator.
- **Pre-commit** (after `make hooks`) checks staged formatting and runs the full unit suite.
  Bypass with `KTPLACE_SKIP_HOOKS=1`; skip just the tests with `KTPLACE_HOOK_TESTS=0`. CI also
  gates total line coverage (`COVERAGE_MIN`, default 80) and runs two smoke placements
  (`adaptec1`, and `ibm01` for the multi-row path).
- **Benchmarks are not committed** except vendored `benchmark/ISPD_2005/adaptec1` and
  `benchmark/ICCAD04/ibm01` (both held deliberately for offline CI). Fetch others with
  `python3 benchmark/fetch_benchmarks.py`; do not commit benchmark data or `output/`.
  Generated/gitignored: `build/`, `build-cov/`, `output/`, `compile_commands.json`,
  `ktplace.sh`, `ktplace.csh`, `*.log`.
- **SimPL is the reference algorithm** (`src/placer/simpl/`). Where the implementation departs
  from the paper, the departure is explained in a comment at the decision site with the paper
  text quoted — preserve those comments and follow the same style for new departures. The
  README's "Reported results" section tracks a known legalization defect.
- Commits follow conventional commits with component scopes (`fix(simpl): ...`,
  `perf(viz): ...`, `test(option): ...`).

## Docs

- `README.md` — usage, options, output layout, algorithm status.
- `docs/simpl.md` — SimPL and how this implementation differs.
- `docs/docker.md` — starting the Docker environment and the console API.
- `benchmark/README.md` — suites, sources, and how the fetch script normalizes them.
