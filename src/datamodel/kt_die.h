// @file kt_die.h
// Where a cell is allowed to go: the die box and the row structure.

#pragma once

#include "datamodel/kt_graph.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace ktplace {

/// One contiguous run of sites within a row. A row is split into subrows by
/// whatever blocks it, so placing into a row means choosing a subrow.
struct SubrowInfo {
    double originX = 0.0;
    double numSites = 0.0;

    [[nodiscard]] double xlo() const {
        return originX;
    }
    [[nodiscard]] double xhi(double spacing) const {
        return originX + numSites * spacing;
    }
};

struct RowInfo {
    double coordinate = 0.0;  ///< y of the row's bottom edge
    double height = 0.0;
    double sitewidth = 0.0;
    double sitespacing = 0.0;

    [[nodiscard]] double pitch() const {
        return (sitespacing > 0.0) ? sitespacing : sitewidth;
    }

    std::vector<SubrowInfo> subrows;

    [[nodiscard]] double xlo() const {
        return subrows.empty() ? 0.0 : subrows.front().xlo();
    }
    [[nodiscard]] double xhi() const {
        double hi = 0.0;
        for (const SubrowInfo &sr : subrows) {
            hi = std::max(hi, sr.xhi(pitch()));
        }
        return hi;
    }
};

/// The die box and its rows, as the reader parsed them.
class DieInfo {
public:
    void setDieArea(double xMin, double yMin, double xMax, double yMax);
    [[nodiscard]] std::pair<std::pair<double, double>, std::pair<double, double>> getDieArea()
        const;

    [[nodiscard]] std::size_t addRow(double coordinate, double height, double sitewidth,
                                     double sitespacing);
    [[nodiscard]] std::size_t addSubrow(std::size_t rowId, double originX, double numSites);

    [[nodiscard]] const std::vector<RowInfo> &getRows() const;
    [[nodiscard]] std::size_t getNumRows() const;

    void clear();

private:
    double xMin = 0.0;
    double yMin = 0.0;
    double xMax = 0.0;
    double yMax = 0.0;
    std::vector<RowInfo> rows;
};

/// {xMin, yMin, xMax, yMax}: the union of the declared die area, the bounding box
/// of the fixed cells, and the rows.
///
/// One definition, shared by the placer and by the legality check, because the
/// two disagreeing is how a legal placement gets reported as illegal. The rows
/// are unioned in rather than used only as a fallback, because for adaptec3 they
/// reach past the fixed cells -- its rows start at y=58 while its fixed cells
/// start at y=82 -- so a box built from the fixed cells alone excludes the bottom
/// row, and every cell the legalizer correctly put in that row is then reported
/// as outside the die.
[[nodiscard]] std::array<double, 4> placementDieBox(const DieInfo &die, const Graph &graph);

}  // namespace ktplace
