// @file kt_constraintMgr.cc// Implementation of the placement region constraints


#include "constraint/kt_constraintMgr.h"

#include <algorithm>
#include <limits>

namespace ktplace {

int constraintMgr::addRegion(std::string name, const std::vector<Point> &points) {
    Region region;
    region.name = std::move(name);

    // The fence is a flat list of corners taken two at a time: lower-left then
    // upper-right of one rectangle. A trailing odd corner cannot form a
    // rectangle and is dropped.
    for (std::size_t i = 0; i + 1 < points.size(); i += 2) {
        Rect r;
        r.lo.x = std::min(points[i].x, points[i + 1].x);
        r.lo.y = std::min(points[i].y, points[i + 1].y);
        r.hi.x = std::max(points[i].x, points[i + 1].x);
        r.hi.y = std::max(points[i].y, points[i + 1].y);
        if (!(r.area() > 0.0)) {
            continue;  // degenerate rectangle
        }
        region.rects.push_back(r);
    }
    if (region.rects.empty()) {
        return kNoRegion;  // unusable as a fence
    }

    region.area = 0.0;
    for (const Rect &r : region.rects) {
        region.area += r.area();
    }
    region.minX = region.maxX = region.rects.front().lo.x;
    region.minY = region.maxY = region.rects.front().lo.y;
    for (const Rect &r : region.rects) {
        region.minX = std::min(region.minX, r.lo.x);
        region.maxX = std::max(region.maxX, r.hi.x);
        region.minY = std::min(region.minY, r.lo.y);
        region.maxY = std::max(region.maxY, r.hi.y);
    }

    const int id = static_cast<int>(regions_.size());
    byName_[region.name] = id;
    regions_.push_back(std::move(region));
    return id;
}

bool constraintMgr::contains(int id, double x, double y) const {
    if (id == kNoRegion) {
        return true;  // unconstrained cells are always legal
    }
    const Region *r = region(id);
    return r != nullptr && r->contains(x, y);
}

void constraintMgr::clampToRegion(int id, double &x, double &y) const {
    const Region *r = region(id);
    if (r == nullptr || r->contains(x, y)) {
        return;
    }
    // Snap into the rectangle that is nearest, measured as the distance still
    // to travel along each axis.
    const Rect *best = nullptr;
    double bestDist = std::numeric_limits<double>::max();
    for (const Rect &cand : r->rects) {
        const double cx = std::clamp(x, cand.lo.x, cand.hi.x);
        const double cy = std::clamp(y, cand.lo.y, cand.hi.y);
        const double dx = cx - x;
        const double dy = cy - y;
        const double dist = dx * dx + dy * dy;
        if (dist < bestDist) {
            bestDist = dist;
            best = &cand;
        }
    }
    if (best == nullptr) {
        return;
    }
    x = std::clamp(x, best->lo.x, best->hi.x);
    y = std::clamp(y, best->lo.y, best->hi.y);
}

double constraintMgr::escapeEpsilon() const {
    double scale = 0.0;
    for (const Region &r : regions_) {
        scale = std::max(scale, std::max(r.maxX - r.minX, r.maxY - r.minY));
    }
    return std::max(scale, 1.0) * 1e-6;
}

bool constraintMgr::insideAnyRegion(double x, double y) const {
    for (const Region &r : regions_) {
        if (r.contains(x, y)) {
            return true;
        }
    }
    return false;
}

bool constraintMgr::pushOutOfRegions(double &x, double &y, double dieMinX, double dieMinY,
                                     double dieMaxX, double dieMaxY) const {
    // Escaping has to be planned against the *union* of the fences, not one
    // rectangle at a time. The ISPD fences are often rings or combs of
    // abutting rectangles, so stepping to the nearest edge of the rectangle that
    // caught the point can drop it straight into the neighbour and ping-pong
    // forever. Each pass therefore picks the nearest of the four exits that
    // actually lands outside every region, which makes progress monotone.
    const std::size_t maxPasses = 4 * (regions_.size() + 1) + 8;
    for (std::size_t pass = 0; pass < maxPasses; ++pass) {
        const Rect *host = nullptr;
        double hostArea = std::numeric_limits<double>::max();
        for (const Region &r : regions_) {
            for (const Rect &cand : r.rects) {
                if (cand.contains(x, y) && cand.area() < hostArea) {
                    hostArea = cand.area();
                    host = &cand;
                }
            }
        }
        if (host == nullptr) {
            return pass > 0;  // clear of every fence
        }

        // The step has to be big enough to stay outside the fence once the
        // placement is written out and read back, so it is scaled to the
        // geometry rather than to the rectangle that caught the point: a
        // per-rectangle epsilon can be far below the output resolution, and the
        // cell then lands back exactly on the (inclusive) boundary.
        const double eps = escapeEpsilon();
        const double exits[4][2] = {{host->lo.x - eps, y},
                                    {host->hi.x + eps, y},
                                    {x, host->lo.y - eps},
                                    {x, host->hi.y + eps}};
        const double dists[4] = {x - host->lo.x, host->hi.x - x, y - host->lo.y, host->hi.y - y};

        int best = -1;
        double bestDist = std::numeric_limits<double>::max();
        bool bestLegal = false;
        for (int k = 0; k < 4; ++k) {
            const bool onDie = exits[k][0] >= dieMinX && exits[k][0] <= dieMaxX &&
                               exits[k][1] >= dieMinY && exits[k][1] <= dieMaxY;
            const bool legal = onDie && !insideAnyRegion(exits[k][0], exits[k][1]);
            if (legal && (!bestLegal || dists[k] < bestDist)) {
                best = k;
                bestDist = dists[k];
                bestLegal = true;
            }
        }
        if (best < 0) {
            // No side leads to legal die area (every side abuts another fence, or
            // the rest of the fence is off-die): take the shortest way out and
            // let the next pass continue from there.
            best = static_cast<int>(std::min_element(dists, dists + 4) - dists);
        }
        x = exits[best][0];
        y = exits[best][1];
    }
    return true;
}

std::size_t constraintMgr::assignByPrefix(int regionId, const std::string &prefix, Graph &graph) {
    if (regionId == kNoRegion || region(regionId) == nullptr || prefix.empty()) {
        return 0;
    }
    std::size_t assigned = 0;
    const std::size_t count = graph.getNumCells();
    for (std::size_t v = 0; v < count; ++v) {
        Vertex &vert = graph.getCell(v);
        if (vert.regionId != kNoRegion) {
            continue;  // first matching group wins
        }
        if (vert.name.compare(0, prefix.size(), prefix) == 0) {
            vert.regionId = regionId;
            ++assigned;
        }
    }
    if (regionId < static_cast<int>(regions_.size())) {
        regions_[regionId].cellCount = assigned;
    }
    return assigned;
}

std::size_t constraintMgr::countViolations(const std::vector<double> &positions,
                                           const std::vector<int> &regionIds) const {
    std::size_t violations = 0;
    const std::size_t count = std::min(positions.size() / 2, regionIds.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (!contains(regionIds[i], positions[2 * i], positions[2 * i + 1])) {
            ++violations;
        }
    }
    return violations;
}

}  // namespace ktplace
