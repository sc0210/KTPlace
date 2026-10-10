// @file kt_multiRowLegalizer.cc
// Legalizer that can place cells taller than one row

#include "legalizer/kt_multiRowLegalizer.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_plotter.h"

#include <algorithm>
#include <cmath>
#include <fmt/format.h>
#include <limits>
#include <vector>

namespace ktplace {

namespace {

/// A free rectangle inside one row: an x range, the y range it covers, and how
/// much of it has been committed from the left.
///
/// `ylo` is not always the row's base. Slicing leaves segments that sit *under*
/// a cell taller than the row, and those start above it -- which is exactly how a
/// later cell finds the space beneath a tall one.
struct Segment {
    double xlo = 0.0;
    double xhi = 0.0;
    /// Origin of the site grid this segment was cut from. A legal cell sits at
    /// origin + k*pitch, so this is what a placement snaps to.
    double originX = 0.0;
    double ylo = 0.0;
    double yhi = 0.0;
    double used = 0.0;

    [[nodiscard]] double width() const {
        return xhi - xlo;
    }
    [[nodiscard]] double free() const {
        return xhi - used;
    }
};

struct Track {
    double y = 0.0;
    double height = 0.0;
    double pitch = 1.0;
    std::vector<Segment> segments;  // sorted by xlo, disjoint in x

    [[nodiscard]] double freeWidth() const {
        double total = 0.0;
        for (const Segment &s : segments) {
            total += s.free();
        }
        return total;
    }

    /// The segment wholly containing [x0, x1], or nullptr.
    [[nodiscard]] const Segment *covering(double x0, double x1) const {
        for (const Segment &s : segments) {
            if (s.xlo <= x0 + 1e-9 && s.xhi >= x1 - 1e-9) {
                return &s;
            }
        }
        return nullptr;
    }
};

/// A cell waiting to be placed, in the placer's own indexing.
struct Item {
    std::uint32_t vertex = 0;
    double w = 0.0;
    double h = 0.0;
    double area = 0.0;
    double x = 0.0;  // global-placement position
    double y = 0.0;
};

constexpr double kSiteEps = 1e-6;

}  // namespace

// ---------------------------------------------------------------------------

class MultiRowLegalizer::Impl {
public:
    explicit Impl(ktDM &db) : db_(db), graph_(db.getGraph()) {}

    MultiRowLegalizeResult run(const MultiRowLegalizeParams &params);

private:
    void buildTracks();
    void collect();
    /// Cut [x0,x1] x [y0,y1] out of every track it touches. This is the whole
    /// mechanism: a cell that spans several rows leaves a segment under itself and
    /// one either side, and everything placed afterwards sees those segments as
    /// ordinary free space.
    void carve(const Item &cell);
    void carveBox(double x0, double y0, double x1, double y1);
    /// Index of the track whose y range contains `y`, or the closest one.
    [[nodiscard]] std::size_t trackNear(double y) const;
    /// Can a cell of this size sit with its base on track `t` and left edge `x`?
    [[nodiscard]] bool fits(std::size_t t, double x, double w, double h) const;
    /// Nearest legal site to `x`.
    [[nodiscard]] double snap(double x, double originX, double pitch) const;
    /// A candidate placement: a left edge and the grid origin to snap it to.
    struct Candidate {
        double x = 0.0;
        double originX = 0.0;
    };

    /// Left edges worth trying on this track.
    [[nodiscard]] std::vector<Candidate> candidates(std::size_t t) const;
    void selfCheck(MultiRowLegalizeResult &res) const;
    [[nodiscard]] double hpwl() const;
    void writeFrame(const std::string &path, const std::string &note, std::size_t step,
                    std::size_t total);

    ktDM &db_;
    const Graph &graph_;

    std::vector<Track> tracks_;
    std::vector<Item> items_;
    std::vector<std::size_t> order_;  // placement order: largest area first

    double dieX0_ = 0.0;
    double dieY0_ = 0.0;
    double dieX1_ = 0.0;
    double dieY1_ = 0.0;
    std::array<double, 4> die_{};
    const constraintMgr *fences_ = nullptr;
    std::size_t frames_ = 0;
};

void MultiRowLegalizer::Impl::buildTracks() {
    tracks_.clear();
    for (const RowInfo &ri : db_.getRows()) {
        if (!(ri.height > 0.0) || ri.subrows.empty()) {
            continue;
        }
        Track t;
        t.y = ri.coordinate;
        t.height = ri.height;
        t.pitch = ri.pitch();
        for (const SubrowInfo &si : ri.subrows) {
            Segment s;
            s.xlo = si.xlo();
            s.xhi = si.xhi(ri.pitch());
            s.originX = si.xlo();
            s.ylo = ri.coordinate;
            s.yhi = ri.coordinate + ri.height;
            s.used = s.xlo;
            if (s.width() > kSiteEps) {
                t.segments.push_back(s);
            }
        }
        if (!t.segments.empty()) {
            tracks_.push_back(std::move(t));
        }
    }
    std::sort(tracks_.begin(), tracks_.end(), [](const Track &a, const Track &b) {
        return a.y < b.y;
    });
}

void MultiRowLegalizer::Impl::collect() {
    items_.clear();
    const std::size_t nv = graph_.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getCell(v);
        if (vert.isFixed || vert.isTerminal) {
            continue;
        }
        if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
            continue;
        }
        Item item;
        item.vertex = static_cast<std::uint32_t>(v);
        item.w = vert.width;
        item.h = vert.height;
        item.area = vert.width * vert.height;
        item.x = vert.x;
        item.y = vert.y;
        items_.push_back(item);
    }
    // Largest first, as the reference placer does: a big cell that cannot be
    // placed later is worth discovering while the design is still loose.
    order_.resize(items_.size());
    for (std::size_t i = 0; i < order_.size(); ++i) {
        order_[i] = i;
    }
    std::sort(order_.begin(), order_.end(), [&](std::size_t a, std::size_t b) {
        return items_[a].area > items_[b].area;
    });
}

void MultiRowLegalizer::Impl::carveBox(double x0, double y0, double x1, double y1) {
    if (tracks_.empty()) {
        return;
    }
    // Tracks are sorted by y, so the affected ones are a contiguous run.
    auto first = std::lower_bound(tracks_.begin(), tracks_.end(), y0, [](const Track &t, double v) {
        return t.y + t.height < v - kSiteEps;
    });
    for (auto it = first; it != tracks_.end(); ++it) {
        if (it->y > y1 - kSiteEps) {
            break;
        }
        const double overlapY0 = std::max(y0, it->y);
        const double overlapY1 = std::min(y1, it->y + it->height);
        if (overlapY1 - overlapY0 <= kSiteEps) {
            continue;
        }

        std::vector<Segment> next;
        next.reserve(it->segments.size() + 2);
        for (const Segment &s : it->segments) {
            if (s.xhi <= x0 + kSiteEps || s.xlo >= x1 - kSiteEps) {
                next.push_back(s);  // no x overlap: untouched
                continue;
            }
            // (a) left of the box
            if (s.xlo < x0 - kSiteEps) {
                Segment left = s;
                left.xhi = x0;
                left.used = std::min(left.used, left.xhi);
                next.push_back(left);
            }
            // (b) under the box, if the box starts above this row's base
            if (overlapY0 > it->y + kSiteEps) {
                Segment under = s;
                under.xlo = std::max(s.xlo, x0);
                under.xhi = std::min(s.xhi, x1);
                under.ylo = s.ylo;
                under.yhi = overlapY0;
                under.used = under.xlo;
                if (under.width() > kSiteEps) {
                    next.push_back(under);
                }
            }
            // (c) right of the box
            if (s.xhi > x1 + kSiteEps) {
                Segment right = s;
                right.xlo = x1;
                right.used = std::max(right.used, right.xlo);
                next.push_back(right);
            }
        }
        std::sort(next.begin(), next.end(), [](const Segment &a, const Segment &b) {
            return a.xlo < b.xlo;
        });
        it->segments = std::move(next);
    }
}

void MultiRowLegalizer::Impl::carve(const Item &cell) {
    carveBox(cell.x, cell.y, cell.x + cell.w, cell.y + cell.h);
}

std::size_t MultiRowLegalizer::Impl::trackNear(double y) const {
    if (tracks_.empty()) {
        return 0;
    }
    // The last track whose base is at or below y, else the first.
    std::size_t lo = 0;
    std::size_t hi = tracks_.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (tracks_[mid].y <= y + kSiteEps) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return (lo == 0) ? 0 : lo - 1;
}

double MultiRowLegalizer::Impl::snap(double x, double originX, double pitch) const {
    if (!(pitch > 0.0)) {
        return x;
    }
    // Nearest site. A cell is legal only at origin + k*pitch, and an unsnapped
    // left edge is the commonest way a placement comes out unusable even with no
    // overlaps reported.
    return originX + std::round((x - originX) / pitch) * pitch;
}

bool MultiRowLegalizer::Impl::fits(std::size_t t, double x, double w, double h) const {
    if (t >= tracks_.size()) {
        return false;
    }
    if (x < dieX0_ - kSiteEps || x + w > dieX1_ + kSiteEps) {
        return false;
    }
    // The candidate position has its base on the track, not where the cell came
    // from: the whole question is whether this row can take it.
    const double base = tracks_[t].y;
    const double top = base + h;
    if (top <= base) {
        return true;
    }

    std::size_t i = t;
    while (i < tracks_.size() && top > tracks_[i].y + kSiteEps) {
        const Track &tr = tracks_[i];
        const double spanLo = std::max(base, tr.y);
        const double spanHi = std::min(top, tr.y + tr.height);
        if (spanHi - spanLo > kSiteEps) {
            const Segment *seg = tr.covering(x, x + w);
            if (seg == nullptr || seg->ylo > spanLo + kSiteEps || seg->yhi < spanHi - kSiteEps) {
                return false;
            }
        }
        ++i;
    }
    // i > t keeps this from reading tracks_[t - 1] when the cell stops inside the
    // base row, and t == 0 has no row below it.
    return i > t && top <= tracks_[i - 1].y + tracks_[i - 1].height + kSiteEps;
}

std::vector<MultiRowLegalizer::Impl::Candidate> MultiRowLegalizer::Impl::candidates(
    std::size_t t) const {
    std::vector<Candidate> xs;
    if (t >= tracks_.size()) {
        return xs;
    }
    for (const Segment &s : tracks_[t].segments) {
        xs.push_back({s.xlo, s.originX});
        if (s.used > s.xlo + kSiteEps && s.used < s.xhi - kSiteEps) {
            xs.push_back({s.used, s.originX});
        }
    }
    return xs;
}

MultiRowLegalizeResult MultiRowLegalizer::Impl::run(const MultiRowLegalizeParams &params) {
    MultiRowLegalizeResult res;
    ScopedTimer timer("legalize");
    res.hpwlBefore = db_.hpwl();

    const std::array<double, 4> box = db_.placementDieBox();
    die_ = box;
    dieX0_ = box[0];
    dieY0_ = box[1];
    dieX1_ = box[2];
    dieY1_ = box[3];
    fences_ = &db_.constraints();

    buildTracks();
    collect();
    res.movable = items_.size();
    if (tracks_.empty() || items_.empty()) {
        res.seconds = timer.elapsedSeconds();
        return res;
    }

    // The row height that decides what "multi-row" means. Rows may differ, so the
    // shortest row is the safe test: a cell taller than every row needs slicing,
    // and one taller than the shortest may still fit a taller row.
    double minRow = std::numeric_limits<double>::max();
    for (const Track &t : tracks_) {
        minRow = std::min(minRow, t.height);
    }
    const double tall = minRow * 1.5;

    // Fixed blocks first: they are immovable, so the free space has to be
    // described around them before anything else competes for it.
    const std::size_t nv = graph_.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getCell(v);
        if (!vert.isFixed || vert.isTerminal) {
            continue;
        }
        if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
            continue;
        }
        carveBox(vert.x, vert.y, vert.x + vert.width, vert.y + vert.height);
    }

    for (std::size_t i : order_) {
        Item &cell = items_[i];
        const bool isTall = cell.h > tall;
        if (isTall) {
            ++res.multiRow;
        }

        // Search outward from the track the cell came from, and stop as soon as
        // the rows are further away vertically than the best placement found so
        // far: a cell cannot be improved by moving further than it already would
        // have to move. That bound is what keeps this linear in practice.
        const std::size_t home = trackNear(cell.y);
        double bestCost = std::numeric_limits<double>::max();
        double bestX = 0.0;
        std::size_t bestTrack = home;
        bool found = false;

        for (std::size_t d = 0; d < tracks_.size(); ++d) {
            const bool down = d <= home;
            const std::size_t t = down ? home - d : home + d;
            if (t >= tracks_.size()) {
                break;
            }
            const double dy = std::fabs(tracks_[t].y - cell.y);
            if (found && dy >= bestCost) {
                break;  // every remaining row is further away than the best so far
            }
            for (const Candidate &cand : candidates(t)) {
                const double x = snap(cand.x, cand.originX, tracks_[t].pitch);
                if (!fits(t, x, cell.w, cell.h)) {
                    continue;
                }
                const double cost = dy + std::fabs(x - cell.x);
                if (cost < bestCost) {
                    bestCost = cost;
                    bestX = x;
                    bestTrack = t;
                    found = true;
                }
            }
        }

        if (!found) {
            ++res.unplaced;
            continue;
        }

        // Keeping the cell's own x is worth a small amount of vertical movement:
        // a placement that only had to change rows preserves wirelength far better
        // than one that also slid sideways to a segment edge.
        {
            const Segment *seg = tracks_[bestTrack].covering(cell.x, cell.x + cell.w);
            const double origin = seg != nullptr ? seg->originX : dieX0_;
            const double snapped = snap(cell.x, origin, tracks_[bestTrack].pitch);
            if (fits(bestTrack, snapped, cell.w, cell.h)) {
                bestX = snapped;
            }
        }
        cell.x = bestX;
        cell.y = tracks_[bestTrack].y;
        db_.setCellPosition(cell.vertex, cell.x, cell.y);
        carve(cell);
        ++res.placed;
        if (isTall) {
            ++res.multiRowPlaced;
        }

        if (!params.plotDir.empty() && params.frameEvery > 0 &&
            (res.placed % params.frameEvery) == 0) {
            writeFrame(params.plotDir + "/legalize_multi_" + std::to_string(res.placed) + ".svg",
                       "multi-row legalization", res.placed, items_.size());
        }
    }

    selfCheck(res);
    res.hpwlAfter = db_.hpwl();
    res.seconds = timer.elapsedSeconds();
    return res;
}

void MultiRowLegalizer::Impl::selfCheck(MultiRowLegalizeResult &res) const {
    const std::size_t nv = graph_.getNumCells();
    double rowHeight = 0.0;
    double pitch = 1.0;
    if (!tracks_.empty()) {
        rowHeight = tracks_.front().height;
        pitch = tracks_.front().pitch;
    }

    std::size_t offRow = 0;
    std::size_t offSite = 0;
    std::size_t overFixed = 0;
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph_.getCell(v);
        if (vert.isFixed || vert.isTerminal) {
            continue;
        }
        // A cell belongs to a row when its base sits on that row's base line.
        // Testing the base rather than "inside one band" is what makes this work
        // for a cell taller than a row: such a cell is deliberately not inside any
        // single band, and counting that as an off-row cell reported every legal
        // placement of a tall cell as illegal.
        const std::size_t t = trackNear(vert.y);
        const bool onRow = !tracks_.empty() && std::fabs(vert.y - tracks_[t].y) <= 1e-6;
        if (!onRow) {
            ++offRow;
        } else {
            // The grid origin belongs to the segment the cell actually sits in,
            // not to the first segment of the row: slicing moved that edge, and
            // measuring against it reports every carved row as off-site.
            const Segment *seg = tracks_[t].covering(vert.x, vert.x + vert.width);
            const double origin = seg != nullptr ? seg->originX : tracks_[t].segments.front().xlo;
            const double off = std::fmod(vert.x - origin, tracks_[t].pitch);
            if (off > 1e-3 && off < tracks_[t].pitch - 1e-3) {
                ++offSite;
            }
        }
        if (fences_ != nullptr && vert.regionId != constraintMgr::kNoRegion) {
            std::vector<double> flat{vert.x, vert.y};
            std::vector<int> ids{vert.regionId};
            overFixed += fences_->countViolations(flat, ids);
        }
    }
    res.offRow = offRow;
    res.offSite = offSite;
    res.overFixed = overFixed;

    // Overlaps, by a coarse grid. A full pairwise check is quadratic and this is
    // a self-check, not the run's verdict, so the grid is deliberately coarse --
    // but it is still bounded in both directions, because a bin size derived from
    // a small row pitch on a large die asks for a table of tens of millions of
    // buckets to count one number.
    std::size_t overlaps = 0;
    const double spanX = std::max(dieX1_ - dieX0_, 1.0);
    const double spanY = std::max(dieY1_ - dieY0_, 1.0);
    const double target = std::max(std::max(pitch, rowHeight), std::sqrt(spanX * spanY / 4e6));
    const std::size_t nbx = std::min<std::size_t>(
        2048, std::max<std::size_t>(1, static_cast<std::size_t>(spanX / target)));
    const std::size_t nby = std::min<std::size_t>(
        2048, std::max<std::size_t>(1, static_cast<std::size_t>(spanY / target)));
    const double dx = spanX / static_cast<double>(nbx);
    const double dy = spanY / static_cast<double>(nby);
    const auto cellAt = [&](std::size_t v) -> const Vertex * {
        const Vertex &c = graph_.getCell(v);
        return (!c.isFixed && !c.isTerminal) ? &c : nullptr;
    };
    std::vector<std::vector<std::size_t>> buckets(nbx * nby);
    const std::size_t nv2 = graph_.getNumCells();
    for (std::size_t v = 0; v < nv2; ++v) {
        const Vertex *c = cellAt(v);
        if (c == nullptr) {
            continue;
        }
        const auto bx = static_cast<std::size_t>(
            std::clamp((c->x - dieX0_) / dx, 0.0, static_cast<double>(nbx) - 1.0));
        const auto by = static_cast<std::size_t>(
            std::clamp((c->y - dieY0_) / dy, 0.0, static_cast<double>(nby) - 1.0));
        // A cell wider than a bin reaches its neighbour, so a 3x3 neighbourhood
        // is what a bucket count has to consider.
        const std::size_t bx0 = (bx > 0) ? bx - 1 : 0;
        const std::size_t bx1 = std::min(bx + 2, nbx);
        const std::size_t by0 = (by > 0) ? by - 1 : 0;
        const std::size_t by1 = std::min(by + 2, nby);
        for (std::size_t yy = by0; yy < by1; ++yy) {
            for (std::size_t xx = bx0; xx < bx1; ++xx) {
                for (const std::size_t u : buckets[xx * nby + yy]) {
                    const Vertex &d = *cellAt(u);
                    if (c->x < d.x + d.width - 1e-9 && d.x < c->x + c->width - 1e-9 &&
                        c->y < d.y + d.height - 1e-9 && d.y < c->y + c->height - 1e-9) {
                        ++overlaps;
                    }
                }
            }
        }
        buckets[bx * nby + by].push_back(v);
    }
    res.overlappingPairs = overlaps / 2;
}

void MultiRowLegalizer::Impl::writeFrame(const std::string &path, const std::string &note,
                                         std::size_t step, std::size_t total) {
    ++frames_;
    const std::size_t nv = graph_.getNumCells();
    std::vector<float> xs(nv, 0.0f);
    std::vector<float> ys(nv, 0.0f);
    for (std::size_t v = 0; v < nv; ++v) {
        xs[v] = static_cast<float>(graph_.getCell(v).x);
        ys[v] = static_cast<float>(graph_.getCell(v).y);
    }
    writeFrameSvg(path, graph_, xs, ys, die_, step, total, db_.hpwl(), 0.0, 0.0, note, fences_,
                  /*fixedView=*/true);
}

double MultiRowLegalizer::Impl::hpwl() const {
    return db_.hpwl();
}

// ---------------------------------------------------------------------------

MultiRowLegalizer::MultiRowLegalizer(ktDM &db) : pImpl(std::make_unique<Impl>(db)) {}

MultiRowLegalizer::~MultiRowLegalizer() = default;

MultiRowLegalizer::MultiRowLegalizer(MultiRowLegalizer &&) noexcept = default;

MultiRowLegalizer &MultiRowLegalizer::operator=(MultiRowLegalizer &&) noexcept = default;

namespace {

void report(const MultiRowLegalizeResult &r) {
    ktReportTable t("Legalization (multi-row)");
    t.setHeaders({"metric", "value"});
    t.addRow({"movable cells", fmt::format("{}", r.movable)});
    t.addRow({"cells placed", fmt::format("{}", r.placed)});
    t.addRow({"cells unplaced", fmt::format("{}", r.unplaced)});
    t.addRow({"cells taller than a row", fmt::format("{}", r.multiRow)});
    t.addRow({"of those, placed", fmt::format("{}", r.multiRowPlaced)});
    t.addRow({"HPWL before", fmt::format("{:.6}", r.hpwlBefore)});
    t.addRow({"HPWL after", fmt::format("{:.6}", r.hpwlAfter)});
    t.addRow({"time (s)", fmt::format("{:.6}", r.seconds)});
    t.addRow({"overlapping pairs", fmt::format("{}", r.overlappingPairs)});
    t.addRow({"cells off row", fmt::format("{}", r.offRow)});
    t.addRow({"cells off site", fmt::format("{}", r.offSite)});
    t.addRow({"cells over macro", fmt::format("{}", r.overFixed)});
    t.emit();
    if (r.overlappingPairs != 0 || r.offRow != 0 || r.overFixed != 0) {
        ktlog.warning("legalization is not legal; see the counts above");
    }
    if (r.unplaced > 0) {
        ktlog.warning("{} cell(s) could not be placed in any row and kept their global placement",
                      r.unplaced);
    }
}

}  // namespace

MultiRowLegalizeResult MultiRowLegalizer::legalize(const MultiRowLegalizeParams &params) {
    MultiRowLegalizeResult result = pImpl->run(params);
    report(result);
    return result;
}

}  // namespace ktplace
