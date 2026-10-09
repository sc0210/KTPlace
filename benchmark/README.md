# KTPlace benchmarks

The benchmarks are **not** stored in this repository, with two exceptions
(`adaptec1` and `ibm01`, below). `fetch_benchmarks.py` downloads them from their
original publishers and unpacks them into the layout the ktplace adapters expect.

```sh
python3 benchmark/fetch_benchmarks.py            # fetch everything available
python3 benchmark/fetch_benchmarks.py --list     # show the sources
python3 benchmark/fetch_benchmarks.py --suite ICCAD04 --design ibm01
```

The script only needs the Python standard library. It skips designs that are
already present, so it is safe to re-run.

## The first vendored design: `ISPD_2005/adaptec1`

`benchmark/ISPD_2005/adaptec1/` (5.4 MB, gzipped Bookshelf, dated March 2005) is
committed. It is the CI smoke design, and it is in the tree because the ISPD 2005
suite cannot be fetched: the
UMich mirror has no copy and the tarball DREAMPlace points at now returns 403,
while the suite itself is redistributed by dozens of placement papers without a
canonical home. Vendoring one 5.4 MB design is a fair price for a smoke test
that cannot fail on someone else's uptime. The other seven ISPD 2005 designs
(`adaptec2`–`adaptec4`, `bigblue1`–`bigblue4`) stay ignored, and
`output/run_ispd2005.sh` skips any design it cannot find, so a partial checkout
still runs the designs it has.

## The second vendored design: `ICCAD04/ibm01`

`benchmark/ICCAD04/ibm01/` (436 KB gzipped Bookshelf, dated June 2004) is also
committed. Unlike `adaptec1` its suite *is* fetchable, but it holds 23 designs,
and pulling the whole archive on every push to exercise one of them is a bad
trade for CI. It is here for two reasons:

* It is the only benchmark in the tree with **multi-row cells** — 120 of its
  12,380 movable cells are taller than a row — so it is the only design that
  reaches `MultiRowLegalizer` at all. Every other benchmark goes through Abacus.
* It is small and deterministic enough to assert legality on. Adaptec1's verdict
  moves with the machine, so CI deliberately does not gate on it; `ibm01`
  legalizes in well under a second and lands on zero overlapping pairs every
  run, so the multi-row path can be held to its actual contract.

## Sources

| suite | format | designs | source |
|-------|--------|---------|--------|
| `ISPD_2005` | Bookshelf | `adaptec1` | vendored here; see above |
| `ICCAD04` | Bookshelf | `ibm01` | vendored here; see above |
| `ISPD_2015` | LEF/DEF | `mgc_*` (16) | [ispd.cc contest site](https://www.ispd.cc/contests/15/web/benchmarks/ispd_2015_contest_benchmark.tgz) |
| `ICCAD04` | Bookshelf | `ibm01`–`ibm18` | [UMich ICCAD04bench](https://vlsicad.eecs.umich.edu/BK/ICCAD04bench/ibmMSWpinsICCAD04Bench_BOOKSHELF.tar.gz) (IBM-MSwPins) |
| `ICCAD04` | Bookshelf | `dma`, `dsp1`, `dsp2`, `risc1`, `risc2` | [UMich ICCAD04bench](https://vlsicad.eecs.umich.edu/BK/ICCAD04bench/FARADAY_ICCAD04Bench.tar.gz) (Faraday) |
| `ISPD02` | Bookshelf | `ibm01`–`ibm18` | [UMich ISPD02bench](https://vlsicad.eecs.umich.edu/BK/ISPD02bench/ibmISPD02Bench_Bookshelf.tar.gz) (IBM-MS) |

## Resulting layout

One directory per design, holding plain text files named after the design:

```
benchmark/ISPD_2015/mgc_des_perf_a/{floorplan.def, cells.lef, tech.lef, design.v, ...}
benchmark/ICCAD04/ibm01/{ibm01.nodes.gz, ibm01.nets.gz, ibm01.pl.gz, ...}
benchmark/ICCAD04/dma/{dma.nodes, dma.nets, dma.pl, dma.scl, dma.wts, dma.aux}
benchmark/ISPD02/ibm01/{ibm01.nodes, ibm01.nets, ibm01.pl, ibm01.scl, ibm01.wts, ibm01.aux}
benchmark/ISPD_2005/adaptec1/{adaptec1.nodes.gz, adaptec1.nets.gz, ...}
```

The upstream archives are inconsistent — the Faraday archive nests its files in
`DMA/BOOKSHELF/dma_BS.*` with a `_BS` suffix, and some members are gzipped — so
the script normalises all of that away. You do not need to do it by hand.
`adaptec1` and `ibm01` are the exceptions: they are committed in gzipped form,
which the Bookshelf reader accepts transparently.

## Running

```sh
./build/bin/ktplace ./benchmark/ISPD_2015/mgc_des_perf_a -w ./output/mgc_des_perf_a
./build/bin/ktplace ./benchmark/ICCAD04/ibm01 -w ./output/ibm01
```

The input format is auto-detected: a directory containing a `.def`/`.def.gz`
is read as LEF/DEF, anything else as Bookshelf. The placement is written to
`<work-dir>/placed.pl`, and SVG frames of the solve plus an HTML gallery to
`<work-dir>/plots/` (open `plots/index.html`).

## File formats

Bookshelf circuits ship as `.nodes` (cells), `.nets` (connectivity), `.pl`
(initial placement), `.scl` (row structure), `.wts` (net weights) and `.aux`
(manifest). Terminals in `.nodes` are the die I/O pads; they are fixed and are
marked `/FIXED` in `.pl`. Gzipped (`.gz`) inputs are read transparently.
