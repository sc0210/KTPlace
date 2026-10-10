// Global placement by ratio partitioning, after the ISPD'05 NTUPlace paper
// (Chen, Hsu, Jiang and Chang, "NTUplace: A Ratio Partitioning Based Placement
// Algorithm for Large-Scale Mixed-Size Designs").
//
// A peer of SimplePlacer, not a variant of it. SimPL is analytical: a quadratic
// solve with the look-ahead legalizer as its upper bound. This one is
// partitioning-based: blocks are recursively bipartitioned and each block is put
// at the centre of the sub-region it lands in, so the placement is a product of
// cuts rather than of an optimisation. The two share the tail -- both hand an
// overfull placement to the same legalizer and detailed placer -- so the
// difference measured between them is the global placement, which is the point of
// having both.
//
// Each cut is a Fiduccia-Mattheyses min-cut, with three ideas from the paper on
// top that make it a ratio partitioner rather than a balanced one:
//
//   * whitespace distribution: each side's share of the cell area is set by the
//     free row area it has (rows less the fixed blocks on them), so both halves
//     end up equally utilised and a half that is mostly macro gets few cells;
//   * terminal propagation: a pin outside the region -- a fixed pin, or a block
//     already sent to another region -- acts as a block locked on its side of
//     the cut (the paper's dummy node), so a net is pulled towards its outside
//     pins instead of being cut at random;
//   * look-ahead: a cut that leaves either side fuller than it can hold is
//     redone with a tighter balance window, so the recursion does not paint
//     itself into a corner the legalizer then has to rescue.
//
// The placement area is the rows' bounding box, not the die box: the die also
// spans pads and macros outside the rows, where no cell can go.

#pragma once

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ktplace {

struct RatioPlaceParams {
    // Recursion stops when a region holds no more than this many blocks. The paper
    // leaves it as "a threshold"; small enough that the leaves are genuinely small,
    // large enough that the cut count stays sane.
    std::size_t targetLeafCells = 32;
    // Backstop on depth. A region is halved on its longer side each level, so
    // depth is bounded by log2 of the die extent in bins; this only bites if the
    // ratio constraint drives a cut to a sliver, which the look-ahead check is
    // meant to prevent but which a pathological design can still provoke.
    std::size_t maxLevels = 40;
    // How many times a cut may be redone when the look-ahead check finds a side
    // fuller than it can hold. Each retry halves the balance window.
    std::size_t maxRatioRetries = 24;
    // Set to log per-region decisions.
    bool verbose = false;
};

struct RatioPlaceResult {
    std::size_t numMovable = 0;
    std::size_t numFixed = 0;
    std::size_t nets = 0;
    // Cuts accepted, and cuts redone because the look-ahead check found a side
    // fuller than it could hold.
    std::size_t cuts = 0;
    std::size_t ratioRetries = 0;
    std::size_t maxDepth = 0;
    // Smallest leaf, i.e. the tightest the recursion packed a region.
    std::size_t minLeafCells = 0;
    double hpwlFinal = 0.0;
    // Look-ahead retries per accepted cut. A run that needed many is one whose
    // balance was fighting the design, and that is worth seeing.
    double meanImbalance = 0.0;
};

// The partitioner's own summary, emitted from the component that produced it.
void reportNtuPlace1(const RatioPlaceResult &res);

class RatioPlacer {
public:
    explicit RatioPlacer(ktDM &db);
    ~RatioPlacer();

    RatioPlacer(const RatioPlacer &) = delete;
    RatioPlacer &operator=(const RatioPlacer &) = delete;
    RatioPlacer(RatioPlacer &&) noexcept;
    RatioPlacer &operator=(RatioPlacer &&) noexcept;

    // Run global placement and write the result into the database. The placement
    // handed back is deliberately overfull: the paper's global placement is, and
    // the legalizer is a separate step in both this paper and in this flow.
    RatioPlaceResult place(const RatioPlaceParams &params = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
