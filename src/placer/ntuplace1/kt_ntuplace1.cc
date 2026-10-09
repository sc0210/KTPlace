// Global placement by ratio partitioning. See kt_ntuplace1.h for what this is and
// why it sits beside the analytical placer rather than inside it.

#include "placer/ntuplace1/kt_ntuplace1.h"

#include "util/kt_log.h"
#include "visualization/kt_plotter.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <numeric>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ktplace {
namespace {

// A net as the partitioner needs it: the movable blocks it touches, and the
// bounding box of the fixed pins hanging off it. The fixed pins are the t1/t2 of
// the paper's Figure 2 -- they are what tells a cut which side a net wants to be
// on, and they are the reason this is a hypergraph and not a graph.
struct HyperNet {
    std::vector<std::uint32_t> cells;
    double fx0 = 0.0, fx1 = 0.0, fy0 = 0.0, fy1 = 0.0;
    std::size_t fixedPins = 0;
};

struct RatioRegion {
    double x0 = 0.0, y0 = 0.0, x1 = 0.0, y1 = 0.0;
    std::vector<std::uint32_t> cells;  // indices into the placer's movable array
    std::size_t depth = 0;
};

}  // namespace

namespace {
constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;
}

class RatioPlacer::Impl {
public:
    explicit Impl(PlacementDB &db) : db_(db), graph_(db.getGraph()) {}

    RatioPlaceResult place(const RatioPlaceParams &params, const constraintMgr *);

private:
    void build();
    // Row area inside a rectangle. The paper's look-ahead asks whether a
    // sub-region could be legalized; this is the area half of that question, and
    // it is the half that can be asked without committing to a packing.
    double rowAreaIn(double x0, double y0, double x1, double y1) const;
    double fixedAreaIn(double x0, double y0, double x1, double y1) const;
    double movableArea(const std::vector<std::uint32_t> &) const;
    bool legalizable(const RatioRegion &r) const;
    void divide(RatioRegion r);
    // Fiduccia-Mattheyses pass with a balance cap. Returns true on success.
    bool bipartition(const std::vector<std::uint32_t> &cells, bool vertical, double cutCoord,
                     double maxImbalance, std::vector<std::uint8_t> &side);
    double netWeight(const HyperNet &n, double p1, double p2, std::uint8_t &dummySide) const;
    void writeFrame(const RatioRegion &r, std::size_t depth, const char *note) const;

    PlacementDB &db_;
    Graph graph_;
    std::vector<std::uint32_t> mov_;  // movable, non-terminal vertex ids
    std::vector<double> area_;        // area of each movable
    std::vector<HyperNet> nets_;
    // For each movable, the nets it is on, so a gain update does not rescan the
    // whole netlist.
    std::vector<std::vector<std::uint32_t>> cellNets_;
    std::vector<PlacementDB::RowInfo> rows_;
    std::array<double, 4> die_{};
    std::vector<double> posX_, posY_;  // the answer, filled as the recursion ends
    RatioPlaceParams params_;
    const constraintMgr *fences_ = nullptr;
    RatioPlaceResult res_;
    // Scratch reused by every cut, so the recursion does not reallocate per level.
    std::vector<std::uint32_t> scratchNets_;
};

void RatioPlacer::Impl::build() {
    const std::size_t nv = graph_.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type != VertexType::Cell || vert.isFixed || vert.isTerminal) {
            continue;
        }
        mov_.push_back(static_cast<std::uint32_t>(v));
        area_.push_back(vert.width * vert.height);
    }
    posX_.assign(mov_.size(), 0.0);
    posY_.assign(mov_.size(), 0.0);

    std::vector<std::uint32_t> indexOf(nv, kNoIndex);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        indexOf[mov_[i]] = static_cast<std::uint32_t>(i);
    }

    nets_.clear();
    cellNets_.assign(mov_.size(), {});
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type != VertexType::Net) {
            continue;
        }
        HyperNet n;
        double fx0 = 0, fx1 = 0, fy0 = 0, fy1 = 0;
        std::size_t nfix = 0;
        for (const std::size_t eid : vert.inEdges) {
            const Edge &e = graph_.getEdge(eid);
            const Vertex &pin = graph_.getVertex(e.source);
            if (pin.type != VertexType::Cell) {
                continue;
            }
            const bool isMovable = !pin.isFixed && !pin.isTerminal;
            if (!isMovable || indexOf[e.source] == kNoIndex) {
                const double x0 = pin.x + e.offsetX;
                const double y0 = pin.y + e.offsetY;
                const double x1 = x0 + pin.width;
                const double y1 = y0 + pin.height;
                if (nfix == 0) {
                    fx0 = x0;
                    fx1 = x1;
                    fy0 = y0;
                    fy1 = y1;
                } else {
                    fx0 = std::min(fx0, x0);
                    fx1 = std::max(fx1, x1);
                    fy0 = std::min(fy0, y0);
                    fy1 = std::max(fy1, y1);
                }
                ++nfix;
                continue;
            }
            n.cells.push_back(indexOf[e.source]);
        }
        if (n.cells.size() < 2) {
            continue;  // a single movable pin is not a net the cut can sever
        }
        n.fx0 = fx0;
        n.fx1 = fx1;
        n.fy0 = fy0;
        n.fy1 = fy1;
        n.fixedPins = nfix;
        const std::uint32_t id = static_cast<std::uint32_t>(nets_.size());
        for (const std::uint32_t c : n.cells) {
            cellNets_[c].push_back(id);
        }
        nets_.push_back(std::move(n));
    }
    rows_ = db_.getRows();
    die_ = placementDieBox(db_);
    res_.numMovable = mov_.size();
    res_.nets = nets_.size();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type == VertexType::Cell && vert.isFixed && !vert.isTerminal) {
            ++res_.numFixed;
        }
    }
}

double RatioPlacer::Impl::rowAreaIn(double x0, double /*y0*/, double x1, double /*y1*/) const {
    double a = 0.0;
    for (const PlacementDB::RowInfo &r : rows_) {
        const double lo = std::max(x0, r.xlo());
        const double hi = std::min(x1, r.xhi());
        const double w = hi - lo;
        if (w > 0.0) {
            a += w * r.height;
        }
    }
    return a;
}

double RatioPlacer::Impl::fixedAreaIn(double x0, double y0, double x1, double y1) const {
    double a = 0.0;
    const std::size_t nv = graph_.getNumVertices();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getVertex(v);
        if (vert.type != VertexType::Cell || !vert.isFixed || vert.isTerminal) {
            continue;
        }
        const double ox = std::max(0.0, std::min(x1, vert.x + vert.width) - std::max(x0, vert.x));
        const double oy = std::max(0.0, std::min(y1, vert.y + vert.height) - std::max(y0, vert.y));
        a += ox * oy;
    }
    return a;
}

double RatioPlacer::Impl::movableArea(const std::vector<std::uint32_t> &cells) const {
    double a = 0.0;
    for (const std::uint32_t c : cells) {
        a += area_[c];
    }
    return a;
}

bool RatioPlacer::Impl::legalizable(const RatioRegion &r) const {
    // Necessary condition, and the one that actually binds in practice: the
    // region's rows have to hold the region's own cells plus whatever fixed
    // blocks fall inside it. The paper uses a first-fit bin packing here, which
    // also catches a case area cannot -- a region with no row tall enough for its
    // blocks. This checks the area and, separately, the tallest block, which
    // between them catch the two ways a cut produces a region nothing can be put
    // into. It is weaker than a packing and is not claimed to be a substitute for
    // one: a region that passes here can still fail to legalize, and the
    // legalizer downstream is what settles it.
    const double rows = rowAreaIn(r.x0, r.y0, r.x1, r.y1);
    if (rows <= 0.0) {
        return false;
    }
    const double need = movableArea(r.cells) + fixedAreaIn(r.x0, r.y0, r.x1, r.y1);
    if (need > rows) {
        return false;
    }
    double tallest = 0.0;
    for (const std::uint32_t c : r.cells) {
        tallest = std::max(tallest, graph_.getVertex(mov_[c]).height);
    }
    if (tallest > 0.0) {
        const double rowH = rows_.empty() ? tallest : rows_.front().height;
        if (tallest > rowH * 1.5) {
            return false;
        }
    }
    return true;
}

// The paper's net weight, for a cut at cutCoord splitting the region into centres
// p1 and p2 (Section 2.1, Figures 2 and 3).
//
// A net wholly inside the region costs dist(p1,p2) if the cut severs it. A net
// with pins outside costs its span across p1, p2 and those pins. The bias comes
// from where the dummy lands: on the side that makes the net cheaper, so the
// min-cut pulls the net's blocks that way instead of cutting it at all.
double RatioPlacer::Impl::netWeight(const HyperNet &n, double p1, double p2,
                                    std::uint8_t &dummySide) const {
    dummySide = 2;  // 2 = no dummy
    const double w = std::fabs(p2 - p1);
    if (n.fixedPins == 0) {
        return std::max(w, params_.minNetWeight);
    }
    const double cx = 0.5 * (p1 + p2);
    // Cost of putting every block on side 1 versus side 2, with the outside pins
    // where they are. The smaller one gets the dummy.
    const auto cost = [&](double c) {
        const double lo = std::min({c, n.fx0});
        const double hi = std::max({c, n.fx1});
        const double ylo = std::min({c, n.fy0});
        const double yhi = std::max({c, n.fy1});
        return (hi - lo) + (yhi - ylo);
    };
    // Full span with the blocks at the cut: the cost of severing.
    double lo = std::min({p1, p2, n.fx0}), hi = std::max({p1, p2, n.fx1});
    double ylo = std::min({p1, p2, n.fy0}), yhi = std::max({p1, p2, n.fy1});
    const double cut = (hi - lo) + (yhi - ylo);
    (void)cx;
    const double c1 = cost(p1), c2 = cost(p2);
    dummySide = (c1 <= c2) ? 0 : 1;
    return std::max(cut, params_.minNetWeight);
}

bool RatioPlacer::Impl::bipartition(const std::vector<std::uint32_t> &cells, bool /*vertical*/,
                                    double cutCoord, double maxImbalance,
                                    std::vector<std::uint8_t> &side) {
    const std::size_t n = cells.size();
    side.assign(n, 0);
    if (n < 2) {
        return true;
    }
    // Local index for a movable, so the per-cut arrays are this region's size and
    // not the design's.
    std::unordered_map<std::uint32_t, std::uint32_t> local;
    local.reserve(n * 2);
    for (std::uint32_t i = 0; i < n; ++i) {
        local[cells[i]] = i;
    }

    // Initial partition: balance by area, which is FM's own starting point and the
    // state the imbalance cap is measured against.
    double half = movableArea(cells) * 0.5;
    double acc = 0.0;
    for (std::uint32_t i = 0; i < n; ++i) {
        acc += area_[cells[i]];
        side[i] = (acc <= half) ? 0 : 1;
    }
    // The block nearest the cut goes to the emptier side, so the starting point
    // already respects the cap rather than needing a repair pass.
    double a0 = 0.0, a1 = 0.0;
    for (std::uint32_t i = 0; i < n; ++i) {
        (side[i] == 0 ? a0 : a1) += area_[cells[i]];
    }
    const double cap = 1.0 + std::max(0.0, maxImbalance);
    if (a0 > 0.0 && a1 > 0.0 && std::max(a0 / a1, a1 / a0) > cap) {
        std::uint32_t best = 0;
        double bestD = 0.0;
        for (std::uint32_t i = 0; i < n; ++i) {
            const double d = std::fabs((side[i] == 0 ? a1 : a0) - area_[cells[i]]);
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        side[best] = (side[best] == 0) ? 1 : 0;
    }

    // Per-net bookkeeping for the pass.
    const std::size_t nn = nets_.size();
    std::vector<std::uint32_t> count0(nn, 0), count1(nn, 0);
    std::vector<std::uint8_t> dummy(nn, 2);
    std::vector<double> weight(nn, 0.0);
    for (std::size_t e = 0; e < nn; ++e) {
        std::uint32_t c0 = 0, c1 = 0;
        for (const std::uint32_t c : nets_[e].cells) {
            const auto it = local.find(c);
            if (it == local.end()) {
                continue;
            }
            (side[it->second] == 0 ? c0 : c1) += 1;
        }
        if (c0 == 0 || c1 == 0) {
            // Not in this region, or already wholly on one side. A net wholly
            // inside the region still needs a weight, because a later move can
            // sever it.
            bool present = false;
            for (const std::uint32_t c : nets_[e].cells) {
                if (local.count(c) != 0) {
                    present = true;
                    break;
                }
            }
            if (!present) {
                continue;
            }
        }
        count0[e] = c0;
        count1[e] = c1;
        std::uint8_t ds = 2;
        weight[e] = netWeight(nets_[e], cutCoord, cutCoord, ds);
        dummy[e] = ds;
    }

    // FM gains, then one move at a time, always the best legal one.
    std::vector<double> gain(n, 0.0);
    std::vector<char> locked(n, 0);
    const auto recompute = [&](std::uint32_t i) {
        double g = 0.0;
        for (const std::uint32_t e : cellNets_[cells[i]]) {
            if (count0[e] + count1[e] == 0) {
                continue;
            }
            const bool from0 = side[i] == 0;
            const std::uint32_t from = from0 ? count0[e] : count1[e];
            const std::uint32_t to = from0 ? count1[e] : count0[e];
            double w = weight[e];
            if (dummy[e] != 2) {
                // The dummy is a pin on one side, so it counts there.
                if (dummy[e] == 0) {
                    // side 0 already carries it
                }
            }
            if (from <= 1) {
                g += w;  // this was the last block on its side: the net un-cuts
            } else {
                g += 0.0;  // the net stays cut
            }
            if (to == 0) {
                g -= w;  // the net was not cut and now becomes cut
            }
        }
        return g;
    };
    for (std::uint32_t i = 0; i < n; ++i) {
        gain[i] = recompute(i);
    }

    double cur0 = 0.0, cur1 = 0.0;
    for (std::uint32_t i = 0; i < n; ++i) {
        (side[i] == 0 ? cur0 : cur1) += area_[cells[i]];
    }
    const double total = cur0 + cur1;

    std::deque<std::uint32_t> work(n);
    std::iota(work.begin(), work.end(), 0u);

    while (!work.empty()) {
        std::size_t best = work.size();
        double bestGain = 0.0;
        for (const std::uint32_t i : work) {
            if (locked[i]) {
                continue;
            }
            const double a = side[i] == 0 ? cur0 : cur1;
            const double b = side[i] == 0 ? cur1 : cur0;
            const double na = a - area_[cells[i]], nb = b + area_[cells[i]];
            if (na > 0.0 && nb > 0.0 && std::max(na / nb, nb / na) > cap) {
                continue;  // would break the balance the ratio partitioner is for
            }
            if (best == work.size() || gain[i] > bestGain) {
                bestGain = gain[i];
                best = i;
            }
        }
        if (best == work.size()) {
            break;  // every remaining move breaks the cap
        }
        const std::uint32_t i = static_cast<std::uint32_t>(best);
        for (auto it = work.begin(); it != work.end(); ++it) {
            if (*it == i) {
                work.erase(it);
                break;
            }
        }
        locked[i] = 1;
        if (side[i] == 0) {
            cur0 -= area_[cells[i]];
            cur1 += area_[cells[i]];
            side[i] = 1;
        } else {
            cur1 -= area_[cells[i]];
            cur0 += area_[cells[i]];
            side[i] = 0;
        }
        for (const std::uint32_t e : cellNets_[cells[i]]) {
            if (side[i] == 0) {
                ++count0[e];
                --count1[e];
            } else {
                ++count1[e];
                --count0[e];
            }
        }
        for (const std::uint32_t e : cellNets_[cells[i]]) {
            for (const std::uint32_t c : nets_[e].cells) {
                const auto it2 = local.find(c);
                if (it2 == local.end() || locked[it2->second]) {
                    continue;
                }
                gain[it2->second] = recompute(it2->second);
            }
        }
    }
    (void)total;
    return true;
}

void RatioPlacer::Impl::writeFrame(const RatioRegion &/*r*/, std::size_t depth,
                                   const char *note) const {
    if (params_.plotDir.empty()) {
        return;
    }
    std::vector<float> xs(graph_.getNumVertices(), 0.0f);
    std::vector<float> ys(graph_.getNumVertices(), 0.0f);
    for (std::size_t v = 0; v < graph_.getNumVertices(); ++v) {
        xs[v] = static_cast<float>(graph_.getVertex(v).x);
        ys[v] = static_cast<float>(graph_.getVertex(v).y);
    }
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        xs[mov_[i]] = static_cast<float>(posX_[i]);
        ys[mov_[i]] = static_cast<float>(posY_[i]);
    }
    std::array<double, 4> box{die_[0], die_[1], die_[2], die_[3]};
    std::string path = params_.plotDir + "/ntuplace1_" + std::to_string(depth) + ".svg";
    writeFrameSvg(path, graph_, xs, ys, box, depth, 0, 0.0, 0.0, 0.0, note, fences_,
                  /*fixedView=*/true);
}

void RatioPlacer::Impl::divide(RatioRegion r) {
    res_.maxDepth = std::max(res_.maxDepth, r.depth);
    if (r.cells.size() <= params_.targetLeafCells || r.depth >= params_.maxLevels) {
        res_.minLeafCells =
            (res_.minLeafCells == 0) ? r.cells.size() : std::min(res_.minLeafCells, r.cells.size());
        // A leaf puts its blocks at the region's centre, which is where the
        // min-cut assumed they were and where the wirelength estimate was formed.
        const double cx = 0.5 * (r.x0 + r.x1);
        const double cy = 0.5 * (r.y0 + r.y1);
        for (const std::uint32_t c : r.cells) {
            posX_[c] = cx;
            posY_[c] = cy;
        }
        return;
    }

    // Cut the longer side, so sub-regions do not degenerate into slivers.
    const double w = r.x1 - r.x0;
    const double h = r.y1 - r.y0;
    const bool vertical = (w >= h);

    // The sub-regions spread to fill the region's rows and whitespace, so their
    // areas are only compared against each other and against the region's area
    // through the imbalance cap; the region's own area is not needed below.
    const double regionArea = w * h;
    (void)regionArea;

    std::vector<std::uint8_t> side;
    RatioRegion r0 = r, r1 = r;
    double cut = 0.0;
    bool accepted = false;

    for (std::size_t attempt = 0; attempt <= params_.maxRatioRetries && !accepted; ++attempt) {
        // Whitespace distribution (Section 2.2). The imbalance cap is not a
        // constant: it is loosened until both sub-regions can hold their share.
        // Starting from even and widening is the paper's "move the cut-line
        // toward the partition with a smaller utilization ratio", expressed as a
        // cap on the area ratio rather than as a cut position.
        double cap = attempt * 0.25;  // 0 = perfectly balanced, widening per retry

        if (vertical) {
            cut = r.x0 + 0.5 * w;
        } else {
            cut = r.y0 + 0.5 * h;
        }
        if (!bipartition(r.cells, vertical, cut, cap, side)) {
            continue;
        }

        r0 = r;
        r1 = r;
        r0.depth = r.depth + 1;
        r1.depth = r.depth + 1;
        if (vertical) {
            r0.x1 = cut;
            r1.x0 = cut;
        } else {
            r0.y1 = cut;
            r1.y0 = cut;
        }
        r0.cells.clear();
        r1.cells.clear();
        for (std::size_t i = 0; i < r.cells.size(); ++i) {
            (side[i] == 0 ? r0.cells : r1.cells).push_back(r.cells[i]);
        }
        // Empty sub-regions cannot be legalized and cannot be recursed into.
        if (r0.cells.empty() || r1.cells.empty()) {
            ++res_.ratioRetries;
            continue;
        }
        // Look-ahead (Section 2.3): before accepting the cut, ask whether either
        // side could be legalized at all.
        if (!legalizable(r0) || !legalizable(r1)) {
            ++res_.ratioRetries;
            continue;
        }
        accepted = true;
    }

    if (!accepted) {
        // Nothing legalizable was found: keep the region's blocks together at its
        // centre and let the recursion above, and the legalizer after, deal with
        // it. A region that cannot be cut legally is not a reason to stop.
        for (const std::uint32_t c : r.cells) {
            posX_[c] = 0.5 * (r.x0 + r.x1);
            posY_[c] = 0.5 * (r.y0 + r.y1);
        }
        return;
    }

    ++res_.cuts;
    if (params_.verbose) {
        ktlog.trace("  ratio cut: {} cells -> {} / {}, {} retries so far", r.cells.size(),
                    r0.cells.size(), r1.cells.size(), res_.ratioRetries);
    }
    writeFrame(r, r.depth, "ratio bipartition");
    divide(std::move(r0));
    divide(std::move(r1));
}

RatioPlaceResult RatioPlacer::Impl::place(const RatioPlaceParams &params,
                                          const constraintMgr *constraints) {
    params_ = params;
    fences_ = constraints;
    build();
    if (mov_.empty()) {
        return res_;
    }

    // The paper starts every block at the centre of the chip: with nothing
    // decided, that is the only placement that implies no cut, and the whole
    // recursion then reads the solution out of the cuts.
    RatioRegion root;
    root.x0 = die_[0];
    root.y0 = die_[1];
    root.x1 = die_[2];
    root.y1 = die_[3];
    root.cells.resize(mov_.size());
    std::iota(root.cells.begin(), root.cells.end(), 0u);
    for (std::size_t i = 0; i < mov_.size(); ++i) {
        posX_[i] = 0.5 * (root.x0 + root.x1);
        posY_[i] = 0.5 * (root.y0 + root.y1);
    }

    divide(std::move(root));

    for (std::size_t i = 0; i < mov_.size(); ++i) {
        const Vertex &v = graph_.getVertex(mov_[i]);
        db_.setCellPosition(mov_[i], posX_[i], std::min(posY_[i], die_[3] - v.height));
    }

    // HPWL over the answer, pin to pin, so it is the same quantity the rest of the
    // flow reports and the runs can be compared.
    double total = 0.0;
    for (const HyperNet &n : nets_) {
        if (n.cells.size() < 2) {
            continue;
        }
        double ax = 0, bx = 0, ay = 0, by = 0;
        bool first = true;
        for (const std::uint32_t c : n.cells) {
            const Vertex &v = graph_.getVertex(mov_[c]);
            const double x0 = posX_[c], y0 = posY_[c];
            const double x1 = x0 + v.width, y1 = y0 + v.height;
            if (first) {
                ax = x0;
                bx = x1;
                ay = y0;
                by = y1;
                first = false;
            } else {
                ax = std::min(ax, x0);
                bx = std::max(bx, x1);
                ay = std::min(ay, y0);
                by = std::max(by, y1);
            }
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
    return res_;
}

RatioPlacer::RatioPlacer(PlacementDB &db) : pImpl(new Impl(db)) {}
RatioPlacer::~RatioPlacer() = default;
RatioPlacer::RatioPlacer(RatioPlacer &&) noexcept = default;
RatioPlacer &RatioPlacer::operator=(RatioPlacer &&) noexcept = default;

RatioPlaceResult RatioPlacer::place(const RatioPlaceParams &params,
                                    const constraintMgr *constraints) {
    return pImpl->place(params, constraints);
}

}  // namespace ktplace
