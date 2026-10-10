// @file kt_multiRowLegalizer.h
// Legalizer that can place cells taller than one row

#pragma once

#include "datamodel/kt_dm.h"

#include <memory>
#include <string>

namespace ktplace {

class constraintMgr;

struct MultiRowLegalizeParams {
    std::string plotDir;
    std::size_t frameEvery = 0;
};

struct MultiRowLegalizeResult {
    std::size_t movable = 0;
    std::size_t placed = 0;
    std::size_t unplaced = 0;
    /// Cells taller than one row, and how many of those got a position.
    std::size_t multiRow = 0;
    std::size_t multiRowPlaced = 0;

    std::size_t overlappingPairs = 0;
    std::size_t offRow = 0;
    std::size_t offSite = 0;
    std::size_t overFixed = 0;

    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    double seconds = 0.0;
};

/// Places movable cells into the rows, including cells that span several of them.
///
/// A single-row legalizer cannot: a cell of height h > row height has no row to go
/// in, so it is left where global placement put it, overlapping. This one slices
/// the rows instead of accepting them as they are. Slicing is what makes the
/// multi-row case fall out of the same machinery as the single-row case rather
/// than needing a second placer: a cell is cut out of every row it spans, leaving
/// a left segment, a right segment, and the space underneath it. A later cell
/// that fits in the resulting well is placed like any other cell.
class MultiRowLegalizer {
public:
    explicit MultiRowLegalizer(ktDM &db);
    ~MultiRowLegalizer();

    MultiRowLegalizer(const MultiRowLegalizer &) = delete;
    MultiRowLegalizer &operator=(const MultiRowLegalizer &) = delete;

    MultiRowLegalizer(MultiRowLegalizer &&) noexcept;
    MultiRowLegalizer &operator=(MultiRowLegalizer &&) noexcept;

    MultiRowLegalizeResult legalize(const MultiRowLegalizeParams &params = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
