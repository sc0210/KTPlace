// @file kt_abacus.cc// Abacus legalization, DP-over-clusters form. See kt_abacus.h.


#include "legalizer/kt_abacus.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_animator.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <numeric>

namespace ktplace {

namespace {

/// Undo information for one place() call on a row.
///
/// Candidate rows are scored by inserting the cell, reading off the cost delta,
/// and undoing it. Snapshotting the row instead would deep-copy every cluster's
/// member vector on every candidate, which cost about 1e8 heap allocations on
/// adaptec1. Every mutation place() makes is O(1) to record, so the rollback is
/// O(1) in the number of clusters it touched.
struct RowUndo {
    std::size_t savedSize = 0;
    /// The cell went into a brand new cluster, which was inserted at insertIdx.
    /// Erasing by resize would drop the *last* cluster instead, since the insert
    /// index is usually in the middle of the list.
    bool inserted = false;
    std::size_t insertIdx = 0;
    bool merged = false;
    std::size_t mergeIdx = 0;
    std::size_t mergePos = 0;
    double savedW = 0.0, savedN = 0.0, savedA = 0.0, savedB = 0.0, savedX = 0.0;
    double savedCost = 0.0, savedUsed = 0.0;
    std::vector<std::pair<std::size_t, double>> moved;
};

/// Axis-aligned box of a fixed cell (a macro or a pin), used to carve subrows.
struct FixedBox {
    double y0, y1, x0, x1;
};

/// Subrows narrower than this many sites are dropped. A sliver narrower than a
/// cell can never hold it, and keeping them only makes the row search walk
/// candidates that cannot win.
double minSubrowSites() {
    const char *e = std::getenv("KTPLACE_ABACUS_MIN_SUBROW_SITES");
    return e ? std::atof(e) : 1.0;
}

/// Cost of a candidate that does not fit; no real cost reaches this.
constexpr double kInfeasible = std::numeric_limits<double>::infinity();

/// Cells closer than this many site widths to their neighbour are candidates to
/// be merged into one cluster. Abacus uses a small constant gap here; clustering
/// is what stops a row's cells from being dragged apart one at a time.
double clusterGapSites() {
    const char *e = std::getenv("KTPLACE_ABACUS_CLUSTER_GAP");
    return e ? std::atof(e) : 0.0;
}

/// A run of adjacent cells treated as one placeable item.
///
/// Abacus lets neighbouring cells form a cluster that the row's dynamic program
/// then places as a single unit, which is what stops the row's cells from being
/// nudged apart one at a time. Members are laid out *contiguously* inside the
/// cluster at cumulative-width offsets -- not at their original offsets, which
/// after global placement are frequently negative and would leave the cells
/// overlapping. Laying them out contiguously is what makes the cluster
/// internally legal by construction.
///
/// With off_m the cumulative offset of member m, the cluster cost is
///     sum_m (x + off_m - target_m)^2 = n*x^2 + 2*A*x + B
/// so it evaluates in constant time and the ideal left edge is -A/n.
struct Cluster {
    std::vector<std::size_t> members;  ///< movable slots, ascending target x
    double w = 0.0;                    ///< total width == the space reserved
    double x = 0.0;                    ///< committed left edge
    double n = 0.0;                    ///< member count
    double a = 0.0;                    ///< sum of (off_m - target_m)
    double b = 0.0;                    ///< sum of (off_m - target_m)^2

    [[nodiscard]] double costAt(double pos) const {
        return n * pos * pos + 2.0 * a * pos + b;
    }
    [[nodiscard]] double ideal() const {
        return (n > 0.0) ? (-a / n) : 0.0;
    }
};

/// One contiguous run of placeable sites within a row.
///
/// A row is not one span of x. Macros cut it into subrows of very different
/// widths, so placing into a row means choosing a subrow first, and a subrow
/// narrower than a cell cannot hold it at all. Modelling a row as a single span
/// (as an earlier version did) made the placer slide cells along x straight
/// across a blockage, which is what destroyed the wirelength: a cell that only
/// needed to change rows was instead dragged to the far end of its own row.
struct Subrow {
    double xlo = 0.0;
    double xhi = 0.0;
    /// Clusters assigned here, in increasing left edge.
    std::vector<Cluster> clusters;
    double cost = 0.0;
    /// Committed right edge of the last cluster, i.e. the used extent.
    double used = 0.0;

    [[nodiscard]] double width() const {
        return xhi - xlo;
    }
    [[nodiscard]] double free() const {
        return xhi - used;
    }
};

/// One placement row: a y band plus the subrows inside it.
struct RowTrack {
    double y = 0.0;
    double height = 0.0;
    double siteWidth = 1.0;
    std::vector<Subrow> subrows;

    /// Total placeable width left in the row.
    [[nodiscard]] double capacity() const {
        double t = 0.0;
        for (const Subrow &sr : subrows) {
            t += sr.free();
        }
        return t;
    }
};

}  // namespace

// ---------------------------------------------------------------------------

class AbacusLegalizer::Impl {
public:
    explicit Impl(ktDM &db) : db_(db), graph_(db.getGraph()) {}

    LegalizeResult run(const LegalizeParams &params);

private:
    void buildRows();
    /// Insert slot `i` into row `r` and re-place the affected clusters.
    /// Returns the row's cost delta, or kInfeasible. `xsOut` receives the
    /// clusters' new left edges for the suffix that moved.
    /// Insert slot `i` into row `r` and re-place the affected clusters,
    /// returning the row's cost delta or kInfeasible. Only the row's cluster
    /// list and cost are touched; cell positions are not written here, so an
    /// unused candidate leaves nothing behind. `undo` records enough to roll
    /// the row back in time proportional to what actually moved.
    double place(Subrow &sr, double grid, std::size_t i, RowUndo &undo);
    void rollback(Subrow &sr, RowUndo undo);
    [[nodiscard]] double hpwlOf(const std::vector<double> &x, const std::vector<double> &y) const;
    void writeFrame(const std::string &path, const std::string &note, std::size_t step,
                    std::size_t total) const;
    void selfCheck(LegalizeResult &res) const;

    static constexpr std::size_t kNoRow = std::numeric_limits<std::size_t>::max();

    ktDM &db_;
    Graph &graph_;
    std::vector<RowTrack> rows_;
    std::vector<std::size_t> mov_;  ///< graph vertex id per movable slot
    std::vector<double> w_, h_;     ///< per movable slot
    std::vector<double> x0_, y0_;   ///< pre-legalization position (the target)
    std::vector<double> xs_, ys_;   ///< live position
    std::unordered_map<std::size_t, std::size_t> slotOf_;
    std::vector<FixedBox> fixed_;
    BBox die_ = {0.0, 0.0, 0.0, 0.0};
    /// Fences, carried from the params so the frame writer can draw them.
    const constraintMgr *constraints_ = nullptr;
};

void AbacusLegalizer::Impl::buildRows() {
    rows_.clear();
    for (const RowInfo &ri : db_.getRows()) {
        RowTrack r;
        r.y = ri.coordinate;
        r.height = ri.height;
        r.siteWidth = ri.pitch();
        if (!(r.siteWidth > 0.0)) {
            r.siteWidth = 1.0;
        }
        // Each .scl subrow becomes a Subrow, trimmed by any fixed cell that
        // crosses the row's band, so a macro crossing a subrow shortens it
        // instead of being ignored.
        for (const SubrowInfo &si : ri.subrows) {
            if (!(si.xhi(r.siteWidth) > si.xlo())) {
                continue;
            }
            Subrow sr;
            sr.xlo = si.xlo();
            sr.xhi = si.xhi(r.siteWidth);
            sr.used = sr.xlo;
            r.subrows.push_back(sr);
        }
        if (r.subrows.empty()) {
            continue;
        }
        std::sort(r.subrows.begin(), r.subrows.end(), [](const Subrow &a, const Subrow &b) {
            return a.xlo < b.xlo;
        });
        rows_.push_back(r);
    }
    std::sort(rows_.begin(), rows_.end(), [](const RowTrack &a, const RowTrack &b) {
        return a.y < b.y;
    });

    // Trim each subrow against the fixed cells crossing its row band. A macro
    // that lands inside a .scl subrow splits it into two placeable subrows, so
    // the trimmed pieces are what the search actually chooses between.
    for (RowTrack &r : rows_) {
        std::vector<Subrow> pieces;
        for (const Subrow &sr : r.subrows) {
            std::vector<std::pair<double, double>> hit;
            for (const FixedBox &f : fixed_) {
                if (f.y1 <= r.y + 1e-9 || f.y0 >= r.y + r.height - 1e-9) {
                    continue;
                }
                const double a = std::max(f.x0, sr.xlo);
                const double b = std::min(f.x1, sr.xhi);
                if (b > a) {
                    hit.emplace_back(a, b);
                }
            }
            std::sort(hit.begin(), hit.end());
            double cur = sr.xlo;
            for (const auto &iv : hit) {
                if (iv.first > cur) {
                    Subrow piece = sr;
                    piece.xlo = cur;
                    piece.xhi = iv.first;
                    piece.used = cur;
                    pieces.push_back(piece);
                }
                cur = std::max(cur, iv.second);
            }
            if (cur < sr.xhi) {
                Subrow piece = sr;
                piece.xlo = cur;
                piece.used = cur;
                pieces.push_back(piece);
            }
        }
        // Subrows too narrow to be worth searching are dropped: a sliver a cell
        // cannot fit into only makes the search slower and can never be chosen.
        std::vector<Subrow> kept;
        for (Subrow &piece : pieces) {
            if (piece.width() >= r.siteWidth * minSubrowSites()) {
                kept.push_back(std::move(piece));
            }
        }
        r.subrows = std::move(kept);
    }
}

double AbacusLegalizer::Impl::place(Subrow &sr, double grid, std::size_t i, RowUndo &undo) {
    undo = RowUndo{};
    undo.savedSize = sr.clusters.size();
    undo.savedCost = sr.cost;
    undo.savedUsed = sr.used;

    const double target = x0_[i];

    // Where the cell lands in the subrow's cluster order, and whether it can
    // join a neighbour. Groups that are already touching in the target
    // placement become one placeable item, which is what keeps the number of
    // items per subrow small and stops cells being nudged apart one at a time.
    std::size_t at = 0;
    while (at < sr.clusters.size() && sr.clusters[at].x <= target) {
        ++at;
    }
    const double gap = clusterGapSites() * grid;
    std::size_t merge = sr.clusters.size();
    double bestGap = std::numeric_limits<double>::max();
    if (at < sr.clusters.size()) {
        const double g = sr.clusters[at].x - target;
        if (g <= gap && g < bestGap) {
            bestGap = g;
            merge = at;
        }
    }
    if (at > 0) {
        const double g = target - (sr.clusters[at - 1].x + sr.clusters[at - 1].w);
        if (g <= gap && g < bestGap) {
            bestGap = g;
            merge = at - 1;
        }
    }

    if (merge < sr.clusters.size()) {
        at = merge;
        Cluster &c = sr.clusters[merge];
        const auto pos =
            static_cast<std::size_t>(std::lower_bound(c.members.begin(), c.members.end(), i,
                                                      [&](std::size_t p, std::size_t q) {
                                                          return x0_[p] < x0_[q];
                                                      }) -
                                     c.members.begin());
        undo.merged = true;
        undo.mergeIdx = merge;
        undo.mergePos = pos;
        undo.savedW = c.w;
        undo.savedN = c.n;
        undo.savedA = c.a;
        undo.savedB = c.b;
        undo.savedX = c.x;
        c.members.insert(c.members.begin() + static_cast<long>(pos), i);
    } else {
        Cluster c;
        c.members.push_back(i);
        sr.clusters.insert(sr.clusters.begin() + static_cast<long>(at), std::move(c));
        undo.inserted = true;
        undo.insertIdx = at;
    }

    // Recompute the touched cluster's sums for its new membership, laying its
    // members out contiguously: after global placement the original offsets are
    // often negative, and keeping them would leave the cells overlapping.
    {
        Cluster &c = sr.clusters[at];
        c.n = static_cast<double>(c.members.size());
        c.w = 0.0;
        c.a = 0.0;
        c.b = 0.0;
        double off = 0.0;
        for (const std::size_t m : c.members) {
            const double d = off - x0_[m];
            c.a += d;
            c.b += d * d;
            off += w_[m];
        }
        c.w = off;
    }

    // Re-place clusters from the touched one rightwards. The greedy is monotone,
    // so the first cluster that holds its position proves every cluster to its
    // right is unchanged too and the loop stops there. That early stop is what
    // makes the pass near-linear instead of O(subrow length) for every cell.
    double cursor = (at > 0) ? (sr.clusters[at - 1].x + sr.clusters[at - 1].w) : sr.xlo;
    double delta = 0.0;
    for (std::size_t k = at; k < sr.clusters.size(); ++k) {
        Cluster &c = sr.clusters[k];
        const double oldX = c.x;
        const double oldCost = c.costAt(oldX);

        // Ideal left edge: the cluster's own best position, pulled inside the
        // subrow, and then pushed right past the previous cluster. The order
        // matters. Clamping to the subrow's right edge *after* the cursor has
        // been applied would drag a cluster leftwards on top of its predecessor
        // when the subrow overflows, which silently reintroduced overlaps; if it
        // does not fit past the cursor, the subrow is simply full.
        double ideal = c.ideal();
        const double hi = sr.xhi - c.w;
        if (hi >= sr.xlo) {
            ideal = std::min(ideal, hi);
        }
        ideal = std::max(ideal, sr.xlo);
        if (grid > 0.0) {
            ideal = std::round(ideal / grid) * grid;
        }
        ideal = std::max(ideal, cursor);
        if (ideal < sr.xlo) {
            ideal = sr.xlo;
        }
        // A cluster that would not fit the subrow's right edge cannot go here.
        if (ideal + c.w > sr.xhi + 1e-9) {
            rollback(sr, undo);
            return kInfeasible;
        }
        if (k > at && std::fabs(ideal - oldX) < 1e-12) {
            break;  // nothing further right can move either
        }
        delta += c.costAt(ideal) - oldCost;
        undo.moved.emplace_back(k, oldX);
        c.x = ideal;
        cursor = ideal + c.w;
        sr.used = std::max(sr.used, cursor);
    }
    if (!std::isfinite(delta)) {
        rollback(sr, undo);
        return kInfeasible;
    }
    sr.cost += delta;
    return delta;
}

void AbacusLegalizer::Impl::rollback(Subrow &sr, RowUndo undo) {
    sr.cost = undo.savedCost;
    sr.used = undo.savedUsed;
    if (undo.merged) {
        Cluster &c = sr.clusters[undo.mergeIdx];
        if (undo.mergePos < c.members.size()) {
            c.members.erase(c.members.begin() + static_cast<long>(undo.mergePos));
        }
        c.w = undo.savedW;
        c.n = undo.savedN;
        c.a = undo.savedA;
        c.b = undo.savedB;
        c.x = undo.savedX;
    } else if (undo.inserted && undo.insertIdx < sr.clusters.size()) {
        // Erasing the inserted cluster shifts every later index down by one, so
        // the recorded positions have to be shifted with it. Without this the
        // restore wrote each old x into the *next* cluster over, which left most
        // clusters sitting at x = 0 and produced a badly overlapping placement.
        sr.clusters.erase(sr.clusters.begin() + static_cast<long>(undo.insertIdx));
        std::vector<std::pair<std::size_t, double>> fixed;
        fixed.reserve(undo.moved.size());
        for (const auto &mv : undo.moved) {
            if (mv.first == undo.insertIdx) {
                continue;  // the cluster that was just removed
            }
            fixed.emplace_back(mv.first > undo.insertIdx ? mv.first - 1 : mv.first, mv.second);
        }
        undo.moved = std::move(fixed);
    } else if (sr.clusters.size() > undo.savedSize) {
        sr.clusters.resize(undo.savedSize);
    }
    for (const auto &mv : undo.moved) {
        sr.clusters[mv.first].x = mv.second;
    }
}

double AbacusLegalizer::Impl::hpwlOf(const std::vector<double> &x,
                                     const std::vector<double> &y) const {
    double total = 0.0;
    for (std::size_t n = 0; n < graph_.getNumNets(); ++n) {
        const std::vector<std::size_t> &pins = graph_.getNetPins(n);
        if (pins.empty()) {
            continue;
        }
        double ax = std::numeric_limits<double>::max(), bx = -std::numeric_limits<double>::max();
        double ay = std::numeric_limits<double>::max(), by = -std::numeric_limits<double>::max();
        for (const std::size_t pinId : pins) {
            const Pin &pin = graph_.getPin(pinId);
            const auto it = slotOf_.find(pin.cellId);
            double cx = 0.0, cy = 0.0;
            if (it != slotOf_.end()) {
                cx = x[it->second] + pin.offsetX;
                cy = y[it->second] + pin.offsetY;
            } else {
                const Vertex &c = graph_.getCell(pin.cellId);
                cx = c.x + pin.offsetX;
                cy = c.y + pin.offsetY;
            }
            ax = std::min(ax, cx);
            bx = std::max(bx, cx);
            ay = std::min(ay, cy);
            by = std::max(by, cy);
        }
        total += (bx - ax) + (by - ay);
    }
    return total;
}

void AbacusLegalizer::Impl::writeFrame(const std::string &path, const std::string &note,
                                       std::size_t step, std::size_t total) const {
    const std::size_t nv = graph_.getNumCells();
    std::vector<float> fx(nv), fy(nv);
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getCell(v);
        fx[v] = static_cast<float>(vert.x);
        fy[v] = static_cast<float>(vert.y);
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        fx[mov_[i]] = static_cast<float>(xs_[i]);
        fy[mov_[i]] = static_cast<float>(ys_[i]);
    }
    writeFrameSvg(path, graph_, fx, fy, die_, step, total, hpwlOf(xs_, ys_), 0.0, 0.0, note,
                  nullptr, /*fixedView=*/true);
    // The same frame into the run's animation, so the GIF shows the legalizer
    // pulling the placement back onto its rows rather than cutting straight from
    // a scattered global placement to a legal one.
    PlacementAnimator::instance().record(graph_, fx, fy, die_, step, total, hpwlOf(xs_, ys_),
                                         hpwlOf(xs_, ys_), 0.0, note, constraints_);
}

void AbacusLegalizer::Impl::selfCheck(LegalizeResult &res) const {
    const double eps = 1e-6;
    res.overlappingPairs = 0;
    res.commitFailures = 0;
    res.offRow = 0;
    res.offSite = 0;
    res.overFixed = 0;
    res.outOfRows = 0;
    // Row of each cell, read off the cluster structure rather than by scanning
    // every row for every cell.
    std::vector<std::size_t> rowOf(mov_.size(), kNoRow);
    std::vector<std::size_t> subOf(mov_.size(), 0);
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        for (std::size_t si = 0; si < rows_[r].subrows.size(); ++si) {
            for (const Cluster &c : rows_[r].subrows[si].clusters) {
                for (const std::size_t m : c.members) {
                    rowOf[m] = r;
                    subOf[m] = si;
                }
            }
        }
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const RowTrack *inRow = (rowOf[i] == kNoRow) ? nullptr : &rows_[rowOf[i]];
        if (inRow == nullptr) {
            ++res.offRow;
            ++res.outOfRows;
            continue;
        }

        const double q = xs_[i] / inRow->siteWidth;
        const double off = std::fabs(q - std::round(q)) * inRow->siteWidth;
        if (off > 1e-6 * std::max(1.0, std::fabs(xs_[i]))) {
            ++res.offSite;
        }
        // Inside some subrow of this row, with the cell on a site?
        bool inside = false;
        for (const Subrow &sr : inRow->subrows) {
            if (xs_[i] >= sr.xlo - eps && xs_[i] + w_[i] <= sr.xhi + eps) {
                inside = true;
                break;
            }
        }
        if (!inside) {
            ++res.outOfRows;
        }
    }

    // Overlap: sort every movable cell by (row, x) and test only adjacent pairs.
    // Comparing each row's cells pairwise is quadratic and was both slow and,
    // before the early-stop fix, a source of phantom overlaps.
    std::vector<std::uint64_t> order(mov_.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (rowOf[a] != rowOf[b]) {
            return rowOf[a] < rowOf[b];
        }
        return xs_[a] < xs_[b];
    });
    for (std::size_t k = 1; k < order.size(); ++k) {
        const std::size_t a = order[k - 1], b = order[k];
        if (rowOf[a] == rowOf[b] && xs_[a] + w_[a] > xs_[b] + eps) {
            ++res.overlappingPairs;
        }
    }

    // Movable-vs-fixed, x-sorted sweep.
    const std::vector<FixedBox> &fixed = fixed_;
    std::vector<std::size_t> byX(mov_.size());
    std::iota(byX.begin(), byX.end(), 0u);
    std::sort(byX.begin(), byX.end(), [&](std::size_t a, std::size_t b) {
        return xs_[a] < xs_[b];
    });
    std::size_t lo = 0;
    for (const std::size_t i : byX) {
        while (lo < fixed.size() && fixed[lo].x1 <= xs_[i] + eps) {
            ++lo;
        }
        for (std::size_t f = lo; f < fixed.size() && fixed[f].x0 < xs_[i] + w_[i]; ++f) {
            if (fixed[f].x1 <= xs_[i] + eps) {
                continue;  // sorted by x0, so a later box can still end earlier
            }
            if (ys_[i] + h_[i] > fixed[f].y0 + eps && ys_[i] < fixed[f].y1 - eps) {
                ++res.overFixed;
            }
        }
    }
}

LegalizeResult AbacusLegalizer::Impl::run(const LegalizeParams &params) {
    LegalizeResult res;
    // ScopedTimer both times the phase and records it into the shared registry,
    // so legalization appears in the flow's Timings table next to load, place
    // and write instead of reporting a private duration.
    ScopedTimer timer("legalize");
    constraints_ = &db_.constraints();

    for (std::size_t v = 0; v < graph_.getNumCells(); ++v) {
        const Vertex &vert = graph_.getCell(v);
        if (vert.isFixed || vert.isTerminal) {
            continue;
        }
        const std::size_t slot = mov_.size();
        mov_.push_back(v);
        slotOf_[v] = slot;
        w_.push_back(vert.width);
        h_.push_back(vert.height);
        x0_.push_back(vert.x);
        y0_.push_back(vert.y);
    }
    xs_ = x0_;
    ys_ = y0_;
    res.cellsPlaced = mov_.size();
    if (mov_.empty()) {
        return res;
    }
    res.hpwlBefore = hpwlOf(xs_, ys_);

    for (std::size_t v = 0; v < graph_.getNumCells(); ++v) {
        const Vertex &vert = graph_.getCell(v);
        if (!vert.isFixed) {
            continue;
        }
        fixed_.push_back(FixedBox{vert.y, vert.y + vert.height, vert.x, vert.x + vert.width});
    }

    buildRows();
    if (rows_.empty()) {
        res.unplaced = mov_.size();
        res.hpwlAfter = res.hpwlBefore;
        return res;
    }
    const BBox d = fixedCellBBox(graph_);
    die_ = BBox{d[0], d[1], d[2], d[3]};

    if (!params.plotDir.empty()) {
        std::filesystem::create_directories(params.plotDir);
        writeFrame(params.plotDir + "/legalize_000000.svg", "input placement", 0, mov_.size());
    }

    // Cells are legalized in order of target y, so rows fill from one side and
    // each row's DP sees cells in roughly the order they will sit.
    std::vector<std::size_t> order(mov_.size());
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return y0_[a] < y0_[b];
    });

    std::size_t placed = 0;
    std::size_t commitFail = 0;

    for (const std::size_t i : order) {
        // Home row: the band containing the cell's target y, else the nearest.
        std::size_t home = 0;
        double bestD = std::numeric_limits<double>::max();
        for (std::size_t r = 0; r < rows_.size(); ++r) {
            const RowTrack &R = rows_[r];
            const double dd = (y0_[i] < R.y)              ? (R.y - y0_[i])
                              : (y0_[i] > R.y + R.height) ? (y0_[i] - (R.y + R.height))
                                                          : 0.0;
            if (dd < bestD) {
                bestD = dd;
                home = r;
            }
        }

        // A candidate is a (row, subrow) pair. Scoring uses the *incremental*
        // cost of the insertion plus the cell's squared distance to that
        // subrow in both axes. Charging the horizontal distance is what lets a
        // cell blocked in x pick a different subrow instead of being pushed to
        // the far end of its own row, and it is also a true lower bound on the
        // cost, so it prunes the outward scan.
        std::size_t bestRow = kNoRow, bestSub = 0;
        double bestCost = std::numeric_limits<double>::max();

        const auto trySub = [&](std::size_t ri, std::size_t si) {
            if (ri >= rows_.size() || si >= rows_[ri].subrows.size()) {
                return;
            }
            RowTrack &R = rows_[ri];
            Subrow &sr = R.subrows[si];
            if (h_[i] > R.height + 1e-9) {
                return;
            }
            // Only the geometric width is pre-filtered. Whether there is room is
            // left to place(), which places at max(target, previous end) and so
            // can drop a cell into an interior gap. Pre-filtering on remaining
            // room at the right edge used to reject subrows that still had a
            // gap big enough, which left 16833 adaptec2 cells unplaced.
            if (w_[i] > sr.width() + 1e-9) {
                return;
            }
            const double dy = (y0_[i] < R.y)              ? (R.y - y0_[i])
                              : (y0_[i] > R.y + R.height) ? (y0_[i] - (R.y + R.height))
                                                          : 0.0;
            // Horizontal distance to the nearest point of the subrow: zero when
            // the cell's target x already lies inside it.
            const double dx = (x0_[i] < sr.xlo)           ? (sr.xlo - x0_[i])
                              : (x0_[i] > sr.xhi - w_[i]) ? (x0_[i] - (sr.xhi - w_[i]))
                                                          : 0.0;
            // dx and dy are both true lower bounds on this cell's movement: it
            // cannot reach the subrow without moving at least that far.
            const double floorCost = dx * dx + dy * dy;
            if (floorCost >= bestCost) {
                return;  // lower bound already loses
            }
            RowUndo undo;
            const double delta = place(sr, R.siteWidth, i, undo);
            if (!std::isfinite(delta)) {
                return;
            }
            // Cost = the subrow's marginal squared-displacement change plus the
            // cell's distance to the subrow in both axes. The horizontal term was
            // tried without the vertical-only variant: dropping it in favour of
            // delta + dy^2 alone made HPWL worse (1.01e9 vs 9.61e8), because a
            // cell can sit near its target inside a subrow that is itself far to
            // the left, and only the horizontal term notices that.
            const double total = delta + floorCost;
            if (total < bestCost) {
                bestCost = total;
                bestRow = ri;
                bestSub = si;
            }
            rollback(sr, undo);
        };

        const auto tryRow = [&](std::size_t ri) {
            if (ri >= rows_.size()) {
                return;
            }
            for (std::size_t si = 0; si < rows_[ri].subrows.size(); ++si) {
                trySub(ri, si);
            }
        };

        tryRow(home);
        for (std::size_t step = 1; step < rows_.size(); ++step) {
            if (params.maxRowDistance > 0 && step > params.maxRowDistance) {
                break;
            }
            // Every row at distance `step` is at least this far away vertically,
            // and the horizontal term is never negative, so once that vertical
            // bound exceeds the incumbent no farther row can win. This is the
            // paper's bounding rule and is the difference between scanning every
            // row per cell and scanning a handful.
            const double bound = [&] {
                const double a = (home + step < rows_.size())
                                     ? std::fabs(rows_[home + step].y - rows_[home].y)
                                     : std::numeric_limits<double>::max();
                const double b = (step <= home) ? std::fabs(rows_[home - step].y - rows_[home].y)
                                                : std::numeric_limits<double>::max();
                return std::min(a, b);
            }();
            if (bound * bound > bestCost) {
                break;
            }
            tryRow(home + step);
            if (step <= home) {
                tryRow(home - step);
            }
        }

        if (bestRow == kNoRow) {
            ++res.unplaced;
            continue;
        }
        // Commit: place the cell in the winning subrow for real. The commit can
        // still fail even though the candidate scored, because a merged cluster
        // reserves more width than the cell alone. A failure leaves the subrow
        // rolled back and the cell in no cluster at all, so it must be counted
        // rather than assumed placed.
        RowUndo commitUndo;
        if (!std::isfinite(
                place(rows_[bestRow].subrows[bestSub], rows_[bestRow].siteWidth, i, commitUndo))) {
            ++commitFail;
            continue;
        }
        ++placed;

        if (!params.plotDir.empty() && params.frameEvery > 0 && placed % params.frameEvery == 0) {
            char name[64];
            std::snprintf(name, sizeof(name), "/legalize_%06zu.svg", placed);
            writeFrame(params.plotDir + name, "legalizing", placed, mov_.size());
        }
    }

    // Materialise cell positions from the cluster structure. Doing this once at
    // the end, rather than inside place(), keeps the inner loop free of any
    // per-member work and is what lets a rejected candidate cost nothing.
    for (const RowTrack &R : rows_) {
        for (const Subrow &sr : R.subrows) {
            for (const Cluster &c : sr.clusters) {
                double off = 0.0;
                for (const std::size_t m : c.members) {
                    xs_[m] = c.x + off;
                    ys_[m] = R.y;
                    off += w_[m];
                }
            }
        }
    }

    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const double dx = xs_[i] - x0_[i];
        const double dy = ys_[i] - y0_[i];
        res.totalSquaredDisplacement += dx * dx + dy * dy;
        res.maxDisplacement = std::max(res.maxDisplacement, std::hypot(dx, dy));
        db_.setCellPosition(mov_[i], xs_[i], ys_[i]);
    }
    res.hpwlAfter = hpwlOf(xs_, ys_);
    res.unplaced = mov_.size() - placed;
    res.commitFailures = commitFail;
    if (!params.plotDir.empty()) {
        char name[64];
        std::snprintf(name, sizeof(name), "/legalize_%06zu.svg", placed);
        writeFrame(params.plotDir + name, "legal placement", placed, mov_.size());
    }
    selfCheck(res);
    res.seconds = timer.elapsedSeconds();
    ktlog.echo(
        "Abacus: {} cells in {:.3f}s, HPWL {:.6e} -> {:.6e}, squared displacement {:.6e} "
        "(max {:.1f}), {} unplaced",
        res.cellsPlaced, res.seconds, res.hpwlBefore, res.hpwlAfter, res.totalSquaredDisplacement,
        res.maxDisplacement, res.unplaced);
    return res;
}

// ---------------------------------------------------------------------------

AbacusLegalizer::AbacusLegalizer(ktDM &db) : pImpl(std::make_unique<Impl>(db)) {}

AbacusLegalizer::~AbacusLegalizer() = default;

namespace {

void report(const LegalizeResult &r) {
    ktReportTable t("Legalization (Abacus)");
    t.setHeaders({"metric", "value"});
    t.addRow({"cells placed", fmt::format("{}", r.cellsPlaced)});
    t.addRow({"cells unplaced", fmt::format("{}", r.unplaced)});
    t.addRow({"squared displacement", fmt::format("{:.6}", r.totalSquaredDisplacement)});
    t.addRow({"max displacement", fmt::format("{:.6}", r.maxDisplacement)});
    t.addRow({"HPWL before", fmt::format("{:.6}", r.hpwlBefore)});
    t.addRow({"HPWL after", fmt::format("{:.6}", r.hpwlAfter)});
    t.addRow({"time (s)", fmt::format("{:.6}", r.seconds)});
    t.addRow({"overlapping pairs", fmt::format("{}", r.overlappingPairs)});
    t.addRow({"cells off row", fmt::format("{}", r.offRow)});
    t.addRow({"cells off site", fmt::format("{}", r.offSite)});
    t.addRow({"cells over macro", fmt::format("{}", r.overFixed)});
    t.addRow({"cells out of rows", fmt::format("{}", r.outOfRows)});
    t.addRow({"commit failures", fmt::format("{}", r.commitFailures)});
    t.emit();

    if (r.overlappingPairs != 0 || r.offRow != 0 || r.overFixed != 0) {
        ktlog.warning("legalization is not legal; see the counts above");
    }
    // A cell Abacus could not fit into any row it searched stays where global
    // placement left it, overlapping. That is a cell taller than one row (the flow
    // sends those to the multi-row legalizer instead), or one whose nearby rows
    // were full -- typically a global placement that piled cells onto macros or
    // outside the rows. Named here rather than left to be inferred from a count.
    if (r.outOfRows > 0) {
        ktlog.warning(
            "{} cell(s) could not be placed in any row and are still at their global placement "
            "positions (no room within KTPLACE_ABACUS_MAX_ROW_DIST rows, or taller than a row). "
            "The placement is not legal; see \"cells out of rows\" above.",
            r.outOfRows);
    }
}

}  // namespace

LegalizeResult AbacusLegalizer::legalize(const LegalizeParams &params) {
    LegalizeResult result = pImpl->run(params);
    report(result);
    return result;
}
}  // namespace ktplace
