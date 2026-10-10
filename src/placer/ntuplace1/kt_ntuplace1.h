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
// Three ideas from the paper are implemented, and they are the ones that make it
// a ratio partitioner rather than a balanced one:
//
//   * whitespace distribution sets the balance constraint per cut, so neither
//     side is overfilled before the cut is even made;
//   * net weighting prices each hyperedge by the wirelength it would add if the
//     cut severed it, with dummy nodes standing in for the pins outside the
//     region, so a net is pulled towards the side its outside pins are on;
//   * look-ahead bipartitioning asks whether a sub-region could be legalized at
//     all, and shifts the cut towards the emptier side until it could. This is
//     what stops the recursion from painting itself into a corner that the
//     legalizer then has to rescue.

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
    // Hyperedge weight floor. A net whose cut cost computes to zero -- every pin
    // coincident, or a net already entirely on one side -- would otherwise
    // contribute nothing to the min-cut and stop pulling.
    double minNetWeight = 1e-3;
    // How many times a cut may be re-balanced when the look-ahead check says the
    // sub-region cannot be legalized. Each retry moves the cut towards the
    // emptier side, so this is a bound on how far the ratio may be skewed.
    std::size_t maxRatioRetries = 24;
    // Set to log per-region decisions.
    bool verbose = false;
};

struct RatioPlaceResult {
    std::size_t numMovable = 0;
    std::size_t numFixed = 0;
    std::size_t nets = 0;
    // Cuts accepted, and cuts discarded and re-balanced because the look-ahead
    // check found the sub-region unlegalizable.
    std::size_t cuts = 0;
    std::size_t ratioRetries = 0;
    std::size_t maxDepth = 0;
    // Smallest leaf, i.e. the tightest the recursion packed a region.
    std::size_t minLeafCells = 0;
    double hpwlFinal = 0.0;
    // Fraction of the die a block set can occupy before the ratio is skewed away
    // from even. Reported because a run that needed many retries is a run whose
    // balance was fighting the design, and that is worth seeing.
    double meanImbalance = 0.0;
};

// The partitioner's own summary, emitted from the component that produced it.
void reportNtuPlace1(const RatioPlaceResult &res);

class RatioPlacer {
public:
    explicit RatioPlacer(PlacementDB &db);
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
