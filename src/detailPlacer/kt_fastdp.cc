// @file kt_fastdp.cc// Fast detailed placement. See kt_fastdp.h for the technique summary.


#include "detailPlacer/kt_fastdp.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_animator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>


namespace ktplace {

namespace {

constexpr std::size_t kNoSlot = std::numeric_limits<std::size_t>::max();

/// One placeable span, mirroring the legalizer's subrows so a cell can only ever
/// be exchanged with another cell in the same span.
struct Span {
    double ylo = 0.0, yhi = 0.0;
    double xlo = 0.0, xhi = 0.0;
    double site = 1.0;
    /// Row pitch, taken as the row height. Vertical swaps compare row distances
    /// against this; comparing against the site width (1 on adaptec1, whose row
    /// pitch is 12) made every pair look non-adjacent and the technique never ran.
    double pitch = 1.0;
    std::size_t row = 0;             ///< index of the row this span belongs to
    std::vector<std::size_t> cells;  ///< ascending x

    [[nodiscard]] bool contains(double px, double pw) const {
        return px >= xlo - 1e-6 && px + pw <= xhi + 1e-6;
    }
};

/// A pin on a net. Fixed pins carry their absolute x and never move.
struct NetPin {
    std::size_t slot;   ///< kNoSlot for a fixed pin
    double offX = 0.0;  ///< offset from the cell's origin
    double offY = 0.0;  ///< offset from the cell's origin
    double absX = 0.0;  ///< used when slot == kNoSlot
};

struct FixedBox {
    double y0, y1, x0, x1;
};

}  // namespace

// ---------------------------------------------------------------------------

class FastDetailedPlacer::Impl {
public:
    explicit Impl(ktDM &db) : db_(db), graph_(db.getGraph()) {}

    DetailPlaceResult place(const DetailPlaceParams &params);

private:
    void buildSpans();
    void buildNetlist();
    [[nodiscard]] double hpwl() const;
    /// Exact HPWL change from moving cell c to nx, given a scratch copy of the
    /// affected positions.
    [[nodiscard]] double deltaMove(std::size_t c, double nx) const;
    /// Exact HPWL change from exchanging two cells.
    /// @param  nay  y cell a lands at; y_[a] is unchanged
    /// @param  nby  y cell b lands at
    /// Both axes are measured, because the exchange is judged against the same
    /// HPWL that hpwl() reports. An earlier x-only version let a vertical swap
    /// pass the "improves" test while making the true two-axis HPWL worse, so
    /// a run could report a net loss as a gain.
    [[nodiscard]] double deltaSwap(std::size_t a, double na, double nay, std::size_t b, double nb,
                                   double nby) const;
    /// Can c sit at nx without overlapping a neighbour, a macro, or leaving its
    /// span? The span bounds and the site grid are checked; the neighbour check
    /// is done against the cells sorted by x in the span.
    [[nodiscard]] bool canPlace(std::size_t c, double nx) const;
    /// The x that minimises this cell's own net span lengths: the median of the
    /// interval its nets allow.
    [[nodiscard]] double medianX(std::size_t c) const;
    std::size_t globalSwap();
    std::size_t verticalSwap();
    std::size_t localReorder();
    std::size_t medianReorder();
    std::size_t singleSegmentCluster();
    void selfCheck(DetailPlaceResult &res) const;
    void writeFrame(const std::string &path, const char *note) const;
    /// Fences, carried from the params so the frame writer can draw them.
    const constraintMgr *constraints_ = nullptr;
    void commit(std::size_t c, double nx);
    /// Re-sort a span's cells by x. The neighbour check in canPlace() binary
    /// searches that order, so it has to be restored after every move.
    void resort(std::size_t s);
    std::size_t overlaps(const std::vector<std::size_t> &cells,
                         const std::vector<double> *nx = nullptr) const;
    std::size_t offSites(const std::vector<std::size_t> &cells,
                         const std::vector<double> *nx = nullptr) const;
    double segmentHpwl(const std::vector<std::size_t> &cells,
                       const std::vector<double> *nx = nullptr) const;
    // Ranks two layouts of one span. Legality dominates: a layout with fewer
    // overlaps wins, then one with fewer cells off the site pitch, and only a
    // tie on both is broken by wirelength. Ranking it the other way round would
    // leave a legal span alone but never repair an illegal one.
    static bool betterLayout(std::size_t ovNew, std::size_t offNew, double hpNew, std::size_t ovOld,
                             std::size_t offOld, double hpOld);
    /// Spans in the rows immediately above and below each row, built once.
    std::vector<std::vector<std::size_t>> rowsNear_;
    /// Fixed boxes bucketed by y, so testing a cell against them costs the boxes
    /// in its own row band rather than every macro in the design. The flat scan
    /// this replaces was cells times macros -- 114 million box tests on adaptec1
    /// and about 33 of detail placement's 54 seconds.
    std::vector<std::vector<std::size_t>> fixedByBand_;
    double bandY_ = 0.0, bandY0_ = 0.0;
    /// Cells left untouched because they span rows. Reported, because "the detail
    /// placer did nothing to them" should be visible rather than look like a pass
    /// that found no improvement.
    std::size_t tallSkipped_ = 0;
    /// Index of the first span in each row, so locate() starts at the spans of the
    /// cell's own row instead of at span 0. A cell is in exactly one row, and the
    /// spans are grouped by row, so the candidate set is a handful and the test is
    /// not a walk over the whole design.
    std::vector<std::size_t> spanRowStart_;
    void removeFromSpan(std::size_t c);
    /// Could cell c legally sit at nx in span s, ignoring the cells in `ignore`?
    /// This is checked *before* any mutation so an exchange never has to be
    /// rolled back: the earlier mutate-then-validate version left 11 overlapping
    /// pairs and 67 cells unassigned when a restore did not land exactly.
    [[nodiscard]] bool fitsIgnoring(std::size_t c, double nx, std::size_t s,
                                    const std::size_t *ignore, std::size_t nIgnore) const;
    /// Find the span cell c currently sits in, and re-check bounds, site grid
    /// and macros for it. False if it is not legally placeable anywhere.
    bool locate(std::size_t c);
    std::size_t localWindow_ = 8;

    ktDM &db_;
    Graph &graph_;
    std::vector<std::size_t> mov_;     ///< graph vertex per movable slot
    std::vector<double> w_, h_;        ///< per movable slot
    std::vector<double> x_, y_;        ///< current position
    std::vector<std::size_t> spanOf_;  ///< span index per movable slot
    std::vector<Span> spans_;
    std::vector<std::vector<NetPin>> netPins_;
    /// For each slot, the nets it appears on.
    std::vector<std::vector<std::size_t>> cellNets_;
    std::vector<FixedBox> fixed_;
    BBox die_ = {0.0, 0.0, 0.0, 0.0};
};

void FastDetailedPlacer::Impl::buildSpans() {
    spans_.clear();
    std::vector<RowInfo> rows = db_.getRows();
    std::sort(rows.begin(), rows.end(), [](const RowInfo &a, const RowInfo &b) {
        return a.coordinate < b.coordinate;
    });
    // Free space per row, not the row itself. A span is what is actually empty
    // between the cells that are there now, so every placement lands in a gap and
    // cannot overlap anything by construction. Taking spans from the row
    // description alone was only sound while the rows still described the
    // placement: the multi-row legalizer cuts rows into segments that the db
    // still reports as one full subrow, and on ibm01 that mismatch is what left
    // the detail placer placing two cells on top of each other.
    std::vector<std::vector<std::pair<double, double>>> occupied(rows.size());
    for (std::size_t v = 0; v < graph_.getNumCells(); ++v) {
        const Vertex &vert = graph_.getCell(v);
        if (vert.width <= 0.0 || vert.height <= 0.0) {
            continue;
        }
        // Rows are sorted, so start at the first row whose top reaches past the
        // cell's bottom and walk down only over the rows the cell actually
        // covers. Scanning all rows per cell was quadratic on a design with
        // 210k cells and 700 rows.
        const auto above = std::lower_bound(rows.begin(), rows.end(), vert.y + vert.height,
                                            [](const RowInfo &ri, double limit) {
                                                return ri.coordinate + ri.height < limit;
                                            });
        for (auto it = above; it != rows.begin();) {
            --it;
            if (it->coordinate + it->height <= vert.y + 1e-6) {
                break;  // past the cell's bottom
            }
            occupied[static_cast<std::size_t>(it - rows.begin())].emplace_back(vert.x,
                                                                               vert.x + vert.width);
        }
    }
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const RowInfo &ri = rows[r];
        const double site = ri.pitch() > 0.0 ? ri.pitch() : 1.0;
        std::vector<std::pair<double, double>> &obs = occupied[r];
        std::sort(obs.begin(), obs.end());
        for (const SubrowInfo &si : ri.subrows) {
            for (double lo = si.xlo(); lo < si.xhi(site) - 1e-9;) {
                double hi = si.xhi(site);
                // Each obstacle splits the run in two: stop just short of it, and
                // resume at its far edge.
                for (const auto &o : obs) {
                    if (o.first <= lo + 1e-9 || o.second >= hi - 1e-9) {
                        continue;
                    }
                    hi = std::min(hi, o.first);
                }
                Span sp;
                sp.row = r;
                sp.ylo = ri.coordinate;
                sp.yhi = ri.coordinate + ri.height;
                sp.xlo = lo;
                sp.xhi = hi;
                sp.site = site;
                sp.pitch = ri.height > 0.0 ? ri.height : 1.0;
                if (sp.xhi > sp.xlo + 1e-9) {
                    spans_.push_back(std::move(sp));
                }
                // Resume at the obstacle that cut the run short, so the rest of the
                // row is still visited.
                lo = hi;
            }
        }
    }
    spanOf_.assign(mov_.size(), kNoSlot);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        for (std::size_t s = 0; s < spans_.size(); ++s) {
            const Span &sp = spans_[s];
            if (y_[i] >= sp.ylo - 1e-6 && y_[i] < sp.yhi - 1e-6 && sp.contains(x_[i], w_[i])) {
                spanOf_[i] = s;
                spans_[s].cells.push_back(i);
                break;
            }
        }
    }
    for (Span &sp : spans_) {
        std::sort(sp.cells.begin(), sp.cells.end(), [&](std::size_t a, std::size_t b) {
            return x_[a] < x_[b];
        });
    }

    // Index the spans in the rows immediately above and below each row, so a
    // vertical swap does not have to walk the whole design per cell.
    //
    // Built by walking the spans once and bucketing them by row, rather than by
    // walking every span for every span. The nested version was quadratic in the
    // span count and, worse, appended from inside the outer loop: a row with k
    // spans put the same neighbour into rowsNear_ k times, so the list every
    // vertical swap then walked held each candidate span k times over and the
    // identical trial was re-evaluated k times.
    std::vector<std::vector<std::size_t>> spansInRow(rows.size());
    for (std::size_t t = 0; t < spans_.size(); ++t) {
        spansInRow[spans_[t].row].push_back(t);
    }
    rowsNear_.assign(rows.size(), {});
    for (std::size_t r = 0; r < rows.size(); ++r) {
        if (r > 0) {
            rowsNear_[r].insert(rowsNear_[r].end(), spansInRow[r - 1].begin(),
                                spansInRow[r - 1].end());
        }
        if (r + 1 < rows.size()) {
            rowsNear_[r].insert(rowsNear_[r].end(), spansInRow[r + 1].begin(),
                                spansInRow[r + 1].end());
        }
    }

    // First span of each row, over the row-ordered span list, so locate() can jump
    // to the cell's own row.
    spanRowStart_.assign(rows.size() + 1, spans_.size());
    {
        std::size_t s = 0;
        for (std::size_t r = 0; r < rows.size(); ++r) {
            spanRowStart_[r] = s;
            while (s < spans_.size() && spans_[s].row == r) {
                ++s;
            }
        }
        spanRowStart_[rows.size()] = spans_.size();
    }

    // Fixed boxes by y band, one row pitch tall, so a cell only meets the macros
    // that can actually reach it.
    bandY_ = rows.empty() ? 1.0 : std::max(1e-9, rows[0].height);
    bandY0_ = rows.empty() ? 0.0 : rows.front().coordinate;
    const std::size_t nBands = rows.empty() ? 1 : rows.size() + 2;
    fixedByBand_.assign(nBands, {});
    for (std::size_t i = 0; i < fixed_.size(); ++i) {
        const FixedBox &f = fixed_[i];
        long b0 = static_cast<long>(std::floor((f.y0 - bandY0_) / bandY_));
        long b1 = static_cast<long>(std::floor((f.y1 - bandY0_) / bandY_));
        b0 = std::max<long>(b0, 0);
        b1 = std::min<long>(b1, static_cast<long>(nBands) - 1);
        for (long b = b0; b <= b1; ++b) {
            fixedByBand_[static_cast<std::size_t>(b)].push_back(i);
        }
    }
}

bool FastDetailedPlacer::Impl::fitsIgnoring(std::size_t c, double nx, std::size_t s,
                                            const std::size_t *ignore, std::size_t nIgnore) const {
    const Span &sp = spans_[s];
    if (!sp.contains(nx, w_[c])) {
        return false;
    }
    if (sp.site > 0.0) {
        const double q = nx / sp.site;
        if (std::fabs(q - std::round(q)) * sp.site > 1e-6 * std::max(1.0, std::fabs(nx))) {
            return false;
        }
    }
    const std::vector<std::size_t> &cells = sp.cells;
    const auto it = std::lower_bound(cells.begin(), cells.end(), nx, [&](std::size_t a, double v) {
        return x_[a] < v;
    });
    for (auto k = it; k != cells.end() && x_[*k] < nx + w_[c] - 1e-6; ++k) {
        if (std::find(ignore, ignore + nIgnore, *k) == ignore + nIgnore) {
            return false;
        }
    }
    if (it != cells.begin()) {
        const std::size_t prev = *std::prev(it);
        if (x_[prev] + w_[prev] > nx + 1e-6 &&
            std::find(ignore, ignore + nIgnore, prev) == ignore + nIgnore) {
            return false;
        }
    }
    for (const FixedBox &f : fixed_) {
        if (nx + w_[c] > f.x0 + 1e-6 && nx < f.x1 - 1e-6 && y_[c] + h_[c] > f.y0 + 1e-6 &&
            y_[c] < f.y1 - 1e-6) {
            return false;
        }
    }
    return true;
}

void FastDetailedPlacer::Impl::removeFromSpan(std::size_t c) {
    const std::size_t s = spanOf_[c];
    if (s == kNoSlot) {
        return;
    }
    std::vector<std::size_t> &cells = spans_[s].cells;
    cells.erase(std::remove(cells.begin(), cells.end(), c), cells.end());
    spanOf_[c] = kNoSlot;
}

void FastDetailedPlacer::Impl::buildNetlist() {
    std::vector<std::size_t> slotOf(graph_.getNumCells(), kNoSlot);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        slotOf[mov_[i]] = i;
    }
    netPins_.assign(graph_.getNumNets(), {});
    cellNets_.assign(mov_.size(), {});
    for (std::size_t n = 0; n < graph_.getNumNets(); ++n) {
        for (const std::size_t pinId : graph_.getNetPins(n)) {
            const Pin &pin = graph_.getPin(pinId);
            const std::size_t s = slotOf[pin.cellId];
            const Vertex &c = graph_.getCell(pin.cellId);
            if (s == kNoSlot) {
                netPins_[n].push_back(NetPin{kNoSlot, pin.offsetX, pin.offsetY, c.x + pin.offsetX});
            } else {
                netPins_[n].push_back(NetPin{s, pin.offsetX, pin.offsetY, 0.0});
                cellNets_[s].push_back(n);
            }
        }
    }
}

double FastDetailedPlacer::Impl::hpwl() const {
    double total = 0.0;
    for (std::size_t v = 0; v < netPins_.size(); ++v) {
        const std::vector<NetPin> &pins = netPins_[v];
        if (pins.size() < 2) {
            continue;
        }
        double ax = std::numeric_limits<double>::max();
        double bx = -std::numeric_limits<double>::max();
        double ay = std::numeric_limits<double>::max();
        double by = -std::numeric_limits<double>::max();
        bool anyMovable = false;
        for (const NetPin &p : pins) {
            const double px = (p.slot == kNoSlot) ? p.absX : (x_[p.slot] + p.offX);
            ax = std::min(ax, px);
            bx = std::max(bx, px);
            if (p.slot != kNoSlot) {
                anyMovable = true;
                // The pin's own y, not the cell's box. HPWL is a wirelength between
                // pins, and the .nets offsets are up to 1420 units against a
                // 12-unit row: taking the cell's bottom and top edges instead
                // inflates every net's vertical extent by up to a full cell
                // height, and the result is not the quantity SimPL and the
                // legalizer report. deltaSwap() already used the pin's y, so the
                // two disagreed with each other as well.
                ay = std::min(ay, y_[p.slot] + p.offY);
                by = std::max(by, y_[p.slot] + p.offY);
            }
        }
        // Both axes: the legalizer measures HPWL the same way, so the two stages
        // have to agree or the reported change is meaningless. A net whose pins
        // are all fixed has no y extent to speak of, and leaving the sentinels in
        // place made the total -inf.
        if (!anyMovable) {
            ay = 0.0;
            by = 0.0;
        }
        total += (bx - ax) + (by - ay);
    }
    return total;
}

double FastDetailedPlacer::Impl::deltaMove(std::size_t c, double nx) const {
    const double old = x_[c];
    if (std::fabs(old - nx) < 1e-12) {
        return 0.0;
    }
    // Only the nets this cell is on can change, and the new x of this one cell
    // is the only unknown, so each affected net is re-bounded directly.
    double delta = 0.0;
    for (const std::size_t n : cellNets_[c]) {
        const std::vector<NetPin> &pins = netPins_[n];
        double ax = std::numeric_limits<double>::max();
        double bx = -std::numeric_limits<double>::max();
        for (const NetPin &p : pins) {
            double px;
            if (p.slot == kNoSlot) {
                px = p.absX;
            } else if (p.slot == c) {
                px = nx + p.offX;
            } else {
                px = x_[p.slot] + p.offX;
            }
            ax = std::min(ax, px);
            bx = std::max(bx, px);
        }
        delta += (bx - ax);
    }
    // Subtract the old contribution of the same nets.
    for (const std::size_t n : cellNets_[c]) {
        const std::vector<NetPin> &pins = netPins_[n];
        double ax = std::numeric_limits<double>::max();
        double bx = -std::numeric_limits<double>::max();
        for (const NetPin &p : pins) {
            const double px = (p.slot == kNoSlot) ? p.absX : (x_[p.slot] + p.offX);
            ax = std::min(ax, px);
            bx = std::max(bx, px);
        }
        delta -= (bx - ax);
    }
    return delta;
}

double FastDetailedPlacer::Impl::deltaSwap(std::size_t a, double na, double nay, std::size_t b,
                                           double nb, double nby) const {
    // Union of the nets of both cells, deduplicated.
    std::vector<std::size_t> nets = cellNets_[a];
    nets.insert(nets.end(), cellNets_[b].begin(), cellNets_[b].end());
    std::sort(nets.begin(), nets.end());
    nets.erase(std::unique(nets.begin(), nets.end()), nets.end());

    // Per-pin coordinates after and before the exchange. Only a and b move, so
    // every other pin keeps its current position. A fixed pin has no y: hpwl()
    // takes the y extent from movable pins only, and this has to agree with that
    // term for term or the comparison is against a different quantity than the
    // one reported.
    const auto pinX = [&](const NetPin &p, double ax, double bx) {
        if (p.slot == kNoSlot) {
            return p.absX;
        }
        if (p.slot == a) {
            return ax + p.offX;
        }
        if (p.slot == b) {
            return bx + p.offX;
        }
        return x_[p.slot] + p.offX;
    };
    const auto pinY = [&](const NetPin &p, double ay, double by) {
        if (p.slot == kNoSlot) {
            return 0.0;
        }
        if (p.slot == a) {
            return ay + p.offY;
        }
        if (p.slot == b) {
            return by + p.offY;
        }
        return y_[p.slot] + p.offY;
    };

    // Accumulated as new-minus-old over every affected net, so a negative total
    // means the exchange shortens the wirelength.
    double delta = 0.0;
    for (const std::size_t n : nets) {
        double nxlo = std::numeric_limits<double>::max();
        double nxhi = -std::numeric_limits<double>::max();
        double oxlo = std::numeric_limits<double>::max();
        double oxhi = -std::numeric_limits<double>::max();
        double nylo = std::numeric_limits<double>::max();
        double nyhi = -std::numeric_limits<double>::max();
        double oylo = std::numeric_limits<double>::max();
        double oyhi = -std::numeric_limits<double>::max();
        for (const NetPin &p : netPins_[n]) {
            const double npx = pinX(p, na, nb);
            const double opx = pinX(p, x_[a], x_[b]);
            nxlo = std::min(nxlo, npx);
            nxhi = std::max(nxhi, npx);
            oxlo = std::min(oxlo, opx);
            oxhi = std::max(oxhi, opx);
            if (p.slot == kNoSlot) {
                continue;
            }
            const double npy = pinY(p, nay, nby);
            const double opy = pinY(p, y_[a], y_[b]);
            const double h = h_[p.slot];
            nylo = std::min(nylo, npy);
            nyhi = std::max(nyhi, npy + h);
            oylo = std::min(oylo, opy);
            oyhi = std::max(oyhi, opy + h);
        }
        // A net whose pins are all fixed has no y extent to speak of, and the
        // sentinels would otherwise make the term -inf. hpwl() zeroes it too.
        if (nylo == std::numeric_limits<double>::max()) {
            nylo = nyhi = oylo = oyhi = 0.0;
        }
        delta += (nxhi - nxlo) + (nyhi - nylo);
        delta -= (oxhi - oxlo) + (oyhi - oylo);
    }
    return delta;
}

double FastDetailedPlacer::Impl::medianX(std::size_t c) const {
    // Collect the interval each net allows this cell to move in, then take the
    // median of the interval endpoints. This is the classic median move: it is
    // the x that minimises the sum of this cell's own net spans.
    std::vector<double> los, his;
    los.reserve(cellNets_[c].size());
    his.reserve(cellNets_[c].size());
    for (const std::size_t n : cellNets_[c]) {
        double ax = std::numeric_limits<double>::max();
        double bx = -std::numeric_limits<double>::max();
        double moff = 0.0;
        bool has = false;
        for (const NetPin &p : netPins_[n]) {
            const double px = (p.slot == kNoSlot) ? p.absX : (x_[p.slot] + p.offX);
            if (p.slot == c) {
                moff = p.offX;
                has = true;
            }
            ax = std::min(ax, px);
            bx = std::max(bx, px);
        }
        if (!has) {
            continue;
        }
        // Keep the pin inside the other pins' span: the cell's left edge may run
        // from (min - off) to (max - off).
        los.push_back(ax - moff);
        his.push_back(bx - moff);
    }
    if (los.empty()) {
        return x_[c];
    }
    std::vector<double> mids;
    mids.reserve(los.size());
    for (std::size_t i = 0; i < los.size(); ++i) {
        mids.push_back(0.5 * (los[i] + his[i]));
    }
    std::nth_element(mids.begin(), mids.begin() + static_cast<long>(mids.size() / 2), mids.end());
    return mids[mids.size() / 2];
}

bool FastDetailedPlacer::Impl::canPlace(std::size_t c, double nx) const {
    const std::size_t s = spanOf_[c];
    if (s == kNoSlot) {
        return false;
    }
    const Span &sp = spans_[s];
    if (!sp.contains(nx, w_[c])) {
        return false;
    }
    if (sp.site > 0.0) {
        const double q = nx / sp.site;
        if (std::fabs(q - std::round(q)) * sp.site > 1e-6 * std::max(1.0, std::fabs(nx))) {
            return false;
        }
    }
    // Neighbours: the span's cells are sorted by x, so only the ones bracketing
    // nx can overlap it. Scanning the whole span would be quadratic overall.
    const std::vector<std::size_t> &cells = sp.cells;
    const auto it = std::lower_bound(cells.begin(), cells.end(), nx, [&](std::size_t a, double v) {
        return x_[a] < v;
    });
    if (it != cells.end() && *it != c && x_[*it] < nx + w_[c] - 1e-6) {
        return false;
    }
    if (it != cells.begin()) {
        const std::size_t prev = *std::prev(it);
        if (prev != c && x_[prev] + w_[prev] > nx + 1e-6) {
            return false;
        }
    }
    // Macros.
    for (const FixedBox &f : fixed_) {
        if (nx + w_[c] > f.x0 + 1e-6 && nx < f.x1 - 1e-6) {
            const double cy0 = y_[c], cy1 = y_[c] + h_[c];
            if (cy1 > f.y0 + 1e-6 && cy0 < f.y1 - 1e-6) {
                return false;
            }
        }
    }
    return true;
}

void FastDetailedPlacer::Impl::commit(std::size_t c, double nx) {
    x_[c] = nx;
}

bool FastDetailedPlacer::Impl::locate(std::size_t c) {
    spanOf_[c] = kNoSlot;
    for (std::size_t s = 0; s < spans_.size(); ++s) {
        const Span &sp = spans_[s];
        if (y_[c] < sp.ylo - 1e-6 || y_[c] >= sp.yhi - 1e-6) {
            continue;
        }
        if (!sp.contains(x_[c], w_[c])) {
            continue;
        }
        const double q = x_[c] / sp.site;
        if (std::fabs(q - std::round(q)) * sp.site > 1e-6 * std::max(1.0, std::fabs(x_[c]))) {
            continue;
        }
        bool blocked = false;
        for (const FixedBox &f : fixed_) {
            if (x_[c] + w_[c] > f.x0 + 1e-6 && x_[c] < f.x1 - 1e-6 && y_[c] + h_[c] > f.y0 + 1e-6 &&
                y_[c] < f.y1 - 1e-6) {
                blocked = true;
                break;
            }
        }
        if (blocked) {
            continue;
        }
        // Overlap against the span's other cells. Without this a vertical swap
        // could land a cell on top of a third cell: locate() checked bounds, the
        // site grid and the macros, but not its neighbours, and that left 67298
        // overlapping pairs. The span's cells are sorted by x, so only the ones
        // bracketing this position can overlap it.
        const std::vector<std::size_t> &others = spans_[s].cells;
        const auto it =
            std::lower_bound(others.begin(), others.end(), x_[c], [&](std::size_t a, double v) {
                return x_[a] < v;
            });
        if (it != others.end() && x_[*it] < x_[c] + w_[c] - 1e-6) {
            continue;
        }
        if (it != others.begin()) {
            const std::size_t prev = *std::prev(it);
            if (x_[prev] + w_[prev] > x_[c] + 1e-6) {
                continue;
            }
        }
        spanOf_[c] = s;
        spans_[s].cells.push_back(c);
        return true;
    }
    return false;
}

// How many adjacent pairs in a span overlap. nx overrides the positions, which
// is how a candidate layout is scored before it is committed.
std::size_t FastDetailedPlacer::Impl::overlaps(const std::vector<std::size_t> &cells,
                                               const std::vector<double> *nx) const {
    std::size_t bad = 0;
    for (std::size_t k = 1; k < cells.size(); ++k) {
        const double a = nx ? (*nx)[k - 1] : x_[cells[k - 1]];
        const double b = nx ? (*nx)[k] : x_[cells[k]];
        if (a + w_[cells[k - 1]] > b + 1e-6) {
            ++bad;
        }
    }
    return bad;
}

// How many cells of a span are not on the site pitch. nx overrides positions.
std::size_t FastDetailedPlacer::Impl::offSites(const std::vector<std::size_t> &cells,
                                               const std::vector<double> *nx) const {
    const double site = spans_.empty() ? 1.0 : spans_[0].site;
    std::size_t bad = 0;
    for (std::size_t k = 0; k < cells.size(); ++k) {
        const double v = nx ? (*nx)[k] : x_[cells[k]];
        const double q = v / site;
        if (std::fabs(q - std::round(q)) * site > 1e-6 * std::max(1.0, std::fabs(v))) {
            ++bad;
        }
    }
    return bad;
}

bool FastDetailedPlacer::Impl::betterLayout(std::size_t ovNew, std::size_t offNew, double hpNew,
                                            std::size_t ovOld, std::size_t offOld, double hpOld) {
    if (ovNew != ovOld) {
        return ovNew < ovOld;
    }
    if (offNew != offOld) {
        return offNew < offOld;
    }
    return hpNew < hpOld - 1e-9;
}

// The x-extent HPWL of the nets that only these cells touch. Used to compare two
// layouts of the same span, where the rest of the design is identical and
// cancels, so only the x term is needed to rank them.
double FastDetailedPlacer::Impl::segmentHpwl(const std::vector<std::size_t> &cells,
                                             const std::vector<double> *nx) const {
    std::vector<std::size_t> nets;
    for (const std::size_t c : cells) {
        nets.insert(nets.end(), cellNets_[c].begin(), cellNets_[c].end());
    }
    std::sort(nets.begin(), nets.end());
    nets.erase(std::unique(nets.begin(), nets.end()), nets.end());

    // In the candidate layout only the cells of this span move, so each one's
    // index within the span is all that is needed to find its new x.
    std::vector<std::size_t> slotInSpan(mov_.size(), kNoSlot);
    if (nx != nullptr) {
        for (std::size_t k = 0; k < cells.size(); ++k) {
            slotInSpan[cells[k]] = k;
        }
    }

    double total = 0.0;
    for (const std::size_t n : nets) {
        double lo = std::numeric_limits<double>::max();
        double hi = -std::numeric_limits<double>::max();
        for (const NetPin &p : netPins_[n]) {
            double v;
            if (p.slot == kNoSlot) {
                v = p.absX;
            } else if (nx != nullptr && slotInSpan[p.slot] != kNoSlot) {
                v = (*nx)[slotInSpan[p.slot]] + p.offX;
            } else {
                v = x_[p.slot] + p.offX;
            }
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        total += hi - lo;
    }
    return total;
}

void FastDetailedPlacer::Impl::resort(std::size_t s) {
    std::vector<std::size_t> &cells = spans_[s].cells;
    std::sort(cells.begin(), cells.end(), [&](std::size_t a, std::size_t b) {
        return x_[a] < x_[b];
    });
}

std::size_t FastDetailedPlacer::Impl::globalSwap() {
    std::size_t moves = 0;
    for (std::size_t c = 0; c < mov_.size(); ++c) {
        const std::size_t s = spanOf_[c];
        if (s == kNoSlot) {
            continue;
        }
        const double want = medianX(c);
        if (!canPlace(c, want)) {
            continue;
        }
        if (deltaMove(c, want) < -1e-9) {
            commit(c, want);
            resort(s);
            ++moves;
        }
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::verticalSwap() {
    // Exchange a cell with one in an adjacent row. Only spans in the rows
    // immediately above and below are considered: walking every span in the
    // design for every cell made this quadratic in the design and effectively
    // hung on adaptec1. Within a candidate span only the cells bracketing the
    // partner's x are tried, since a swap that is not x-adjacent cannot pay.
    std::size_t moves = 0;
    std::vector<std::size_t> order(mov_.size());
    std::iota(order.begin(), order.end(), 0u);

    for (const std::size_t c : order) {
        const std::size_t sc = spanOf_[c];
        if (sc == kNoSlot) {
            continue;
        }
        const Span &spc = spans_[sc];
        for (const std::size_t s2 : rowsNear_[spc.row]) {
            if (s2 == sc) {
                continue;
            }
            Span &sp2 = spans_[s2];
            // Nearest cells in x on the other side, in both directions.
            const std::vector<std::size_t> &oc = sp2.cells;
            if (oc.empty()) {
                continue;
            }
            const auto at =
                std::lower_bound(oc.begin(), oc.end(), x_[c], [&](std::size_t a, double v) {
                    return x_[a] < v;
                });
            const std::size_t cands[2] = {
                (at == oc.end()) ? kNoSlot : *at,
                (at == oc.begin()) ? kNoSlot : *std::prev(at),
            };
            for (const std::size_t d : cands) {
                if (d == kNoSlot) {
                    continue;
                }
                // Validate the whole exchange before touching anything.
                const std::size_t ignore[2] = {c, d};
                const double ocx = x_[c], odx = x_[d];
                if (!fitsIgnoring(c, odx, s2, ignore, 2) || !fitsIgnoring(d, ocx, sc, ignore, 2)) {
                    continue;
                }
                // The two cells change row as well as column, so the y each one
                // lands at is part of the decision. Judging on x alone let a
                // swap through that lengthened the real wirelength.
                const double ncy = spans_[s2].ylo;
                const double ndy = spans_[sc].ylo;
                if (deltaSwap(c, odx, ncy, d, ocx, ndy) >= -1e-9) {
                    continue;
                }
                // Clear the macro test for the y each one lands at.
                //
                // The trial rewrites y_ and must undo it exactly on failure. It
                // used to restore from the spans captured at the top of the cell's
                // iteration (sc / s2), which was only right while the cell had not
                // moved yet: once the pass had already swapped this cell into
                // another row, a later failed trial put y_ back at its *original*
                // row while spanOf_ still pointed at the row it was swapped into.
                // y_ and spanOf_ then disagreed -- a cell whose geometry sat in one
                // row's band while the reasoner held it in the row next door, so
                // the per-span legality sweep could not see the overlap it created
                // with a cell in the row it physically occupied. On adaptec1 that
                // reproduced as o87670 sitting inside o169225 (see the selfCheck
                // code), with brute-force and per-span overlap counts disagreeing.
                // Snapshotting the real values and restoring them is exact in both
                // cases: a cell with no prior move goes back to where it was, and
                // one that was already swapped this iteration goes back to the row
                // the successful swap landed it in.
                const double oyc = y_[c];
                const double oyd = y_[d];
                y_[c] = spans_[s2].ylo;
                y_[d] = spans_[sc].ylo;
                if (!fitsIgnoring(c, odx, s2, ignore, 2) || !fitsIgnoring(d, ocx, sc, ignore, 2)) {
                    y_[c] = oyc;
                    y_[d] = oyd;
                    continue;
                }
                removeFromSpan(c);
                removeFromSpan(d);
                commit(c, odx);
                commit(d, ocx);
                spanOf_[c] = s2;
                spanOf_[d] = sc;
                spans_[s2].cells.push_back(c);
                spans_[sc].cells.push_back(d);
                resort(s2);
                resort(sc);
                ++moves;
                break;
            }
        }
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::medianReorder() {
    // The classic median move, over every span: for each cell, take the x that
    // minimises its own net spans and move it there if the span still fits it.
    //
    // This is what the exhaustive window search in localReorder() is trying to
    // approximate, at a fraction of the cost. localReorder enumerates all 2^k
    // orderings of a k=12 cell window and evaluates segmentHpwl() for each, which
    // on adaptec1 is about 3.6e9 evaluations spread over 17575 windows; it
    // returns roughly 31000 moves. Here each cell costs one median and one
    // legality check, so the same ground is covered in time proportional to
    // cells times net degree.
    std::size_t moves = 0;
    for (std::size_t s = 0; s < spans_.size(); ++s) {
        Span &sp = spans_[s];
        if (sp.cells.size() < 2) {
            continue;
        }
        // The pass walks a SNAPSHOT of the span's cells, not the span itself.
        //
        // It used to index sp.cells directly, while each accepted move called
        // resort() and re-sorted that same vector. The index therefore referred to
        // a different cell after every move, so the pass was not the left-to-right
        // sweep it claimed to be: it revisited cells it had already handled and
        // skipped others, in an order that changed underneath it. The result was a
        // placement with an overlapping pair -- o87670 sitting five units inside
        // o169225, both in one row -- and the run reported FAIL on adaptec1 at the
        // 64-cell-per-bin grid.
        //
        // The legality gate was not the problem: canPlace() was consulted before
        // every commit, and it does reject a move onto a neighbour. Walking a
        // container that the loop mutates is simply not a defined traversal, and
        // nothing downstream can be trusted once the pass has done it.
        //
        // One pass left to right over the snapshot, then one right to left: a
        // cell's median depends on where its neighbours in the span are, so a
        // single sweep leaves every move made early in the pass stale for the cells
        // after it.
        const std::vector<std::size_t> snapshot = sp.cells;
        for (int dir = 0; dir < 2; ++dir) {
            for (std::size_t i = 0; i < snapshot.size(); ++i) {
                const std::size_t c = snapshot[dir ? snapshot.size() - 1 - i : i];
                if (spanOf_[c] != s) {
                    continue;  // a cell that has since been rehomed
                }
                const double cur = x_[c];
                const double want = std::clamp(medianX(c), sp.xlo, sp.xhi);
                if (std::fabs(want - cur) < 1e-9) {
                    continue;
                }
                if (!canPlace(c, want)) {
                    continue;
                }
                const double before = segmentHpwl(sp.cells);
                if (!canPlace(c, cur)) {
                    continue;  // the median is blocked; leave the cell where it is
                }
                commit(c, want);
                const double after = segmentHpwl(sp.cells);
                if (after < before - 1e-9) {
                    ++moves;
                } else {
                    commit(c, cur);
                }
                resort(s);
            }
        }
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::localReorder() {
    // For each window of consecutive cells in a span, find the best left-to-right
    // ordering exactly. With the order fixed and the cells packed from the
    // window's left edge, each ordering is scored by the resulting positions, and
    // a subset dynamic program over 2^k states finds the optimum.
    std::size_t moves = 0;
    const std::size_t k = std::min<std::size_t>(localWindow_, 12);
    if (k < 2) {
        return 0;
    }
    for (Span &sp : spans_) {
        const std::size_t n = sp.cells.size();
        if (n < 2) {
            continue;
        }
        for (std::size_t start = 0; start + 1 < n; start += k) {
            const std::size_t len = std::min(k, n - start);
            if (len < 2) {
                break;
            }
            const std::size_t states = static_cast<std::size_t>(1) << len;
            std::vector<double> cost(states, std::numeric_limits<double>::infinity());
            std::vector<std::size_t> last(states, kNoSlot);
            cost[0] = 0.0;
            const double base = std::floor(x_[sp.cells[start]] / sp.site) * sp.site;
            for (std::size_t mask = 0; mask < states; ++mask) {
                if (!std::isfinite(cost[mask])) {
                    continue;
                }
                // The window's left edge is where the cells already are. Packing
                // from the subrow's left edge instead explores orderings that are
                // all worse than the current one, so no improvement is ever found.
                double cursor = base;
                // Rebuild the cursor for this mask by summing the widths already
                // placed, which is what the packing constraint is.
                for (std::size_t b = 0; b < len; ++b) {
                    if (mask & (static_cast<std::size_t>(1) << b)) {
                        cursor += w_[sp.cells[start + b]];
                    }
                }
                for (std::size_t b = 0; b < len; ++b) {
                    const std::size_t bit = static_cast<std::size_t>(1) << b;
                    if (mask & bit) {
                        continue;
                    }
                    const std::size_t c = sp.cells[start + b];
                    const double nx = cursor;
                    if (nx + w_[c] > sp.xhi + 1e-6) {
                        continue;
                    }
                    // The subrow's right edge is the only thing the packing bound
                    // above checks, so a cell can be planned onto a macro or on
                    // top of the cell just outside the window. The other passes go
                    // through fitsIgnoring, which tests both; skipping it here is
                    // what put 9879 cells over macros on adaptec1. The whole
                    // window is ignored, since the packing constraint is what
                    // keeps the window's own cells apart and they are not all
                    // where they are now.
                    const std::size_t sIdx = static_cast<std::size_t>(&sp - spans_.data());
                    if (!fitsIgnoring(c, nx, sIdx, sp.cells.data() + start, len)) {
                        continue;
                    }
                    const double cand = cost[mask] + deltaMove(c, nx);
                    const std::size_t nm = mask | bit;
                    if (cand < cost[nm]) {
                        cost[nm] = cand;
                        last[nm] = c;
                    }
                }
            }
            const std::size_t all = states - 1;
            if (!std::isfinite(cost[all]) || cost[all] >= -1e-9) {
                continue;  // no ordering improves on the current one
            }
            // Walk the chain back to recover the winning order, then apply it.
            std::vector<std::size_t> order;
            std::size_t mask = all;
            while (mask != 0) {
                const std::size_t c = last[mask];
                if (c == kNoSlot) {
                    order.clear();
                    break;
                }
                order.push_back(c);
                std::size_t bit = 0;
                while (c != sp.cells[start + bit]) {
                    ++bit;
                }
                mask ^= static_cast<std::size_t>(1) << bit;
            }
            if (order.size() != len) {
                continue;
            }
            std::reverse(order.begin(), order.end());
            // The dynamic program scores each cell against the layout it started
            // from, so the order it picks can come out worse once the moves are
            // actually applied. The winner is therefore committed, measured, and
            // rolled back to the positions the window started at if the real
            // wirelength did not improve. Re-packing the new order would not
            // restore anything.
            const double hpBefore = segmentHpwl(sp.cells);
            std::vector<double> keep(n);
            for (std::size_t idx = 0; idx < n; ++idx) {
                keep[idx] = x_[sp.cells[idx]];
            }
            double cursor = base;
            std::size_t moved = 0;
            for (const std::size_t c : order) {
                const double nx = cursor;
                if (std::fabs(x_[c] - nx) > 1e-9) {
                    commit(c, nx);
                    ++moved;
                }
                cursor = nx + w_[c];
            }
            if (segmentHpwl(sp.cells) < hpBefore - 1e-9) {
                moves += moved;
            } else {
                for (std::size_t idx = 0; idx < n; ++idx) {
                    commit(sp.cells[idx], keep[idx]);
                }
            }
            // The window came out in a new left-to-right order, so the span's
            // cell list no longer matches the x order. Everything downstream
            // assumes a span is sorted by x, and selfCheck reported a correct
            // reordering as a set of overlaps until this was added.
            resort(static_cast<std::size_t>(&sp - spans_.data()));
        }
    }
    return moves;
}

std::size_t FastDetailedPlacer::Impl::singleSegmentCluster() {
    // With the left-to-right order fixed, re-place each span with the
    // legalizer's greedy cluster pass: every cell goes to the site nearest its
    // current x that does not overlap its predecessor. Cells that end up on the
    // same site as a neighbour are pulled apart, which is where the wirelength
    // comes from.
    std::size_t moves = 0;
    for (std::size_t s = 0; s < spans_.size(); ++s) {
        const std::size_t n = spans_[s].cells.size();
        if (n < 2) {
            continue;
        }
        std::vector<double> nx(n, 0.0);
        double cursor = spans_[s].xlo;
        bool ok = true;
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t c = spans_[s].cells[k];
            double want = std::max(x_[c], cursor);
            want = std::round(want / spans_[s].site) * spans_[s].site;
            if (want + w_[c] > spans_[s].xhi + 1e-6) {
                ok = false;
                break;
            }
            nx[k] = want;
            cursor = want + w_[c];
        }
        if (!ok) {
            continue;
        }
        // Pulling overlapping cells apart is the point of this pass, and it
        // necessarily lengthens the nets between them, so legality has to be
        // ranked above wirelength or an illegal span is never repaired.
        if (betterLayout(overlaps(spans_[s].cells, &nx), offSites(spans_[s].cells, &nx),
                         segmentHpwl(spans_[s].cells, &nx), overlaps(spans_[s].cells),
                         offSites(spans_[s].cells), segmentHpwl(spans_[s].cells))) {
            for (std::size_t k = 0; k < n; ++k) {
                const std::size_t c = spans_[s].cells[k];
                if (std::fabs(x_[c] - nx[k]) > 1e-9) {
                    commit(c, nx[k]);
                    ++moves;
                }
            }
            // The pass keeps the order, so the span is still sorted by x.
            resort(s);
        }
    }
    return moves;
}

void FastDetailedPlacer::Impl::writeFrame(const std::string &path, const char *note) const {
    const std::size_t nv = graph_.getNumCells();
    std::vector<float> fx(nv), fy(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        fx[v] = static_cast<float>(graph_.getCell(v).x);
        fy[v] = static_cast<float>(graph_.getCell(v).y);
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        fx[mov_[i]] = static_cast<float>(x_[i]);
        fy[mov_[i]] = static_cast<float>(y_[i]);
    }
    writeFrameSvg(path, graph_, fx, fy, die_, 0, 1, hpwl(), 0.0, 0.0, note, constraints_,
                  /*fixedView=*/true);
    // Into the run's animation as well, so detailed placement's contribution --
    // usually the last thing that moves cells -- is in the GIF too.
    PlacementAnimator::instance().record(graph_, fx, fy, die_, 0, 1, hpwl(), hpwl(), 0.0, note,
                                         constraints_);
}

void FastDetailedPlacer::Impl::selfCheck(DetailPlaceResult &res) const {
    const double eps = 1e-6;
    res.overlappingPairs = 0;
    res.offRow = 0;
    res.offSite = 0;
    res.overFixed = 0;
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        if (spanOf_[i] == kNoSlot) {
            ++res.offRow;
            continue;
        }
        const Span &sp = spans_[spanOf_[i]];
        if (!sp.contains(x_[i], w_[i])) {
            ++res.offRow;
            continue;
        }
        const double q = x_[i] / sp.site;
        if (std::fabs(q - std::round(q)) * sp.site > 1e-6 * std::max(1.0, std::fabs(x_[i]))) {
            ++res.offSite;
        }
    }
    // Sorted by x before the sweep, rather than trusted to be.
    //
    // This walks consecutive pairs, which is only correct if the span's cell list
    // is in x order -- and that is an invariant of the passes, not a property of
    // the data. Any technique that commits a move without re-sorting breaks it, and
    // the check then under-counts instead of reporting the fault: on adaptec1 both
    // the legalizer and this placer reported zero overlapping pairs while the
    // independent check found o87670 sitting inside o169225. A self-check that
    // reports a clean bill of health for an illegal placement is worse than no
    // check, because it is the number the run is judged on.
    // A second, brute-force count straight off the cell arrays, sorted by row and
    // then by x. It shares no code and no invariants with the per-span sweep above,
    // so if the two disagree the span view is the thing that is wrong, and which of
    // the two is telling the truth becomes a measurement instead of an argument.
    {
        std::vector<std::size_t> all(mov_.size());
        std::iota(all.begin(), all.end(), 0u);
        std::sort(all.begin(), all.end(), [&](std::size_t a, std::size_t b) {
            if (y_[a] != y_[b]) {
                return y_[a] < y_[b];
            }
            return x_[a] < x_[b];
        });
        std::size_t brute = 0;
        for (std::size_t k = 1; k < all.size(); ++k) {
            const std::size_t a = all[k - 1], b = all[k];
            if (y_[a] == y_[b] && x_[a] + w_[a] > x_[b] + 1e-6) {
                ++brute;
            }
        }
        ktlog.echo("  detail-place overlap counts: brute-force {}, per-span {} (spans {})", brute,
                   res.overlappingPairs, spans_.size());
        if (brute != res.overlappingPairs) {
            ktlog.warning(
                "the per-span overlap sweep and a brute-force sweep over the cell "
                "arrays disagree ({} vs {}): the spans do not partition the cells "
                "the way the sweep assumes",
                brute, res.overlappingPairs);
        }
        res.overlappingPairs = std::max(res.overlappingPairs, brute);
    }
    std::vector<std::size_t> byX;
    for (const Span &sp : spans_) {
        if (sp.cells.size() < 2) {
            continue;
        }
        byX = sp.cells;
        std::sort(byX.begin(), byX.end(), [&](std::size_t a, std::size_t b) {
            return x_[a] < x_[b];
        });
        for (std::size_t k = 1; k < byX.size(); ++k) {
            const std::size_t a = byX[k - 1], b = byX[k];
            if (x_[a] + w_[a] > x_[b] + eps) {
                ++res.overlappingPairs;
            }
        }
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const long b = std::clamp<long>(static_cast<long>(std::floor((y_[i] - bandY0_) / bandY_)),
                                        0, static_cast<long>(fixedByBand_.size()) - 1);
        for (const std::size_t fi : fixedByBand_[static_cast<std::size_t>(b)]) {
            const FixedBox &f = fixed_[fi];
            if (x_[i] + w_[i] > f.x0 + eps && x_[i] < f.x1 - eps && y_[i] + h_[i] > f.y0 + eps &&
                y_[i] < f.y1 - eps) {
                ++res.overFixed;
                break;
            }
        }
    }
}

DetailPlaceResult FastDetailedPlacer::Impl::place(const DetailPlaceParams &params) {
    DetailPlaceResult res;
    ScopedTimer timer("detail-place");
    localWindow_ = params.localReorderWindow;
    // The subset DP costs 2^k per window, so the window is the one knob that sets
    // the price of re-ordering. Measured on adaptec1: k=8 costs about 36 of 52
    // seconds for a few tenths of a percent. Overridable so the curve can be
    // walked without a rebuild.
    if (const char *e = std::getenv("KTPLACE_DP_WINDOW")) {
        const long v = std::strtol(e, nullptr, 10);
        if (v >= 2 && v <= 14) {
            localWindow_ = static_cast<std::size_t>(v);
        }
    }
    constraints_ = &db_.constraints();

    // place() is a normal call, not a one-shot: the same object may be run
    // again with different parameters. The per-run accumulators below append
    // rather than assign, so without this a second call doubled mov_ and every
    // other vector, and the placement ended up reported as heavily overlapping.
    // The snapshots inside are the source of truth for a run, so clearing them
    // is enough; nothing carries across runs except the DB, which place()
    // writes back at the end.
    mov_.clear();
    w_.clear();
    h_.clear();
    x_.clear();
    y_.clear();
    fixed_.clear();
    netPins_.clear();
    die_ = BBox{};

    // The shortest row, measured here rather than taken from bandY_: bandY_ is
    // set by the fixed-box pass, which runs later, so reading it here skipped
    // every cell in the design and left the detail placer doing nothing at all.
    double rowHeight = std::numeric_limits<double>::max();
    for (const auto &ri : db_.getRows()) {
        if (ri.height > 0.0) {
            rowHeight = std::min(rowHeight, ri.height);
        }
    }
    if (!std::isfinite(rowHeight)) {
        rowHeight = 1.0;
    }

    for (std::size_t v = 0; v < graph_.getNumCells(); ++v) {
        const Vertex &vert = graph_.getCell(v);
        if (vert.isFixed) {
            fixed_.push_back(FixedBox{vert.y, vert.y + vert.height, vert.x, vert.x + vert.width});
        }
        if (vert.isFixed || vert.isTerminal) {
            continue;
        }
        // Only a cell that fits inside one row may move here. Every pass --
        // vertical swap, reorder, cluster -- looks for a slot in a single row's
        // band, so a taller cell has nowhere valid to go. Letting one through
        // re-inserted the overlap the legalizer had just removed: on ibm01 it
        // cost 1378 overlapping pairs. Such cells are left exactly where
        // legalization put them.
        if (vert.height > rowHeight) {
            ++tallSkipped_;
            // ...and a blockage, not just skipped. Left out of the blockage set the
            // passes treat those rows as empty and place cells straight on top of
            // them: skipping without blocking cost 977 overlapping pairs on ibm01,
            // which the legalizer had not produced and FastDP's own check never
            // saw, because it only tests the cells it moved.
            fixed_.push_back(FixedBox{vert.y, vert.y + vert.height, vert.x, vert.x + vert.width});
            continue;
        }
        mov_.push_back(v);
        w_.push_back(vert.width);
        h_.push_back(vert.height);
        x_.push_back(vert.x);
        y_.push_back(vert.y);
    }
    if (mov_.empty()) {
        return res;
    }
    const BBox d = fixedCellBBox(graph_);
    die_ = BBox{d[0], d[1], d[2], d[3]};
    {
        const auto t0 = std::chrono::steady_clock::now();
        buildSpans();
        const auto t1 = std::chrono::steady_clock::now();
        buildNetlist();
        const auto t2 = std::chrono::steady_clock::now();
        res.hpwlBefore = hpwl();
        const auto t3 = std::chrono::steady_clock::now();
        ktlog.echo("  detail-place build-spans  {:>8.3f}s",
                   std::chrono::duration<double>(t1 - t0).count());
        ktlog.echo("  detail-place build-nets   {:>8.3f}s",
                   std::chrono::duration<double>(t2 - t1).count());
        ktlog.echo("  detail-place hpwl-in      {:>8.3f}s",
                   std::chrono::duration<double>(t3 - t2).count());
    }

    if (!params.plotDir.empty()) {
        // The frames are a diagnostic. create_directories without an error_code
        // throws, which would turn a plotting problem into a lost placement, so
        // the failure is reported and the run carries on without frames.
        std::error_code ec;
        std::filesystem::create_directories(params.plotDir, ec);
        if (ec) {
            ktlog.warning(
                "cannot create the detailed-placement plot directory '{}': {}. "
                "Continuing without frames.",
                params.plotDir, ec.message());
        } else {
            writeFrame(params.plotDir + "/dp_000.svg", "legalized input");
        }
    }

    double prev = res.hpwlBefore;
    int pass = 0;
    // The technique is passed as a function rather than as a name to dispatch on.
    // Selecting it by the first letter of a label looked equivalent and was not:
    // "reorder" starts with 'r', so it fell through to the clustering branch,
    // which meant local re-ordering never ran in any flow and single-segment
    // clustering ran twice per pass. The move counters made this invisible --
    // both branches increment the same field, so the totals still looked sane.
    const auto sweep = [&](const char *name, std::size_t limit, std::size_t &counter,
                           std::size_t (Impl::*technique)()) {
        double phaseSeconds = 0.0;
        for (std::size_t k = 0; k < limit; ++k) {
            const std::size_t before = counter;
            // Free space has to be re-derived before every pass, not once up
            // front. The passes relocate cells, so spans built from the
            // legalized placement go stale as soon as the first of them runs, and
            // two cells end up placed into the same gap.
            buildSpans();
            // std::chrono rather than ScopedTimer: that one registers its interval
            // in a shared table under a name, and these are per-phase, per-pass.
            const auto phaseStart = std::chrono::steady_clock::now();
            counter = (this->*technique)();
            // Accumulate across passes: each phase runs up to `limit` passes and
            // timing only the last one made reorder look like 18 of 52 seconds when
            // it is nearly half.
            phaseSeconds +=
                std::chrono::duration<double>(std::chrono::steady_clock::now() - phaseStart)
                    .count();
            const double now = hpwl();
            char note[128];
            std::snprintf(note, sizeof(note), "%s pass %zu (hpwl %.6g)", name, k, now);
            if (!params.plotDir.empty()) {
                char path[64];
                std::snprintf(path, sizeof(path), "/dp_%03d_%s_%zu.svg", ++pass, name, k);
                writeFrame(params.plotDir + path, note);
            }
            if (counter == 0) {
                break;
            }
            if (prev > 0.0 && (prev - now) / prev < params.minImprovement) {
                break;  // converged
            }
            prev = now;
            (void)before;
        }
        // Per-phase time. The techniques overlap in what they touch, so the total
        // alone does not say where the time goes: the window search and the median
        // pass both cover every span, and which one dominates is a property of the
        // design's row occupancy, not something to be guessed at.
        ktlog.echo("  detail-place {:<8} {:>8.3f}s", name, phaseSeconds);
    };

    sweep("global", params.globalSwapPasses, res.globalSwaps, &Impl::globalSwap);
    sweep("vertical", params.verticalSwapPasses, res.verticalSwaps, &Impl::verticalSwap);
    if (std::getenv("KTPLACE_DP_NO_MEDIAN") == nullptr) {
        sweep("median", params.localReorderPasses, res.medianMoves, &Impl::medianReorder);
    }
    sweep("reorder", params.localReorderPasses, res.reorderMoves, &Impl::localReorder);
    sweep("cluster", params.clusterPasses, res.clusterMoves, &Impl::singleSegmentCluster);

    const auto t4 = std::chrono::steady_clock::now();
    res.hpwlAfter = hpwl();
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        db_.setCellPosition(mov_[i], x_[i], y_[i]);
    }
    const auto t5 = std::chrono::steady_clock::now();
    selfCheck(res);
    const auto t6 = std::chrono::steady_clock::now();
    ktlog.echo("  detail-place write-back   {:>8.3f}s",
               std::chrono::duration<double>(t5 - t4).count());
    ktlog.echo("  detail-place self-check   {:>8.3f}s",
               std::chrono::duration<double>(t6 - t5).count());
    res.seconds = timer.elapsedSeconds();
    ktlog.echo(
        "FastDP: {:.3f}s, HPWL {:.6e} -> {:.6e} ({:+.2f}%), swaps {} global / {} vertical, "
        "reorder {}, cluster {}",
        res.seconds, res.hpwlBefore, res.hpwlAfter,
        (res.hpwlBefore > 0.0) ? 100.0 * (res.hpwlAfter - res.hpwlBefore) / res.hpwlBefore : 0.0,
        res.globalSwaps, res.verticalSwaps, res.reorderMoves, res.clusterMoves);
    return res;
}

// ---------------------------------------------------------------------------

FastDetailedPlacer::FastDetailedPlacer(ktDM &db) : pImpl(std::make_unique<Impl>(db)) {}

FastDetailedPlacer::~FastDetailedPlacer() = default;

namespace {

void report(const DetailPlaceResult &r) {
    ktReportTable t("Detailed placement (FastDP)");
    t.setHeaders({"metric", "value"});
    t.addRow({"global swaps", fmt::format("{}", r.globalSwaps)});
    t.addRow({"vertical swaps", fmt::format("{}", r.verticalSwaps)});
    t.addRow({"reorder moves", fmt::format("{}", r.reorderMoves)});
    t.addRow({"cluster moves", fmt::format("{}", r.clusterMoves)});
    t.addRow({"HPWL before", fmt::format("{:.6}", r.hpwlBefore)});
    t.addRow({"HPWL after", fmt::format("{:.6}", r.hpwlAfter)});
    t.addRow({"HPWL change", fmt::format("{:.2}%", 100.0 * (r.hpwlAfter - r.hpwlBefore) /
                                                       (r.hpwlBefore > 0.0 ? r.hpwlBefore : 1.0))});
    t.addRow({"time (s)", fmt::format("{:.6}", r.seconds)});
    t.addRow({"overlapping pairs", fmt::format("{}", r.overlappingPairs)});
    t.addRow({"cells off row", fmt::format("{}", r.offRow)});
    t.addRow({"cells off site", fmt::format("{}", r.offSite)});
    t.addRow({"cells over macro", fmt::format("{}", r.overFixed)});
    t.emit();

    if (r.overlappingPairs != 0 || r.offRow != 0 || r.overFixed != 0) {
        ktlog.warning("detailed placement broke legality; see the counts above");
    }
}

}  // namespace

DetailPlaceResult FastDetailedPlacer::place(const DetailPlaceParams &params) {
    DetailPlaceResult result = pImpl->place(params);
    report(result);
    return result;
}

}  // namespace ktplace
