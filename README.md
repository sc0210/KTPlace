# KTPlace
An open-source know thyself placement engine.

## Build

Requirements: g++ 13+ (C++23), oneTBB, Boost.Iostreams, zlib, libfmt (`libfmt-dev`).

```sh
make            # builds build/bin/ktplace
make rebuild    # clean + rebuild
```

### Any OS: the Docker environment

The build targets Linux. On macOS or Windows -- or on a Linux whose packages
differ from CI's -- work inside the Docker environment instead: Ubuntu 24.04
with exactly the packages CI installs (g++ 13, oneTBB, Boost, fmt,
clang-format 18), so a build, a test run or a placement behaves the same on
every machine. All it needs is Docker (Docker Desktop on macOS and Windows).

> **Step-by-step guide:** [docs/docker.md](docs/docker.md) -- starting the
> environment, running commands in it (shell, one-off, the web console's
> Terminal panel, the API), settings and troubleshooting.

**One shot: the whole environment.** `scripts/devenv.sh up` starts both
containers -- the development container below and the web console further down
-- waits for SSH, and compiles `build/bin/ktplace` inside. That single binary
is what the shell and the browser both run, so there is nothing else to set up:

```sh
scripts/devenv.sh up
#   shell   scripts/devenv.sh ssh        (ssh -p 2222 dev@127.0.0.1)
#   web     http://127.0.0.1:8080
#   engine  build/bin/ktplace
```

`scripts/devenv.sh build` recompiles later (extra arguments go to `make`), and
`up --no-build` starts everything without compiling. Without a bash shell --
PowerShell included -- the same two containers come up with compose, and the
binary is then one `make` inside the container away:

```sh
docker compose up -d --build        # any OS, PowerShell included
```

**Development container.** A long-lived container with the repository mounted
live at `/workspace`: edit on the host with your usual editor, and build, test
and run in Linux over SSH.

```sh
ssh -p 2222 dev@127.0.0.1           # log in as `dev`, in /workspace
make -j"$(nproc)" && make test      # (inside) build and test
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1
```

`docker compose stop` stops both containers, `docker compose down` removes
them.

- **SSH** listens on `127.0.0.1:2222` only (not the network) and accepts keys
  only. The key installed is `~/.ssh/id_ed25519.pub`; set `KTPLACE_SSH_PUBKEY` to
  use another, and `KTPLACE_SSH_PORT` to change the port. Create a key with
  `ssh-keygen -t ed25519` if you have none. Windows 10 and later ship the `ssh`
  client.
- **Editors and scripts**: with an `~/.ssh/config` entry (below) `ssh ktplace-dev`,
  `scp`, `rsync` and VS Code's *Remote-SSH* all work against it, and
  `ssh ktplace-dev 'make test'` runs one command in the repository with
  `ktplace` on `PATH`.

  ```
  Host ktplace-dev
      HostName 127.0.0.1
      Port 2222
      User dev
      HostKeyAlias ktplace-dev
      IdentityFile ~/.ssh/id_ed25519
  ```
- **Without SSH**: `docker exec -it -u dev ktplace-dev bash -l`.
- **Builds do not collide.** The container's `build/` and `build-cov/` are Docker
  volumes laid over the repository's, so a Linux build never mixes with anything
  built on the host. Files under `output/` are shared with the host, and
  `build/` is mounted into the web console too -- one compile, used by both.
- **State that survives**: the build volumes and the SSH host key outlive
  `stop`, `down` and image rebuilds, so a new container needs no full recompile
  and ssh does not warn about a changed host key. The SSH key is copied in when
  the container starts; after changing it, run `down` and then `up`.
- **Windows**: `.gitattributes` keeps every file's line endings LF on checkout,
  which the Linux side needs. For speed, keep the clone inside WSL 2 rather than
  on `C:`; bind mounts from the Windows filesystem are slow.

`scripts/devenv.sh` wraps all of this for a bash shell (macOS, Linux, WSL, Git
Bash): `up` picks whichever key you have, waits for SSH, starts the web console
beside it and builds the binaries, and `build`, `ssh`, `exec`, `status`,
`ssh-config`, `stop` and `down` do what they say.

**One-shot CI run.** Building the default image target compiles a copy of the
tree and runs the unit tests, exactly as CI does, without starting anything:

```sh
docker build -t ktplace .
docker run -v "$PWD/output:/ktplace/output" ktplace \
    ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1
```

**Lightweight run image.** The `ci` image above carries the toolchain, sources
and objects -- 1.2 GB -- because it must be able to build and test. To only
*run* placements, the `run` target packages the same binary with the four
libraries it links and nothing else (~112 MB):

```sh
docker build --target run -t ktplace-run .
docker run --rm -v "$PWD/output:/ktplace/output" ktplace-run \
    benchmark/ISPD_2005/adaptec1 -w output/adaptec1
```

`ktplace` is the image's entrypoint, so everything after the image name goes
to it: plots go to `<work-dir>/plots` unless you pass `--no-plots`, and
`-v .../benchmark:/ktplace/benchmark:ro` mounts other suites. The vendored adaptec1 is already inside, so this runs
a full placement with no mounts at all. Like the `ci` build it runs on the
host's own architecture -- arm64 on Apple Silicon, x86-64 otherwise.

**Web console.** A browser front end: pick a design and an algorithm, start the
placement, watch the transcript stream live, then open the gallery, final image
and log the run produced. Everything is served from the container -- the console
is a Python stdlib HTTP server on a small runtime image, and the results are the
engine's own output (`plots/index.html`, `final.png`, `placed.pl`, `ktplace.log`,
`ktplace_trace.log`), not a re-implementation of it.

While a run is going the page carries a phase bar (load, placement,
legalization, detailed placement, output) and, under it, a live heartbeat -- the
SVG frames the engine streams, the child's CPU and resident memory, and how long
the output has been quiet -- so a long but healthy solve reads as working rather
than hung. A machine panel shows the container's CPU load and memory, which is
what the placement actually runs on. Ticking *verbose* streams the engine's
per-iteration `ktplace_trace.log` below the transcript.

Around that:

- **Every design, not just one.** The console scans `/ktplace/benchmark`, and
  compose mounts the repository's whole suite there read-only, so all of
  ICCAD04, ISPD02, ISPD_2005 and ISPD_2015 are selectable behind the filter box.
  A standalone `docker run` keeps the vendored `adaptec1`.
- **Environment overrides.** The engine tunes from `KTPLACE_*` variables rather
  than flags, so the *Environment overrides* box takes `KEY=VALUE` lines
  (`KTPLACE_SIMPL_ITERS=25`, `KTPLACE_SIMPL_SEED=1`, ...). The server keeps only
  `KTPLACE_`-namespaced pairs whose value is shell- and environment-safe; the
  rest are dropped, and what survives is recorded in the run's `request.json`.
- **Export.** Each run has one-click downloads for the transcript, trace,
  `placed.pl` and `request.json`, plus an *everything (.zip)* that bundles those
  with `final.png` and a `summary.json` -- deliberately without the frame
  gallery, which can run to hundreds of megabytes.
- **History and comparison.** Every run is listed with its headline numbers
  (status, elapsed, HPWL, verdict) side by side; runs can be labeled and deleted
  from the list or the table.
- **A command runner.** The *Terminal* panel runs commands in the console
  container and streams their output -- handy for `ktplace <design> -a simpl`
  with custom flags, or poking at `runs/<id>/`. It is off by default, and
  compose turns it on with `KTPLACE_WEB_ALLOW_EXEC=1` because the published port
  is loopback-only and the server refuses requests from other sites or host
  names (see [docs/docker.md](docs/docker.md#calling-the-consoles-api)). It runs as the server's user in the runtime image (python3
  and the engine, no compiler), so builds still belong in the dev container.

The console carries no engine of its own: it runs the binary in the build
volume the dev container compiles, so shell and browser see one ktplace rather
than two that can drift apart. It starts with everything else:

```sh
scripts/devenv.sh up                # both containers and the binary
# open http://127.0.0.1:8080
```

Compose starts it too, with no profile to remember:

```sh
docker compose up -d --build        # dev container + console
docker compose up -d --build web    # console alone, against a built volume
```

and the image can be run by hand against that same volume:

```sh
docker build --target web -t ktplace-web .
docker run --rm -p 127.0.0.1:8080:8080 \
    -v ktplace_build:/ktplace/build -v ktplace_web-runs:/ktplace/runs ktplace-web
```

With no binary anywhere the page still loads and reports the engine as not
built yet; `scripts/devenv.sh build` is what produces it.

Runs land in `/ktplace/runs/<id>/`; mount a directory there (as above) to
keep them across container restarts. The host-side mapping in the examples is
loopback-only, so the console is visible only from this machine; inside the
container the server listens on `0.0.0.0` with the port from
`KTPLACE_WEB_PORT` (8080), and `KTPLACE_WEB_HOST` overrides the bind address.
Mount other benchmark suites at `/ktplace/benchmark` to make them selectable,
exactly as with the `run` image. `KTPLACE_WEB_ALLOW_EXEC=1` enables the command
runner when the console is run by hand.

A successful build also writes two environment helpers into the repository
root. Source the one for your shell and the engine is callable by name:

```sh
source ktplace.sh     # bash / sh
source ktplace.csh    # csh / tcsh
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1
```

They only appear when the link succeeds, are safe to source repeatedly, and
export `KTPLACE_HOME` pointing at the repository. The maintained copies live in
`scripts/`.

The top-level Makefile drives a hierarchy under `src/`, where `src/Master.make`
coordinates the components -- `datamodel`, `adaptor`, `placer`, `visualization`
and `util` -- each of which builds through its own `Master.make`.

Layout:

```
src/
  Master.make          # coordinates the subdirectories
  kt_place.cc          # entry point
  kt_flowMgr.{h,cc}    # load -> place -> write flow
  kt_option.{h,cc}     # command line
  datamodel/           # PlacementDB, Graph, Vertex, Edge
  adaptor/             # Bookshelf and LEF/DEF readers
  placer/              # quadratic placer (clique/star, CG, density)
  visualization/       # SVG frames, bounds curve, HTML gallery, GIF
  util/                # kt_log, kt_reportTable, kt_scopedTimer
```

## Tests

Unit tests use Boost.Test and live next to the code they cover:

```
src/datamodel/test/test_datamodel.cc   PlacementDB and the placement graph
src/adaptor/test/test_adaptor.cc      Bookshelf and LEF/DEF readers
src/test/test_flow.cc                 end-to-end load -> place -> write
```

```sh
make test        # build and run every suite (alias: make check)
```

Each test writes its own tiny synthetic design into a scratch directory, so the
suites need no benchmark data. Error paths that end the process through
`ktlog::fatal` are checked with `fork(2)`, since Boost.Test 1.83 has no
death-test macros.

CI (`.github/workflows/ci.yml`) runs on every push and pull request: it
installs the dependencies, rejects any source that is not `clang-format` clean,
builds, runs the unit tests, then fetches one small benchmark (ICCAD04 `dma`,
~8 MiB) and asserts the placement completes and writes one record per cell.

## Code style

All C++ sources are formatted with `clang-format` (configuration in
`.clang-format`, tuned to the project's 4-space / 100-column style):

```sh
clang-format -i src/**/*.cc src/**/*.h
```

## Logging

All output goes through a single logger (`src/util/kt_log.h`) to a transcript
file plus stderr; **stdout is never written to**, so redirecting it stays clean.

Both log files are written into the work directory:

```sh
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1      # ktplace.log + ktplace_trace.log
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1 -v   # trace also on stderr
```

| call | `ktplace.log` | `ktplace_trace.log` | stderr |
|------|:--------------:|:-----------------:|:------:|
| `ktlog.echo(...)`  | yes | no | yes |
| `ktlog.trace(...)` | no  | yes | only with `-v` |
| `ktlog.fatal(...)` | yes | no | yes, then `exit(1)` |

Trace records go to a **separate** file so the main transcript stays readable;
`-v` additionally echoes them to the console. Messages are built with
`fmt::format` and checked at compile time.

`ktReportTable` (`src/util/kt_reportTable.h`) accumulates cells and renders an
aligned table — column widths measured from content, numeric cells
right-aligned — emitted as a single log record:

```cpp
ktReportTable table("Summary");
table.setHeaders({"phase", "wall", "cpu"});
table.addRow({"load", "4.75s", "7.09s"});
table.emit();
```

## The algorithm

SimPL is implemented from the paper, which is the reference for every design
decision in `src/placer/simpl/` -- the pseudonet weight law, the alpha schedule,
and the convergence rule:

> M.-C. Kim, D.-J. Lee, I. L. Markov. *SimPL: An Algorithm for Placing VLSI
> Circuits.* Communications of the ACM 56(6), June 2013. DOI
> 10.1145/2461256.2461279.

The paper is paywalled and is not redistributed here. Where this implementation
departs from it, the departure is stated in a comment at the site of the
decision, with the paper text quoted, so the two can be compared rather than
taken on trust. Two known departures: the returned placement is the best upper
bound rather than the last (our upper bound rises where the paper's oscillates
and then improves, so "last" would be worse), and the pseudonet weight is the
quadratic surrogate `w = alpha` for the paper's Manhattan-distance penalty.

### Reported results, and where we stand

The paper's Table 1, ISPD 2005, HPWL in units of 10^6. The paper's figures are
after FastPlace-DP, so they are comparable with a full run of this tool.

| design | cells | SimPL (paper) | this implementation | ratio |
| --- | --- | --- | --- | --- |
| adaptec1 | 211 K | 77.42 | 355.9 | 4.6x |
| adaptec2 | 255 K | 91.01 | see `output/_logs/suite.log` | |

The gap is in the look-ahead legalization, not in the solver, and the per-iteration
trace says so. On adaptec1 the *lower* bound -- the unconstrained quadratic
solution -- matches the paper closely: 6.8e+07 at iteration 11 against the
paper's 6.8e+07 at iteration 20. The *upper* bound, which is the lower bound put
through look-ahead legalization, is where it goes wrong: ours is 4.1e+08 where
the paper's is 9.2e+07. Legalization is costing a factor of about five, and
because the upper bound is what the run returns and what the next iteration's
anchors are built from, that cost is paid back on every subsequent iteration.

The signature of the fault is in how the lower bound evolves. The paper's stays
flat -- 4.5e+07 at initial placement, 6.8e+07 at iteration 20, a factor of 1.5.
Ours grows 8.07e+07 to 6.86e+08, a factor of 8.5, monotonically. A lower bound
that grows like that is being dragged outward by its anchors every iteration,
which is what happens when the legalization moves cells much further than the
paper's does and the pseudonets then pull the next solve out to meet them.

Three things have been found and fixed so far, and none of them is the whole of
it:

- **The die was defined twice and inconsistently.** The placer used the
  fixed-cell bounding box, the checker the same, and neither consulted the rows
  -- but the rows are the authoritative statement of where a cell may go, and for
  adaptec3 they reach past the fixed cells. The placer's density grid therefore
  saw 2.1e+07 units of placeable area where the design has 5.4e+07, thought the
  design 327% full, and spread against a region less than half the real size.
  `placementDieBox()` is now one definition, used by both.
- **The initial placement ran one round instead of five to seven.** The result is
  not thrown away -- it seeds the global loop, and the first anchors are its first
  legalization -- so the cap was removing the step the paper says "can determine
  the overall shape of the final placement solutions". One round gives 4.054e+08
  on adaptec1, seven give 3.559e+08.
- **Utilisation was reported against the wrong denominator**, charging the fixed
  cells' area to the row area. adaptec1 read 89% when its movable demand is 58%
  of the rows, which is the number to check when a placement comes out illegal.

What is left is the over-spreading itself, and it is not a small change. The
paper's legalization preserves the placement's shape: it sorts cells by distance
from a cutline and packs them into stripes, which spreads without reordering.
Ours follows that structure, but two of its choices push further than the
paper's. The region a cluster is legalized into is the whole die once the cluster
holds half the movable area (`globalClusterFrac`), which spreads a collapsed
placement uniformly over the chip -- and the I/O pads ring the die, so that
sends every cell to the wrong end of it. Using the paper's minimal region
instead is not the fix either: measured on adaptec1 it is worse (7.69e+08),
because confining a collapsed placement to the 58% of the die its cells strictly
need leaves the rest of the chip empty. The paper gets both properties at once
and the mechanism for it has not been identified here.

## Placement images

Unless run with `--no-plots`, every run writes, under `<work-dir>/plots`, per-stage SVG frames (vector, so
they stay sharp at any zoom), a per-iteration HPWL curve, and — unless
`KTPLACE_ANIM=0` — an animated GIF assembled from those frames. On top of that, every run
writes one high-resolution still of the *finished* placement to
`plots/final/final.png`: 6144x6144 by default, which on an ISPD 2005 design is
about fourteen pixels across for a standard cell. The animation frames are
deliberately small, because a GIF has to be, and at that size a 200k-cell design
is a texture rather than a placement; the still is the one meant to be looked at.
It is a PNG because a PPM — the only format the raster path could always write —
is not a thing any viewer opens. The PNG encoder is in-process on top of zlib,
which the build already links; CImg's own PNG support needs libpng headers that
are not installed here.

Both are self-contained: no external tool, no image library beyond the vendored
CImg, and the result is reproducible from `ktplace` alone.

| variable | effect |
|----------|--------|
| `KTPLACE_ANIM` | `0` turns the animation off (on by default) |
| `KTPLACE_ANIM_MAX_FRAMES` | frame budget for the whole run (default 1200) |
| `KTPLACE_ANIM_ZOOM` | animation frame scale, vs 768x768 (default 3, so 2304x2304) |
| `KTPLACE_ANIM_BLEND` | in-between frames per placement (default 3) |
| `KTPLACE_ANIM_DELAY_CS` | GIF frame delay, in hundredths of a second |
| `KTPLACE_FINAL_ZOOM` | final still scale, vs 768x768 (default 8, so 6144x6144) |
| `KTPLACE_FINAL_PPM` | also write the lossless 113 MB PPM beside the PNG |

Rows are drawn behind the cells on any frame whose placement is genuinely on a
row grid, and are left out when it is not — see `rowBands()` in
`src/visualization/kt_plotter.cc` for how that is decided from the cells alone.

## Third-party code

`src/visualization/CImg.h` is the [CImg](https://cimg.eu) library, vendored as a
single header. It is dual-licensed by its author under CeCILL-C and CeCILL; see
the header for the full terms. It is used only to rasterise animation frames and
writes no files of its own — the GIF container, its LZW stream and the palette
quantiser are all in `src/visualization/kt_gif.cc`, and the PNG writer is in
`src/visualization/kt_plotter.cc` on top of zlib. Everything else in this
repository is original work under the MIT license in `LICENSE`.

## Benchmarks

Benchmarks are not vendored (they are several GB). Fetch them from their
original publishers:

```sh
python3 benchmark/fetch_benchmarks.py            # everything available
python3 benchmark/fetch_benchmarks.py --list     # show the sources
python3 benchmark/fetch_benchmarks.py --suite ICCAD04 --design ibm01
```

| Directory | Designs | Source |
| --- | --- | --- |
| `ISPD_2015/` | 16 `mgc_*` LEF/DEF designs | ispd.cc contest site |
| `ICCAD04/` | ibm01-18 (IBM-MSwPins), dma, dsp1/2, risc1/2 (Faraday) | vlsicad.eecs.umich.edu |
| `ISPD02/` | ibm01-18 (IBM-MS) | vlsicad.eecs.umich.edu |

See [benchmark/README.md](benchmark/README.md) for details.

## Usage

```sh
ktplace <input_dir> [options]
ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1
```

`<input_dir>` holds one design whose files are named after it -- for adaptec1,
`adaptec1.aux`, `adaptec1.nodes`, `adaptec1.nets` and so on, optionally
gzipped. The format is auto-detected: a directory containing a `.def`/`.def.gz`
goes through the LEF/DEF adapter, everything else is loaded as Bookshelf.

| option | meaning |
| --- | --- |
| `-a, --algorithm <name>` | `simpl` (default) or `ntuplace1` |
| `-w, --work-dir <dir>` | where to write everything; created if missing (default: current directory) |
| `--no-plots` | draw nothing; skip every frame and picture |
| `-v, --verbose` | also echo trace records to the console |
| `-h, --help` / `-V, --version` | help / version |

Everything a run writes goes under the work directory:

| path | contents |
| --- | --- |
| `placed.pl` | the placement, Bookshelf `.pl`, one line per cell |
| `ktplace.log` | the transcript (also on stderr) |
| `ktplace_trace.log` | per-iteration diagnostics |
| `plots/` | images of the run; see below |

stdout is never written to. Behaviour is tuned with `KTPLACE_*` environment
variables rather than flags; the ones that matter most are listed under
*Placement images*.

### Visualizing the solve

Open `<work-dir>/plots/index.html` in a browser: it is a gallery of the run.
Behind it:

| path | contents |
| --- | --- |
| `plots/simpl/` | global placement: a frame per CG iterate (`simpl_cg_*`), the lower and upper bound per iteration (`simpl_LSS_*`, `simpl_LAL_*`), and density maps |
| `plots/simpl_bounds.csv`, `.svg` | HPWL of the lower and upper bound per iteration, and their gap |
| `plots/legalize/`, `plots/detailplace/` | legalization and detailed placement frames |
| `plots/final/` | the finished placement, `final.png` (6144x6144) and `final.svg` |
| `plots/anim/placement.gif` | the whole run animated |

Every frame draws every cell; nothing is decimated. On a large design that makes
the frames a large share of the run -- adaptec1 writes about 1.2 GB of them --
so see *Placement images* for turning them off.

The run's report gives the **scaled overflow** of the lower bound and of the
final placement: the fraction of movable-cell area sitting in bins that
exceed a full 64x-bin capacity. 0.0 means the die is uniformly covered; 1.0
means everything is stacked in a single bin. `ktplace` spreads cells with a
SimPL-style projection: a gated equi-area fill drains over-packed bins into
empty die area, so the collapsed center seed never shows up as a pile of cells
in a corner.

## Features

- Bookshelf format input (`.nodes`, `.nets`, `.pl`, `.scl`, `.wts`), transparent `.gz` support via Boost.Iostreams
- LEF/DEF input (contest style: `floorplan.def` + `cells.lef`/`tech.lef`): macro sizes/pins, die area, rows, fixed macros, I/O pads, and the flat DEF netlist; DEF micron units respected (LEF sizes auto-scaled)
- Gzip + node/net parsing parallelized with oneTBB (`tbb::parallel_for`)
- Quadratic placement: clique/star-hybrid net model, CSR matrix, Jacobi-preconditioned CG, all parallelized with oneTBB
- Bookshelf `.pl` output writer
- Density-aware global placement: 64x64 occupancy grid over the die, SimPL-style projection spreading (gated equi-area drain) followed by optional wirelength refinement
- Iteration-by-iteration placement visualization on every run (SVG frames, lower/upper-bound HPWL curve, HTML gallery, animated GIF) — all generated in C++; see *Placement images*

## Timing

Elapsed time is measured with `src/util/kt_scopedTimer.h`: `ScopedTimer` is an RAII
stopwatch that records on scope exit, and `TimerRegistry` accumulates named
totals that are reported once per run. Every interval is recorded as both
wall-clock and processor time, so the table shows how much parallelism a phase
actually used:

```
Timings (wall 15.383s, cpu 53.081s, 3.45x parallelism)
+-------+---------+---------+-------+----------+
| phase | wall    | cpu     | calls | cpu/wall |
+-------+---------+---------+-------+----------+
| load  | 4.752s  | 7.086s  |     1 |    1.49x |
| place | 10.338s | 45.701s |     1 |    4.42x |
| write | 0.293s  | 0.293s  |     1 |    1.00x |
+-------+---------+---------+-------+----------+
```


```cpp
{
    ScopedTimer timer("load");
    runLoad();
}                        // recorded here
TimerRegistry::instance().report();
```

This is measurement only. Circuit delay (cell delay, net delay, slack) needs a
timing graph and cell libraries, so it belongs in a separate timing engine --
which can use `ScopedTimer` to report its own cost.

## Documentation

- [Benchmarks](benchmark/README.md) - suites, sources and layout
- [SimPL and this placer](docs/simpl.md) - what SimPL does, what we do differently, and a known defect in the phase schedule

## Notes

Pure quadratic placement minimizes *squared* wirelength; without a spreading
step every cell slides to a single point (the netlist's force-balance point),
so KTPlace couples the wirelength solve to a density-aware projection-spreading
pass, followed by a refinement phase that pulls the spread placement back toward
wirelength optimum. The consequence is visible in the reported `HPWL / seed`
ratio: spreading raises wirelength well above the seed, and there is no
legalization stage yet to bring it back down.

| design | cells | seed HPWL | after spreading | ratio |
| --- | ---: | ---: | ---: | ---: |
| `adaptec2` (ISPD 2005) | 255,023 | 7.27e7 | 1.32e9 | 18.2x |
| `adaptec5` (ISPD 2006) | 843,128 | 2.17e8 | 6.71e9 | 31.0x |
| `mgc_superblue16_a` (ISPD 2015) | 698,367 | 3.59e10 | 3.35e11 | 9.3x |
| `dma` (ICCAD 2004) | 11,734 | 0 | 6.6e3 | degenerate seed |

Two different things are being compared in that table, and it matters when
reading it:

- **A real seed.** ISPD 2005/2006 and ISPD 2002 ship a legal placement in
  `.pl`; the ratio is then a meaningful "how much did spreading cost" figure.
  Published placers are normally within 1.05-1.3x of such a seed because they
  finish with legalization and detail placement, so a 20-50x ratio means this
  is a post-spread, pre-legalization snapshot rather than a competitive result.
- **Our own seed.** ISPD 2015 LEF/DEF leaves standard cells `UNPLACED`, so the
  seed HPWL is measured from the die-center seed KTPlace invents, not from the
  input. Comparing against it says nothing about input quality.
- **A degenerate seed.** Some Bookshelf suites place every cell on the origin
  (ICCAD 2004 `dma` above), where any percentage is meaningless and is reported
  as `n/a`.
