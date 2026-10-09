// @file kt_flowMgr.cc
// Implementation of FlowMgr


#include "kt_flowMgr.h"

#include "kt_option.h"

#include "adaptor/bookshelfToKTAdaptor.h"
#include "adaptor/lefdefToKTAdaptor.h"
#include "datamodel/kt_dm.h"
#include "detailPlacer/kt_fastdp.h"
#include "legalizer/kt_abacus.h"
#include "placer/ntuplace1/kt_ntuplace1.h"
#include "placer/simpl/kt_simpl.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_animator.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace ktplace {

namespace {
// One-line report of a single timer, issued when the phase it names
// completes, so a long run tells its cost as it goes rather than only in the
// summary table at the end (TimerRegistry::report). The registry accumulates,
// so this reports the phase's own totals at the moment they are final.
void reportPhase(const std::string &name) {
    if (const TimerStats *s = TimerRegistry::instance().find(name)) {
        ktlog.echo("phase {}: {:.3f}s wall, {:.3f}s cpu, {} call(s)", name, s->wallSeconds,
                   s->cpuSeconds, s->calls);
    }
}
// Said before placement runs, not after legalization fails. A legalizer handed a
// design that does not fit will produce an illegal placement and a table of
// confident numbers; checking the arithmetic first turns "the legalizer is
// broken" into "this design is 102% full".
struct Utilisation {
    double cellArea = 0.0;
    double fixedArea = 0.0;
    double rowArea = 0.0;
    double rowHeight = 0.0;
    std::size_t multiRow = 0;
    std::size_t cells = 0;
};

Utilisation measureUtilisation(const PlacementDB &db) {
    Utilisation u;
    const Graph &g = db.getGraph();
    const std::size_t nv = g.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        const double a = vert.width * vert.height;
        // A terminal is fixed area, not absent area. In the ISPD 2005 Bookshelf
        // suites the macros *are* the terminals, so skipping them reports adaptec1
        // as having no macros at all and understates the demand on the rows.
        if (vert.isFixed || vert.isTerminal) {
            u.fixedArea += a;
        } else {
            u.cellArea += a;
            ++u.cells;
        }
    }
    double pitch = std::numeric_limits<double>::max();
    for (const PlacementDB::RowInfo &r : db.getRows()) {
        if (!(r.pitch() > 0.0) || !(r.height > 0.0)) {
            continue;
        }
        u.rowHeight = std::max(u.rowHeight, r.height);
        pitch = std::min(pitch, r.pitch());
        for (const PlacementDB::SubrowInfo &si : r.subrows) {
            if (si.xhi(r.pitch()) > si.xlo()) {
                u.rowArea += (si.xhi(r.pitch()) - si.xlo()) * r.height;
            }
        }
    }
    // Cells that cannot fit a single row. Counted here because it is the other
    // way a design can be unplaceable at any density.
    if (u.rowHeight > 0.0) {
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type == VertexType::Cell && !vert.isFixed && !vert.isTerminal &&
                vert.height > u.rowHeight * 1.5) {
                ++u.multiRow;
            }
        }
    }
    (void)pitch;
    return u;
}

void reportUtilisation(const PlacementDB &db) {
    const Utilisation u = measureUtilisation(db);
    // Movable demand against the rows, which decides whether the design fits. The
    // fixed cells already occupy the rows rather than compete for them, so
    // charging their area here double-counts it (on adaptec1, 58% reads as 89%).
    // Macro area is still reported: it is real, it is not demand.
    const double util = u.rowArea > 0.0 ? 100.0 * u.cellArea / u.rowArea : 0.0;
    const double withFixed = u.rowArea > 0.0 ? 100.0 * (u.cellArea + u.fixedArea) / u.rowArea : 0.0;
    ktReportTable t("Design utilisation (before placement)");
    t.setHeaders({"measure", "value"});
    t.addRow({"movable cell area", fmt::format("{:.6e}", u.cellArea)});
    t.addRow({"fixed cell area", fmt::format("{:.6e}", u.fixedArea)});
    t.addRow({"row (placeable) area", fmt::format("{:.6e}", u.rowArea)});
    t.addRow({"utilisation (movable / rows)", fmt::format("{:.2}%", util)});
    t.addRow({"utilisation (incl. fixed cells)", fmt::format("{:.2}%", withFixed)});
    t.addRow({"movable cells", fmt::format("{}", u.cells)});
    if (u.rowHeight > 0.0) {
        t.addRow({"row height", fmt::format("{:.3}", u.rowHeight)});
        t.addRow({"cells taller than one row", fmt::format("{}", u.multiRow)});
    }
    t.emit();
    if (u.rowArea > 0.0 && util > 100.0) {
        ktlog.warning(
            "the design needs {:.6e} of movable cell area but only {:.6e} of row is placeable, "
            "so it is {:.1f}% full. No legal placement exists for this input: the cells do not "
            "fit, however the placer is retried.",
            u.cellArea, u.rowArea, util);
    }
    if (u.multiRow > 0) {
        // Said here, before placement runs, rather than only after legalization
        // fails: this is a property of the input, so the reader learns it before
        // spending several minutes on a global placement that cannot end legal.
        ktlog.warning(
            "{} cell(s) are taller than one row (row height {:.3}) and the legalizer only "
            "places into single rows, so those cells will be left unplaced and the result will "
            "not be legal. Legalizing this design needs a multi-height legalizer, which this "
            "build does not have.",
            u.multiRow, u.rowHeight);
    }
}
}  // namespace

class FlowMgr::Impl {
public:
    std::unique_ptr<PlacementDB> db;
    std::unique_ptr<BookshelfInputAdapter> bookshelfAdapter;
    std::unique_ptr<LefDefInputAdapter> lefdefAdapter;
    bool loaded = false;
    bool placed = false;

    bool loadInput(const std::string &dirPath);
    bool loadBookshelf(const std::string &dirPath);
    bool loadBookshelfFromFiles(const std::string &nodesFile, const std::string &netsFile,
                                const std::string &plFile = "", const std::string &sclFile = "",
                                const std::string &wtsFile = "");
    bool runPlacement(const std::string &algorithm = "simpl", const std::string &plotDir = "",
                      const std::string &snapshotDir = "");
    // Legalize and then detail-place, for the algorithms that stop at a global
    // placement. Split out of runPlacement because RePlAce and SimPL both end
    // here, and the reporting is identical -- a second copy would drift.
    bool legalizeAndDetail(const std::string &plotDir, const constraintMgr *fences);

    // HPWL after legalization and detailed placement -- the number the paper
    // reports, and the only one that ranks two global-placement runs. Kept so
    // the summary can show it next to SimPL's own upper bound, because the two
    // differ by more than the differences being compared: the upper bound is
    // the look-ahead legalized placement, taken before either stage runs.
    double hpwlFinalPlaced_ = -1.0;
    bool writePlacement(const std::string &outputPath);
    PlacementDB &getPlacementDB();
    const PlacementDB &getPlacementDB() const;
    bool isLoaded() const;
    void clear();
};


FlowMgr::FlowMgr() : pImpl(std::make_unique<Impl>()) {
    pImpl->db = std::make_unique<PlacementDB>();
    pImpl->bookshelfAdapter = std::make_unique<BookshelfInputAdapter>();
}

FlowMgr::~FlowMgr() = default;

FlowMgr::FlowMgr(FlowMgr &&) noexcept = default;
FlowMgr &FlowMgr::operator=(FlowMgr &&) noexcept = default;

void FlowMgr::run(const kt_option &options) {
    // Phase timers accumulate into the registry; the summary is reported once
    // at the end of the run.
    {
        ScopedTimer timer("load");
        if (!pImpl->loadInput(options.inputPath)) {
            throw std::runtime_error("Failed to load input files");
        }
    }
    reportPhase("load");

    {
        PlacementDB &db = pImpl->getPlacementDB();
        auto [numCells, numNets] = db.getStats();
        // What placement did the design actually ship with? Worth logging: if it
        // is missing or degenerate every placer silently falls back to its own
        // seed, which looks like a placer bug and is not one.
        {
            std::size_t moved = 0;
            double lo = 1e300, hi = -1e300, loY = 1e300, hiY = -1e300;
            const Graph &g = db.getGraph();
            for (std::size_t v = 0; v < g.getNumVertices(); ++v) {
                const Vertex &vert = g.getVertex(v);
                if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
                    continue;
                }
                lo = std::min(lo, vert.x);
                hi = std::max(hi, vert.x);
                loY = std::min(loY, vert.y);
                hiY = std::max(hiY, vert.y);
                moved += (vert.x != 0.0 || vert.y != 0.0) ? 1 : 0;
            }
            ktlog.echo(
                "Loaded placement: {}/{} cells carry a position, bbox "
                "x[{:.1f},{:.1f}] y[{:.1f},{:.1f}]",
                moved, g.getNumVertices(), lo, hi, loY, hiY);
        }
        ktlog.echo("Loaded: {} cells ({} terminals), {} nets, {} pins, {} rows", numCells,
                   db.getNumTerminals(), numNets, db.getNumPins(), db.getNumRows());
    }

    {
        ScopedTimer timer("place");
        const std::string snapshotDir =
            options.getOutputPath().find_last_of('/') == std::string::npos
                ? std::string(".")
                : options.getOutputPath().substr(0, options.getOutputPath().find_last_of('/'));
        // Every run records per-iteration SVG frames and an HPWL curve, so an
        // iteration can be inspected afterwards without asking for them.
        const std::string effectivePlotDir =
            !options.getPlotDir().empty() ? options.getPlotDir() : snapshotDir + "/plots";
        // Before the placer, not after: once the solver is running, every number
        // downstream is derived from a density model, and on an over-full design
        // that model is describing an impossibility. The reader needs to know the
        // design did not fit before they read a wirelength off it.
        reportUtilisation(*pImpl->db);
        if (!pImpl->runPlacement(options.algorithm, effectivePlotDir, snapshotDir)) {
            throw std::runtime_error("Placement algorithm failed");
        }
    }
    reportPhase("place");

    {
        ScopedTimer timer("write");
        if (!pImpl->writePlacement(options.getOutputPath())) {
            throw std::runtime_error("Failed to write output");
        }
    }
    reportPhase("write");

    TimerRegistry::instance().report();
}


bool FlowMgr::Impl::loadInput(const std::string &dirPath) {
    // Auto-detect the input format: a directory containing LEF/DEF files is
    // loaded through the LEF/DEF adapter; otherwise Bookshelf is assumed.
    namespace fs = std::filesystem;
    bool hasDef = false;
    std::error_code ec;
    fs::directory_iterator it(dirPath, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if ((name.size() >= 4 && name.rfind(".def") == name.size() - 4) ||
            (name.size() >= 7 && name.rfind(".def.gz") == name.size() - 7)) {
            hasDef = true;
            break;
        }
    }
    if (ec) {
        ktlog.fatal("cannot scan input directory: {}", dirPath);
    }
    if (hasDef) {
        ktlog.echo("Detected LEF/DEF input in {}", dirPath);
        clear();
        lefdefAdapter = std::make_unique<LefDefInputAdapter>(std::make_unique<PlacementDB>());
        if (!lefdefAdapter->readFromDirectory(dirPath)) {
            ktlog.fatal("Failed to load LEF/DEF format from {}", dirPath);
        }
        db = lefdefAdapter->releasePlacementDB();
        loaded = true;
        placed = false;
        return true;
    }
    return loadBookshelf(dirPath);
}

bool FlowMgr::Impl::loadBookshelf(const std::string &dirPath) {
    clear();

    bookshelfAdapter = std::make_unique<BookshelfInputAdapter>(std::make_unique<PlacementDB>());

    if (!bookshelfAdapter->readFromDirectory(dirPath)) {
        ktlog.fatal("Failed to load Bookshelf format from {}", dirPath);
    }

    db = bookshelfAdapter->releasePlacementDB();
    loaded = true;
    placed = false;

    return true;
}

bool FlowMgr::Impl::loadBookshelfFromFiles(const std::string &nodesFile,
                                           const std::string &netsFile, const std::string &plFile,
                                           const std::string &sclFile, const std::string &wtsFile) {
    clear();

    bookshelfAdapter = std::make_unique<BookshelfInputAdapter>(std::make_unique<PlacementDB>());

    if (!bookshelfAdapter->readFromFiles(nodesFile, netsFile, plFile, sclFile, wtsFile)) {
        ktlog.fatal("Failed to load Bookshelf format files");
    }

    db = bookshelfAdapter->releasePlacementDB();
    loaded = true;
    placed = false;

    return true;
}


namespace {
// One thing wrong with a finished placement.
struct Defect {
    std::string what;
    std::size_t count = 0;
};

// Independent pass over the placement as it will be written. The stages each
// self-check, but only for what they knew to ask about: a stage reporting zero
// overlaps while the delivered file has them is the failure worth catching, since
// the file is what the next tool reads. Off-die and out-of-fence cells are also
// only counted here.
std::vector<Defect> verifyPlacement(const PlacementDB &db, const constraintMgr *fences) {
    std::vector<Defect> defects;
    const Graph &g = db.getGraph();

    // The same notion of "the die" the placer used. Two different ones would fail
    // a legal placement: the placer spreads over the union of the fixed geometry
    // and the rows, so a checker built from the
    // fixed geometry alone fails every cell in a row that reaches past the pads.
    const std::array<double, 4> box = placementDieBox(db);
    std::size_t outOfDie = 0;
    std::size_t offFence = 0;
    const std::size_t nv = g.getNumVertices();
    // Row height, for deciding what counts as a tall cell below.
    double rowHeight = 0.0;
    for (const PlacementDB::RowInfo &ri : db.getRows()) {
        if (ri.height > rowHeight) {
            rowHeight = ri.height;
        }
    }
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
            continue;
        }
        const double eps = 1e-6;
        if (vert.x < box[0] - eps || vert.y < box[1] - eps || vert.x + vert.width > box[2] + eps ||
            vert.y + vert.height > box[3] + eps) {
            ++outOfDie;
            continue;
        }
        if (fences != nullptr && vert.regionId != constraintMgr::kNoRegion) {
            std::vector<double> flat{vert.x, vert.y};
            std::vector<int> ids{vert.regionId};
            offFence += fences->countViolations(flat, ids);
        }
    }

    // Overlaps. Bulk cells go in a grid with a bin sized for a typical cell, and
    // each pair is examined once, in its bucket and the four forward neighbours.
    // The bin must NOT be sized from the largest cell: ibm01 has one cell 12752
    // units tall, so that makes the bin the size of the die and the check a
    // 12500^2 comparison. Tall cells are outside the grid for the same reason and
    // are compared against everything directly.
    std::size_t overlaps = 0;
    {
        double bin = 0.0;
        std::size_t nTall = 0;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell) {
                continue;
            }
            // "Tall" against the rows, not an absolute height: a cell more than
            // four rows high cannot be found by a standard-cell-sized bin.
            const double rows = rowHeight > 0.0 ? vert.height / rowHeight : vert.height;
            if (rows > 4.0) {
                ++nTall;
                continue;
            }
            bin += vert.width * vert.height;
        }
        const double meanArea = nv > 0 ? bin / std::max<std::size_t>(1, nv - nTall) : 0.0;
        double cell = meanArea > 0.0 ? std::sqrt(meanArea) : 1.0;
        const double dieW = std::max(box[2] - box[0], 1.0);
        const double dieH = std::max(box[3] - box[1], 1.0);
        // A few cells per bucket: enough that the map does not dominate, coarse
        // enough that a standard cell does not span many bins (which is what made
        // the original miss pairs).
        const std::size_t target = 64;
        cell = std::max(cell, std::max(dieW, dieH) / 512.0);
        cell = std::max(cell, 1e-9);
        (void)target;

        std::map<std::pair<long long, long long>, std::vector<std::size_t>> buckets;
        std::vector<std::size_t> tallCells;
        std::vector<char> isTall(nv, 0);
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getVertex(v);
            if (vert.type != VertexType::Cell) {
                continue;
            }
            const double rows = rowHeight > 0.0 ? vert.height / rowHeight : vert.height;
            if (rows > 4.0) {
                tallCells.push_back(v);
                isTall[v] = 1;
                continue;
            }
            const long long bx = static_cast<long long>(std::floor(vert.x / cell));
            const long long by = static_cast<long long>(std::floor(vert.y / cell));
            buckets[{bx, by}].push_back(v);
        }
        const double eps = 1e-9;
        const auto hits = [&](std::size_t a, std::size_t b) {
            const Vertex &p = g.getVertex(a);
            const Vertex &q = g.getVertex(b);
            // Two fixed cells overlapping is the input's business, not ours.
            if (p.isFixed && q.isFixed) {
                return false;
            }
            return p.x < q.x + q.width - eps && q.x < p.x + p.width - eps &&
                   p.y < q.y + q.height - eps && q.y < p.y + p.height - eps;
        };
        // Only forward neighbours: all eight examines each cross-bucket pair twice.
        static const int kFwd[4][2] = {{1, 0}, {-1, 1}, {0, 1}, {1, 1}};
        for (const auto &kv : buckets) {
            const std::vector<std::size_t> &mine = kv.second;
            for (std::size_t a = 0; a < mine.size(); ++a) {
                for (std::size_t b = a + 1; b < mine.size(); ++b) {
                    overlaps += hits(mine[a], mine[b]) ? 1u : 0u;
                }
            }
            for (const auto &d : kFwd) {
                auto it = buckets.find({kv.first.first + d[0], kv.first.second + d[1]});
                if (it == buckets.end()) {
                    continue;
                }
                for (const std::size_t a : mine) {
                    for (const std::size_t b : it->second) {
                        overlaps += hits(a, b) ? 1u : 0u;
                    }
                }
            }
        }
        // Tall cells against every vertex, not just those with a higher index.
        // Restricting to b > a misses a tall cell paired with a lower-indexed
        // normal cell, since the grid pairs normal cells with normal cells only
        // (558 counted where a direct scan found 827). A tall-tall pair is still
        // accepted only once, by index.
        for (const std::size_t a : tallCells) {
            for (std::size_t b = 0; b < nv; ++b) {
                if (b == a) {
                    continue;
                }
                const Vertex &q = g.getVertex(b);
                if (q.type != VertexType::Cell) {
                    continue;
                }
                // Both tall: the same pair is reached from both sides, so keep
                // only the one where this is the lower vertex index.
                if (isTall[b] && b < a) {
                    continue;
                }
                if (hits(a, b)) {
                    ++overlaps;
                }
            }
        }
    }

    if (outOfDie > 0) {
        defects.push_back({"cells outside the die", outOfDie});
    }
    if (overlaps > 0) {
        defects.push_back({"overlapping cell pairs", overlaps});
    }
    if (offFence > 0) {
        defects.push_back({"cells outside their fence", offFence});
    }
    return defects;
}
}  // namespace

bool FlowMgr::Impl::legalizeAndDetail(const std::string &plotDir, const constraintMgr *fences) {
    // Whatever the placer reserved is released, so the later stages get the whole
    // remaining budget.
    if (PlacementAnimator::instance().enabled()) {
        PlacementAnimator::instance().holdBack(0);
    }

    // Abacus removes the overlap global placement left, with the least movement
    // it can, and self-checks so a legalization bug surfaces as a count.
    ktlog.echo("Running Abacus legalization...");
    AbacusLegalizer legalizer(*db);
    LegalizeParams lparams;
    if (const char *e = std::getenv("KTPLACE_ABACUS_MAX_ROW_DIST")) {
        lparams.maxRowDistance = static_cast<std::size_t>(std::atoll(e));
    }
    lparams.constraints = fences;
    if (!plotDir.empty()) {
        lparams.plotDir = plotDir + "/legalize";
        lparams.frameEvery =
            std::getenv("KTPLACE_ABACUS_FRAME_EVERY")
                ? static_cast<std::size_t>(std::atoll(std::getenv("KTPLACE_ABACUS_FRAME_EVERY")))
                : 20000;
    }
    const LegalizeResult lres = legalizer.legalize(lparams);
    reportPhase("legalize");
    ktReportTable lsummary("Legalization (Abacus)");
    lsummary.setHeaders({"metric", "value"});
    lsummary.addRow({"cells placed", fmt::format("{}", lres.cellsPlaced)});
    lsummary.addRow({"cells unplaced", fmt::format("{}", lres.unplaced)});
    lsummary.addRow({"squared displacement", fmt::format("{:.6}", lres.totalSquaredDisplacement)});
    lsummary.addRow({"max displacement", fmt::format("{:.6}", lres.maxDisplacement)});
    lsummary.addRow({"HPWL before", fmt::format("{:.6}", lres.hpwlBefore)});
    lsummary.addRow({"HPWL after", fmt::format("{:.6}", lres.hpwlAfter)});
    lsummary.addRow({"time (s)", fmt::format("{:.6}", lres.seconds)});
    lsummary.addRow({"overlapping pairs", fmt::format("{}", lres.overlappingPairs)});
    lsummary.addRow({"cells off row", fmt::format("{}", lres.offRow)});
    lsummary.addRow({"cells off site", fmt::format("{}", lres.offSite)});
    lsummary.addRow({"cells over macro", fmt::format("{}", lres.overFixed)});
    lsummary.addRow({"cells out of rows", fmt::format("{}", lres.outOfRows)});
    lsummary.addRow({"commit failures", fmt::format("{}", lres.commitFailures)});
    lsummary.emit();
    if (lres.overlappingPairs != 0 || lres.offRow != 0 || lres.overFixed != 0) {
        ktlog.warning("legalization is not legal; see the counts above");
    }

    // Abacus places into single rows, so a cell taller than one row stays where
    // global placement left it, overlapping (120 on ibm01). There is no multi-row
    // legalizer in this build, so the affected cells are named rather than left to
    // be inferred from a count.
    if (lres.outOfRows > 0) {
        // After the fact, so the pre-flight estimate above can be checked.
        ktlog.warning(
            "{} cell(s) taller than one row could not be placed and are still at their global "
            "placement positions. The placement is not legal; see \"cells out of rows\" above.",
            lres.outOfRows);
    }

    // Abacus minimises displacement, not wirelength, so legalization usually
    // costs a little HPWL; detailed placement wins it back.
    ktlog.echo("Running FastDP detailed placement...");
    FastDetailedPlacer dp(*db);
    DetailPlaceParams dparams;
    if (const char *e = std::getenv("KTPLACE_DP_WINDOW")) {
        dparams.localReorderWindow = static_cast<std::size_t>(std::atoll(e));
    }
    dparams.constraints = fences;
    if (!plotDir.empty()) {
        dparams.plotDir = plotDir + "/detailplace";
    }
    const DetailPlaceResult dres = dp.place(dparams);
    reportPhase("detail-place");
    ktReportTable dsummary("Detailed placement (FastDP)");
    dsummary.setHeaders({"metric", "value"});
    dsummary.addRow({"global swaps", fmt::format("{}", dres.globalSwaps)});
    dsummary.addRow({"vertical swaps", fmt::format("{}", dres.verticalSwaps)});
    dsummary.addRow({"reorder moves", fmt::format("{}", dres.reorderMoves)});
    dsummary.addRow({"cluster moves", fmt::format("{}", dres.clusterMoves)});
    dsummary.addRow({"HPWL before", fmt::format("{:.6}", dres.hpwlBefore)});
    hpwlFinalPlaced_ = dres.hpwlAfter;
    dsummary.addRow({"HPWL after", fmt::format("{:.6}", dres.hpwlAfter)});
    dsummary.addRow({"HPWL change",
                     fmt::format("{:.2}%", 100.0 * (dres.hpwlAfter - dres.hpwlBefore) /
                                               (dres.hpwlBefore > 0.0 ? dres.hpwlBefore : 1.0))});
    dsummary.addRow({"time (s)", fmt::format("{:.6}", dres.seconds)});
    dsummary.addRow({"overlapping pairs", fmt::format("{}", dres.overlappingPairs)});
    dsummary.addRow({"cells off row", fmt::format("{}", dres.offRow)});
    dsummary.addRow({"cells off site", fmt::format("{}", dres.offSite)});
    dsummary.addRow({"cells over macro", fmt::format("{}", dres.overFixed)});
    dsummary.emit();
    if (dres.overlappingPairs != 0 || dres.offRow != 0 || dres.overFixed != 0) {
        ktlog.warning("detailed placement broke legality; see the counts above");
    }
    // Last, so it sees the effect of both stages, and separate from their
    // self-checks, so a disagreement between what a stage claims and what the file
    // contains stays visible.
    {
        // Timed on its own: it reads the whole placement, changes nothing, and is
        // the phase that goes quadratic if the spatial index degrades.
        const ScopedTimer checkTimer("place-check");
        const std::vector<Defect> defects = verifyPlacement(*db, fences);
        ktReportTable check("Placement check (independent, after legalization)");
        check.setHeaders({"check", "result"});
        if (defects.empty()) {
            check.addRow({"overlapping cell pairs", "0"});
            check.addRow({"cells outside the die", "0"});
            check.addRow({"cells outside their fence", fences != nullptr ? "0" : "n/a"});
            check.addRow({"verdict", "PASS"});
        } else {
            for (const Defect &d : defects) {
                check.addRow({d.what, fmt::format("{}", d.count)});
            }
            check.addRow({"verdict", "FAIL"});
        }
        check.emit();
        if (!defects.empty()) {
            ktlog.warning(
                "the placement that will be written is not legal; see the placement check "
                "above");
        }
        reportPhase("place-check");
    }

    // A separate high-resolution still, because GIF frames have to stay small and
    // a big design's cells collapse to a pixel each at that size. The zoom is on
    // both axes: 8 turns a 768x768 frame into 6144x6144, about fourteen pixels
    // per standard cell. Written once, and it compresses to a couple of MB.
    if (!plotDir.empty()) {
        double zoom = 8.0;
        if (const char *e = std::getenv("KTPLACE_FINAL_ZOOM")) {
            const double v = std::atof(e);
            if (v >= 1.0) {
                zoom = v;
            }
        }
        {
            ScopedTimer finalTimer("final-image");
            const std::string finalDir = plotDir + "/final";
            std::error_code ec;
            std::filesystem::create_directories(finalDir, ec);
            if (ec) {
                ktlog.warning(
                    "cannot create the final-image plot directory '{}': {}. "
                    "No high-resolution final still written.",
                    finalDir, ec.message());
            } else {
                // PNG, not PPM: no viewer opens a PPM, and flat colour compresses
                // to a couple of megabytes at this resolution.
                writeFinalFrameRaster(finalDir + "/final.png", db->getGraph(), fences, zoom,
                                      dres.hpwlAfter);
                // The vector form, and the one to open when a region needs looking
                // at closely. One <rect> per cell, so "every cell is in the
                // picture" is a count, which is what the CI smoke test checks.
                writeFinalFrameSvg(finalDir + "/final.svg", db->getGraph(), fences, dres.hpwlAfter);
                // 113 MB of raw pixels at this size, so only on request: the PNG is
                // the artefact anyone looks at.
                if (std::getenv("KTPLACE_FINAL_PPM") != nullptr) {
                    writeFinalFrameRaster(finalDir + "/final.ppm", db->getGraph(), fences, zoom,
                                          dres.hpwlAfter);
                }
                ktlog.echo("final high-resolution image: {}/final.png ({}x{})", finalDir,
                           static_cast<int>(std::lround(zoom * 768.0)),
                           static_cast<int>(std::lround(zoom * 768.0)));
            }
        }
        reportPhase("final-image");
    }

    // Every stage has now contributed, so the run can be told as one animation.
    // Assembling it here, and not at the end of global placement, is the whole
    // point: the legalizer's pull back onto the rows is usually the most
    // consequential motion of the run, and it happens after the placer is done.
    if (const auto &anim = PlacementAnimator::instance(); anim.enabled()) {
        // Timed because encoding the GIF is pure output cost: it can be longer
        // than the detailed placement on a small design, and without a number here
        // it looks like the run hung at the end.
        ScopedTimer animTimer("anim-finish");
        if (anim.finish()) {
            ktlog.echo(
                "animation: {} frames -> {}/anim/placement.gif (global placement, then "
                "legalization, then detailed placement)",
                anim.frameCount(), plotDir);
        } else {
            ktlog.echo(
                "animation: {} frame(s) recorded, no GIF written (one animation needs at "
                "least two frames)",
                anim.frameCount());
        }
        reportPhase("anim-finish");
    }
    placed = true;
    return true;
}

bool FlowMgr::Impl::runPlacement(const std::string &algorithm, const std::string &plotDir,
                                 const std::string &snapshotDir) {
    if (!loaded) {
        ktlog.fatal("No placement database loaded");
    }

    // Configured here rather than inside a stage, so the legalizer and the
    // detailed placer add to the same GIF. Declared before both the animator setup
    // and the budget split below, which used to disagree about the frame count.
    std::size_t animFrameBudget = 0;
    // The value is read, not just its presence: KTPLACE_ANIM=0 used to enable the
    // animation, so every run meant to skip it still spent the encode time.
    const char *animEnv = std::getenv("KTPLACE_ANIM");
    const bool animOn = animEnv == nullptr || std::atoi(animEnv) != 0;
    if (!plotDir.empty() && animOn) {
        // The budget decides how much of the run the GIF shows. At 300 a run of a
        // few dozen iterations spent the lot before legalization began, so it
        // stopped where it gets interesting. Frames cost time and size, not
        // correctness. KTPLACE_ANIM_MAX_FRAMES still caps it for a quick look.
        //
        // Frame scale against the 768x768 frame size. Two is the largest at which
        // a few hundred CG frames still fit the byte budget below and a standard
        // cell stays a couple of pixels rather than one.
        double animZoom = 2.0;
        if (const char *e = std::getenv("KTPLACE_ANIM_ZOOM")) {
            const double v = std::atof(e);
            if (v >= 1.0) {
                animZoom = v;
            }
        }
        // Byte target for the finished GIF, overridable. 64 MB opens in a browser
        // and in a file manager's preview.
        const std::size_t animGifByteCap =
            std::getenv("KTPLACE_ANIM_MAX_BYTES")
                ? static_cast<std::size_t>(std::atoll(std::getenv("KTPLACE_ANIM_MAX_BYTES")))
                : std::size_t{96} << 20;

        // Budget by bytes, not by frame count. Placement frames are nearly
        // incompressible, so size is essentially w * h * frames / 8: a frame count
        // alone allowed an 8 GB file. Sizing to a byte target keeps the resolution
        // and cuts the frame count instead.
        const double animW = 768.0 * animZoom;
        const double animH = 768.0 * animZoom;
        const double kBytesPerPixel = 0.9 / 8.0;  // measured, not assumed
        // bytes = width * height * bytesPerPixel * frames, so the frame count that
        // fills the budget divides it out. Multiplying by the bytes-per-pixel
        // instead gave 12 frames, which is a flicker and not an animation.
        const std::size_t sizeCapFrames = static_cast<std::size_t>(
            static_cast<double>(animGifByteCap) / (animW * animH * kBytesPerPixel));
        const std::size_t maxFrames =
            std::getenv("KTPLACE_ANIM_MAX_FRAMES")
                ? static_cast<std::size_t>(std::atoll(std::getenv("KTPLACE_ANIM_MAX_FRAMES")))
                : std::max<std::size_t>(12, sizeCapFrames);
        animFrameBudget = maxFrames;
        // 12 centiseconds (120 ms) per frame. The default 6 was quick enough that
        // a 300-frame animation flashed past in under two seconds, which is not
        // long enough to follow a placement moving.
        const int delayCs = std::getenv("KTPLACE_ANIM_DELAY_CS")
                                ? std::atoi(std::getenv("KTPLACE_ANIM_DELAY_CS"))
                                : 12;
        // Three frames per recorded placement: the two in-between plus the
        // placement itself, which is what turns a per-iteration jump into motion.
        const int blend =
            std::getenv("KTPLACE_ANIM_BLEND") ? std::atoi(std::getenv("KTPLACE_ANIM_BLEND")) : 3;
        PlacementAnimator::instance().configure(plotDir + "/anim", maxFrames, delayCs, blend,
                                                animZoom);
    } else {
        PlacementAnimator::instance().reset();
    }

    // Global placement is by far the most frame-hungry stage, so it does not get
    // to spend the whole budget: a quarter is held back for legalization and
    // detailed placement, which are the stages that turn a legal-looking but
    // unusable placement into a real one.
    if (PlacementAnimator::instance().enabled()) {
        const std::size_t total = animFrameBudget;
        // Two fifths held back. Global placement records a frame per solve
        // iteration and can spend anything it is given; with a fifth held back the
        // stages that make the placement legal got a handful of frames.
        PlacementAnimator::instance().holdBack(total * 2 / 5);
    }

    if (algorithm == "simpl") {
        ktlog.echo("Running SimPL global placement (B2B net model + look-ahead legalization)...");
        SimplePlacer placer(*db);
        // Fences come from the LEF/DEF reader; Bookshelf carries none, so a null
        // pointer there means a correctly unconstrained design. Ignoring fences
        // scatters fenced cells across the die and draws no regions.
        const constraintMgr *regions = nullptr;
        if (lefdefAdapter && lefdefAdapter->getConstraints().numRegions() > 0) {
            regions = &lefdefAdapter->getConstraints();
        }
        // KTPLACE_SIMPL_FENCES=0 shows the placement without them, which is the
        // only way to see what the constraint costs. Off means neither enforced
        // nor drawn, so the frames do not show regions that are not honoured.
        const char *fenceEnv = std::getenv("KTPLACE_SIMPL_FENCES");
        const bool fencesOn = (fenceEnv == nullptr) || (std::atoi(fenceEnv) != 0);
        if (!fencesOn) {
            regions = nullptr;
            ktlog.echo("SimPL: fence enforcement DISABLED by KTPLACE_SIMPL_FENCES=0");
        }
        SimplParams params;
        params.traceEvery = 5;
        // Per-iteration frames, for watching the LSS/LAL interaction.
        if (const char *e = std::getenv("KTPLACE_SIMPL_TRACE_EVERY")) {
            params.traceEvery = static_cast<std::size_t>(std::atoll(e));
        }
        // Every CG iteration of every round is offered to the animation, so the GIF
        // shows the solve converging and not just the outer loop. The animator
        // subsamples to fit its budget, so this costs thinning, not truncation.
        params.cgEvery = 1;
        if (const char *e = std::getenv("KTPLACE_SIMPL_CG_EVERY")) {
            params.cgEvery = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_SIMPL_DENSITY_MAPS")) {
            params.densityMaps = std::atoll(e) != 0;
        }
        const SimplResult res = placer.place(params, plotDir, snapshotDir, regions);
        ktReportTable summary("Solver results");
        summary.setHeaders({"metric", "initial", "final"});
        summary.addRow({"movable cells", "", fmt::format("{}", res.numMovable)});
        summary.addRow({"fixed cells", "", fmt::format("{}", res.numFixed)});
        summary.addRow({"nets", "", fmt::format("{}", res.nets)});
        summary.addRow({"init iterations", "", fmt::format("{}", res.initIters)});
        summary.addRow({"global iterations", "", fmt::format("{}", res.globalIters)});
        summary.addRow({"bin grid", "", fmt::format("{}x{}", res.binsX, res.binsY)});
        summary.addRow({"matrix build (s)", "", fmt::format("{:.6}", res.buildSeconds)});
        summary.addRow({"look-ahead (s)", "", fmt::format("{:.6}", res.spreadSeconds)});
        summary.addRow({"linear solves (s)", "", fmt::format("{:.6}", res.solveSeconds)});
        summary.addRow({"look-ahead legalization", "", res.usedLookAhead ? "on" : "OFF (raw LSS)"});
        summary.addRow({"fence regions", "",
                        regions == nullptr ? (fencesOn ? "none" : "OFF (disabled)")
                                           : fmt::format("{}", regions->numRegions())});
        summary.addRow({"cells held in fence", "", fmt::format("{}", res.fenceClamps)});
        summary.addRow({"cells pushed out of a fence", "", fmt::format("{}", res.fencePushes)});
        summary.addRow(
            {"cells outside their fence at exit", "", fmt::format("{}", res.fenceViolations)});
        summary.addRow({"HPWL seed", fmt::format("{:.6}", res.hpwlSeed), ""});
        summary.addRow({"HPWL lower bound", "", fmt::format("{:.6}", res.hpwlLower)});
        summary.addRow({"HPWL final", "", fmt::format("{:.6}", res.hpwlFinal)});
        summary.addRow({"returned from iteration", "",
                        fmt::format("{} of {}", res.bestIter, res.globalIters)});
        summary.addRow({"bound gap", "", fmt::format("{:.6}", res.gap)});
        summary.addRow({"scaled overflow (lower)", "", fmt::format("{:.6}", res.overflowLower)});
        summary.addRow({"scaled overflow (final)", "", fmt::format("{:.6}", res.overflowFinal)});
        summary.addRow({"SVG frames written", "", fmt::format("{}", res.framesWritten)});
        summary.emit();

        legalizeAndDetail(plotDir, regions);
        if (hpwlFinalPlaced_ > 0.0) {
            // Rankable result, beside the bound it is derived from, so a change
            // can be judged on the metric the paper publishes rather than on the
            // intermediate upper bound, which moves for reasons that do not
            // survive legalization.
            ktReportTable quality("Placement quality (after legalization and detail placement)");
            quality.setHeaders({"metric", "value"});
            quality.addRow({"HPWL detailed", fmt::format("{:.6}", hpwlFinalPlaced_)});
            quality.addRow({"HPWL global upper bound", fmt::format("{:.6}", res.hpwlFinal)});
            quality.addRow({"legalization + detail change",
                           fmt::format("{:.2}%", 100.0 * (hpwlFinalPlaced_ - res.hpwlFinal) /
                                                     (res.hpwlFinal > 0.0 ? res.hpwlFinal : 1.0))});
            quality.addRow({"paper reference (adaptec1)", "77410738"});
            quality.emit();
        }
        return true;

    } else if (algorithm == "ntuplace1") {
        ktlog.echo("Running NTUPlace1 global placement (ratio partitioning)...");
        RatioPlacer placer(*db);
        const constraintMgr *regions = nullptr;
        if (lefdefAdapter && lefdefAdapter->getConstraints().numRegions() > 0) {
            regions = &lefdefAdapter->getConstraints();
        }
        RatioPlaceParams params;
        params.plotDir = plotDir;
        if (const char *e = std::getenv("KTPLACE_NTU_LEAF_CELLS")) {
            params.targetLeafCells = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_NTU_MAX_LEVELS")) {
            params.maxLevels = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_NTU_RETRIES")) {
            params.maxRatioRetries = static_cast<std::size_t>(std::atoll(e));
        }
        if (const char *e = std::getenv("KTPLACE_NTU_MIN_NET_WEIGHT")) {
            params.minNetWeight = std::atof(e);
        }
        if (const char *e = std::getenv("KTPLACE_NTU_VERBOSE")) {
            params.verbose = std::atoll(e) != 0;
        }
        const RatioPlaceResult res = placer.place(params, regions);
        ktReportTable summary("NTUplace1 solver results");
        summary.setHeaders({"metric", "value"});
        summary.addRow({"movable cells", fmt::format("{}", res.numMovable)});
        summary.addRow({"fixed cells", fmt::format("{}", res.numFixed)});
        summary.addRow({"hypergraph nets", fmt::format("{}", res.nets)});
        summary.addRow({"cuts accepted", fmt::format("{}", res.cuts)});
        summary.addRow({"ratio retries", fmt::format("{}", res.ratioRetries)});
        summary.addRow({"retries per cut", fmt::format("{:.3}", res.meanImbalance)});
        summary.addRow({"recursion depth reached", fmt::format("{}", res.maxDepth)});
        summary.addRow({"smallest leaf", fmt::format("{}", res.minLeafCells)});
        summary.addRow({"HPWL (pre-legalization)", fmt::format("{:.6}", res.hpwlFinal)});
        summary.addRow({"paper reference (adaptec1)", "44800000"});
        summary.emit();

        // The ratio partitioner deliberately leaves the design overfull: GP here is
        // only a geometric ordering, and the legalizer plus detail placer is what
        // turns it into a legal placement. That is the same split the paper uses.
        legalizeAndDetail(plotDir, regions);
        if (hpwlFinalPlaced_ > 0.0) {
            ktReportTable quality("Placement quality (after legalization and detail placement)");
            quality.setHeaders({"metric", "value"});
            quality.addRow({"HPWL detailed", fmt::format("{:.6}", hpwlFinalPlaced_)});
            quality.addRow({"HPWL from partitioning", fmt::format("{:.6}", res.hpwlFinal)});
            quality.addRow({"legalization + detail change",
                           fmt::format("{:.2}%", 100.0 * (hpwlFinalPlaced_ - res.hpwlFinal) /
                                                     (res.hpwlFinal > 0.0 ? res.hpwlFinal : 1.0))});
            quality.emit();
        }
        return true;

    } else {
        ktlog.fatal("Unknown placement algorithm: {}", algorithm);
    }
}

bool FlowMgr::Impl::writePlacement(const std::string &outputPath) {
    if (!placed) {
        ktlog.fatal("No placement result available");
    }

    std::ofstream out(outputPath);
    if (!out.is_open()) {
        ktlog.fatal("Cannot open output file: {}", outputPath);
    }
    // Database coordinates reach ~1.5e6, so the default 6 significant digits
    // would round positions by several units and can move a cell across a
    // placement-region boundary. Keep enough digits to round-trip.
    out << std::setprecision(10);
    const Graph &g = db->getGraph();
    const std::size_t nv = g.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getVertex(v);
        if (vert.type != VertexType::Cell) {
            continue;
        }
        // Bookshelf .pl: "<name> <x> <y> : <orientation>"
        out << vert.name << '\t' << vert.x << '\t' << vert.y
            << "\t: " << (vert.isFixed ? "N /FIXED" : "N") << '\n';
    }
    return true;
}

PlacementDB &FlowMgr::Impl::getPlacementDB() {
    if (!db) {
        throw std::runtime_error("PlacementDB not initialized");
    }
    return *db;
}

const PlacementDB &FlowMgr::Impl::getPlacementDB() const {
    if (!db) {
        throw std::runtime_error("PlacementDB not initialized");
    }
    return *db;
}

bool FlowMgr::Impl::isLoaded() const {
    return loaded;
}

void FlowMgr::Impl::clear() {
    db.reset();
    bookshelfAdapter.reset();
    lefdefAdapter.reset();
    loaded = false;
    placed = false;
}

}  // namespace ktplace
