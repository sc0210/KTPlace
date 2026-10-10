// @file kt_dm.cc// Implementation of ktDM using PIMPL pattern with kt_graph


#include "datamodel/kt_dm.h"

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_graph.h"
#include "datamodel/kt_solutionMgr.h"
#include "util/kt_log.h"
#include "util/kt_reportTable.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

// oneTBB - parallel stats
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_reduce.h>

namespace ktplace {

// ktDM implementation

std::size_t ktDM::addCell(const std::string &name, double width, double height, bool isTerminal) {
    const std::size_t id = graph.addCell(name);
    Vertex &v = graph.getCell(id);
    v.width = width;
    v.height = height;
    v.isTerminal = isTerminal;
    return id;
}

bool ktDM::hasCell(const std::string &name) const {
    return graph.hasCell(name);
}

std::size_t ktDM::getCellId(const std::string &name) const {
    return graph.getCellId(name);
}

std::size_t ktDM::getNumCells() const {
    return graph.getNumCells();
}

std::size_t ktDM::getNumTerminals() const {
    const Graph &g = graph;
    return tbb::parallel_reduce(
        tbb::blocked_range<std::size_t>(0, g.getNumCells()), std::size_t(0),
        [&](const tbb::blocked_range<std::size_t> &r, std::size_t acc) {
            for (std::size_t i = r.begin(); i < r.end(); ++i) {
                if (g.getCell(i).isTerminal) {
                    ++acc;
                }
            }
            return acc;
        },
        [](std::size_t a, std::size_t b) {
            return a + b;
        });
}

std::size_t ktDM::addNet(const std::string &name, double weight) {
    return graph.addNet(name, weight);
}

bool ktDM::hasNet(const std::string &name) const {
    return graph.hasNet(name);
}

std::size_t ktDM::getNetId(const std::string &name) const {
    return graph.getNetId(name);
}

std::size_t ktDM::getNumNets() const {
    return graph.getNumNets();
}

std::size_t ktDM::addPin(const std::string &cellName, const std::string &netName, double offsetX,
                         double offsetY, bool isInput) {
    const std::size_t cellId = getCellId(cellName);
    const std::size_t netId = getNetId(netName);
    return graph.addPin(cellId, netId, isInput ? PinRole::Receiver : PinRole::Driver, offsetX,
                        offsetY);
}

std::size_t ktDM::getNumPins() const {
    return graph.getNumPins();
}

void ktDM::setCellPosition(std::size_t cellId, double x, double y) {
    Vertex &v = graph.getCell(cellId);
    v.x = x;
    v.y = y;
}

void ktDM::setCellPosition(const std::string &cellName, double x, double y) {
    setCellPosition(getCellId(cellName), x, y);
}

std::pair<double, double> ktDM::getCellPosition(std::size_t cellId) const {
    const Vertex &v = graph.getCell(cellId);
    return {v.x, v.y};
}

std::pair<double, double> ktDM::getCellPosition(const std::string &cellName) const {
    return getCellPosition(getCellId(cellName));
}

void ktDM::setCellFixed(std::size_t cellId, bool fixed) {
    graph.getCell(cellId).isFixed = fixed;
}

void ktDM::setCellFixed(const std::string &cellName, bool fixed) {
    setCellFixed(getCellId(cellName), fixed);
}

bool ktDM::isCellFixed(std::size_t cellId) const {
    return graph.getCell(cellId).isFixed;
}

bool ktDM::isCellFixed(const std::string &cellName) const {
    return isCellFixed(getCellId(cellName));
}

std::size_t ktDM::addRow(double coordinate, double height, double sitewidth, double sitespacing) {
    return dieInfo.addRow(coordinate, height, sitewidth, sitespacing);
}

std::size_t ktDM::addSubrow(std::size_t rowId, double originX, double numSites) {
    return dieInfo.addSubrow(rowId, originX, numSites);
}

void ktDM::clear() {
    graph.clear();
    dieInfo.clear();
}

std::pair<std::size_t, std::size_t> ktDM::getStats() const {
    return {getNumCells(), getNumNets()};
}

std::size_t ktDM::getNumRows() const {
    return dieInfo.getNumRows();
}

const std::vector<RowInfo> &ktDM::getRows() const {
    return dieInfo.getRows();
}

void ktDM::setDieArea(double xMin, double yMin, double xMax, double yMax) {
    dieInfo.setDieArea(xMin, yMin, xMax, yMax);
}

std::pair<std::pair<double, double>, std::pair<double, double>> ktDM::getDieArea() const {
    return dieInfo.getDieArea();
}

const std::vector<std::size_t> &ktDM::getNetPins(std::size_t netId) const {
    return graph.getNetPins(netId);
}

const std::vector<std::size_t> &ktDM::getCellPins(std::size_t cellId) const {
    return graph.getCellPins(cellId);
}

const constraintMgr &ktDM::constraints() const {
    return fences;
}

std::array<double, 4> ktDM::placementDieBox() const {
    return ktplace::placementDieBox(dieInfo, graph);
}

void ktDM::setConstraints(constraintMgr regions) {
    fences = std::move(regions);
}

bool ktDM::hasFences() const {
    return !fences.regions().empty();
}

ktDM::Utilisation ktDM::measureUtilisation() const {
    Utilisation u;
    const Graph &g = getGraph();
    const std::size_t nv = g.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
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
    for (const RowInfo &r : getRows()) {
        if (!(r.pitch() > 0.0) || !(r.height > 0.0)) {
            continue;
        }
        u.rowHeight = std::max(u.rowHeight, r.height);
        for (const SubrowInfo &si : r.subrows) {
            if (si.xhi(r.pitch()) > si.xlo()) {
                u.rowArea += (si.xhi(r.pitch()) - si.xlo()) * r.height;
            }
        }
    }
    // Cells that cannot fit a single row: the other way a design can be
    // unplaceable at any density.
    if (u.rowHeight > 0.0) {
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = g.getCell(v);
            if (!vert.isFixed && !vert.isTerminal && vert.height > u.rowHeight * 1.5) {
                ++u.multiRow;
            }
        }
    }
    return u;
}

void ktDM::reportUtilisation() const {
    const Utilisation u = measureUtilisation();
    // Movable demand against the rows, which decides whether the design fits. The
    // fixed cells already occupy the rows rather than compete for them, so
    // charging their area here double-counts it (on adaptec1, 58% reads as 89%).
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
    // Multi-row cells are not flagged here. This used to warn that no
    // multi-height legalizer existed and that the result would not be legal --
    // printed moments before the multi-row legalizer ran and legalized the design
    // perfectly well, so it described a limitation that had already been removed.
    // The row of the report carries the count, and legalization announces itself
    // when it picks the path.
}


void ktDM::report() const {
    const auto [numCells, numNets] = getStats();
    // What placement did the design ship with? Worth logging: if it is missing or
    // degenerate every placer silently falls back to its own seed, which looks
    // like a placer bug and is not one.
    std::size_t moved = 0;
    double lo = 1e300;
    double hi = -1e300;
    double loY = 1e300;
    double hiY = -1e300;
    const Graph &g = getGraph();
    for (std::size_t v = 0; v < g.getNumCells(); ++v) {
        const Vertex &vert = g.getCell(v);
        lo = std::min(lo, vert.x);
        hi = std::max(hi, vert.x);
        loY = std::min(loY, vert.y);
        hiY = std::max(hiY, vert.y);
        moved += (vert.x != 0.0 || vert.y != 0.0) ? 1 : 0;
    }
    ktlog.echo(
        "Loaded placement: {}/{} cells carry a position, bbox x[{:.1f},{:.1f}] y[{:.1f},{:.1f}]",
        moved, g.getNumCells(), lo, hi, loY, hiY);
    ktlog.echo("Loaded: {} cells ({} terminals), {} nets, {} pins, {} rows", numCells,
               getNumTerminals(), numNets, getNumPins(), getNumRows());
}

std::vector<ktDM::Defect> ktDM::verify() const {
    std::vector<Defect> defects;
    const Graph &g = getGraph();

    // The same notion of "the die" the placer used. Two different ones would fail
    // a legal placement: the placer spreads over the union of the fixed geometry
    // and the rows, so a checker built from the
    // fixed geometry alone fails every cell in a row that reaches past the pads.
    const std::array<double, 4> box = placementDieBox();
    std::size_t outOfDie = 0;
    std::size_t offFence = 0;
    const std::size_t nv = g.getNumCells();
    // Row height, for deciding what counts as a tall cell below.
    double rowHeight = 0.0;
    for (const RowInfo &ri : dieInfo.getRows()) {
        if (ri.height > rowHeight) {
            rowHeight = ri.height;
        }
    }
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = g.getCell(v);
        const double eps = 1e-6;
        if (vert.x < box[0] - eps || vert.y < box[1] - eps || vert.x + vert.width > box[2] + eps ||
            vert.y + vert.height > box[3] + eps) {
            ++outOfDie;
            continue;
        }
        if (hasFences() && vert.regionId != constraintMgr::kNoRegion) {
            std::vector<double> flat{vert.x, vert.y};
            std::vector<int> ids{vert.regionId};
            offFence += constraints().countViolations(flat, ids);
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
            const Vertex &vert = g.getCell(v);
            // "Tall" against the rows, not an absolute height: a cell more than
            // four rows high cannot be found by a standard-cell-sized bin.
            const double tall = rowHeight > 0.0 ? vert.height / rowHeight : vert.height;
            if (tall > 4.0) {
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
            const Vertex &vert = g.getCell(v);
            const double tall = rowHeight > 0.0 ? vert.height / rowHeight : vert.height;
            if (tall > 4.0) {
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
            const Vertex &p = g.getCell(a);
            const Vertex &q = g.getCell(b);
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

void ktDM::setPlacementSolution(const solutionMgr &solution) {
    solution.commitTo(graph);
}

solutionMgr ktDM::getPlacementSolution() const {
    return solutionMgr::fromGraph(graph);
}

double ktDM::hpwl() const {
    // The one HPWL, in kt_solution.cc. This used to be a second implementation of
    // the same arithmetic, alongside the ones in SimplePlacer and FastDP; it is
    // now the same call they all make, over the positions as committed.
    const solutionMgr sol = getPlacementSolution();
    return netlistHPWL(graph, sol.xs(), sol.ys());
}

}  // namespace ktplace
