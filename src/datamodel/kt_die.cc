// @file kt_die.cc

#include "datamodel/kt_die.h"

#include <limits>

namespace ktplace {

void DieInfo::setDieArea(double minX, double minY, double maxX, double maxY) {
    xMin = minX;
    yMin = minY;
    xMax = maxX;
    yMax = maxY;
}

std::pair<std::pair<double, double>, std::pair<double, double>> DieInfo::getDieArea() const {
    return {{xMin, yMin}, {xMax, yMax}};
}

std::size_t DieInfo::addRow(double coordinate, double height, double sitewidth,
                            double sitespacing) {
    RowInfo row;
    row.coordinate = coordinate;
    row.height = height;
    row.sitewidth = sitewidth;
    row.sitespacing = sitespacing;
    rows.push_back(std::move(row));
    return rows.size() - 1;
}

std::size_t DieInfo::addSubrow(std::size_t rowId, double originX, double numSites) {
    if (rowId >= rows.size()) {
        return static_cast<std::size_t>(-1);
    }
    rows[rowId].subrows.push_back(SubrowInfo{originX, numSites});
    return rows[rowId].subrows.size() - 1;
}

const std::vector<RowInfo> &DieInfo::getRows() const {
    return rows;
}

std::size_t DieInfo::getNumRows() const {
    return rows.size();
}

void DieInfo::clear() {
    xMin = yMin = xMax = yMax = 0.0;
    rows.clear();
}

std::array<double, 4> placementDieBox(const DieInfo &die, const Graph &graph) {
    const double inf = std::numeric_limits<double>::max();
    // The fixed cells: the I/O pad ring bounds the die in a Bookshelf design.
    double lo[2] = {inf, inf};
    double hi[2] = {-inf, -inf};
    const std::size_t nv = graph.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph.getCell(v);
        if (!vert.isFixed && !vert.isTerminal) {
            continue;
        }
        lo[0] = std::min(lo[0], vert.x);
        lo[1] = std::min(lo[1], vert.y);
        hi[0] = std::max(hi[0], vert.x + std::max(vert.width, 1.0));
        hi[1] = std::max(hi[1], vert.y + std::max(vert.height, 1.0));
    }
    std::array<double, 4> box{lo[0], lo[1], hi[0], hi[1]};
    const bool haveFixed = (box[2] > box[0]) && (box[3] > box[1]);

    // A declared die area, when the format carries one and it contains every
    // fixed cell. A declared area that excludes a fixed cell is not describing the
    // same die the pads describe, so it is not trusted.
    const auto da = die.getDieArea();
    if (da.second.first > da.first.first && da.second.second > da.first.second) {
        bool contains = true;
        for (std::size_t v = 0; v < nv; ++v) {
            const Vertex &vert = graph.getCell(v);
            if (!vert.isFixed) {
                continue;
            }
            if (vert.x < da.first.first - 1.0 || vert.y < da.first.second - 1.0 ||
                vert.x + vert.width > da.second.first + 1.0 ||
                vert.y + vert.height > da.second.second + 1.0) {
                contains = false;
                break;
            }
        }
        if (contains) {
            box = {da.first.first, da.first.second, da.second.first, da.second.second};
        }
    }

    // The rows, unioned in. A cell in a row is legal by definition of a row.
    double rlo = inf, rhi = -inf, blo = inf, bhi = -inf;
    bool anyRow = false;
    for (const RowInfo &ri : die.getRows()) {
        if (!(ri.pitch() > 0.0)) {
            continue;
        }
        rlo = std::min(rlo, ri.coordinate);
        rhi = std::max(rhi, ri.coordinate + ri.height);
        blo = std::min(blo, ri.xlo());
        bhi = std::max(bhi, ri.xhi());
        anyRow = true;
    }
    if (anyRow && (bhi > blo) && (rhi > rlo)) {
        if (!haveFixed) {
            box = {blo, rlo, bhi, rhi};
        } else {
            box = {std::min(box[0], blo), std::min(box[1], rlo), std::max(box[2], bhi),
                   std::max(box[3], rhi)};
        }
    }
    return box;
}

}  // namespace ktplace
