// @file kt_fastdp.h// Fast detailed placement: global swap, vertical swap, local re-ordering,// single-segment clustering// Detailed placement runs after legalization. The legalizer minimises// displacement, which is not the same as minimising wirelength, so a legal// placement is usually a little worse than the global placement it came from and// this stage wins it back.// Implements the four techniques of Pan, Viswanathan and Chu, "Fast and// effective detailed placement", ICCAD 2005:// 1. Global swap. For each cell, the best x for it is the median of the x// coordinates its nets allow, which is the classic "median" move. If some// other cell already sits near that x and the two can trade places without// breaking legality, swap them.// 2. Vertical swap. The same exchange restricted to cells in adjacent rows,// which fixes cells that want to change row.// 3. Local re-ordering. Within a short window of a row, find the best left to// right ordering exactly, by a subset dynamic program.// 4. Single-segment clustering. With the order fixed, re-place a segment with// the legalizer's cluster dynamic program, which is the same optimisation// applied to one segment rather than a whole row.// Every technique preserves legality: a move is only applied if each cell stays// in its row band, on a site, and clear of its neighbours and of any macro.


#pragma once

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ktplace {

struct DetailPlaceParams {
    /// Number of global-swap sweeps. Each sweep is a full pass over all cells.
    std::size_t globalSwapPasses = 4;
    /// Number of vertical-swap sweeps.
    std::size_t verticalSwapPasses = 2;
    /// Number of local re-ordering passes.
    std::size_t localReorderPasses = 2;
    /// Length of the window local re-ordering re-orders, and the one knob that
    /// sets its price: the subset dynamic program costs 2^k per window. Measured
    /// on adaptec1, k=8 spends 44 of 61 seconds for -3.69% wirelength, k=6
    /// spends 19 of 36 for -3.06%. The 0.66% between them is a fifth of what
    /// legalization costs on the way in, so the bigger window is not worth it.
    std::size_t localReorderWindow = 6;
    /// Number of single-segment clustering passes.
    std::size_t clusterPasses = 2;
    /// Stop early once a pass improves HPWL by less than this fraction.
    double minImprovement = 1e-4;
    /// If non-empty, write an SVG frame per pass to this directory.
    std::string plotDir;
    /// Fence regions, drawn in the frames but not enforced by the optimiser.
};

struct DetailPlaceResult {
    std::size_t globalSwaps = 0;
    std::size_t verticalSwaps = 0;
    std::size_t reorderMoves = 0;
    /// Median moves: the O(cells x degree) pass, as against the window search.
    std::size_t medianMoves = 0;
    std::size_t clusterMoves = 0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    /// Legality self-check, so a detailed placement bug shows up as a number.
    std::size_t overlappingPairs = 0;
    std::size_t offRow = 0;
    std::size_t offSite = 0;
    std::size_t overFixed = 0;
    double seconds = 0.0;
};

class FastDetailedPlacer {
public:
    explicit FastDetailedPlacer(ktDM &db);
    ~FastDetailedPlacer();

    FastDetailedPlacer(const FastDetailedPlacer &) = delete;
    FastDetailedPlacer &operator=(const FastDetailedPlacer &) = delete;

    /// Improve the placement in place, writing positions back to the database.
    DetailPlaceResult place(const DetailPlaceParams &params = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
