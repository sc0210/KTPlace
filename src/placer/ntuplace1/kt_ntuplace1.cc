// Global placement by ratio partitioning. See kt_ntuplace1.h for what this is and
// why it sits beside the analytical placer rather than inside it.

#include "placer/ntuplace1/kt_ntuplace1.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "visualization/kt_animator.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <queue>
#include <utility>
#include <vector>

namespace ktplace {
namespace {

// A net as the partitioner needs it: the movable blocks it touches, and the
// extent of its fixed pins. The fixed pins are the t1/t2 of the paper's Figure 2
// -- they tell a cut which side a net wants to be on, and they are the reason
// this is a hypergraph and not a graph.
struct HyperNet {
    std::vector<std::uint32_t> cells;
    double fx0 = 0.0, fx1 = 0.0, fy0 = 0.0, fy1 = 0.0;  // fixed pin points, min/max
    std::size_t fixedPins = 0;
};

struct RatioRegion {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    std::vector<std::uint32_t> cells;  // indices into the placer's movable array
    std::size_t depth = 0;
};

struct AreaRect {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
};

double overlap(const AreaRect &a, double x0, double y0, double x1, double y1) {
    const double w = std::min(a.x1, x1) - std::max(a.x0, x0);
    const double h = std::min(a.y1, y1) - std::max(a.y0, y0);
    return (w > 0.0 && h > 0.0) ? w * h : 0.0;
}

constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;
// FM passes per cut. A pass that finds no improving prefix ends the cut sooner;
// this only bounds the pathological case.
constexpr int kMaxPasses = 8;
// Balance slack: side 0 may hold target +- this fraction of the region's cell
// area (and never less than two of its largest cells, so FM can always move).
constexpr double kBalanceSlack = 0.02;

}  // namespace

class RatioPlacer::Impl {
public:
    explicit Impl(ktDM &db) : db_(db), graph_(db.getGraph()) {}

    RatioPlaceResult place(const RatioPlaceParams &params);

private:
    void build();
    // Row area inside a rectangle, less the fixed blocks that sit on those rows:
    // the area a region can actually give its cells. This is the capacity the
    // whitespace distribution splits by and the look-ahead checks against.
    double freeAreaIn(double x0, double y0, double x1, double y1) const;
    double movableArea(const std::vector<std::uint32_t> &) const;
    void divide(RatioRegion r);
    // Fiduccia-Mattheyses min-cut aiming at `target` cell area on side 0, within
    // +-tol. Pins outside the region are fixed on the side of the cut they lie
    // on (terminal propagation). Returns the number of nets cut.
    std::size_t bipartition(const std::vector<std::uint32_t> &cells, bool vertical, double cut,
                            double target, double tol, std::vector<std::uint8_t> &side);
    void placeLeaf(const RatioRegion &r);
    void setRegionCentre(const RatioRegion &r);
    void writeFrame(std::size_t depth, const char *note) const;

    ktDM &db_;
    const Graph &graph_;
    std::vector<std::uint32_t> mov_;  // movable, non-terminal vertex ids
    std::vector<double> area_;        // area of each movable
    std::vector<HyperNet> nets_;
    // For each movable, the nets it is on, so a gain update does not rescan the
    // whole netlist.
    std::vector<std::vector<std::uint32_t>> cellNets_;
    std::vector<AreaRect> rowRects_;  // one per subrow, sorted by y
    double maxRowH_ = 0.0;
    std::vector<AreaRect> fixedRects_;  // fixed blocks, terminals included
    // Where cells may go: the bounding box of the rows. Not the die box, which also
    // spans pads and macros outside the rows -- a region out there has no capacity
    // at all, and every cell the recursion sends there is one the legalizer must
    // drag back.
    std::array<double, 4> box_{};
    // Each movable's position: the centre of the region it currently belongs to,
    // refined as the recursion descends. Pins outside a region are read from here
    // when a cut decides which side they pull towards.
    std::vector<double> posX_, posY_;
    RatioPlaceParams params_;
    const constraintMgr *fences_ = nullptr;
    RatioPlaceResult res_;

    // Per-cut scratch, sized once and stamped with gen_ rather than cleared, so a
    // cut costs the region's cells and nets, not the whole design's.
    std::uint32_t gen_ = 0;
    std::vector<std::uint32_t> localGen_, localOf_;  // per movable
    std::vector<std::uint32_t> netGen_;              // per net
    std::vector<std::uint8_t> anchor_;               // per net: bit s = a fixed pin on side s
    std::vector<std::array<std::uint32_t, 2>> cnt_;  // per net: pins on each side
    std::vector<std::uint32_t> regionNets_;
};

void RatioPlacer::Impl::build() {
    const std::size_t nv = graph_.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getCell(v);
        if (vert.isFixed || vert.isTerminal) {
            // Bookshelf marks fixed macros as terminals (every one of adaptec1's 543
            // is), so a "fixed and not terminal" test finds no obstacles at all.
            if (vert.width > 0.0 && vert.height > 0.0) {
                fixedRects_.push_back({vert.x, vert.y, vert.x + vert.width, vert.y + vert.height});
            }
            continue;
        }
        mov_.push_back(static_cast<std::uint32_t>(v));
        area_.push_back(vert.width * vert.height);
    }
    res_.numMovable = mov_.size();
    res_.numFixed = fixedRects_.size();

    std::vector<std::uint32_t> indexOf(nv, kNoIndex);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        indexOf[mov_[i]] = static_cast<std::uint32_t>(i);
    }

    nets_.clear();
    cellNets_.assign(mov_.size(), {});
    // A block with two pins on one net is still one block of that net; counted
    // twice, its side count never drops to 1 and FM never sees it un-cut the net.
    std::vector<std::size_t> lastNet(mov_.size(), std::numeric_limits<std::size_t>::max());
    for (std::size_t v = 0; v < graph_.getNumNets(); ++v) {
        HyperNet n;
        for (const std::size_t pinId : graph_.getNetPins(v)) {
            const Pin &e = graph_.getPin(pinId);
            const Vertex &pin = graph_.getCell(e.cellId);
            if (indexOf[e.cellId] != kNoIndex) {
                const std::uint32_t c = indexOf[e.cellId];
                if (lastNet[c] != v) {
                    lastNet[c] = v;
                    n.cells.push_back(c);
                }
                continue;
            }
            // Bookshelf pin offsets are from the cell's centre.
            const double px = pin.x + 0.5 * pin.width + e.offsetX;
            const double py = pin.y + 0.5 * pin.height + e.offsetY;
            if (n.fixedPins == 0) {
                n.fx0 = n.fx1 = px;
                n.fy0 = n.fy1 = py;
            } else {
                n.fx0 = std::min(n.fx0, px);
                n.fx1 = std::max(n.fx1, px);
                n.fy0 = std::min(n.fy0, py);
                n.fy1 = std::max(n.fy1, py);
            }
            ++n.fixedPins;
        }
        // A net with one movable block still matters when it has fixed pins: it is
        // what pulls that block towards a pad or macro.
        if (n.cells.empty() || n.cells.size() + n.fixedPins < 2) {
            continue;
        }
        const std::uint32_t id = static_cast<std::uint32_t>(nets_.size());
        for (const std::uint32_t c : n.cells) {
            cellNets_[c].push_back(id);
        }
        nets_.push_back(std::move(n));
    }
    res_.nets = nets_.size();

    localGen_.assign(mov_.size(), 0);
    localOf_.assign(mov_.size(), 0);
    netGen_.assign(nets_.size(), 0);
    anchor_.assign(nets_.size(), 0);
    cnt_.assign(nets_.size(), {0, 0});
    gen_ = 0;

    box_ = {std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
            -std::numeric_limits<double>::max(), -std::numeric_limits<double>::max()};
    for (const RowInfo &r : db_.getRows()) {
        for (const auto &sr : r.subrows) {
            const AreaRect rr{sr.xlo(), r.coordinate, sr.xhi(r.pitch()), r.coordinate + r.height};
            if (rr.x1 <= rr.x0) {
                continue;
            }
            rowRects_.push_back(rr);
            box_[0] = std::min(box_[0], rr.x0);
            box_[1] = std::min(box_[1], rr.y0);
            box_[2] = std::max(box_[2], rr.x1);
            box_[3] = std::max(box_[3], rr.y1);
        }
    }
    if (rowRects_.empty()) {
        box_ = db_.placementDieBox();  // no rows: the die is all there is
    }
    // Sorted by y so freeAreaIn only visits the rows a region spans, not all of
    // them on every cut.
    std::sort(rowRects_.begin(), rowRects_.end(), [](const AreaRect &a, const AreaRect &b) {
        return a.y0 < b.y0;
    });
    maxRowH_ = 0.0;
    for (const AreaRect &r : rowRects_) {
        maxRowH_ = std::max(maxRowH_, r.y1 - r.y0);
    }
    posX_.assign(mov_.size(), 0.5 * (box_[0] + box_[2]));
    posY_.assign(mov_.size(), 0.5 * (box_[1] + box_[3]));
}

double RatioPlacer::Impl::freeAreaIn(double x0, double y0, double x1, double y1) const {
    if (rowRects_.empty()) {
        return std::max(0.0, (x1 - x0) * (y1 - y0));
    }
    double rows = 0.0;
    auto it = std::lower_bound(rowRects_.begin(), rowRects_.end(), y0 - maxRowH_,
                               [](const AreaRect &r, double y) {
                                   return r.y0 < y;
                               });
    for (; it != rowRects_.end() && it->y0 < y1; ++it) {
        rows += overlap(*it, x0, y0, x1, y1);
    }
    // Fixed blocks only take capacity where they sit on rows; the rows are a solid
    // band in every benchmark here, so clipping to the rows' bounding box is what
    // keeps a pad outside the rows from being charged against a region.
    const double cx0 = std::max(x0, box_[0]), cy0 = std::max(y0, box_[1]);
    const double cx1 = std::min(x1, box_[2]), cy1 = std::min(y1, box_[3]);
    double fixed = 0.0;
    if (cx1 > cx0 && cy1 > cy0) {
        for (const AreaRect &f : fixedRects_) {
            fixed += overlap(f, cx0, cy0, cx1, cy1);
        }
    }
    return std::max(0.0, rows - fixed);
}

double RatioPlacer::Impl::movableArea(const std::vector<std::uint32_t> &cells) const {
    double a = 0.0;
    for (const std::uint32_t c : cells) {
        a += area_[c];
    }
    return a;
}

std::size_t RatioPlacer::Impl::bipartition(const std::vector<std::uint32_t> &cells, bool vertical,
                                           double cut, double target, double tol,
                                           std::vector<std::uint8_t> &side) {
    const std::size_t n = cells.size();
    side.assign(n, 0);
    ++gen_;
    for (std::uint32_t i = 0; i < n; ++i) {
        localGen_[cells[i]] = gen_;
        localOf_[cells[i]] = i;
    }

    // The region's nets, and where each one's outside pins pull. A pin outside the
    // region -- a fixed pin, or a movable already sent to another region -- is a
    // block locked on its side of the cut (the paper's dummy node). A net with
    // outside pins on both sides is cut whatever this partition does, so it is
    // left out of the gains entirely.
    regionNets_.clear();
    for (std::uint32_t i = 0; i < n; ++i) {
        for (const std::uint32_t e : cellNets_[cells[i]]) {
            if (netGen_[e] == gen_) {
                continue;
            }
            netGen_[e] = gen_;
            regionNets_.push_back(e);
            const HyperNet &net = nets_[e];
            std::uint8_t mask = 0;
            for (const std::uint32_t c : net.cells) {
                if (localGen_[c] == gen_) {
                    continue;
                }
                const double p = vertical ? posX_[c] : posY_[c];
                mask |= (p < cut) ? 1 : (p > cut) ? 2 : 0;
            }
            if (net.fixedPins > 0) {
                const double lo = vertical ? net.fx0 : net.fy0;
                const double hi = vertical ? net.fx1 : net.fy1;
                mask |= (hi < cut) ? 1 : (lo > cut) ? 2 : 3;
            }
            anchor_[e] = mask;
        }
    }

    // Initial partition: fill side 0 up to the target in netlist order. FM's
    // passes are what make it a min-cut; this only has to be balanced.
    double a0 = 0.0;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (a0 + 0.5 * area_[cells[i]] <= target) {
            a0 += area_[cells[i]];
            side[i] = 0;
        } else {
            side[i] = 1;
        }
    }

    const auto gainOf = [&](std::uint32_t i) {
        const std::uint8_t s = side[i], t = 1 - s;
        int g = 0;
        for (const std::uint32_t e : cellNets_[cells[i]]) {
            if (anchor_[e] == 3) {
                continue;
            }
            if (cnt_[e][s] == 1) {
                ++g;  // the last pin on its side: moving it un-cuts the net
            }
            if (cnt_[e][t] == 0) {
                --g;  // the net was whole on this side and moving it cuts it
            }
        }
        return g;
    };
    const auto dev = [&](double a) {
        return std::fabs(a - target);
    };

    std::vector<int> gain(n, 0);
    std::vector<char> locked(n, 0);
    std::vector<std::uint32_t> moves;
    moves.reserve(n);
    using Entry = std::pair<int, std::uint32_t>;

    for (int pass = 0; pass < kMaxPasses; ++pass) {
        for (const std::uint32_t e : regionNets_) {
            cnt_[e] = {static_cast<std::uint32_t>((anchor_[e] & 1) ? 1 : 0),
                       static_cast<std::uint32_t>((anchor_[e] & 2) ? 1 : 0)};
        }
        for (std::uint32_t i = 0; i < n; ++i) {
            for (const std::uint32_t e : cellNets_[cells[i]]) {
                ++cnt_[e][side[i]];
            }
        }
        std::priority_queue<Entry> heap[2];
        for (std::uint32_t i = 0; i < n; ++i) {
            gain[i] = gainOf(i);
            locked[i] = 0;
            heap[side[i]].push({gain[i], i});
        }

        moves.clear();
        int cum = 0, bestCum = 0;
        std::size_t bestLen = 0;
        double bestDev = dev(a0);
        double cur = a0;
        while (true) {
            // The best unlocked block on each side; stale heap entries (locked, or
            // pushed before a later gain change) are discarded on the way.
            int pick = -1;
            int pickGain = std::numeric_limits<int>::min();
            for (int s = 0; s < 2; ++s) {
                auto &h = heap[s];
                while (!h.empty() &&
                       (locked[h.top().second] || gain[h.top().second] != h.top().first ||
                        side[h.top().second] != s)) {
                    h.pop();
                }
                if (h.empty()) {
                    continue;
                }
                const std::uint32_t i = h.top().second;
                const double na = (s == 0) ? cur - area_[cells[i]] : cur + area_[cells[i]];
                // A move may leave the window only towards the target; with the
                // window wider than two blocks, at most one side is ever blocked.
                if (dev(na) > tol && dev(na) >= dev(cur)) {
                    continue;
                }
                if (h.top().first > pickGain) {
                    pickGain = h.top().first;
                    pick = s;
                }
            }
            if (pick < 0) {
                break;
            }
            const std::uint32_t i = heap[pick].top().second;
            heap[pick].pop();
            const std::uint8_t s = side[i], t = 1 - s;
            locked[i] = 1;
            side[i] = t;
            cur += (s == 0) ? -area_[cells[i]] : area_[cells[i]];
            cum += pickGain;
            moves.push_back(i);

            for (const std::uint32_t e : cellNets_[cells[i]]) {
                if (anchor_[e] == 3) {
                    continue;
                }
                // Only a net whose counts cross 0 or 1 changes anyone's gain (the
                // usual FM critical-net test), so large nets are rarely rescanned.
                const bool critical = cnt_[e][t] <= 1 || cnt_[e][s] <= 2;
                --cnt_[e][s];
                ++cnt_[e][t];
                if (!critical) {
                    continue;
                }
                for (const std::uint32_t c : nets_[e].cells) {
                    if (localGen_[c] != gen_) {
                        continue;
                    }
                    const std::uint32_t j = localOf_[c];
                    if (locked[j]) {
                        continue;
                    }
                    const int g = gainOf(j);
                    if (g != gain[j]) {
                        gain[j] = g;
                        heap[side[j]].push({g, j});
                    }
                }
            }
            if (dev(cur) <= tol && (cum > bestCum || (cum == bestCum && dev(cur) < bestDev))) {
                bestCum = cum;
                bestLen = moves.size();
                bestDev = dev(cur);
            }
        }
        // Keep the best prefix of the pass, undo the rest.
        for (std::size_t k = moves.size(); k > bestLen; --k) {
            const std::uint32_t i = moves[k - 1];
            cur += (side[i] == 0) ? -area_[cells[i]] : area_[cells[i]];
            side[i] = 1 - side[i];
        }
        a0 = cur;
        if (bestLen == 0) {
            break;  // no improving prefix: a further pass would find the same
        }
    }

    // Cut size of the result, for the trace.
    for (const std::uint32_t e : regionNets_) {
        cnt_[e] = {static_cast<std::uint32_t>((anchor_[e] & 1) ? 1 : 0),
                   static_cast<std::uint32_t>((anchor_[e] & 2) ? 1 : 0)};
    }
    for (std::uint32_t i = 0; i < n; ++i) {
        for (const std::uint32_t e : cellNets_[cells[i]]) {
            ++cnt_[e][side[i]];
        }
    }
    std::size_t cutNets = 0;
    for (const std::uint32_t e : regionNets_) {
        if (anchor_[e] != 3 && cnt_[e][0] > 0 && cnt_[e][1] > 0) {
            ++cutNets;
        }
    }
    return cutNets;
}

void RatioPlacer::Impl::setRegionCentre(const RatioRegion &r) {
    const double cx = 0.5 * (r.x0 + r.x1), cy = 0.5 * (r.y0 + r.y1);
    for (const std::uint32_t c : r.cells) {
        posX_[c] = cx;
        posY_[c] = cy;
    }
}

void RatioPlacer::Impl::placeLeaf(const RatioRegion &r) {
    // Spread a leaf's blocks over a grid in its region rather than stacking them on
    // its centre: the legalizer then moves each a short way instead of fanning a
    // pile of dozens out of one point.
    const std::size_t k = r.cells.size();
    if (k == 0) {
        return;
    }
    const double w = std::max(r.x1 - r.x0, 1e-9), h = std::max(r.y1 - r.y0, 1e-9);
    const std::size_t cols =
        std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(std::sqrt(k * w / h))));
    const std::size_t rowsN = (k + cols - 1) / cols;
    for (std::size_t q = 0; q < k; ++q) {
        const std::size_t cx = q % cols, cy = q / cols;
        posX_[r.cells[q]] = r.x0 + w * (static_cast<double>(cx) + 0.5) / static_cast<double>(cols);
        posY_[r.cells[q]] = r.y0 + h * (static_cast<double>(cy) + 0.5) / static_cast<double>(rowsN);
    }
}

void RatioPlacer::Impl::writeFrame(std::size_t depth, const char *note) const {
    // Frames go through the run's animator, the same budgeted sink every other
    // stage uses; past its cap record() is a no-op, so do not build the arrays.
    PlacementAnimator &anim = PlacementAnimator::instance();
    if (!anim.enabled() || anim.capped()) {
        return;
    }
    std::vector<float> xs(graph_.getNumCells(), 0.0f);
    std::vector<float> ys(graph_.getNumCells(), 0.0f);
    for (std::size_t v = 0; v < graph_.getNumCells(); ++v) {
        xs[v] = static_cast<float>(graph_.getCell(v).x);
        ys[v] = static_cast<float>(graph_.getCell(v).y);
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const Vertex &v = graph_.getCell(mov_[i]);
        xs[mov_[i]] = static_cast<float>(posX_[i] - 0.5 * v.width);
        ys[mov_[i]] = static_cast<float>(posY_[i] - 0.5 * v.height);
    }
    const std::array<double, 4> box = db_.placementDieBox();
    anim.record(graph_, xs, ys, box, depth, params_.maxLevels, 0.0, 0.0, 0.0, note, fences_,
                /*mandatory=*/false);
}

void RatioPlacer::Impl::divide(RatioRegion r) {
    res_.maxDepth = std::max(res_.maxDepth, r.depth);
    if (r.cells.size() <= params_.targetLeafCells || r.depth >= params_.maxLevels) {
        if (!r.cells.empty()) {
            res_.minLeafCells = (res_.minLeafCells == 0)
                                    ? r.cells.size()
                                    : std::min(res_.minLeafCells, r.cells.size());
        }
        placeLeaf(r);
        return;
    }

    // Cut the longer side through its middle, so sub-regions do not degenerate
    // into slivers.
    const double w = r.x1 - r.x0;
    const double h = r.y1 - r.y0;
    const bool vertical = (w >= h);
    const double cut = vertical ? r.x0 + 0.5 * w : r.y0 + 0.5 * h;

    // Geometry only: the cells are dealt out after the partition, so copying the
    // parent's list into both children first would be wasted work at every level.
    RatioRegion r0, r1;
    r0.x0 = r1.x0 = r.x0;
    r0.y0 = r1.y0 = r.y0;
    r0.x1 = r1.x1 = r.x1;
    r0.y1 = r1.y1 = r.y1;
    r0.depth = r1.depth = r.depth + 1;
    (vertical ? r0.x1 : r0.y1) = cut;
    (vertical ? r1.x0 : r1.y0) = cut;

    const double free0 = freeAreaIn(r0.x0, r0.y0, r0.x1, r0.y1);
    const double free1 = freeAreaIn(r1.x0, r1.y0, r1.x1, r1.y1);
    if (free0 <= 0.0 || free1 <= 0.0) {
        // One half has no room at all (it is all macro, or off the rows): there is
        // nothing to partition, so the region just shrinks to the other half.
        RatioRegion &keep = (free0 > 0.0) ? r0 : r1;
        if (free0 <= 0.0 && free1 <= 0.0) {
            placeLeaf(r);
            return;
        }
        keep.cells = std::move(r.cells);
        setRegionCentre(keep);
        divide(std::move(keep));
        return;
    }

    // Whitespace distribution (Section 2.2): each side gets cell area in
    // proportion to the room it has, so both halves end up equally utilised. A
    // fixed 50/50 split, whatever the halves can hold, is what overfills the half
    // with the macro in it.
    const double total = movableArea(r.cells);
    const double target = total * free0 / (free0 + free1);
    double maxCell = 0.0;
    for (const std::uint32_t c : r.cells) {
        maxCell = std::max(maxCell, area_[c]);
    }
    double tol = std::max(kBalanceSlack * total, 2.0 * maxCell);

    std::vector<std::uint8_t> side;
    std::size_t cutNets = 0;
    for (std::size_t attempt = 0;; ++attempt) {
        cutNets = bipartition(r.cells, vertical, cut, target, tol, side);
        r0.cells.clear();
        r1.cells.clear();
        for (std::size_t i = 0; i < r.cells.size(); ++i) {
            (side[i] == 0 ? r0.cells : r1.cells).push_back(r.cells[i]);
        }
        // Look-ahead (Section 2.3): a side the cut leaves fuller than it can hold
        // is one the legalizer would have to empty. Unless the region as a whole is
        // overfull -- then no cut can help -- retry with a tighter window.
        const double a0 = movableArea(r0.cells), a1 = total - a0;
        const bool fits = (a0 <= free0 && a1 <= free1) || total > free0 + free1;
        if (fits || attempt >= params_.maxRatioRetries || tol <= 2.0 * maxCell) {
            break;
        }
        ++res_.ratioRetries;
        tol = std::max(0.5 * tol, 2.0 * maxCell);
    }

    ++res_.cuts;
    // One line per accepted cut: the recursion's heartbeat. The trace file is what
    // the web console streams into its trace pane, so it is written whether or not
    // -v is set.
    ktlog.trace("  ratio cut depth {}: {} cells -> {} / {} (target {:.1f}%), {} nets cut", r.depth,
                r.cells.size(), r0.cells.size(), r1.cells.size(), 100.0 * free0 / (free0 + free1),
                cutNets);
    setRegionCentre(r0);
    setRegionCentre(r1);
    writeFrame(r.depth, "ratio bipartition");
    divide(std::move(r0));
    divide(std::move(r1));
}

RatioPlaceResult RatioPlacer::Impl::place(const RatioPlaceParams &params) {
    params_ = params;
    fences_ = &db_.constraints();
    build();
    if (mov_.empty()) {
        return res_;
    }
    ktlog.trace("  ratio build: {} movable cells, {} fixed blocks, {} hypernets", res_.numMovable,
                res_.numFixed, res_.nets);

    // The paper starts every block at the centre of the placement area: with
    // nothing decided, that is the only placement that implies no cut, and the
    // whole recursion then reads the solution out of the cuts.
    RatioRegion root;
    root.x0 = box_[0];
    root.y0 = box_[1];
    root.x1 = box_[2];
    root.y1 = box_[3];
    root.cells.resize(mov_.size());
    std::iota(root.cells.begin(), root.cells.end(), 0u);
    divide(std::move(root));

    // posX_/posY_ are centres; the database holds lower-left corners, kept inside
    // the placement area.
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const Vertex &v = graph_.getCell(mov_[i]);
        const double x =
            std::clamp(posX_[i] - 0.5 * v.width, box_[0], std::max(box_[0], box_[2] - v.width));
        const double y =
            std::clamp(posY_[i] - 0.5 * v.height, box_[1], std::max(box_[1], box_[3] - v.height));
        db_.setCellPosition(mov_[i], x, y);
    }

    // HPWL over the answer, from cell centres and fixed pin points: the
    // partitioner's own estimate. The flow reports the pin-exact figure after
    // legalization.
    double total = 0.0;
    for (const HyperNet &n : nets_) {
        double ax = std::numeric_limits<double>::max(), bx = -ax, ay = ax, by = -ax;
        for (const std::uint32_t c : n.cells) {
            ax = std::min(ax, posX_[c]);
            bx = std::max(bx, posX_[c]);
            ay = std::min(ay, posY_[c]);
            by = std::max(by, posY_[c]);
        }
        if (n.fixedPins > 0) {
            ax = std::min(ax, n.fx0);
            bx = std::max(bx, n.fx1);
            ay = std::min(ay, n.fy0);
            by = std::max(by, n.fy1);
        }
        total += (bx - ax) + (by - ay);
    }
    res_.hpwlFinal = total;
    res_.meanImbalance =
        res_.cuts > 0 ? static_cast<double>(res_.ratioRetries) / static_cast<double>(res_.cuts)
                      : 0.0;
    ktlog.trace("  ratio placement: {} cuts, {} retries, {} levels, HPWL {:.6e}", res_.cuts,
                res_.ratioRetries, res_.maxDepth, res_.hpwlFinal);
    return res_;
}

RatioPlacer::RatioPlacer(ktDM &db) : pImpl(new Impl(db)) {}
RatioPlacer::~RatioPlacer() = default;
RatioPlacer::RatioPlacer(RatioPlacer &&) noexcept = default;
RatioPlacer &RatioPlacer::operator=(RatioPlacer &&) noexcept = default;

RatioPlaceResult RatioPlacer::place(const RatioPlaceParams &params) {
    return pImpl->place(params);
}

void reportNtuPlace1(const RatioPlaceResult &r) {
    ktReportTable t("NTUplace1 solver results");
    t.setHeaders({"metric", "value"});
    t.addRow({"movable cells", fmt::format("{}", r.numMovable)});
    t.addRow({"fixed blocks", fmt::format("{}", r.numFixed)});
    t.addRow({"hypergraph nets", fmt::format("{}", r.nets)});
    t.addRow({"cuts accepted", fmt::format("{}", r.cuts)});
    t.addRow({"look-ahead retries", fmt::format("{}", r.ratioRetries)});
    t.addRow({"retries per cut", fmt::format("{:.3}", r.meanImbalance)});
    t.addRow({"recursion depth reached", fmt::format("{}", r.maxDepth)});
    t.addRow({"smallest leaf", fmt::format("{}", r.minLeafCells)});
    t.addRow({"HPWL estimate (pre-legalization)", fmt::format("{:.6}", r.hpwlFinal)});
    t.emit();
}
}  // namespace ktplace
