// @file kt_abacus.h// Abacus legalization: minimal-movement row legalization// After global placement the cells overlap and are not aligned to the placement// rows. Legalization snaps them onto rows, removes all overlap, and does so with// the least total movement it can.// The algorithm is Spindler, Schlichtmann and Johannes, "Abacus: fast// legalization of standard cell circuits with minimal movement", ISPD 2008. The// idea that distinguishes it from row-by-row Tetris is that a cell is not simply// dropped into a gap: whenever a cell is considered for a row, **every cell// already placed in that row is re-placed too**, by a dynamic program that// minimises the total squared movement of the whole row. A row is therefore// allowed to compact around the new arrival, which is what buys the ~30%// movement reduction the paper reports against Tetris.// Structure here:// - rows are cut into free x-intervals by the fixed cells (macros) that cross// them, so a cell can never be placed on top of a blockage;// - cells are processed in order of their target y, and for each one candidate// rows are scanned outward from its own row while a lower bound on the row// cost still beats the best legal placement found so far;// - the row's optimal placement is a left-to-right pass. For cells in a fixed// left-to-right order, minimising sum (x'_i - x_i)^2 subject to// x'_{i+1} >= x'_i + w_i has the greedy solution x'_i = max(x_i, x'_{i-1} +// w_{i-1}) -- an exchange argument shows any other choice is no better -- so// the DP is linear in the row length. That is the "clustering + movement" the// paper describes, in its whole-row form.


#pragma once

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"
#include "visualization/kt_plotter.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace ktplace {

struct LegalizeParams {
    /// Rows further than this many row pitches from the cell's target row are
    /// never considered. 0 means unlimited (the paper's own bound is the cost
    /// lower bound, not a fixed distance).
    std::size_t maxRowDistance = 0;
    /// If non-empty, write SVG frames here: the input placement, one frame every
    /// `frameEvery` cells legalized, and the final legal placement.
    std::string plotDir;
    std::size_t frameEvery = 0;
    /// Fence regions, for the frames only. The legalizer is not constrained by
    /// them -- it only draws them, so a frame of the legalizer's work shows the
    /// regions the result has to end up inside.
};

struct LegalizeResult {
    std::size_t cellsPlaced = 0;
    /// Cells that could not be placed in any row and were left where they were.
    std::size_t unplaced = 0;
    /// Sum of squared displacement, the quantity Abacus minimises.
    double totalSquaredDisplacement = 0.0;
    double maxDisplacement = 0.0;
    double hpwlBefore = 0.0;
    double hpwlAfter = 0.0;
    double seconds = 0.0;
    /// Self-check of the produced placement, so a legalizer bug shows up as a
    /// number rather than as a silently bad result.
    std::size_t overlappingPairs = 0;
    std::size_t offRow = 0;     ///< cells not aligned to a row band
    std::size_t offSite = 0;    ///< cells not aligned to the site grid
    std::size_t overFixed = 0;  ///< cells overlapping a macro / fixed cell
    std::size_t outOfRows = 0;  ///< cells not inside any subrow of their row
    /// Cells whose chosen subrow turned out not to fit on commit, so they were
    /// never actually placed. A non-zero value here with `unplaced == 0` means
    /// the commit path silently dropped cells.
    std::size_t commitFailures = 0;
};

class AbacusLegalizer {
public:
    explicit AbacusLegalizer(ktDM &db);
    ~AbacusLegalizer();

    AbacusLegalizer(const AbacusLegalizer &) = delete;
    AbacusLegalizer &operator=(const AbacusLegalizer &) = delete;

    /// Legalize in place, writing the new positions back into the database.
    LegalizeResult legalize(const LegalizeParams &params = {});

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
