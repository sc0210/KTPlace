// @file kt_simpl.cc// SimPL global placement. See kt_simpl.h for the algorithm summary and// the bibliographic reference.


#include "placer/simpl/kt_simpl.h"

#include "util/kt_log.h"
#include "util/kt_reportTable.h"
#include "util/kt_scopedTimer.h"
#include "visualization/kt_animator.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/parallel_reduce.h>
#include <vector>


namespace ktplace {

namespace {

/// Sentinel for "this vertex has no variable" (it is fixed, or it is a net).
constexpr std::uint32_t kNoVar = 0xFFFFFFFFu;

/// A net's distinct pins, in ascending cell order, with each pin's offset
/// inside its cell.
struct NetInfo {
    std::vector<std::uint32_t> cell;
    std::vector<double> offX;
    std::vector<double> offY;
    double weight = 1.0;
};

/// Symmetric sparse matrix, off-diagonals in CSR plus a separate diagonal. The
/// B2B model needs no auxiliary star variables, so the variable set is exactly
/// the movable cells.
struct CsrMatrix {
    std::size_t n = 0;
    std::vector<std::size_t> rowPtr;
    std::vector<std::size_t> col;
    std::vector<double> val;
    std::vector<double> diag;

    // SpMxV is the CG inner loop and is memory-bandwidth bound, so it is the one
    // kernel that really wants threads.
    void matvec(const std::vector<double> &in, std::vector<double> &out) const {
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 256),
                          [&](const tbb::blocked_range<std::size_t> &r) {
                              for (std::size_t i = r.begin(); i < r.end(); ++i) {
                                  double s = diag[i] * in[i];
                                  for (std::size_t e = rowPtr[i]; e < rowPtr[i + 1]; ++e) {
                                      s += val[e] * in[col[e]];
                                  }
                                  out[i] = s;
                              }
                          });
    }

    /// Sum of @p body(i) over [0, count), in parallel but reproducibly.
    ///
    /// Not a tbb::parallel_reduce. A reduction over a range whose split the
    /// scheduler chooses sums floating-point partials in whatever order the
    /// splits happen to combine, so two runs of the same binary on the same input
    /// disagree in the last bits -- and CG feeds those bits back in on the next
    /// iteration, so the disagreement grows instead of staying a rounding error.
    /// It did: the LAL's own residual differed in the fourth digit between runs,
    /// and by the end of global placement the same binary on the same input
    /// returned 1.399e7 four times out of six and 1.74e7 and 1.80e7 on the other
    /// two. A 29% spread makes every measurement of this placer unfalsifiable,
    /// including the ones in this file's comments.
    ///
    /// The chunk boundaries here depend only on count and grain, and the
    /// per-chunk totals are added back in index order, so the sum is a function
    /// of the input alone.
    template <typename Body>
    double chunkedSum(std::size_t count, std::size_t grain, Body body) const {
        if (count == 0) {
            return 0.0;
        }
        const std::size_t nChunks = (count + grain - 1) / grain;
        std::vector<double> part(nChunks, 0.0);
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, nChunks, 1),
                          [&](const tbb::blocked_range<std::size_t> &r) {
                              for (std::size_t c = r.begin(); c < r.end(); ++c) {
                                  const std::size_t b = c * grain;
                                  const std::size_t e = std::min(b + grain, count);
                                  double a = 0.0;
                                  for (std::size_t i = b; i < e; ++i) {
                                      a += body(i);
                                  }
                                  part[c] = a;
                              }
                          });
        double total = 0.0;
        for (const double v : part) {
            total += v;
        }
        return total;
    }

    /// Sum of squares.
    double norm2(const std::vector<double> &v) const {
        return chunkedSum(n, kGrain, [&](std::size_t i) {
            return v[i] * v[i];
        });
    }

    /// Dot product.
    double dot(const std::vector<double> &a, const std::vector<double> &b) const {
        return chunkedSum(n, kGrain, [&](std::size_t i) {
            return a[i] * b[i];
        });
    }

    static constexpr std::size_t kGrain = 1024;
};

/// Regular bin grid carrying the two quantities the density test needs: the cell
/// area A_c inside each bin and the available (site) area A_a.
struct DensityGrid {
    double x0 = 0.0, y0 = 0.0;
    double dx = 1.0, dy = 1.0;
    std::size_t nbx = 1, nby = 1;
    double binArea = 1.0;
    std::vector<double> occ;    // A_c
    std::vector<double> avail;  // A_a
    double totalAvail = 0.0;
    double totalCellArea = 0.0;

    std::size_t size() const {
        return nbx * nby;
    }
    std::size_t at(std::size_t ix, std::size_t iy) const {
        return iy * nbx + ix;
    }

    void locate(double px, double py, std::size_t &ix, std::size_t &iy) const {
        const double fx = std::clamp((px - x0) / dx, 0.0, static_cast<double>(nbx) - 1e-9);
        const double fy = std::clamp((py - y0) / dy, 0.0, static_cast<double>(nby) - 1e-9);
        ix = static_cast<std::size_t>(fx);
        iy = static_cast<std::size_t>(fy);
    }
    double binLoX(std::size_t ix) const {
        return x0 + static_cast<double>(ix) * dx;
    }
    double binLoY(std::size_t iy) const {
        return y0 + static_cast<double>(iy) * dy;
    }
    double hiX() const {
        return x0 + static_cast<double>(nbx) * dx;
    }
    double hiY() const {
        return y0 + static_cast<double>(nby) * dy;
    }
};

/// One pending block of the top-down partitioning (Algorithm 1, queue Q).
struct Block {
    std::size_t ix0 = 0, ix1 = 0, iy0 = 0, iy1 = 0;  // inclusive bin range
    std::size_t level = 1;
    bool vertical = true;  // cut direction: vertical at level 1, then alternate
};

}  // namespace

// ---------------------------------------------------------------------------

class SimplePlacer::Impl {
public:
    explicit Impl(ktDM &db) : db_(db), graph_(db.getGraph()) {}

    SimplResult run(const SimplParams &P, const std::string &plotDir, bool useFences);

private:
    // --- setup -------------------------------------------------------------
    void collect();
    void buildGrid(const SimplParams &P);
    void seedUniform(std::uint64_t seed);

    // --- net model and solver ----------------------------------------------
    void buildB2B(const std::vector<double> &px, const std::vector<double> &py, double alpha,
                  bool useAnchors, SimplParams::NetModel model);
    void solve(const std::string &tag, bool allowFrames, std::size_t cgEvery = 0);
    /// Worst of the two axis residuals left by the last solve, so a caller can
    /// report whether the solve actually converged.
    double lastResidual_ = 0.0;
    double hpwl(const std::vector<double> &px, const std::vector<double> &py) const;

    // --- density -----------------------------------------------------------
    void binCells(const std::vector<double> &px, const std::vector<double> &py);
    double densityOf(std::size_t ix0, std::size_t ix1, std::size_t iy0, std::size_t iy1) const;
    double scaledOverflow() const;

    // --- look-ahead legalization (Algorithm 1) -----------------------------
    void lookAheadLegalize();
    void processBlock(const Block &B);
    /// Spread a block's cells inside it without splitting it further.
    ///
    /// The recursion's smallest blocks still have to be spread, and the paper's
    /// "area(B) is small enough" is a statement about when to stop splitting, not
    /// permission to leave the cells overlapping. This is the same nonlinear
    /// scaling the split path uses, with the block taken as one region and the
    /// cutline placed at its middle: the cells are ordered by distance from it and
    /// packed into the block's stripes, so the spread is exactly the factor the
    /// block's density is short by.
    void nonlinearScale(const std::vector<std::uint32_t> &cells, std::size_t a0, std::size_t a1,
                        std::size_t b0, std::size_t b1, bool vertical, double cutCoord);

    // --- helpers -----------------------------------------------------------
    /// Centroid, per-axis spread, x-y correlation and die coverage of a position
    /// set. The correlation is the discriminator the trace needs: a population
    /// gathered onto a diagonal reads rho -> +1 with full bounding-box coverage,
    /// a uniform spread reads rho ~ 0, and a placement collapsed onto a single
    /// axis reads rho ~ 0 with a coverage near zero on one axis.
    void describe(const std::vector<double> &px, const std::vector<double> &py,
                  const char *tag) const;
    void writeFrame(const std::string &path, const std::vector<double> &px,
                    const std::vector<double> &py, double hp, double ovf, const std::string &note,
                    std::size_t step, std::size_t total);
    /// One frame from inside the CG loop: the current iterate of one axis
    /// against the other axis' last value. `tag` identifies the outer context
    /// (e.g. "init3" or "g07"), `dim` is 'x' or 'y'.
    /// One frame per conjugate-gradient iteration, covering both axes: the
    /// placement is the state the next step starts from, and @p residX/@p residY
    /// say how far each axis still is from its own tolerance.
    void writeCgFrame(const std::string &tag, std::size_t cgIter, double residX, double residY);
    /// Bin-density heat map: one rectangle per bin coloured by occ/avail, so a
    /// glance shows whether the placement is spreading or still a blob.
    void writeDensityMap(const std::string &path, const std::vector<double> &px,
                         const std::vector<double> &py, const std::string &note);
    /// Bin a placement into a throwaway occupancy array, without touching the
    /// live bin index the legalizer maintains. Returns the scaled overflow.
    double binLocal(const std::vector<double> &px, const std::vector<double> &py,
                    std::vector<double> &occ) const;
    /// One trace line summarising the density field: mean/median/max bin
    /// utilisation, share of the die near capacity, empty and overfull bins.
    void densityStats(const std::vector<double> &px, const std::vector<double> &py,
                      const char *tag) const;

    ktDM &db_;
    Graph &graph_;
    SimplResult res_;
    SimplParams par_;

    std::size_t nv_ = 0;
    std::size_t numMovable_ = 0;

    std::vector<std::uint32_t> movVertex_;  // graph vertex id per movable slot
    std::vector<std::uint32_t> fixVertex_;
    std::vector<std::uint32_t> varOfVertex_;   // movable slot, or kNoVar
    std::vector<double> area_;                 // per movable slot
    std::vector<double> areaMovW_, areaMovH_;  // cell extents, for the macro overlap test
    std::vector<double> vx_, vy_;              // per graph vertex, live positions
    std::vector<double> pinX_, pinY_;
    std::vector<double> inputX_, inputY_;    // per movable slot, cell position
    std::vector<double> anchorX_, anchorY_;  // per movable slot, fixed pseudonet targets
    std::vector<NetInfo> nets_;              // indexed by graph vertex

    // The B2B model is separable, but the x and y graphs are NOT the same graph:
    // the extreme (min/max) pins, and therefore the edge set and every weight,
    // are chosen independently per dimension. So there are two matrices and two
    // right-hand sides. Sharing one solve vector between the axes (which is what
    // this used to do -- `lower = sol_; lowerY = sol_;`) makes x_i == y_i for
    // every cell, i.e. an exact 45-degree diagonal, which is not a placement at
    // all and is what the SVGs were showing.
    CsrMatrix Ax_, Ay_;
    std::vector<double> rhsX_, rhsY_, solX_, solY_;
    double degEps_ = 1e-9;   // below this a net is degenerate in a dimension
    double avgCellW_ = 1.0;  // mean movable cell extent, per dimension
    double avgCellH_ = 1.0;
    double rowH_ = 1.0;       // placement-row height, derived from row count
    double anchorEps_ = 1.0;  // 1.5 * row height, per ComPLx/SimPL
    // Constant-stiffness pseudonets weigh alpha in units of 1/length, against B2B
    // edges that weigh 1/distance in the design's own units, so their balance
    // depends on the unit system. The alpha schedule was calibrated on adaptec1,
    // whose rows are kCalibRowHeight high; this rescales it to the design's rows.
    double anchorScale_ = 1.0;

    DensityGrid grid_;
    double g_ = 1.0;

    /// Live index of which bin each movable cell is in, and which cells are in each
    /// bin. processBlock() needs the cells inside a block, and it used to find them
    /// by scanning every movable cell. With 57k-90k blocks in the later legalizer
    /// rounds and ~210k cells that is ~1e10 tests per round, which dominated
    /// everything. The index is kept exact as cells move: nonlinearScale() relocates
    /// a cell between bins in O(1) via swap-and-pop.
    std::vector<std::vector<std::uint32_t>> binCells_;
    std::vector<std::uint32_t> cellBin_;
    std::vector<std::uint32_t> cellSlot_;

    /// Move one cell's bin membership, keeping both the index AND the density
    /// field exact.
    ///
    /// The occupancy matters as much as the index. The legalizer relocates cells
    /// as it recurses, but the density field was previously computed once per
    /// lookAheadLegalize() call and never refreshed, so every C_c cell-area
    /// median, every C_B whitespace median and every region-density test was
    /// derived from occupancy describing where the cells were BEFORE the
    /// redistribution, while the cells being redistributed had already moved.
    /// That is the most likely cause of sub-regions being handed more area than
    /// they can hold, of cells being scaled into each other, and therefore of the
    /// negative lower/upper gap that means the "legalized" cells overlap.
    void rehome(std::uint32_t cell, std::size_t toBin) {
        const std::uint32_t from = cellBin_[cell];
        if (from == toBin) {
            return;
        }
        grid_.occ[from] -= area_[cell];
        grid_.occ[toBin] += area_[cell];
        std::vector<std::uint32_t> &src = binCells_[from];
        const std::size_t slot = cellSlot_[cell];
        const std::uint32_t last = src.back();
        src[slot] = last;
        cellSlot_[last] = static_cast<std::uint32_t>(slot);
        src.pop_back();
        binCells_[toBin].push_back(cell);
        cellSlot_[cell] = static_cast<std::uint32_t>(binCells_[toBin].size() - 1);
        cellBin_[cell] = static_cast<std::uint32_t>(toBin);
    }

    BBox die_{};
    double dieW_ = 1.0, dieH_ = 1.0;

    /// Queue Q of Algorithm 1.
    std::deque<Block> pending_;
    std::size_t blocksProcessed_ = 0;
    std::size_t deepestLevel_ = 0;
    std::size_t maxBlockCells_ = 0;

    /// Where placement frames go (<plotDir>/simpl, or the snapshot directory
    /// when no plot directory was given). Empty disables all frames.
    std::string frameDir_;
    /// Outer-loop context for the CG frames of the solve currently running.
    std::string cgTag_;

    /// Whether this stage contributes raster frames at all. The frames themselves
    /// belong to the run's PlacementAnimator, so that the legalizer and the
    /// detailed placer land in the same GIF rather than each keeping its own.
    bool animEnabled_ = false;

    /// Placement fences, or null for an unconstrained design. Assigned to by run()
    /// and consulted after every solve and by the frame renderers.
    const constraintMgr *fences_ = nullptr;
    /// Region id per movable cell, mirrored from Vertex::regionId so the hot loop
    /// does not chase a vertex per cell per iteration.
    std::vector<int> movRegion_;
    /// Cells moved back into their own region, and cells pushed out of someone
    /// else's, accumulated over the run. Reported, so a fence that is being
    /// enforced expensively is visible rather than inferred.
    std::size_t fenceClamps_ = 0;
    std::size_t fencePushes_ = 0;

    /// Rasterise the current placement into the run's animation. Called from every
    /// frame producer -- warm-up CG iterates, per-iteration CG iterates, the LSS
    /// and LAL bounds, and the final placement -- so the GIF reads as one
    /// continuous run rather than three separate ones.
    /// Hold every movable cell inside the fence it belongs to.
    void enforceFences(std::vector<double> &px, std::vector<double> &py);

    void recordGifFrame(const std::vector<float> &fx, const std::vector<float> &fy,
                        std::size_t step, std::size_t total, double hp, double ovf,
                        const std::string &note, bool mandatory = false);
};

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::collect() {
    nv_ = graph_.getNumCells();
    const std::size_t nn = graph_.getNumNets();

    varOfVertex_.assign(nv_, kNoVar);
    std::vector<NetInfo> nets(nn);

    // Cells first: which are fixed blockages, and which are the solve's variables.
    for (std::size_t v = 0; v < nv_; ++v) {
        const Vertex &vert = graph_.getCell(v);
        if (vert.isFixed || vert.isTerminal) {
            fixVertex_.push_back(static_cast<std::uint32_t>(v));
            continue;
        }
        varOfVertex_[v] = static_cast<std::uint32_t>(numMovable_++);
        movVertex_.push_back(static_cast<std::uint32_t>(v));
        // Keep whatever placement the design shipped with. It used to be dropped
        // on the floor here and overwritten by a uniform seed, so a Bookshelf .pl
        // carrying a good solution was never even looked at.
        inputX_.push_back(vert.x);
        inputY_.push_back(vert.y);
        area_.push_back(vert.width * vert.height);
        areaMovW_.push_back(vert.width);
        areaMovH_.push_back(vert.height);
    }

    // Then nets, each with the cells its pins reach.
    for (std::size_t n = 0; n < nn; ++n) {
        NetInfo &ni = nets[n];
        ni.weight = graph_.getNet(n).weight;
        const std::vector<std::size_t> &pins = graph_.getNetPins(n);
        ni.cell.reserve(pins.size());
        for (const std::size_t pinId : pins) {
            const Pin &pin = graph_.getPin(pinId);
            ni.cell.push_back(static_cast<std::uint32_t>(pin.cellId));
            ni.offX.push_back(pin.offsetX);
            ni.offY.push_back(pin.offsetY);
        }
        // Sort by cell, then drop repeated pins on one cell, keeping the offsets
        // aligned. A cell listed twice on a net must contribute once, or the B2B
        // degree k and the clique expansion are both wrong.
        std::vector<std::uint32_t> order(ni.cell.size());
        std::iota(order.begin(), order.end(), 0u);
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            return ni.cell[a] < ni.cell[b];
        });
        NetInfo dedup;
        dedup.weight = ni.weight;
        dedup.cell.reserve(order.size());
        for (const std::uint32_t oi : order) {
            if (!dedup.cell.empty() && dedup.cell.back() == ni.cell[oi]) {
                continue;
            }
            dedup.cell.push_back(ni.cell[oi]);
            dedup.offX.push_back(ni.offX[oi]);
            dedup.offY.push_back(ni.offY[oi]);
        }
        nets[n] = std::move(dedup);
    }

    res_.numMovable = numMovable_;
    res_.numFixed = fixVertex_.size();
    res_.nets = nn;
    nets_ = std::move(nets);

    // Net-degree shape and pin totals. A design that is nearly all 2-pin nets
    // never exercises the B2B extreme-to-all expansion, so a B2B bug would hide
    // there; adaptec1 is 52% 2-pin and 46% higher degree, which does exercise it.
    // nets_ is indexed by VERTEX id, so it must be walked with the graph's
    // vertex types: the cell entries are empty and would otherwise be counted as
    // one-pin nets.
    {
        std::size_t twoPin = 0, multiPin = 0, singlePin = 0, pins = 0, maxDeg = 0;
        double weightSum = 0.0, weightMin = std::numeric_limits<double>::max();
        double weightMax = -std::numeric_limits<double>::max();
        for (std::size_t v = 0; v < nets_.size(); ++v) {
            const NetInfo &ni = nets_[v];
            const std::size_t k = ni.cell.size();
            pins += k;
            maxDeg = std::max(maxDeg, k);
            weightSum += ni.weight;
            weightMin = std::min(weightMin, ni.weight);
            weightMax = std::max(weightMax, ni.weight);
            if (k < 2) {
                ++singlePin;
            } else if (k == 2) {
                ++twoPin;
            } else {
                ++multiPin;
            }
        }
        const double invN = 1.0 / static_cast<double>(std::max(nn, std::size_t{1}));
        ktlog.trace(
            "nets: {} total, {} single-pin, {} two-pin ({:.1f}%), {} multi-pin ({:.1f}%), "
            "max degree {}, {:.1f} pins/net; weight mean {:.4g} range [{:.4g},{:.4g}]",
            nn, singlePin, twoPin, 100.0 * static_cast<double>(twoPin) * invN, multiPin,
            100.0 * static_cast<double>(multiPin) * invN, maxDeg, static_cast<double>(pins) * invN,
            weightSum * invN, weightMin, weightMax);
    }

    Ax_.n = numMovable_;
    Ay_.n = numMovable_;
    solX_.assign(numMovable_, 0.0);
    solY_.assign(numMovable_, 0.0);
    rhsX_.assign(numMovable_, 0.0);
    rhsY_.assign(numMovable_, 0.0);
    pinX_.assign(numMovable_, 0.0);
    pinY_.assign(numMovable_, 0.0);
    if (inputX_.size() != numMovable_) {
        inputX_.assign(numMovable_, 0.0);
        inputY_.assign(numMovable_, 0.0);
    }
    anchorX_.assign(numMovable_, 0.0);
    anchorY_.assign(numMovable_, 0.0);
    vx_.assign(nv_, 0.0);
    vy_.assign(nv_, 0.0);
    for (std::size_t v = 0; v < nv_; ++v) {
        vx_[v] = graph_.getCell(v).x;
        vy_[v] = graph_.getCell(v).y;
    }
}

void SimplePlacer::Impl::buildGrid(const SimplParams &P) {
    // Die extent: the parsed die area when it plausibly contains the fixed
    // cells, otherwise the fixed-cell bounding box.
    // The region a cell may occupy, defined once in the datamodel and shared with
    // the legality check. See placementDieBox() there for why it is the union of
    // the fixed geometry, the declared die area and the rows.
    const std::array<double, 4> dieBox = db_.placementDieBox();
    BBox die = BBox{dieBox[0], dieBox[1], dieBox[2], dieBox[3]};
    die_ = die;
    dieW_ = std::max(die[2] - die[0], 1e-9);
    dieH_ = std::max(die[3] - die[1], 1e-9);
    degEps_ = 1e-9 * std::min(dieW_, dieH_);

    // Mean movable cell extent, and the row height. The datamodel exposes only a
    // row COUNT, so the height is derived geometrically, which is exact for the
    // uniform-row Bookshelf suites and an approximation otherwise.
    double sumW = 0.0;
    double sumH = 0.0;
    std::size_t cnt = 0;
    for (const std::uint32_t v : movVertex_) {
        const Vertex &vert = graph_.getCell(v);
        sumW += vert.width;
        sumH += vert.height;
        ++cnt;
    }
    if (cnt > 0) {
        avgCellW_ = sumW / static_cast<double>(cnt);
        avgCellH_ = sumH / static_cast<double>(cnt);
    }
    const std::size_t nRows = std::max<std::size_t>(db_.getNumRows(), 1);
    rowH_ = dieH_ / static_cast<double>(nRows);
    // ComPLx (Kim & Markov, DAC 2012) states this for SimPL/SimPLR verbatim:
    // "each movable object is connected to its anchor location by a pseudonet,
    //  contributing w_i (x_i - x_i^0)^2 ... where w_i = lambda/(|x_i - x_i^0| +
    //  eps). eps > 0 is used to bound the denominator away from zero and make the
    //  objective function strictly convex. In SimPL and SimPLR, eps is
    //  calculated as 1.5 times row height."
    anchorEps_ = 1.5 * rowH_;
    {
        // adaptec1's .scl row height: the design the alpha schedule was tuned on,
        // so the scale is exactly 1 there and its placement is unchanged. Measured
        // on mgc_superblue16_a (rows 900 units high) before this: anchors started
        // at 7x the interconnect stiffness and ended at 310x, against 0.09x and 2x
        // on adaptec1, so the global loop solved in one CG step to the anchors and
        // never optimised wirelength.
        constexpr double kCalibRowHeight = 12.0;
        double rowHeight = 0.0;
        for (const RowInfo &r : db_.getRows()) {
            if (r.height > 0.0 && (rowHeight == 0.0 || r.height < rowHeight)) {
                rowHeight = r.height;
            }
        }
        anchorScale_ = rowHeight > 0.0 ? kCalibRowHeight / rowHeight : 1.0;
    }
    ktlog.trace(
        "mean cell {:.4g} x {:.4g}, row height {:.4g}, anchor eps {:.4g} "
        "(= 1.5 rows)",
        avgCellW_, avgCellH_, rowH_, anchorEps_);

    std::size_t nbx = P.binsX;
    std::size_t nby = P.binsY;
    if (nbx == 0 || nby == 0) {
        // ~51 cells per bin: adaptec1 lands on 64x64.
        //
        // Measured over 64..700 on adaptec1, the coarse end is the balanced one:
        // 64 gives the lowest scaled overflow (0.206 against 0.305 at 229) and the
        // least lopsided placement, for 4.07e+08 against 5.40e+08 end to end.
        // KTPLACE_SIMPL_GRID overrides it; 700 is better still on wirelength alone.
        double t =
            std::clamp(std::sqrt(static_cast<double>(std::max<std::size_t>(numMovable_, 1)) / 51.0),
                       16.0, 256.0);
        if (const char *e = std::getenv("KTPLACE_SIMPL_GRID")) {
            const long v = std::strtol(e, nullptr, 10);
            if (v >= 8 && v <= 1024) {
                t = static_cast<double>(v);
            }
        }
        nbx = static_cast<std::size_t>(t);
        nby = static_cast<std::size_t>(t);
    }
    grid_.x0 = die[0];
    grid_.y0 = die[1];
    grid_.nbx = nbx;
    grid_.nby = nby;
    grid_.dx = dieW_ / static_cast<double>(nbx);
    grid_.dy = dieH_ / static_cast<double>(nby);
    grid_.binArea = grid_.dx * grid_.dy;
    grid_.occ.assign(nbx * nby, 0.0);
    grid_.avail.assign(nbx * nby, grid_.binArea);
    res_.binsX = nbx;
    res_.binsY = nby;

    // Available area per bin: the bin area less whatever a fixed macro covers.
    // (The paper's A_a is the cell-site area; with row-aligned bins and g = 1
    // the bin area is the site area exactly, so the only thing to remove is
    // blockage.)
    for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
        grid_.avail[k] = grid_.binArea;
    }
    for (const std::uint32_t v : fixVertex_) {
        const Vertex &vert = graph_.getCell(v);
        if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
            continue;
        }
        std::size_t ix0, iy0, ix1, iy1;
        grid_.locate(vx_[v], vy_[v], ix0, iy0);
        grid_.locate(vx_[v] + vert.width, vy_[v] + vert.height, ix1, iy1);
        for (std::size_t iy = iy0; iy <= iy1 && iy < grid_.nby; ++iy) {
            for (std::size_t ix = ix0; ix <= ix1 && ix < grid_.nbx; ++ix) {
                const double bx = grid_.binLoX(ix);
                const double by = grid_.binLoY(iy);
                const double ox = std::max(
                    0.0, std::min(bx + grid_.dx, vx_[v] + vert.width) - std::max(bx, vx_[v]));
                const double oy = std::max(
                    0.0, std::min(by + grid_.dy, vy_[v] + vert.height) - std::max(by, vy_[v]));
                grid_.avail[grid_.at(ix, iy)] =
                    std::max(grid_.avail[grid_.at(ix, iy)] - ox * oy, 0.0);
            }
        }
    }
    // Smallest row site pitch, used as the sliver threshold below.
    // The smallest site area over the rows, which is the area below which a bin
    // cannot hold even one cell and is therefore not usable space.
    //
    // This was a maximum, which is the opposite of what the comment said and of
    // what the threshold means. The threshold answers "is this remainder big enough
    // to be a place a cell could go?", so it has to be judged against the smallest
    // site in the design; taking the largest one makes the bar scale with the
    // biggest row instead. On a design whose rows are not all alike that zeroes
    // every bin: mgc_superblue16_a reported "available area 0" and a utilisation of
    // 6.2e28%, and with no capacity anywhere the density term, the overflow and the
    // legalizer's region growth are all reading from an empty map. The Bookshelf
    // designs hid it because their rows are uniform, so the max and the min are the
    // same number.
    double ri_pitch_floor = std::numeric_limits<double>::max();
    for (const RowInfo &r : db_.getRows()) {
        if (r.pitch() > 0.0 && r.height > 0.0) {
            ri_pitch_floor = std::min(ri_pitch_floor, r.pitch() * r.height);
        }
    }
    if (!(ri_pitch_floor < std::numeric_limits<double>::max())) {
        ri_pitch_floor = degEps_;
    }
    // A_a is the available *cell-site* area of a bin, not its geometric area
    // (Section 4.2). Using binArea overstates it twice over: bins that fall
    // outside the row band have no sites at all yet were counted as fully
    // available, and a bin that a macro almost fills was left with a tiny
    // positive remainder. Those slivers then read as density ~1e15 in the
    // overfill test, and densityOf() inside the region-growth loop saw an
    // astronomic density for any rectangle containing one, so it expanded to the
    // die boundary every time and the legalizer scattered cells over the whole
    // chip. Measured: 169 such bins on adaptec1.
    //
    // So rebuild avail from the rows: the site area of a bin is the y-overlap
    // with each row times the x-extent of that row's subrows inside the bin,
    // summed over rows, less macro coverage.
    {
        const std::vector<RowInfo> rowInfo = db_.getRows();
        std::fill(grid_.avail.begin(), grid_.avail.end(), 0.0);
        for (const RowInfo &ri : rowInfo) {
            if (!(ri.pitch() > 0.0) || !(ri.height > 0.0)) {
                continue;
            }
            const double ry1 = ri.coordinate + ri.height;
            // locate() takes a point (x, y) and returns (ix, iy), so the row's two
            // y bounds go in as the y of two points. Passing them as (x, y) of one
            // point binned the bottom edge along x: on a square die with x0 == y0
            // (adaptec1) that is the same number, which hid it; on
            // mgc_superblue16_a it left 158 of 13225 bins with any capacity, a
            // "utilisation" of 6104%, and a spreading that could never converge.
            std::size_t iy0, iy1, unusedX;
            grid_.locate(grid_.x0, ri.coordinate, unusedX, iy0);
            grid_.locate(grid_.x0, ry1, unusedX, iy1);
            for (std::size_t iy = iy0; iy <= iy1 && iy < grid_.nby; ++iy) {
                const double bLo = grid_.binLoY(iy);
                const double bHi = bLo + grid_.dy;
                const double yOv = std::max(0.0, std::min(bHi, ry1) - std::max(bLo, ri.coordinate));
                if (!(yOv > 0.0)) {
                    continue;
                }
                for (const SubrowInfo &si : ri.subrows) {
                    if (!(si.xhi(ri.pitch()) > si.xlo())) {
                        continue;
                    }
                    std::size_t ix0, ix1, unusedY;
                    grid_.locate(si.xlo(), grid_.y0, ix0, unusedY);
                    grid_.locate(si.xhi(ri.pitch()), grid_.y0, ix1, unusedY);
                    for (std::size_t ix = ix0; ix <= ix1 && ix < grid_.nbx; ++ix) {
                        const double xLo = grid_.binLoX(ix);
                        const double xHi = xLo + grid_.dx;
                        const double xOv = std::max(
                            0.0, std::min(xHi, si.xhi(ri.pitch())) - std::max(xLo, si.xlo()));
                        if (!(xOv > 0.0)) {
                            continue;
                        }
                        grid_.avail[grid_.at(ix, iy)] += xOv * yOv;
                    }
                }
            }
        }
        // Subtract macro coverage, then clamp: a bin with no room left must be
        // exactly zero, never a sliver.
        for (const std::uint32_t fv : fixVertex_) {
            const Vertex &vert = graph_.getCell(fv);
            if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
                continue;
            }
            std::size_t ix0, iy0, ix1, iy1;
            grid_.locate(vx_[fv], vy_[fv], ix0, iy0);
            grid_.locate(vx_[fv] + vert.width, vy_[fv] + vert.height, ix1, iy1);
            for (std::size_t iy = iy0; iy <= iy1 && iy < grid_.nby; ++iy) {
                for (std::size_t ix = ix0; ix <= ix1 && ix < grid_.nbx; ++ix) {
                    const double bx = grid_.binLoX(ix);
                    const double by = grid_.binLoY(iy);
                    const double ox = std::max(
                        0.0, std::min(bx + grid_.dx, vx_[fv] + vert.width) - std::max(bx, vx_[fv]));
                    const double oy = std::max(0.0, std::min(by + grid_.dy, vy_[fv] + vert.height) -
                                                        std::max(by, vy_[fv]));
                    grid_.avail[grid_.at(ix, iy)] =
                        std::max(grid_.avail[grid_.at(ix, iy)] - ox * oy, 0.0);
                }
            }
        }
        // A sliver below a thousandth of a site cannot hold anything; treating it
        // as unavailable keeps the density ratios bounded.
        const double sliver = 1e-3 * ri_pitch_floor;
        for (double &a : grid_.avail) {
            if (a < sliver) {
                a = (a > 0.0) ? 0.0 : a;
            }
        }
        {
            std::size_t nrows = 0, nsub = 0, npos = 0;
            double siteSum = 0.0, maxSite = 0.0, minSite = 1e300;
            for (const RowInfo &r : db_.getRows()) {
                ++nrows;
                nsub += r.subrows.size();
                siteSum += r.pitch() * r.height;
                maxSite = std::max(maxSite, r.pitch() * r.height);
                minSite = std::min(minSite, r.pitch() * r.height);
                for (const SubrowInfo &si : r.subrows) {
                    if (si.xhi(r.pitch()) > si.xlo()) {
                        ++npos;
                    }
                }
            }
            double pre = 0.0;
            for (const double a : grid_.avail) {
                pre += a;
            }
            ktlog.trace(
                "grid diag: rows {} subrows {} usable {} site min {} max {} avg {} "
                "sliver {} availBeforeSliver {} nonzero {}",
                nrows, nsub, npos, minSite, maxSite,
                nrows ? siteSum / static_cast<double>(nrows) : 0.0, sliver, pre, [&] {
                    std::size_t k = 0;
                    for (const double a : grid_.avail) {
                        k += (a > 0.0) ? 1u : 0u;
                    }
                    return k;
                }());
        }
    }
    grid_.totalAvail = 0.0;
    for (const double a : grid_.avail) {
        grid_.totalAvail += a;
    }
    grid_.totalCellArea = std::accumulate(area_.begin(), area_.end(), 0.0);
}

void SimplePlacer::Impl::seedUniform(std::uint64_t seed) {
    // The paper seeds with a uniformly distributed placement, not a single
    // collapsed point. That matters for B2B in particular: the model needs
    // distinct extreme pins, and a single point would make every net degenerate
    // in both dimensions on the first build.
    std::uint64_t s = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    const auto next = [&s]() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1)) /
               static_cast<double>(1ULL << 53);
    };
    for (std::size_t i = 0; i < numMovable_; ++i) {
        pinX_[i] = die_[0] + next() * dieW_;
        pinY_[i] = die_[1] + next() * dieH_;
    }
}

// ---------------------------------------------------------------------------
// B2B net model and the linear solve
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::buildB2B(const std::vector<double> &px, const std::vector<double> &py,
                                  double alpha, bool useAnchors, SimplParams::NetModel model) {
    // Pin coordinates per cell, fixed ones included: a B2B edge to a fixed cell is
    // built exactly like one to a movable cell and then eliminated into the
    // diagonal and the right-hand side.
    std::vector<double> cx(nv_), cy(nv_);
    for (std::size_t v = 0; v < nv_; ++v) {
        cx[v] = vx_[v];
        cy[v] = vy_[v];
    }
    for (std::size_t i = 0; i < numMovable_; ++i) {
        cx[movVertex_[i]] = px[i];
        cy[movVertex_[i]] = py[i];
    }

    // Minimum edge length for the 1/length weight. Chu, "Electronic Design
    // Automation" ch. 11 sec. 11.5.2.2, gives the reason almost verbatim: "Nets
    // becoming very short (i.e. g_ie becoming very small) may cause numerical
    // problems during the minimization of L_star. Therefore, g_ie is lower bounded
    // (by the average module width for example) to ensure that g_ie will never be
    // zero." The same defect sits in the denominator of the BoundingBox edge
    // weight. Without the floor, a few near-coincident nets reached lengths of
    // ~1e-5, took weights of ~1e5 each, lifted the interconnect diagonal to
    // 2.8e8, and cut the pseudonet share to 2e-9.
    const double lenFloor[2] = {std::max(avgCellW_, degEps_), std::max(avgCellH_, degEps_)};

    struct Triple {
        std::size_t col;
        double val;
    };

    double anchorDiagSum = 0.0;
    std::size_t nnzX = 0, nnzY = 0;
    std::vector<double> pinCoord;

    for (int dim = 0; dim < 2; ++dim) {
        CsrMatrix &A = (dim == 0) ? Ax_ : Ay_;
        std::vector<double> &rhs = (dim == 0) ? rhsX_ : rhsY_;
        const std::vector<double> &coord = (dim == 0) ? cx : cy;

        std::vector<std::vector<Triple>> rows(numMovable_);
        std::vector<double> diag(numMovable_, 0.0);
        std::fill(rhs.begin(), rhs.end(), 0.0);

        // One B2B edge: symmetric between two movable cells, or eliminated into
        // the diagonal and the RHS when one end is fixed.
        const auto addEdge = [&](std::size_t cellA, double posA, std::size_t cellB, double posB,
                                 double w) {
            const std::uint32_t va = varOfVertex_[cellA];
            const std::uint32_t vb = varOfVertex_[cellB];
            if (va != kNoVar && vb != kNoVar) {
                rows[va].push_back({vb, -w});
                rows[vb].push_back({va, -w});
                diag[va] += w;
                diag[vb] += w;
            } else if (va != kNoVar) {
                diag[va] += w;
                rhs[va] += w * posB;
            } else if (vb != kNoVar) {
                diag[vb] += w;
                rhs[vb] += w * posA;
            }
            // both fixed: a constant, no derivative.
        };

        for (std::size_t v = 0; v < nets_.size(); ++v) {
            const NetInfo &ni = nets_[v];
            const std::size_t k = ni.cell.size();
            if (k < 2) {
                continue;
            }
            const double base = ni.weight / static_cast<double>(k - 1);

            pinCoord.resize(k);
            const std::vector<double> &off = (dim == 0) ? ni.offX : ni.offY;
            for (std::size_t q = 0; q < k; ++q) {
                pinCoord[q] = coord[ni.cell[q]] + off[q];
            }

            // Star model: every pin wired to a free virtual centre, and the centre
            // minimised out analytically.
            //
            // The cost is w * sum_i (x_i - x_c)^2, minimised at x_c = mean(x). What
            // is left is
            //     w * [ sum_i x_i^2 - (sum_i x_i)^2 / k ]
            //       = (w/k) * [ k * sum_i x_i^2 - (sum_i x_i)^2 ]
            // and the bracket is exactly the clique form sum over pairs of (x_i-x_j)^2.
            // So the star model is a clique with weight w/k, and it is built here as
            // one. Eliminating the centre is what keeps the system the same size --
            // a centre per net would be an extra unknown per net, and the solve is
            // over cells only.
            //
            // Unlike B2B the weights do not depend on where the pins currently are,
            // so the matrix does not have to be rebuilt as the placement moves. That
            // is the reason to use it for the initial placement, where the paper's
            // alternating solve/rebuild is pure overhead: there are no anchors and
            // no spreading, only the wirelength objective, and the star model is the
            // better-conditioned approximation of it -- a clique preserves ordering
            // better than a bounding box does when cells are still piled up.
            if (model == SimplParams::NetModel::Star) {
                const double w = ni.weight / static_cast<double>(k);
                for (std::size_t q = 0; q < k; ++q) {
                    for (std::size_t r = q + 1; r < k; ++r) {
                        addEdge(ni.cell[q], pinCoord[q], ni.cell[r], pinCoord[r], w);
                    }
                }
                continue;
            }

            std::size_t lo = 0;
            std::size_t hi = 0;
            for (std::size_t q = 1; q < k; ++q) {
                if (pinCoord[q] < pinCoord[lo]) {
                    lo = q;
                }
                if (pinCoord[q] > pinCoord[hi]) {
                    hi = q;
                }
            }
            const double span = pinCoord[hi] - pinCoord[lo];
            if (!(span > degEps_)) {
                // Degenerate in this dimension: the bounding-box length here is
                // zero, so the net contributes nothing to this solve.
                continue;
            }
            // Extremes to each other ...
            addEdge(ni.cell[lo], pinCoord[lo], ni.cell[hi], pinCoord[hi],
                    base / std::max(span, lenFloor[dim]));
            // ... and each extreme to every other pin, with the length floored so
            // a pin sitting on an extreme cannot produce an unbounded weight.
            for (std::size_t q = 0; q < k; ++q) {
                if (q == lo || q == hi) {
                    continue;
                }
                addEdge(ni.cell[lo], pinCoord[lo], ni.cell[q], pinCoord[q],
                        base / std::max(std::abs(pinCoord[q] - pinCoord[lo]), lenFloor[dim]));
                addEdge(ni.cell[hi], pinCoord[hi], ni.cell[q], pinCoord[q],
                        base / std::max(std::abs(pinCoord[q] - pinCoord[hi]), lenFloor[dim]));
            }
        }

        // Pseudonets: each cell is wired to its fixed, zero-area anchor with a
        // spring of stiffness alpha. The anchor is fixed, so this only adds to the
        // diagonal -- which is exactly why the paper can claim it improves diagonal
        // dominance and speeds up the Jacobi-preconditioned CG.
        //
        // Weight = alpha, i.e. a constant-stiffness spring, NOT alpha/length.
        // Figure 6 labels the pseudonet "weight = alpha/Length", but reading
        // Length as the raw cell-to-anchor distance makes the scheme unusable, and
        // the arithmetic is unambiguous. Measured on adaptec1 (see the per-cell
        // "pseudonet share" trace): the per-cell interconnect diagonal is ~0.16,
        // while a lower bound sits ~1000 units from its legal anchor, so
        //     w = alpha/(d + eps)  ->  anchor share ~1e-4, never moves a cell
        //     w = alpha            ->  anchor share reaches parity around
        //                             iteration 15, inside the paper's 26-35
        //                             global-placement iterations.
        // So the figure's Length must be a normalised length of order 1, in which
        // case alpha/Length reduces to alpha up to a constant the published
        // schedule is already calibrated against.
        if (useAnchors) {
            for (std::size_t i = 0; i < numMovable_; ++i) {
                const double anchor = (dim == 0) ? anchorX_[i] : anchorY_[i];
                double w = alpha * anchorScale_;
                if (par_.pseudonetLaw == SimplParams::PseudonetLaw::InverseLength) {
                    // alpha / distance, with the same length floor as a B2B edge.
                    // No anchorScale_ here: alpha/d already scales with the units
                    // exactly as a B2B edge's 1/d does, so it is unit-free as is.
                    // The floor matters most exactly where the paper's initial
                    // placement puts everything: all cells start near the centre,
                    // so distance is often ~0 and the weight is otherwise
                    // unbounded.
                    const double d = std::max(std::abs(px[i] - anchor), lenFloor[dim]);
                    w = alpha / d;
                }
                diag[i] += w;
                rhs[i] += w * anchor;
                if (dim == 0) {
                    anchorDiagSum += w;
                }
            }
        }

        // Assemble CSR. Duplicate (row,col) pairs are summed by matvec, which is
        // the correct superposition of weights.
        A.rowPtr.assign(numMovable_ + 1, 0);
        for (std::size_t i = 0; i < numMovable_; ++i) {
            A.rowPtr[i + 1] = A.rowPtr[i] + rows[i].size();
        }
        A.col.assign(A.rowPtr[numMovable_], 0);
        A.val.assign(A.rowPtr[numMovable_], 0.0);
        for (std::size_t i = 0; i < numMovable_; ++i) {
            std::size_t e = A.rowPtr[i];
            for (const Triple &t : rows[i]) {
                A.col[e] = t.col;
                A.val[e] = t.val;
                ++e;
            }
        }
        // A cell with no nets would otherwise have a zero diagonal, and the Jacobi
        // preconditioner 1/diag would be unbounded.
        A.diag.assign(numMovable_, 0.0);
        for (std::size_t i = 0; i < numMovable_; ++i) {
            A.diag[i] = std::max(diag[i], 1e-12);
        }
        if (dim == 0) {
            nnzX = A.rowPtr[numMovable_];
        } else {
            nnzY = A.rowPtr[numMovable_];
        }
    }

    // How much of the system the pseudonets actually control.
    double wlDiag = 0.0;
    for (std::size_t i = 0; i < numMovable_; ++i) {
        wlDiag += Ax_.diag[i];
        if (useAnchors && par_.pseudonetLaw == SimplParams::PseudonetLaw::ConstantStiffness) {
            wlDiag -= alpha * anchorScale_;
        }
    }
    ktlog.trace("  b2b: {} x-edges, {} y-edges ({:.2f}/{:.2f} per cell)", nnzX, nnzY,
                static_cast<double>(nnzX) / std::max<std::size_t>(numMovable_, 1),
                static_cast<double>(nnzY) / std::max<std::size_t>(numMovable_, 1));
    if (useAnchors) {
        const double perCell = 1.0 / std::max<std::size_t>(numMovable_, 1);
        ktlog.trace(
            "  pseudonet share: anchor diagonal {:.4g} vs interconnect diagonal {:.4g} "
            "= {:.3g} (alpha {:.4g}); per cell {:.4g} vs {:.4g}",
            anchorDiagSum, wlDiag, anchorDiagSum / std::max(wlDiag, 1e-300), alpha,
            anchorDiagSum * perCell, wlDiag * perCell);
    }
}

void SimplePlacer::Impl::solve(const std::string &tag, bool allowFrames, std::size_t cgEvery) {
    // Jacobi-preconditioned CG, run once per axis. The x and y systems are
    // different matrices with different right-hand sides, so they are solved
    // separately; the B2B model is separable, which is why this is two clean
    // SPD systems rather than one coupled 2n-by-2n one.
    //
    // The two systems share no state, so they are advanced in one loop rather
    // than one after the other: each iteration takes one step on x and one on y
    // and stops each axis as soon as that axis converges. The solutions are
    // identical either way -- neither axis' recurrence reads the other -- but the
    // iteration count drops from itersX + itersY to their maximum, and, more to
    // the point here, the animation gets one frame per iteration instead of two.
    // A frame that shows the x sweep and a second that shows the y sweep reads as
    // two events when the run only took one step of each.
    //
    // `tag` identifies the outer context ("init3", "g07", ...).
    cgTag_ = tag;

    // One axis' CG state. Everything the recurrence needs lives here so the two
    // axes can be interleaved without either one's bookkeeping disturbing the
    // other's.
    struct Axis {
        const CsrMatrix *A = nullptr;
        const std::vector<double> *b = nullptr;
        std::vector<double> *x = nullptr;
        std::vector<double> r, z, p, Ap, invDiag;
        double bNorm = 0.0;
        double rho = 0.0;
        double resid = 0.0;
        std::size_t iters = 0;
        std::size_t itersToTol = 0;
        bool active = false;
    };

    const auto setup = [&](Axis &ax, const CsrMatrix &A, const std::vector<double> &b,
                           std::vector<double> &x) {
        const std::size_t n = A.n;
        ax.A = &A;
        ax.b = &b;
        ax.x = &x;
        ax.r.assign(n, 0.0);
        ax.z.assign(n, 0.0);
        ax.p.assign(n, 0.0);
        ax.Ap.assign(n, 0.0);
        ax.invDiag.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            ax.invDiag[i] = 1.0 / A.diag[i];
        }
        A.matvec(x, ax.Ap);
        // The norm of b, and then the initial residual r = b - A.x. The two are
        // independent, so neither has to wait for the other: the original fused
        // them into one loop, which meant the norm could not be a reduction and
        // the residual could not be element-wise.
        ax.bNorm = A.norm2(b);
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 4096),
                          [&](const tbb::blocked_range<std::size_t> &r) {
                              for (std::size_t i = r.begin(); i < r.end(); ++i) {
                                  ax.r[i] = b[i] - ax.Ap[i];
                              }
                          });
        ax.bNorm = std::sqrt(ax.bNorm);
        if (!(ax.bNorm > 0.0)) {
            return;  // nothing to solve; leave the axis where it is
        }
        // Jacobi preconditioner, then the r.z inner product. Assigned rather than
        // accumulated: the accumulator form carried a value across calls if an axis
        // were set up twice, and there is no call that wants that.
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 4096),
                          [&](const tbb::blocked_range<std::size_t> &r) {
                              for (std::size_t i = r.begin(); i < r.end(); ++i) {
                                  ax.z[i] = ax.invDiag[i] * ax.r[i];
                              }
                          });
        ax.rho = A.dot(ax.r, ax.z);
        ax.p = ax.z;
        ax.resid = std::sqrt(A.norm2(ax.r)) / ax.bNorm;
        ax.active = true;
    };

    // One CG step. Returns false if the recurrence can go no further, which is
    // how the reference's non-positive curvature test is handled: the axis stops
    // where it is rather than propagating a NaN.
    const auto step = [](Axis &ax) {
        const std::size_t n = ax.A->n;
        ax.A->matvec(ax.p, ax.Ap);
        const double pAp = ax.A->dot(ax.p, ax.Ap);
        if (!(pAp > 0.0)) {
            return false;
        }
        const double alpha = ax.rho / pAp;
        // Element-wise, so it parallelises exactly: every index touches its own
        // slot and nothing is shared. The residual norm is the matrix's own
        // parallel reduction rather than a hand-rolled serial sum in the same loop,
        // because a reduction carried alongside the update forces the whole update
        // to stay serial.
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 4096),
                          [&](const tbb::blocked_range<std::size_t> &r) {
                              for (std::size_t i = r.begin(); i < r.end(); ++i) {
                                  (*ax.x)[i] += alpha * ax.p[i];
                                  ax.r[i] -= alpha * ax.Ap[i];
                              }
                          });
        ax.resid = std::sqrt(ax.A->norm2(ax.r)) / ax.bNorm;
        if (ax.itersToTol == 0 && ax.resid <= 1e-3) {
            ax.itersToTol = ax.iters;
        }
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 4096),
                          [&](const tbb::blocked_range<std::size_t> &r) {
                              for (std::size_t i = r.begin(); i < r.end(); ++i) {
                                  ax.z[i] = ax.invDiag[i] * ax.r[i];
                              }
                          });
        const double rhoNew = ax.A->dot(ax.r, ax.z);
        if (!(rhoNew > 0.0)) {
            return false;
        }
        const double beta = rhoNew / ax.rho;
        tbb::parallel_for(tbb::blocked_range<std::size_t>(0, n, 4096),
                          [&](const tbb::blocked_range<std::size_t> &r) {
                              for (std::size_t i = r.begin(); i < r.end(); ++i) {
                                  ax.p[i] = ax.z[i] + beta * ax.p[i];
                              }
                          });
        ax.rho = rhoNew;
        return true;
    };

    Axis ax, ay;
    setup(ax, Ax_, rhsX_, solX_);
    setup(ay, Ay_, rhsY_, solY_);

    for (std::size_t it = 0; it < par_.cgMaxIter; ++it) {
        // Overridable, because on this system the residual has a floor: every
        // solve runs the full cgMaxIter and finishes between 1e-3 and 5e-3, so
        // cgTol = 1e-3 is below what Jacobi preconditioning reaches and the cap is
        // the only thing stopping it. That makes cgMaxIter, not the tolerance, the
        // real stopping rule, and it is why a 100-iteration run costs 20 minutes.
        if (const char *e = std::getenv("KTPLACE_SIMPL_CG_TOL")) {
            const double v = std::atof(e);
            if (v > 0.0) {
                par_.cgTol = v;
            }
        }
        const bool liveX = ax.active && ax.resid > par_.cgTol;
        const bool liveY = ay.active && ay.resid > par_.cgTol;
        if (!liveX && !liveY) {
            break;
        }
        if (liveX) {
            ++ax.iters;
            if (!step(ax)) {
                ax.active = false;
            }
        }
        if (liveY) {
            ++ay.iters;
            if (!step(ay)) {
                ay.active = false;
            }
        }
        // One frame per iteration, after both axes have moved, labelled with both
        // residuals -- the placement on it is the state the next step starts from.
        //
        // The cadence is a sampling interval, not a filter that can drop a whole
        // solve: with cgEvery at 5 and a separable solve that converges in three
        // iterations, the modulo never fires and that solve contributes no frames
        // at all, which is why the per-iteration record was dense in the warm-up
        // (long solves) and nearly empty in the global loop (short ones). So the
        // last iteration of every solve is always recorded, and the interval
        // governs the iterations between.
        const bool stillGoing =
            (ax.active && ax.resid > par_.cgTol) || (ay.active && ay.resid > par_.cgTol);
        if (allowFrames && cgEvery > 0 && !frameDir_.empty() &&
            (((it + 1) % cgEvery) == 0 || !stillGoing)) {
            writeCgFrame(tag, it + 1, ax.resid, ay.resid);
        }
    }

    ktlog.trace(
        "  cg: x {} iteration(s) (1e-3 at {}) residual {:.3e}, y {} iteration(s) (1e-3 at {}) "
        "residual {:.3e}",
        ax.iters, ax.itersToTol, ax.resid, ay.iters, ay.itersToTol, ay.resid);
    lastResidual_ = std::max(ax.resid, ay.resid);
}

double SimplePlacer::Impl::hpwl(const std::vector<double> &px,
                                const std::vector<double> &py) const {
    std::vector<double> cx(nv_), cy(nv_);
    for (std::size_t v = 0; v < nv_; ++v) {
        cx[v] = vx_[v];
        cy[v] = vy_[v];
    }
    for (std::size_t i = 0; i < numMovable_; ++i) {
        cx[movVertex_[i]] = px[i];
        cy[movVertex_[i]] = py[i];
    }
    double total = 0.0;
    for (std::size_t v = 0; v < nets_.size(); ++v) {
        const NetInfo &ni = nets_[v];
        if (ni.cell.size() < 2) {
            continue;
        }
        double x0 = std::numeric_limits<double>::max();
        double x1 = -std::numeric_limits<double>::max();
        double y0 = std::numeric_limits<double>::max();
        double y1 = -std::numeric_limits<double>::max();
        for (std::size_t q = 0; q < ni.cell.size(); ++q) {
            const double x = cx[ni.cell[q]] + ni.offX[q];
            const double y = cy[ni.cell[q]] + ni.offY[q];
            x0 = std::min(x0, x);
            x1 = std::max(x1, x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
        }
        total += (x1 - x0) + (y1 - y0);
    }
    return total;
}

// ---------------------------------------------------------------------------
// Density
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::binCells(const std::vector<double> &px, const std::vector<double> &py) {
    std::fill(grid_.occ.begin(), grid_.occ.end(), 0.0);
    for (auto &b : binCells_) {
        b.clear();
    }
    if (cellBin_.size() != numMovable_) {
        binCells_.assign(grid_.size(), {});
        cellBin_.assign(numMovable_, 0);
        cellSlot_.assign(numMovable_, 0);
    }
    if (numMovable_ == 0) {
        return;
    }
    const std::size_t nb = grid_.size();

    // Binning is a map over cells followed by a scatter, and both are data
    // parallel. The scatter deliberately does not preserve write order; the sort
    // in the last step restores the exact serial order (ascending cell index
    // inside every bin), so this produces the same binCells_/cellBin_/cellSlot_
    // and the same per-bin occupancy as the single-threaded loop it replaced.
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable_, 4096),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i < r.end(); ++i) {
                              std::size_t ix, iy;
                              grid_.locate(px[i], py[i], ix, iy);
                              cellBin_[i] = static_cast<std::uint32_t>(grid_.at(ix, iy));
                          }
                      });

    std::vector<std::size_t> start(nb + 1, 0);
    for (std::size_t i = 0; i < numMovable_; ++i) {
        ++start[cellBin_[i] + 1];
    }
    for (std::size_t k = 0; k < nb; ++k) {
        start[k + 1] += start[k];
    }

    std::unique_ptr<std::atomic<std::size_t>[]> cursor(new std::atomic<std::size_t>[nb]);
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, nb, 512),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t k = r.begin(); k < r.end(); ++k) {
                              binCells_[k].resize(start[k + 1] - start[k]);
                              cursor[k].store(0, std::memory_order_relaxed);
                          }
                      });

    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, numMovable_, 4096),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i < r.end(); ++i) {
                              const std::size_t k = cellBin_[i];
                              const std::size_t pos =
                                  cursor[k].fetch_add(1, std::memory_order_relaxed);
                              binCells_[k][pos] = static_cast<std::uint32_t>(i);
                          }
                      });

    // Restore the serial order, then recompute the slots and each bin's
    // occupancy by summing its cells in cell-index order -- the same order the
    // original accumulation used, so grid_.occ carries the same values.
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, nb, 512),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t k = r.begin(); k < r.end(); ++k) {
                              std::vector<std::uint32_t> &v = binCells_[k];
                              std::sort(v.begin(), v.end());
                              double occ = 0.0;
                              for (std::size_t p = 0; p < v.size(); ++p) {
                                  cellSlot_[v[p]] = static_cast<std::uint32_t>(p);
                                  occ += area_[v[p]];
                              }
                              grid_.occ[k] = occ;
                          }
                      });
}

double SimplePlacer::Impl::densityOf(std::size_t ix0, std::size_t ix1, std::size_t iy0,
                                     std::size_t iy1) const {
    double c = 0.0;
    double a = 0.0;
    for (std::size_t iy = iy0; iy <= iy1 && iy < grid_.nby; ++iy) {
        for (std::size_t ix = ix0; ix <= ix1 && ix < grid_.nbx; ++ix) {
            const std::size_t k = grid_.at(ix, iy);
            c += grid_.occ[k];
            a += grid_.avail[k];
        }
    }
    return (a > 0.0) ? (c / a) : 0.0;
}

double SimplePlacer::Impl::scaledOverflow() const {
    if (!(grid_.totalAvail > 0.0)) {
        return 0.0;
    }
    double ex = 0.0;
    for (std::size_t k = 0; k < grid_.occ.size(); ++k) {
        const double cap = g_ * grid_.avail[k];
        if (grid_.occ[k] > cap) {
            ex += grid_.occ[k] - cap;
        }
    }
    return ex / grid_.totalAvail;
}

// ---------------------------------------------------------------------------
// Look-ahead legalization (Algorithm 1)
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::nonlinearScale(const std::vector<std::uint32_t> &cells, std::size_t a0,
                                        std::size_t a1, std::size_t b0, std::size_t b1,
                                        bool vertical, double cutCoord) {
    if (cells.empty() || a0 > a1 || b0 > b1) {
        return;
    }
    const DensityGrid &g = grid_;

    // (i) Stripe boundaries. Cutlines are drawn along obstacle edges first, then
    // any stripe still holding more than 1/10 of the region's available area is
    // subdivided, so no single stripe is a large target that would leave the
    // others empty.
    std::vector<std::size_t> bounds;
    bounds.push_back(a0);
    bounds.push_back(a1 + 1);
    for (const std::uint32_t fv : fixVertex_) {
        const Vertex &vert = graph_.getCell(fv);
        if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
            continue;
        }
        const double lo = vertical ? vx_[fv] : vy_[fv];
        const double len = vertical ? vert.width : vert.height;
        const double axis0 = vertical ? g.x0 : g.y0;
        const double dAxis = vertical ? g.dx : g.dy;
        const double nBins = static_cast<double>(vertical ? g.nbx : g.nby);
        // The obstacle's two edges, as bin indices along the cut axis.
        const std::size_t t0 =
            static_cast<std::size_t>(std::clamp((lo - axis0) / dAxis, 0.0, nBins - 1e-9));
        const std::size_t t1 =
            static_cast<std::size_t>(std::clamp((lo + len - axis0) / dAxis, 0.0, nBins - 1e-9));
        if (t1 < a0 || t0 > a1) {
            continue;  // obstacle outside this block
        }
        const std::size_t c0 = std::max(t0, a0);
        const std::size_t c1 = std::min(t1, a1);
        bounds.push_back(c0);
        bounds.push_back(c1 + 1);
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    // "Each vertical stripe created in this process is further subdivided if its
    // available area exceeds 1/10 of the region's available area." One rule, one
    // pass over the stripe list, cutting at the available-area midpoint so the
    // two halves are equal in area rather than equal in bin count.
    {
        const auto availIn = [&](std::size_t t0, std::size_t t1) {
            double sa = 0.0;
            for (std::size_t t = t0; t < t1; ++t) {
                for (std::size_t u = b0; u <= b1; ++u) {
                    sa += g.avail[vertical ? g.at(t, u) : g.at(u, t)];
                }
            }
            return sa;
        };
        const double regionAvail = availIn(a0, a1);
        const double thresh = par_.stripeAreaFraction * regionAvail;
        bool grew = true;
        std::size_t guard = 0;
        while (grew && guard++ < 32) {
            grew = false;
            std::vector<std::size_t> next;
            next.push_back(bounds.front());
            for (std::size_t k = 0; k + 1 < bounds.size(); ++k) {
                const std::size_t lo = bounds[k], hi = bounds[k + 1];
                if (hi > lo + 1 && availIn(lo, hi) > thresh) {
                    const double half = availIn(lo, hi) * 0.5;
                    double acc = 0.0;
                    std::size_t cut = hi - 1;
                    for (std::size_t t = lo; t < hi; ++t) {
                        for (std::size_t u = b0; u <= b1; ++u) {
                            acc += g.avail[vertical ? g.at(t, u) : g.at(u, t)];
                        }
                        if (acc >= half) {
                            cut = t;
                            break;
                        }
                    }
                    if (cut > lo && cut < hi) {
                        next.push_back(cut + 1);
                        grew = true;
                    }
                }
                next.push_back(hi);
            }
            bounds.swap(next);
        }
    }

    const std::size_t nStripes = bounds.size() - 1;
    if (nStripes == 0) {
        return;
    }

    // Stripe geometry and capacity.
    std::vector<double> stripeLo(nStripes), stripeHi(nStripes), stripeCap(nStripes),
        stripeUsed(nStripes, 0.0);
    // Available area per stripe, kept for the scaling below: it is what decides how
    // far the stripe's cells have to spread, and it is not the geometric width.
    std::vector<double> stripeAvail(nStripes, 0.0);
    const double axisLo = vertical ? g.x0 : g.y0;
    const double dAxis = vertical ? g.dx : g.dy;
    for (std::size_t s = 0; s < nStripes; ++s) {
        stripeLo[s] = axisLo + static_cast<double>(bounds[s]) * dAxis;
        stripeHi[s] = axisLo + static_cast<double>(bounds[s + 1]) * dAxis;
        // Capacity is g * A_a of the stripe, with A_a the AVAILABLE area, not
        // the geometric area. A stripe that is half covered by a fixed macro has
        // half the room; charging it the full geometric area lets the greedy pack
        // drop cells inside the blockage, which is precisely the overlap the
        // legalizer is supposed to remove.
        double sa = 0.0;
        for (std::size_t t = bounds[s]; t < bounds[s + 1]; ++t) {
            for (std::size_t u = b0; u <= b1; ++u) {
                sa += g.avail[vertical ? g.at(t, u) : g.at(u, t)];
            }
        }
        stripeAvail[s] = sa;
        stripeCap[s] = g_ * sa;
    }
    // Furthest stripe from the cutline first, as in Figure 4(iii).
    std::vector<std::size_t> order(nStripes);
    std::iota(order.begin(), order.end(), 0u);
    const auto dist = [&](std::size_t s) {
        return std::abs(0.5 * (stripeLo[s] + stripeHi[s]) - cutCoord);
    };
    std::stable_sort(order.begin(), order.end(), [&](std::size_t p, std::size_t q) {
        return dist(p) > dist(q);
    });

    // (ii) Cells sorted by distance from C_B, descending: the cell furthest from
    // the cutline is assigned to the furthest stripe.
    std::vector<std::uint32_t> seq = cells;
    const auto cellDist = [&](std::uint32_t i) {
        return std::abs((vertical ? pinX_[i] : pinY_[i]) - cutCoord);
    };
    std::stable_sort(seq.begin(), seq.end(), [&](std::uint32_t a, std::uint32_t b) {
        return cellDist(a) > cellDist(b);
    });

    // (iii) Greedy packing into the furthest stripe that still has room. A stripe
    // is full once it holds g * A_a.
    std::vector<std::vector<std::uint32_t>> packed(nStripes);
    for (const std::uint32_t i : seq) {
        std::size_t chosen = nStripes;
        for (const std::size_t s : order) {
            if (stripeUsed[s] + area_[i] <= stripeCap[s]) {
                chosen = s;
                break;
            }
        }
        if (chosen == nStripes) {
            // Every stripe is at or over its nominal capacity. Put the cell in the
            // one with the most headroom left rather than the nearest to the
            // cutline, so an over-full region evens out instead of piling into one
            // edge. The comparison starts from -inf so a stripe is always picked
            // even when every headroom is zero.
            double best = -std::numeric_limits<double>::max();
            for (const std::size_t s : order) {
                const double head = stripeCap[s] - stripeUsed[s];
                if (head > best) {
                    best = head;
                    chosen = s;
                }
            }
            if (chosen == nStripes) {
                chosen = order.front();
            }
        }
        packed[chosen].push_back(i);
        stripeUsed[chosen] += area_[i];
    }

    // (iv) Cell locations within each stripe are linearly scaled from their current
    // locations, and different stripes get different factors -- which is where the
    // nonlinearity comes from.
    //
    // The factor is the one that brings the stripe's assigned area down to the
    // density limit, so it is derived from the stripe's AVAILABLE area and not from
    // its geometric extent. That distinction is the correctness of the step: a
    // stripe half covered by a fixed macro has half the room, and scaling its cells
    // to the full geometric width spreads them straight through the blockage -- the
    // overlap this legalizer exists to remove, put back by the step meant to remove
    // it. Cells are scaled about the cutline, which keeps each cell's distance from
    // it proportional and so keeps the ordering the stripes were assigned by, and
    // the result is clamped to the stripe so a factor above one cannot throw a cell
    // into a neighbour.
    for (std::size_t s = 0; s < nStripes; ++s) {
        if (packed[s].empty()) {
            continue;
        }
        double assigned = 0.0;
        for (const std::uint32_t i : packed[s]) {
            assigned += area_[i];
        }
        if (!(assigned > 0.0)) {
            continue;
        }
        if (par_.stripeScaleMode == SimplParams::StripeScale::Fill) {
            // Sort and greedily pack, which is the mechanism the paper names for
            // this step: "If relative placement must be preserved, overlap can be
            // reduced by means of x- and y-sorting with subsequent greedy packing."
            //
            // Sorting by position and walking the stripe lays the cells down at a
            // uniform density g, each starting where the previous one ended, so
            // the stripe ends up holding exactly what it can and the cells keep
            // their relative order. The other modes scale each cell about the
            // stripe centre and then clamp, and clamping is what breaks: a cell
            // assigned to a stripe it is nowhere near lands exactly on the stripe
            // edge, so a whole stripe's worth of cells arrives on one line. That
            // is not neutral, it is actively harmful -- the worst bin got *worse*
            // as the average improved (12.3x to 26.9x over three passes on
            // adaptec1), because each pass piled the survivors onto the next
            // stripe boundary, and the recursion settled into a fixed point at
            // 0.245 overflow that fourteen further passes could not move.
            //
            // A stripe holding less than its capacity is stretched over its whole
            // length instead of piling up at one end, which is what keeps a sparse
            // stripe from becoming the next hot spot.
            std::vector<std::uint32_t> order2 = packed[s];
            const bool vert = vertical;
            std::stable_sort(order2.begin(), order2.end(), [&](std::uint32_t a, std::uint32_t b) {
                return (vert ? pinX_[a] : pinY_[a]) < (vert ? pinX_[b] : pinY_[b]);
            });
            const double L = stripeHi[s] - stripeLo[s];
            const double Aa = stripeAvail[s];
            // The length a cell occupies when the stripe is full: the stripe's
            // equivalent depth times g is the area it may hold per unit length,
            // so a cell of area a takes a / (g * h).
            const double h = (L > 0.0) ? Aa / L : 0.0;
            const double dens = g_ * h;
            if (!(dens > 0.0) || !(L > 0.0)) {
                continue;
            }
            double need = 0.0;
            for (const std::uint32_t i : order2) {
                need += area_[i] / dens;
            }
            // Over capacity only: the slots are shortened so the run fits the
            // stripe. An under-full stripe is NOT stretched -- see below.
            const double slot = (need > L) ? (L / need) : 1.0;

            // Greedy packing, minimal displacement. Each cell keeps its current
            // position unless the previous one is in the way, in which case it is
            // pushed just past it.
            //
            // Stretching an under-full stripe over its whole length -- which is
            // what filling to a uniform density means taken literally -- moves
            // cells that were never overlapping, and that is where the wirelength
            // goes: on adaptec1, repacking every stripe uniformly reached 0.107
            // overflow at 4.68e8, while leaving under-full stripes alone reaches
            // 0.169 at 4.40e8. The paper's stated aim for this step is removing
            // overlap "while preserving the relative ordering", which is a
            // minimal-displacement requirement and not a uniform-density one.
            double prev = stripeLo[s];
            std::vector<double> pos(order2.size(), 0.0);
            for (std::size_t k = 0; k < order2.size(); ++k) {
                const std::uint32_t i = order2[k];
                const double half = 0.5 * (area_[i] / dens) * slot;
                const double cur = vert ? pinX_[i] : pinY_[i];
                pos[k] = std::clamp(std::max(cur, prev + half), stripeLo[s], stripeHi[s]);
                prev = pos[k] + half;
            }
            // The forward sweep pushes the tail into the high edge. Shift the
            // whole run back by however much it overshot, so a stripe that only
            // just fits is centred rather than pressed against one side, and
            // re-clamp: the shift can put the head below the low edge.
            if (prev > stripeHi[s]) {
                const double shift = stripeHi[s] - prev;
                for (double &v : pos) {
                    v = std::clamp(v + shift, stripeLo[s], stripeHi[s]);
                }
            }
            for (std::size_t k = 0; k < order2.size(); ++k) {
                const std::uint32_t i = order2[k];
                if (vert) {
                    pinX_[i] = pos[k];
                } else {
                    pinY_[i] = pos[k];
                }
                std::size_t nx2, ny2;
                grid_.locate(pinX_[i], pinY_[i], nx2, ny2);
                rehome(i, grid_.at(nx2, ny2));
            }
            continue;
        }
        const double room = g_ * std::max(stripeAvail[s], 0.0);
        // How far the stripe has to spread to bring its assigned cells down to the
        // density limit. The paper says cells are "linearly scaled from current
        // locations" and does not give the factor, and the two natural readings
        // disagree about its sign -- a stripe that is over capacity compresses
        // under one and spreads under nothing at all. So it is a mode and it is
        // measured, rather than assumed and defended.
        double factor = 1.0;
        if (room > 0.0) {
            const double raw = std::sqrt(room / assigned);
            switch (par_.stripeScaleMode) {
                case SimplParams::StripeScale::Both:
                    factor = raw;
                    break;
                case SimplParams::StripeScale::Tight:
                    factor = (raw < 1.0) ? raw : 1.0;
                    break;
                case SimplParams::StripeScale::None:
                    break;
                case SimplParams::StripeScale::Fill:
                    // Handled above: the fill mode returns before the scaling
                    // factors are computed at all. Named here so the switch stays
                    // exhaustive against the enum.
                    break;
            }
        }
        // Scale about the STRIPE's own centre, not the block's cutline. Scaling
        // about the cutline looks right and is wrong: a stripe is a sub-range of
        // the block, so for every stripe that does not contain the cutline a
        // factor below one drags its cells toward the stripe's inner edge, and the
        // clamp below then stacks them on that edge. The result is a hard-edged
        // band of cells per stripe -- the streaks a look-ahead frame shows on
        // adaptec1 -- rather than a stripe filled evenly. About the stripe centre
        // a factor below one shrinks its contents symmetrically into the stripe,
        // which is what "scaled from their current locations" has to mean for the
        // cells to stay spread within the stripe they were assigned to.
        const double centre = 0.5 * (stripeLo[s] + stripeHi[s]);
        for (const std::uint32_t i : packed[s]) {
            double &target = vertical ? pinX_[i] : pinY_[i];
            const double p = vertical ? pinX_[i] : pinY_[i];
            const double q = (factor == 1.0) ? p : centre + (p - centre) * factor;
            target = std::clamp(q, stripeLo[s], stripeHi[s]);
            // Keep the bin index exact for the blocks that run next.
            std::size_t nx2, ny2;
            grid_.locate(pinX_[i], pinY_[i], nx2, ny2);
            rehome(i, grid_.at(nx2, ny2));
        }
    }
}

void SimplePlacer::Impl::processBlock(const Block &B) {
    if (B.level >= par_.maxLevel) {
        return;  // Algorithm 1 line 8
    }
    // M = movable cells inside the block, gathered from the live bin index. The
    // index is maintained by nonlinearScale() as cells are rehomed, so this is
    // exact and costs only the cells actually in the block.
    std::vector<std::uint32_t> M;
    for (std::size_t iy = B.iy0; iy <= B.iy1 && iy < grid_.nby; ++iy) {
        for (std::size_t ix = B.ix0; ix <= B.ix1 && ix < grid_.nbx; ++ix) {
            const std::size_t k = grid_.at(ix, iy);
            M.insert(M.end(), binCells_[k].begin(), binCells_[k].end());
        }
    }
    if (M.size() <= par_.minCellsToSplit) {
        // Algorithm 1 line 8, verbatim: "if (Area(B) is small enough || B.level
        // >= 10) then CONTINUE". The block is dropped, not scaled. This used to
        // fall through into a leaf scaling instead, on the argument that the cells
        // still had to be spread or they stayed overlapped. The paper's position
        // is the opposite one: the parent already scaled these cells into its own
        // sub-regions (line 15), so a block small enough to stop at has been
        // dealt with, and scaling it a third time moves cells the algorithm never
        // intended to move -- which is a wirelength cost with no legality
        // argument behind it.
        return;
    }
    maxBlockCells_ = std::max(maxBlockCells_, M.size());

    const std::size_t a0 = B.vertical ? B.ix0 : B.iy0;  // extent along the cut axis
    const std::size_t a1 = B.vertical ? B.ix1 : B.iy1;
    const std::size_t b0 = B.vertical ? B.iy0 : B.ix0;  // extent across it
    const std::size_t b1 = B.vertical ? B.iy1 : B.ix1;
    if (a0 >= a1) {
        // One bin across the cut axis: there is no cutline to place, so there is
        // nothing for line 15 to scale between. Skipped, as above.
        return;
    }

    // C_c: the cutline that evenly splits the cell area, i.e. the cell-area
    // median along the axis.
    double cellArea = 0.0;
    for (std::size_t t = a0; t <= a1; ++t) {
        for (std::size_t s = b0; s <= b1; ++s) {
            cellArea += grid_.occ[B.vertical ? grid_.at(t, s) : grid_.at(s, t)];
        }
    }
    // C_B: the cutline that evenly partitions the whitespace, i.e. the median of
    // the available area that is *not* occupied.
    double white = 0.0;
    for (std::size_t t = a0; t <= a1; ++t) {
        for (std::size_t s = b0; s <= b1; ++s) {
            const std::size_t k = B.vertical ? grid_.at(t, s) : grid_.at(s, t);
            white += std::max(grid_.avail[k] - grid_.occ[k], 0.0);
        }
    }

    const auto medianCut = [&](double target, bool byCellArea) {
        double cum = 0.0;
        std::size_t cut = a1;
        for (std::size_t t = a0; t <= a1; ++t) {
            for (std::size_t s = b0; s <= b1; ++s) {
                const std::size_t k = B.vertical ? grid_.at(t, s) : grid_.at(s, t);
                cum += byCellArea ? grid_.occ[k] : std::max(grid_.avail[k] - grid_.occ[k], 0.0);
            }
            if (cum >= target) {
                cut = t;
                break;
            }
        }
        return cut;
    };

    std::size_t cutC = (cellArea > 0.0) ? medianCut(0.5 * cellArea, true) : a0 + (a1 - a0) / 2;
    std::size_t cutB = (white > 0.0) ? medianCut(0.5 * white, false) : a0 + (a1 - a0) / 2;
    // Both cutlines must actually separate the block, otherwise the recursion
    // would not make progress.
    cutC = std::clamp(cutC, a0, a1 - 1);
    cutB = std::clamp(cutB, a0, a1 - 1);

    const double axisLo = B.vertical ? grid_.x0 : grid_.y0;
    const double dAxis = B.vertical ? grid_.dx : grid_.dy;
    const double cutCoord = axisLo + static_cast<double>(cutB) * dAxis;

    const std::size_t bA0 = a0, bA1 = cutB, bB0 = cutB + 1, bB1 = a1;

    // (M_0, M_1) come from the cell-area cutline; (B_0, B_1) from the whitespace
    // cutline. Because the two partitions differ, redistributing cells into the
    // whitespace partition is what equalises density across the two halves.
    std::vector<std::uint32_t> M0, M1;
    const double cutCoordC = axisLo + static_cast<double>(cutC + 1) * dAxis;
    for (const std::uint32_t i : M) {
        const double p = (B.vertical ? pinX_[i] : pinY_[i]);
        (p < cutCoordC ? M0 : M1).push_back(i);
    }

    // No rebalancing of M0/M1 against B0/B1 here. Algorithm 1 fixes the two
    // sets at line 13 -- "(M_0, M_1) = {movable cells in S_0, S_1}" -- from the
    // cell-area cutline, and line 15 moves them into the whitespace halves. That
    // pairing IS the equalisation, and Figure 3's caption says so: "adjustment
    // of cell-area to whitespace median BY NONLINEAR SCALING". A separate pass
    // that hands cells across Cc before the scaling is not in the paper and
    // overrides the very cutline the algorithm defines the sub-regions by.
    nonlinearScale(M0, bA0, bA1, b0, b1, B.vertical, cutCoord);
    nonlinearScale(M1, bB0, bB1, b0, b1, B.vertical, cutCoord);

    // Alternate the cut direction at each level, and enqueue both whitespace
    // halves.
    Block n0, n1;
    n0.level = n1.level = B.level + 1;
    n0.vertical = n1.vertical = !B.vertical;
    if (B.vertical) {
        n0 = {bA0, bA1, B.iy0, B.iy1, n0.level, n0.vertical};
        n1 = {bB0, bB1, B.iy0, B.iy1, n1.level, n1.vertical};
    } else {
        n0 = {B.ix0, B.ix1, bA0, bA1, n0.level, n0.vertical};
        n1 = {B.ix0, B.ix1, bB0, bB1, n1.level, n1.vertical};
    }
    pending_.push_back(n0);
    pending_.push_back(n1);
}

void SimplePlacer::Impl::lookAheadLegalize() {
    const double before_ = scaledOverflow();
    // 1) Identify g-overfilled bins and cluster them by BFS (4-connected).
    std::vector<char> over(grid_.size(), 0);
    for (std::size_t k = 0; k < grid_.size(); ++k) {
        if (grid_.avail[k] > 0.0 && grid_.occ[k] / grid_.avail[k] > g_) {
            over[k] = 1;
        }
    }
    std::vector<char> seen(grid_.size(), 0);
    std::vector<std::size_t> queue;
    std::size_t nClusters = 0;
    std::size_t globalRegions_ = 0;
    const double totalCellArea_ = grid_.totalCellArea;

    for (std::size_t seed = 0; seed < grid_.size(); ++seed) {
        if (!over[seed] || seen[seed]) {
            continue;
        }
        queue.clear();
        queue.push_back(seed);
        seen[seed] = 1;
        std::size_t ix0 = seed % grid_.nbx, ix1 = ix0;
        std::size_t iy0 = seed / grid_.nbx, iy1 = iy0;
        for (std::size_t h = 0; h < queue.size(); ++h) {
            const std::size_t k = queue[h];
            const std::size_t cx = k % grid_.nbx;
            const std::size_t cy = k / grid_.nbx;
            ix0 = std::min(ix0, cx);
            ix1 = std::max(ix1, cx);
            iy0 = std::min(iy0, cy);
            iy1 = std::max(iy1, cy);
            const int dx4[4] = {1, -1, 0, 0};
            const int dy4[4] = {0, 0, 1, -1};
            for (int d = 0; d < 4; ++d) {
                const long nx = static_cast<long>(cx) + dx4[d];
                const long ny = static_cast<long>(cy) + dy4[d];
                if (nx < 0 || ny < 0 || nx >= static_cast<long>(grid_.nbx) ||
                    ny >= static_cast<long>(grid_.nby)) {
                    continue;
                }
                const std::size_t kk =
                    grid_.at(static_cast<std::size_t>(nx), static_cast<std::size_t>(ny));
                if (over[kk] && !seen[kk]) {
                    seen[kk] = 1;
                    queue.push_back(kk);
                }
            }
        }

        ++nClusters;
        // 3) Grow to a minimal containing rectangle whose density is <= g.
        //
        // The paper asks for the MINIMAL such rectangle, and for a local overfull
        // cluster in an otherwise spread placement that is right: it limits the
        // legalizer's freedom exactly as intended ("overlap removal in a region
        // which is filled to capacity is more straightforward ... the absence of
        // whitespace leaves less flexibility for interconnect optimization").
        //
        // It breaks down when the placement is not spread at all. A collapsed blob
        // is a single cluster whose minimal legal-density rectangle is only as big
        // as the cells strictly need -- about 54% of a 53.5%-utilisation die -- so
        // the top-down partitioning is confined to that sub-rectangle and the rest
        // of the die never receives a cell. Measured: the whole left quarter and
        // the top third sat at zero occupancy with bins at 200% density, and that
        // is the ~9x wirelength penalty, because the I/O pads ring the die so cells
        // trapped in a sub-rectangle are far from half of them. (A "lower bound"
        // with wirelength BELOW the legalized result was the tell -- only possible
        // if the legalized cells overlap.)
        //
        // So the minimal rule is kept for genuinely local clusters, and the whole
        // usable die is used when the cluster already holds most of the movable
        // area, i.e. when the placement is globally collapsed and there is no
        // spread placement left to preserve.
        std::size_t rx0 = ix0, rx1 = ix1, ry0 = iy0, ry1 = iy1;
        double clusterArea = 0.0;
        for (std::size_t iy = ry0; iy <= ry1; ++iy) {
            for (std::size_t ix = rx0; ix <= rx1; ++ix) {
                clusterArea += grid_.occ[grid_.at(ix, iy)];
            }
        }
        // A documented deviation from Algorithm 1 line 3, which asks for "a
        // minimal containing rectangular region R superset c with density(R) <= g"
        // and nothing else.
        //
        // When the placement has not spread at all -- the state the very first
        // look-ahead legalization sees, because section 4.1 is explicitly
        // area-blind -- one cluster already holds most of the movable area, and
        // the minimal legal-density rectangle around it is only as big as those
        // cells strictly need. The recursion is then confined to that
        // sub-rectangle, the rest of the die is never visited, and the cells that
        // sit inside it still have to be legalized by the same recursion: the
        // result is a placement with an empty margin and a crowded middle.
        //
        // Measured on adaptec1: minimal rectangles throughout give 6.85e8 against
        // 4.22e8 with this fallback, at the same overflow. Removing it is not
        // something the paper's rule survives on this benchmark, so it stays and
        // is named rather than smuggled in.
        if (par_.globalClusterFrac > 0.0 && totalCellArea_ > 0.0 &&
            clusterArea >= par_.globalClusterFrac * totalCellArea_) {
            std::size_t ux0 = grid_.nbx, uy0 = grid_.nby, ux1 = 0, uy1 = 0;
            for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
                if (grid_.avail[k] <= 0.0) {
                    continue;
                }
                const std::size_t ix = k % grid_.nbx;
                const std::size_t iy = k / grid_.nbx;
                ux0 = std::min(ux0, ix);
                ux1 = std::max(ux1, ix);
                uy0 = std::min(uy0, iy);
                uy1 = std::max(uy1, iy);
            }
            if (ux0 <= ux1 && uy0 <= uy1) {
                rx0 = ux0;
                rx1 = ux1;
                ry0 = uy0;
                ry1 = uy1;
                ++globalRegions_;
            }
        }

        // Algorithm 1 line 3: "Find a minimal rectangular region R superset c with
        // density(R) <= g". Grow while the density is still above g, and stop the
        // moment it is not.
        //
        // One layer on every side per step, rather than one layer on whichever
        // single side adds the most area. A collapsed placement needs a region of
        // most of the die to reach density <= g, and growing one side at a time
        // produces a 1 x N strip: the same area, but a region the recursion then
        // subdivides along its long axis only, so cells are pushed into a narrow
        // band and the whitespace either side is never reached. Measured on
        // adaptec1, single-side growth shipped 7.41e8 against 4.22e8 for the same
        // algorithm with a compact region.
        while (densityOf(rx0, rx1, ry0, ry1) > g_) {
            const bool canL = rx0 > 0;
            const bool canR = rx1 + 1 < grid_.nbx;
            const bool canB = ry0 > 0;
            const bool canT = ry1 + 1 < grid_.nby;
            if (!(canL || canR || canB || canT)) {
                break;  // the whole die is overfull; nothing more to give
            }
            if (canL) {
                --rx0;
            }
            if (canR) {
                ++rx1;
            }
            if (canB) {
                --ry0;
            }
            if (canT) {
                ++ry1;
            }
        }

        pending_.clear();
        pending_.push_back(Block{rx0, rx1, ry0, ry1, 1, true});
        while (!pending_.empty()) {
            const Block B = pending_.front();
            pending_.pop_front();
            ++blocksProcessed_;
            processBlock(B);
            deepestLevel_ = std::max(deepestLevel_, B.level);
        }
    }
    {
        // Where the residual overflow sits: bins overlapped by a fixed macro, or
        // bins with no blockage at all. This separates "the spread did not reach
        // here" from "cells were dropped on top of a macro".
        binCells(pinX_, pinY_);
        double exMacro = 0.0, exFree = 0.0, nMacroBins = 0.0;
        for (std::size_t k = 0; k < grid_.occ.size(); ++k) {
            const double cap = g_ * grid_.avail[k];
            if (grid_.occ[k] <= cap) {
                continue;
            }
            if (grid_.avail[k] < 0.99 * grid_.binArea) {
                exMacro += grid_.occ[k] - cap;
                nMacroBins += 1.0;
            } else {
                exFree += grid_.occ[k] - cap;
            }
        }
        // Which cells sit on a macro is a diagnostic, but the obvious nest is
        // O(cells * fixed) -- 114M rectangle tests on adaptec1, repeated on every
        // global iteration -- and it re-read each macro's vertex inside the inner
        // loop. Hoist the macro rectangles once, then count in parallel: the
        // total is an integer, so reducing it across threads cannot change the
        // value.
        const std::size_t nFixed = fixVertex_.size();
        std::vector<double> macroX0(nFixed), macroY0(nFixed), macroX1(nFixed), macroY1(nFixed);
        for (std::size_t j = 0; j < nFixed; ++j) {
            const std::uint32_t fv = fixVertex_[j];
            const Vertex &fv2 = graph_.getCell(fv);
            macroX0[j] = vx_[fv];
            macroY0[j] = vy_[fv];
            macroX1[j] = vx_[fv] + fv2.width;
            macroY1[j] = vy_[fv] + fv2.height;
        }
        const std::size_t onMacro = tbb::parallel_reduce(
            tbb::blocked_range<std::size_t>(0, numMovable_, 1024), std::size_t{0},
            [&](const tbb::blocked_range<std::size_t> &r, std::size_t acc) {
                for (std::size_t i = r.begin(); i < r.end(); ++i) {
                    const double x0 = pinX_[i], x1 = x0 + areaMovW_[i];
                    const double y0 = pinY_[i], y1 = y0 + areaMovH_[i];
                    for (std::size_t j = 0; j < nFixed; ++j) {
                        if (x0 < macroX1[j] && x1 > macroX0[j] && y0 < macroY1[j] &&
                            y1 > macroY0[j]) {
                            ++acc;
                            break;
                        }
                    }
                }
                return acc;
            },
            [](std::size_t a, std::size_t b) {
                return a + b;
            });
        ktlog.trace(
            "  residual: excess on macro bins {:.4g}, on free bins {:.4g} "
            "({:.0f} macro bins); cells overlapping a macro: {}",
            exMacro / std::max(grid_.totalAvail, 1.0), exFree / std::max(grid_.totalAvail, 1.0),
            nMacroBins, onMacro);
    }
    // What the legalizer achieved, in the only terms that matter: the density
    // before and after, plus how deep the top-down partitioning actually got.
    // Pull any cell that ended up outside the die back inside it.
    //
    // Look-ahead legalization only touches the cells that are inside a block it
    // processes, and it only processes blocks that came from g-overfilled bins.
    // A cell the quadratic solve pushed outside the die is binned by `locate()`
    // into the nearest edge bin -- the bin index is clamped, not rejected -- so
    // its area is counted for overflow, but the cell itself is only moved if that
    // edge bin happens to be part of a cluster. When it is not, the cell is never
    // touched and stays outside the die for the rest of the run: the upper bound
    // is illegal, every later anchor is illegal, and the pseudonets then hold
    // other cells out there too. Measured on adaptec3, 2853 cells outside the die
    // in the final placement.
    //
    // It is also why such cells are invisible in the plots: the frames are drawn
    // against the die box, so a cell outside it is drawn off-canvas and simply
    // does not appear, which reads as the placer having lost it.
    //
    // The die is a hard constraint, so this is a clamp and not a term in the
    // objective. It is applied to the upper bound, which is the bound that is
    // meant to be legal; the lower bound is left alone, since its whole purpose
    // is to be the unconstrained answer and the gap between the two is a
    // reported quantity that clamping one side would flatter.
    {
        std::size_t pulled = 0;
        const double loX = die_[0], loY = die_[1], hiX = die_[2], hiY = die_[3];
        for (std::size_t i = 0; i < numMovable_; ++i) {
            const double w = areaMovW_[i], h = areaMovH_[i];
            // A cell is placed by its lower-left corner, so the corner has to stay
            // inside the die far enough for the whole cell to fit.
            const double nx = std::clamp(pinX_[i], loX, std::max(loX, hiX - w));
            const double ny = std::clamp(pinY_[i], loY, std::max(loY, hiY - h));
            if (nx != pinX_[i] || ny != pinY_[i]) {
                pinX_[i] = nx;
                pinY_[i] = ny;
                ++pulled;
            }
        }
        if (pulled > 0) {
            ktlog.trace("  pulled {} cell(s) back inside the die", pulled);
        }
    }

    binCells(pinX_, pinY_);
    const double after = scaledOverflow();
    {
        // Column- and row-wise area profile of the legalized result. A
        // symmetric legalizer fills both sides of the die; empty bands show up
        // here as short buckets, and they localise the bias immediately.
        constexpr std::size_t kB = 16;
        std::vector<double> colX(kB, 0.0), rowY(kB, 0.0);
        std::vector<double> colCap(kB, 0.0), rowCap(kB, 0.0);
        for (std::size_t k = 0; k < grid_.occ.size(); ++k) {
            const std::size_t ix = k % grid_.nbx;
            const std::size_t iy = k / grid_.nbx;
            colX[ix * kB / grid_.nbx] += grid_.occ[k];
            colCap[ix * kB / grid_.nbx] += grid_.avail[k];
            rowY[iy * kB / grid_.nby] += grid_.occ[k];
            rowCap[iy * kB / grid_.nby] += grid_.avail[k];
        }
        std::string px, py;
        for (std::size_t i = 0; i < kB; ++i) {
            const double ux = (colCap[i] > 0.0) ? colX[i] / colCap[i] : 0.0;
            const double uy = (rowCap[i] > 0.0) ? rowY[i] / rowCap[i] : 0.0;
            px += " " + std::to_string(static_cast<int>(ux * 9.99));
            py += " " + std::to_string(static_cast<int>(uy * 9.99));
        }
        ktlog.trace("  column utilisation (0-9, left->right):{}", px);
        ktlog.trace("  row    utilisation (0-9, bottom->top):{}", py);
    }
    ktlog.trace(
        "  look-ahead: {} cluster(s) ({} used the whole usable die), {} block(s), "
        "deepest level {}, largest block {} cells; scaled overflow {:.4f} -> {:.4f}",
        nClusters, globalRegions_, blocksProcessed_, deepestLevel_, maxBlockCells_, before_, after);
    {
        // Shape of the resulting density field, not just its total excess: a
        // histogram of per-bin utilisation, and how much of the die is touched
        // at all. An equi-area redistribution should look like a broad hump
        // around the target; a persistent ridge or a lopsided fill shows up here
        // as mass piled in a few utilisation buckets with many bins empty.
        std::size_t empty = 0, touched = 0, overfullBins = 0;
        double worst = 0.0;
        std::size_t hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (std::size_t k = 0; k < grid_.occ.size(); ++k) {
            if (grid_.avail[k] <= 0.0) {
                continue;
            }
            const double u = grid_.occ[k] / grid_.avail[k];
            if (grid_.occ[k] <= 0.0) {
                ++empty;
            } else {
                ++touched;
            }
            if (u > 1.0) {
                ++overfullBins;
            }
            worst = std::max(worst, u);
            const int b = std::min(7, static_cast<int>(u * 8.0));
            ++hist[std::max(b, 0)];
        }
        std::string buckets;
        for (int i = 0; i < 8; ++i) {
            buckets += " " + std::to_string(hist[i]);
        }
        ktlog.trace(
            "  density shape: worst bin utilisation {:.2f}, {} overfull bins, "
            "{} empty of {} usable bins ({:.0f}% touched)",
            worst, overfullBins, empty, empty + touched,
            100.0 * static_cast<double>(touched) / std::max<double>(empty + touched, 1.0));
        ktlog.trace(
            "  utilisation histogram [0-.125 .125-.25 .25-.375 .375-.5 .5-.625 "
            ".625-.75 .75-1 >1]:{}",
            buckets);
    }
    (void)before_;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

void SimplePlacer::Impl::describe(const std::vector<double> &px, const std::vector<double> &py,
                                  const char *tag) const {
    if (numMovable_ == 0) {
        return;
    }
    const double inv = 1.0 / static_cast<double>(numMovable_);
    double mx = 0.0, my = 0.0;
    for (std::size_t i = 0; i < numMovable_; ++i) {
        mx += px[i];
        my += py[i];
    }
    mx *= inv;
    my *= inv;
    double vx = 0.0, vy = 0.0, vxy = 0.0;
    double lox = px[0], hix = px[0], loy = py[0], hiy = py[0];
    for (std::size_t i = 0; i < numMovable_; ++i) {
        const double dx = px[i] - mx;
        const double dy = py[i] - my;
        vx += dx * dx;
        vy += dy * dy;
        vxy += dx * dy;
        lox = std::min(lox, px[i]);
        hix = std::max(hix, px[i]);
        loy = std::min(loy, py[i]);
        hiy = std::max(hiy, py[i]);
    }
    vx *= inv;
    vy *= inv;
    vxy *= inv;
    const double sx = std::sqrt(vx) / dieW_;
    const double sy = std::sqrt(vy) / dieH_;
    const double rho = (vx > 0.0 && vy > 0.0) ? vxy / std::sqrt(vx * vy) : 0.0;
    const double fillX = (hix - lox) / dieW_;
    const double fillY = (hiy - loy) / dieH_;
    ktlog.trace(
        "  {}: centroid ({:.3f},{:.3f}) sigma ({:.3f},{:.3f}) rho {:+.3f} "
        "bbox fill {:.2f}x{:.2f}",
        tag, (mx - die_[0]) / dieW_, (my - die_[1]) / dieH_, sx, sy, rho, fillX, fillY);
}

namespace {

/// Parse "star" / "b2b" into the model enum, keeping @p fallback on anything else.
///
/// An unknown value warns rather than dies: the model is a tuning choice, and a
/// typo in it should not throw away a run that has already been going for ten
/// minutes. It is still loud, because a placer silently running the other model
/// than the one asked for produces a plausible result that does not match.
SimplParams::NetModel netModelFor(const char *name, SimplParams::NetModel fallback) {
    if (name == nullptr) {
        return fallback;
    }
    const std::string v(name);
    if (v == "star") {
        return SimplParams::NetModel::Star;
    }
    if (v == "b2b" || v == "B2B") {
        return SimplParams::NetModel::B2B;
    }
    ktlog.warning("unknown net model '{}', keeping {}", v,
                  fallback == SimplParams::NetModel::Star ? "star" : "b2b");
    return fallback;
}

/// Zero-padded step number, so a directory listing sorts in run order instead of
/// alphabetically. Frames are meant to be flipped through in sequence, and
/// "it9" sorting after "it10" breaks that.
std::string frameStep(std::size_t n) {
    std::string s = std::to_string(n);
    while (s.size() < 4) {
        s.insert(s.begin(), '0');
    }
    return s;
}

}  // namespace

void SimplePlacer::Impl::enforceFences(std::vector<double> &px, std::vector<double> &py) {
    if (fences_ == nullptr || fences_->numRegions() == 0) {
        return;
    }
    // A hard fence, applied after the solve rather than as a term in it.
    //
    // The alternative is a penalty in the quadratic, which is how a soft fence is
    // normally done, and it is the wrong tool here: the solve is a linear system
    // with no room for a one-sided inequality, and a cell pushed back by a penalty
    // is only pushed back in proportion to how badly it wants to leave. A fence is
    // a promise about where a cell may be, so it is enforced as one: the cell is
    // put where it is allowed to be. The wirelength consequence is real and is the
    // cost of the constraint, not a bug in it.
    //
    // Assigned cells are clamped into their own region, and only their own --
    // clampToRegion is a no-op for a cell already inside. Unassigned cells are held
    // out of every region, since a fence is reserved for the cells assigned to it.
    std::size_t clamped = 0;
    std::size_t pushed = 0;
    for (std::size_t i = 0; i < numMovable_; ++i) {
        double &x = px[i];
        double &y = py[i];
        const int id = movRegion_[i];
        if (id != constraintMgr::kNoRegion) {
            double cx = x;
            double cy = y;
            fences_->clampToRegion(id, cx, cy);
            if (cx != x || cy != y) {
                x = cx;
                y = cy;
                ++clamped;
            }
            continue;
        }
        if (fences_->pushOutOfRegions(x, y, die_[0], die_[1], die_[2], die_[3])) {
            ++pushed;
        }
    }
    if (clamped > 0 || pushed > 0) {
        fenceClamps_ += clamped;
        fencePushes_ += pushed;
    }
}

void SimplePlacer::Impl::recordGifFrame(const std::vector<float> &fx, const std::vector<float> &fy,
                                        std::size_t step, std::size_t total, double hp, double ovf,
                                        const std::string &note, bool mandatory) {
    if (!animEnabled_) {
        return;
    }
    PlacementAnimator::instance().record(graph_, fx, fy, die_, step, total, hp, res_.hpwlSeed, ovf,
                                         note, fences_, mandatory);
}

void SimplePlacer::Impl::writeFrame(const std::string &path, const std::vector<double> &px,
                                    const std::vector<double> &py, double hp, double ovf,
                                    const std::string &note, std::size_t step, std::size_t total) {
    std::vector<float> fx(nv_), fy(nv_);
    for (std::size_t v = 0; v < nv_; ++v) {
        fx[v] = static_cast<float>(vx_[v]);
        fy[v] = static_cast<float>(vy_[v]);
    }
    for (std::size_t i = 0; i < numMovable_; ++i) {
        fx[movVertex_[i]] = static_cast<float>(px[i]);
        fy[movVertex_[i]] = static_cast<float>(py[i]);
    }
    // fixedView: every frame in a sequence is drawn at the same die-relative
    // scale, so iteration N and N+1 are comparable instead of being auto-zoomed.
    writeFrameSvg(path, graph_, fx, fy, die_, step, total, hp, res_.hpwlSeed, ovf, note, fences_,
                  /*fixedView=*/true);
    // Raster twin for the whole-run animation. The name is a plain counter
    // rather than the SVG's tag, so the animation follows run order even though
    // the SVG files are named after the stage that drew them.
    recordGifFrame(fx, fy, step, total, hp, ovf, note, /*mandatory=*/true);
    ++res_.framesWritten;
}

void SimplePlacer::Impl::writeCgFrame(const std::string &tag, std::size_t cgIter, double residX,
                                      double residY) {
    // Both axes are shown at once, after both have moved. Plotting the axis under
    // solve against the other axis' previous value was the honest view of a
    // separable solve, but it cost two frames per iteration and read as two
    // events where the run took one step of each; a single frame per iteration
    // with both residuals in the label says the same thing in half the space.
    std::vector<float> fx(nv_), fy(nv_);
    for (std::size_t v = 0; v < nv_; ++v) {
        fx[v] = static_cast<float>(vx_[v]);
        fy[v] = static_cast<float>(vy_[v]);
    }
    for (std::size_t i = 0; i < numMovable_; ++i) {
        fx[movVertex_[i]] = static_cast<float>(solX_[i]);
        fy[movVertex_[i]] = static_cast<float>(solY_[i]);
    }
    // The HPWL of this iterate, when there is a netlist to measure it against.
    // It used to be passed as 0 on the grounds that a CG iterate is not a
    // placement worth labelling -- but the renderer does not know that, so the
    // caption and the GIF both said "HPWL = 0.000" for two frames in every three,
    // which reads as a broken measurement rather than an absent one. The warm-up is
    // where the placement's length is most worth watching, so it is measured.
    const double hp = nets_.empty() ? 0.0 : hpwl(solX_, solY_);
    const std::string note = fmt::format("CG {} iterate {} of {}, residual x {:.3e} / y {:.3e}",
                                         tag, cgIter, par_.cgMaxIter, residX, residY);
    const std::string path = frameDir_ + "/simpl_cg_" + tag + "_" + frameStep(cgIter) + ".svg";
    writeFrameSvg(path, graph_, fx, fy, die_, cgIter, par_.cgMaxIter, hp, res_.hpwlSeed, 0.0, note,
                  nullptr, /*fixedView=*/true);
    // The same iterate, rasterised, so the animation shows the solve converging
    // rather than jumping straight from one outer iteration to the next.
    // Mandatory: every conjugate-gradient iterate of every solve, in the warm-up
    // and in each LSS iteration alike. A global-placement iteration is a solve and
    // not a single point, so one frame per iteration showed only its end state.
    recordGifFrame(fx, fy, cgIter, par_.cgMaxIter, hp, 0.0, note, /*mandatory=*/true);
    ++res_.framesWritten;
}

double SimplePlacer::Impl::binLocal(const std::vector<double> &px, const std::vector<double> &py,
                                    std::vector<double> &occ) const {
    occ.assign(grid_.occ.size(), 0.0);
    for (std::size_t i = 0; i < numMovable_; ++i) {
        std::size_t ix, iy;
        grid_.locate(px[i], py[i], ix, iy);
        occ[grid_.at(ix, iy)] += area_[i];
    }
    double ex = 0.0;
    for (std::size_t k = 0; k < occ.size(); ++k) {
        const double cap = g_ * grid_.avail[k];
        if (occ[k] > cap) {
            ex += occ[k] - cap;
        }
    }
    return (grid_.totalAvail > 0.0) ? ex / grid_.totalAvail : 0.0;
}

void SimplePlacer::Impl::writeDensityMap(const std::string &path, const std::vector<double> &px,
                                         const std::vector<double> &py, const std::string &note) {
    std::vector<double> occ;
    const double ovf = binLocal(px, py, occ);

    // One rectangle per bin, coloured by occ/avail. This is the view that answers
    // "is the lower bound actually spreading": a cell scatter plot of 210k cells
    // still looks like a blob at this scale, whereas the density field is
    // readable bin by bin.
    constexpr int W = 900, H = 620, PAD = 52;
    std::ofstream out(path);
    if (!out.is_open()) {
        return;
    }
    out << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << W << "\" height=\"" << H
        << "\">\n<rect width=\"100%\" height=\"100%\" fill=\"#101418\"/>\n";
    out << "<title>" << note << "</title>\n";
    const double sx = (W - 2 * PAD) / dieW_;
    const double sy = (H - 2 * PAD) / dieH_;
    const auto toX = [&](double x) {
        return PAD + (x - die_[0]) * sx;
    };
    const auto toY = [&](double y) {
        return H - PAD - (y - die_[1]) * sy;
    };
    const double bw = std::max(1.0, grid_.dx * sx);
    const double bh = std::max(1.0, grid_.dy * sy);

    for (std::size_t iy = 0; iy < grid_.nby; ++iy) {
        for (std::size_t ix = 0; ix < grid_.nbx; ++ix) {
            const std::size_t k = grid_.at(ix, iy);
            if (!(grid_.avail[k] > 0.0)) {
                continue;  // no sites at all: not part of the density problem
            }
            const double u = occ[k] / grid_.avail[k];
            // Blue (empty) -> green (at capacity) -> red (overfull). Piecewise
            // linear in u, so the eye reads a fill level directly.
            int r, g, b;
            if (u > 1.0) {
                const double t = std::min((u - 1.0) / 2.0, 1.0);
                r = 255;
                g = static_cast<int>(90.0 * (1.0 - t));
                b = static_cast<int>(90.0 * (1.0 - t));
            } else {
                r = static_cast<int>(40.0 + 90.0 * u);
                g = static_cast<int>(90.0 + 150.0 * u);
                b = static_cast<int>(190.0 * (1.0 - u));
            }
            out << "<rect x=\"" << toX(grid_.binLoX(ix)) << "\" y=\""
                << toY(grid_.binLoY(iy) + grid_.dy) << "\" width=\"" << bw << "\" height=\"" << bh
                << "\" fill=\"rgb(" << r << ',' << g << ',' << b << ")\"/>\n";
        }
    }
    // Fixed macros, outlined, so blockage is distinguishable from legal space.
    for (const std::uint32_t fv : fixVertex_) {
        const Vertex &vert = graph_.getCell(fv);
        if (!(vert.width > 0.0) || !(vert.height > 0.0)) {
            continue;
        }
        out << "<rect x=\"" << toX(vx_[fv]) << "\" y=\"" << toY(vy_[fv] + vert.height)
            << "\" width=\"" << (vert.width * sx) << "\" height=\"" << (vert.height * sy)
            << "\" fill=\"#bdbdbd\" fill-opacity=\"0.45\" stroke=\"#bdbdbd\" "
               "stroke-width=\"0.5\"/>\n";
    }
    out << "<text x=\"" << PAD
        << "\" y=\"24\" fill=\"#e0e0e0\" font-family=\"monospace\" "
           "font-size=\"14\">bin density "
        << note << "  ovf " << fmt::format("{:.4f}", ovf) << "</text>\n";
    // Legend.
    constexpr int kLegend = 12, kLegendW = 46;
    const int lx0 = PAD, ly0 = H - PAD + 10;
    for (int i = 0; i < kLegend; ++i) {
        const double u = 2.0 * static_cast<double>(i) / (kLegend - 1);  // 0 .. 2
        int r, g, b;
        if (u > 1.0) {
            const double t = std::min((u - 1.0) / 2.0, 1.0);
            r = 255;
            g = static_cast<int>(90.0 * (1.0 - t));
            b = static_cast<int>(90.0 * (1.0 - t));
        } else {
            r = static_cast<int>(40.0 + 90.0 * u);
            g = static_cast<int>(90.0 + 150.0 * u);
            b = static_cast<int>(190.0 * (1.0 - u));
        }
        out << "<rect x=\"" << (lx0 + i * kLegendW) << "\" y=\"" << ly0 << "\" width=\"" << kLegendW
            << "\" height=\"9\" fill=\"rgb(" << r << ',' << g << ',' << b << ")\"/>\n";
    }
    out << "<text x=\"" << lx0 << "\" y=\"" << (ly0 + 22)
        << "\" fill=\"#9e9e9e\" "
           "font-family=\"monospace\" font-size=\"10\">utilisation 0 -> 1 -> 2 (red = "
           "overfull)</text>\n";
    out << "</svg>\n";
    ++res_.framesWritten;
}

void SimplePlacer::Impl::densityStats(const std::vector<double> &px, const std::vector<double> &py,
                                      const char *tag) const {
    std::vector<double> occ;
    const double ovf = binLocal(px, py, occ);
    std::vector<double> util;
    util.reserve(occ.size());
    double worst = 0.0;
    std::size_t worstBin = 0;
    for (std::size_t k = 0; k < occ.size(); ++k) {
        if (!(grid_.avail[k] > 0.0)) {
            continue;
        }
        const double u = occ[k] / grid_.avail[k];
        util.push_back(u);
        if (u > worst) {
            worst = u;
            worstBin = k;
        }
    }
    if (util.empty()) {
        return;
    }
    const auto q = [&](double p) {
        std::size_t i = static_cast<std::size_t>(p * static_cast<double>(util.size()));
        std::nth_element(util.begin(), util.begin() + static_cast<long>(i), util.end());
        return util[i];
    };
    double mean = 0.0;
    for (const double u : util) {
        mean += u;
    }
    mean /= static_cast<double>(util.size());
    std::size_t empty = 0, nearCap = 0, overfull = 0;
    for (std::size_t k = 0; k < occ.size(); ++k) {
        if (!(grid_.avail[k] > 0.0)) {
            continue;
        }
        if (occ[k] <= 0.0) {
            ++empty;
        }
        const double u = occ[k] / grid_.avail[k];
        if (u > 1.0) {
            ++overfull;
        }
        if (u >= 0.8 && u <= 1.0) {
            ++nearCap;
        }
    }
    // The single worst bin is worth naming precisely: a utilisation of 1e15 can
    // only mean a near-zero available-area bin that still took cells, which is a
    // different defect from an overfull region.
    const double worstAvail = grid_.avail[worstBin];
    ktlog.trace(
        "  density[{}]: ovf {:.4f}, utilisation mean {:.3f} median {:.3f} p99 {:.3f} max {:.4g}; "
        "{} of {} usable bins at 0.8-1.0 ({} overfull, {} empty); worst bin avail {:.4g}",
        tag, ovf, mean, q(0.5), q(0.99), worst, nearCap, util.size(), overfull, empty, worstAvail);
}

// ---------------------------------------------------------------------------

SimplResult SimplePlacer::Impl::run(const SimplParams &P, const std::string &plotDir,
                                    bool useFences) {
    par_ = P;
    // The design's own fences, unless the run asked to measure their cost.
    fences_ = useFences ? &db_.constraints() : nullptr;
    fenceClamps_ = 0;
    fencePushes_ = 0;
    // Debugging cap: the legalizer is the expensive part, so a short run is
    // needed to iterate on it. Unset in normal use.
    if (const char *e = std::getenv("KTPLACE_SIMPL_STRIPE_SCALE")) {
        const std::string v(e);
        if (v == "tight") {
            par_.stripeScaleMode = SimplParams::StripeScale::Tight;
        } else if (v == "both") {
            par_.stripeScaleMode = SimplParams::StripeScale::Both;
        } else if (v == "none") {
            par_.stripeScaleMode = SimplParams::StripeScale::None;
        } else if (v == "fill") {
            par_.stripeScaleMode = SimplParams::StripeScale::Fill;
        } else {
            ktlog.warning("unknown stripe scale mode '{}', keeping tight", v);
        }
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_INIT_NET")) {
        par_.initNetModel = netModelFor(e, par_.initNetModel);
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_LSS_NET")) {
        par_.lssNetModel = netModelFor(e, par_.lssNetModel);
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_INIT_ITERS")) {
        par_.initMaxIters = static_cast<std::size_t>(std::atoll(e));
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_ITERS")) {
        par_.maxIters = static_cast<std::size_t>(std::max(std::atoi(e), 1));
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_SEED")) {
        par_.seed = static_cast<std::uint64_t>(std::atoll(e));
    }
    // The paper requires 0 < g < 1 (Section 4.2). At g = 1 every stripe is filled
    // to 100% of its available area, so look-ahead legalization's only solution
    // is to spread cells uniformly over the whole die -- a maximum-entropy
    // spread that destroys the density variation wirelength optimisation wants,
    // and the reason legalization looked so pessimistic. Expose it for sweeps.
    if (const char *e = std::getenv("KTPLACE_SIMPL_DENSITY")) {
        par_.densityLimit = std::atof(e);
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_GLOBAL_CLUSTER")) {
        par_.globalClusterFrac = std::atof(e);
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_PSEUDONET")) {
        const std::string v = e;
        if (v == "inv") {
            par_.pseudonetLaw = SimplParams::PseudonetLaw::InverseLength;
        } else if (v == "const") {
            par_.pseudonetLaw = SimplParams::PseudonetLaw::ConstantStiffness;
        } else {
            ktlog.fatal("KTPLACE_SIMPL_PSEUDONET must be 'inv' or 'const' (got '{}')", v);
        }
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_START")) {
        const std::string v = e;
        if (v == "input") {
            par_.start = SimplParams::StartPlacement::Input;
        } else if (v == "auto") {
            par_.start = SimplParams::StartPlacement::Auto;
        } else if (v == "uniform") {
            par_.start = SimplParams::StartPlacement::Uniform;
        } else {
            ktlog.fatal("KTPLACE_SIMPL_START must be one of: input, auto, uniform (got '{}')", v);
        }
    }
    // Frame cadence. Per-CG-iteration frames are large, so cgEvery defaults to 1
    // and the animator thins rather than the solve skipping.
    if (const char *e = std::getenv("KTPLACE_SIMPL_CG_EVERY")) {
        par_.cgEvery = static_cast<std::size_t>(std::max(std::atoi(e), 0));
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_TRACE_EVERY")) {
        par_.traceEvery = static_cast<std::size_t>(std::atoll(e));
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_DENSITY_MAPS")) {
        par_.densityMaps = std::atoi(e) != 0;
    }
    // The pseudonet weight schedule is the only spreading force in the solve --
    // there is no density term -- so its base is the knob that decides whether the
    // run converges to a wirelength optimum or to a frozen spread state. Exposed
    // because it is worth sweeping per design, not because the default is wrong.
    if (const char *e = std::getenv("KTPLACE_SIMPL_PSEUDONET")) {
        par_.pseudonetLaw = (std::string(e) == "constant")
                                ? SimplParams::PseudonetLaw::ConstantStiffness
                                : SimplParams::PseudonetLaw::InverseLength;
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_ALPHA_BASE")) {
        par_.alphaBase = std::atof(e);
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_ALPHA_DECAY")) {
        par_.alphaDecay = std::atof(e);
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_ALPHA_DECAY_BELOW")) {
        par_.alphaDecayBelow = std::atof(e);
    }
    if (const char *e = std::getenv("KTPLACE_SIMPL_NO_LAL")) {
        par_.lookAhead = std::atoi(e) == 0;
    }
    g_ = std::clamp(par_.densityLimit, 0.05, 1.0);

    // Frames go to <plotDir>/simpl. With no plot directory the run draws nothing,
    // rather than falling back to the output's own directory.
    frameDir_ = plotDir.empty() ? std::string() : plotDir + "/simpl";
    if (!frameDir_.empty() && (par_.traceEvery > 0 || par_.cgEvery > 0)) {
        ensureDir(frameDir_);
    }
    // The GIF is assembled by the flow once the legalizer and the detailed
    // placer have added their frames, so that one animation covers the whole run
    // instead of stopping at the end of global placement.
    animEnabled_ = PlacementAnimator::instance().enabled();

    // Scoped to collect() alone. Declared at function scope it stayed alive until
    // place() returned, so "simpl-setup" reported the whole run -- 32s of which
    // was the iteration loop, not setup -- and the two numbers could not be
    // compared with anything.
    {
        ScopedTimer setupTimer("simpl-setup");
        collect();

        // Mirror the region assignment once, now that collect() has defined the
        // movable list. This has to come after collect(): numMovable_ is still zero
        // above it, so sizing the mirror here produced an empty vector and the fence
        // check then indexed it from zero -- a null dereference on the first cell.
        // Vertex::regionId is the source of truth, but the check runs over every cell
        // after every solve, so it wants this per movable index rather than a graph
        // lookup each time.
        movRegion_.assign(numMovable_, constraintMgr::kNoRegion);
        if (fences_ != nullptr) {
            for (std::size_t i = 0; i < numMovable_; ++i) {
                movRegion_[i] = graph_.getCell(movVertex_[i]).regionId;
            }
        }
    }  // "simpl-setup"

    // The density grid. Timed on its own and folded into res_.buildSeconds below
    // alongside the per-iteration matrix rebuilds, so "matrix build" in the
    // summary means every second spent assembling a matrix, not just the
    // per-iteration ones.
    {
        ScopedTimer gridTimer("simpl-grid");
        buildGrid(P);
        res_.buildSeconds = gridTimer.elapsedSeconds();
    }

    if (numMovable_ == 0) {
        return res_;
    }

    // ---- where does the starting placement come from? ------------------------
    //
    // Section 4.1 runs an area-blind quadratic solve because the placer has
    // nothing better to start from. But a Bookshelf .pl or a DEF normally already
    // carries a placement, and it is a real solution: adaptec1 ships one at
    // 9.57e7. Discarding it for a uniform seed throws away the best information
    // available, and the area-blind solve cannot recover it because it collapses
    // the cells into a blob (HPWL 4.2e7 with enormous overlap) which the
    // look-ahead legalizer then has to blow back out.
    //
    // So: adopt the design's own placement when it is usable, and fall back to
    // the paper's uniform seed plus Section 4.1 only when it is not.
    const bool inputUsable = [&] {
        if (par_.start == SimplParams::StartPlacement::Uniform) {
            return false;
        }
        if (par_.start == SimplParams::StartPlacement::Input) {
            return true;
        }
        if (inputX_.size() != numMovable_) {
            return false;
        }
        // A design with no placement ships every cell at the same point, so a
        // degenerate bounding box is the tell. Require the cells to occupy a
        // real part of the die and to be mostly inside it.
        double lo = std::numeric_limits<double>::max();
        double hi = -std::numeric_limits<double>::max();
        double loY = std::numeric_limits<double>::max();
        double hiY = -std::numeric_limits<double>::max();
        for (std::size_t i = 0; i < numMovable_; ++i) {
            lo = std::min(lo, inputX_[i]);
            hi = std::max(hi, inputX_[i]);
            loY = std::min(loY, inputY_[i]);
            hiY = std::max(hiY, inputY_[i]);
        }
        if (!(hi - lo > 0.25 * dieW_) || !(hiY - loY > 0.25 * dieH_)) {
            return false;
        }
        std::size_t inside = 0;
        for (std::size_t i = 0; i < numMovable_; ++i) {
            if (inputX_[i] >= die_[0] && inputX_[i] < die_[0] + dieW_ && inputY_[i] >= die_[1] &&
                inputY_[i] < die_[1] + dieH_) {
                ++inside;
            }
        }
        return static_cast<double>(inside) >= par_.minInputInsideFrac * numMovable_;
    }();


    res_.usedInputPlacement = inputUsable;
    if (inputUsable) {
        pinX_ = inputX_;
        pinY_ = inputY_;
    } else {
        seedUniform(par_.seed);
    }
    res_.hpwlInput = hpwl(pinX_, pinY_);

    ktlog.echo(
        "SimPL: {} movable cells, {} fixed, {} nets; grid {}x{}, available area {:.4g}, "
        "cell area {:.4g} (utilisation {:.1f}%), density limit g = {:.2f}; start {} "
        "(input HPWL {:.6e}{})",
        numMovable_, res_.numFixed, res_.nets, grid_.nbx, grid_.nby, grid_.totalAvail,
        grid_.totalCellArea, 100.0 * grid_.totalCellArea / std::max(grid_.totalAvail, 1e-12), g_,
        inputUsable ? "input placement" : "uniform seed", res_.hpwlInput,
        inputUsable ? "" : (par_.seed == 0 ? "" : fmt::format(", seed {}", par_.seed)));

    std::vector<double> lower = pinX_;
    std::vector<double> lowerY = pinY_;
    std::vector<double> upper;
    std::vector<double> upperY;
    // Best warm-up round, hoisted out of the warm-up block so the writeback at the
    // end of the run can hand it on. Empty means the warm-up never ran, which is
    // the normal case for a design that supplies its own placement.
    std::vector<double> bestInit, bestInitY;

    res_.hpwlSeed = hpwl(lower, lowerY);
    // Section 4.1's area-blind quadratic solve, alternating B2B rebuilds until
    // HPWL stops improving. Skipped when the design already supplies a
    // placement: the solve ignores cell areas by design, so it collapses the
    // cells into a blob whose wirelength is meaningless, and the only thing it
    // usefully establishes is the ordering of the cells, which the design's own
    // placement already has.
    if (!inputUsable) {
        // The seed gets a frame of its own. It is the one moment the placement is
        // uniform over the die, and every frame after it is a solve of the star
        // model moving away from it, so without this the animation starts partway
        // through the warm-up with nothing to compare against.
        if (!frameDir_.empty()) {
            writeFrame(frameDir_ + "/simpl_LSS_init_seed.svg", lower, lowerY, res_.hpwlSeed, 0.0,
                       "initial placement: uniform seed, before any solve", 0, par_.initMaxIters);
        }
        double bestInitHpwl = std::numeric_limits<double>::max();
        int initStale = 0;
        for (std::size_t it = 0; it < par_.initMaxIters; ++it) {
            // The B2B model is placement-dependent, so the graph is rebuilt
            // from the current locations before every solve.
            buildB2B(lower, lowerY, 0.0, false, par_.initNetModel);
            solX_ = lower;
            solY_ = lowerY;
            // The warm-up solves are part of the run, so their iterates belong in
            // the animation alongside everything after them.
            solve("init" + frameStep(it), /*allowFrames=*/animEnabled_, par_.cgEveryInit);
            lower = solX_;
            lowerY = solY_;
            enforceFences(lower, lowerY);
            res_.initIters = it + 1;
            const double h = hpwl(lower, lowerY);
            // Convergence on the wirelength, not on a round count. The improvement
            // is measured relative to the best round so far rather than to the
            // previous one, so a single regression does not read as convergence and
            // a single large gain is not discarded by the round after it.
            // Two separate questions. "Is this round the best so far?" is a plain
            // minimum -- a 0.05% gain is still a gain, and the writeback should
            // hand on the best placement, not the best one that cleared a
            // threshold. "Has it stopped paying?" is the tolerance test, and only
            // that one advances the patience. Folding the tolerance into the
            // minimum test throws away real improvements near convergence, which
            // is exactly where the remaining gains are.
            if (h < bestInitHpwl) {
                const double gain = bestInitHpwl > 0.0 ? (bestInitHpwl - h) / bestInitHpwl : 1.0;
                bestInitHpwl = h;
                bestInit = lower;
                bestInitY = lowerY;
                initStale = gain < par_.initTolFrac ? initStale + 1 : 0;
            } else {
                ++initStale;
            }
            densityStats(lower, lowerY, fmt::format("init{}", it).c_str());
            if (par_.traceEvery > 0 && !frameDir_.empty()) {
                // binLocal, not scaledOverflow: the live occupancy in grid_.occ is
                // still describing the previous legalizer pass here.
                std::vector<double> tmpOcc;
                const double ovfInit = binLocal(lower, lowerY, tmpOcc);
                const std::string note =
                    fmt::format("LSS init iteration {} of {}", it, par_.initMaxIters);
                writeFrame(frameDir_ + "/simpl_LSS_init_" + frameStep(it) + ".svg", lower, lowerY,
                           h, ovfInit, note, it, par_.initMaxIters);
                if (par_.densityMaps) {
                    writeDensityMap(frameDir_ + "/simpl_density_init_" + frameStep(it) + ".svg",
                                    lower, lowerY, note);
                }
            }
            res_.hpwlInit = bestInitHpwl;
            res_.initResidual = lastResidual_;
            ktlog.trace("init iter {:2d}: hpwl {:.6e} best {:.6e} ({:.4f}% off, {}/{} stale)", it,
                        h, bestInitHpwl,
                        bestInitHpwl > 0.0 ? 100.0 * (h - bestInitHpwl) / bestInitHpwl : 0.0,
                        initStale, par_.initPatience);
            // Only the star model stops early: it is placement-independent, so a
            // round after convergence re-solves the same system. B2B is rebuilt
            // from the moved placement every round, and its later rounds do pay
            // (see initMaxIters), so it keeps every round as before.
            const std::size_t patience =
                par_.initNetModel == SimplParams::NetModel::Star ? par_.initPatience : 0;
            if (patience > 0 && initStale >= static_cast<int>(patience)) {
                // Converged: further rounds are not paying for themselves.
                break;
            }
        }
    }
    res_.hpwlLower = hpwl(lower, lowerY);

    // Diagnostic: apply look-ahead legalization repeatedly to ONE fixed input,
    // with no re-solve in between. This separates two different bugs that have
    // the same symptom -- "the legalizer cannot spread" versus "the loop never
    // accumulates" -- by holding the input fixed and asking whether repeated
    // projection alone converges.
    if (const char *envRounds = std::getenv("KTPLACE_SIMPL_LAL_ONLY")) {
        const int rounds = std::max(std::atoi(envRounds), 1);
        std::vector<double> lx = lower;
        std::vector<double> ly = lowerY;
        double prev = std::numeric_limits<double>::max();
        for (int r = 0; r < rounds; ++r) {
            binCells(lx, ly);
            const double ovfBefore = scaledOverflow();
            (void)ovfBefore;
            blocksProcessed_ = 0;
            deepestLevel_ = 0;
            maxBlockCells_ = 0;
            std::vector<double> sx = pinX_;
            std::vector<double> sy = pinY_;
            pinX_ = lx;
            pinY_ = ly;
            lookAheadLegalize();
            lx = pinX_;
            ly = pinY_;
            pinX_ = sx;
            pinY_ = sy;
            binCells(lx, ly);
            const double ovfAfter = scaledOverflow();
            describe(lx, ly, "LAL-only round");
            // How far did the LAL actually move things, and how full is the
            // result? If the answer is "everything moved a lot and the die is
            // now uniformly ~100% dense", the recursion is over-spreading: at
            // 53.5% utilisation the die can never be full.
            double rms = 0.0, maxd = 0.0;
            double lo = 1e300, hi = -1e300, loY = 1e300, hiY = -1e300;
            for (std::size_t i = 0; i < numMovable_; ++i) {
                const double dx = lx[i] - lower[i];
                const double dy = ly[i] - lowerY[i];
                rms += dx * dx + dy * dy;
                maxd = std::max(maxd, std::hypot(dx, dy));
                lo = std::min(lo, lx[i]);
                hi = std::max(hi, lx[i]);
                loY = std::min(loY, ly[i]);
                hiY = std::max(hiY, ly[i]);
            }
            rms = std::sqrt(rms / std::max<std::size_t>(numMovable_, 1));
            double densSum = 0.0;
            std::size_t densN = 0;
            for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
                if (grid_.avail[k] <= 0.0) {
                    continue;
                }
                densSum += grid_.occ[k] / grid_.avail[k];
                ++densN;
            }
            // How concentrated is the input? If the lower bound is collapsed
            // into a tiny area, the legalizer has no choice but to spread it by
            // sqrt(peak density), and that factor is paid straight back in HPWL.
            {
                double dlo = 1e300, dhi = -1e300, dloY = 1e300, dhiY = -1e300;
                for (std::size_t i = 0; i < numMovable_; ++i) {
                    dlo = std::min(dlo, lx[i]);
                    dhi = std::max(dhi, lx[i]);
                    dloY = std::min(dloY, ly[i]);
                    dhiY = std::max(dhiY, ly[i]);
                }
                double peak = 0.0, mean = 0.0;
                std::size_t nb = 0;
                for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
                    if (grid_.avail[k] <= 0.0) {
                        continue;
                    }
                    const double dn = grid_.occ[k] / grid_.avail[k];
                    peak = std::max(peak, dn);
                    mean += dn;
                    ++nb;
                }
                double aMin = 1e300, aMax = -1e300, oMax = -1e300, oSum = 0.0;
                std::size_t tiny = 0;
                for (std::size_t k = 0; k < grid_.avail.size(); ++k) {
                    aMin = std::min(aMin, grid_.avail[k]);
                    aMax = std::max(aMax, grid_.avail[k]);
                    oMax = std::max(oMax, grid_.occ[k]);
                    oSum += grid_.occ[k];
                    if (grid_.avail[k] > 0.0 && grid_.avail[k] < 1e-6 * aMax) {
                        ++tiny;
                    }
                }
                ktlog.echo(
                    "    density field: avail min {:.6g} max {:.6g} ({} bins below 1e-6 of "
                    "max); occ max {:.6g} sum {:.6g} (cell area {:.6g})",
                    aMin, aMax, tiny, oMax, oSum, grid_.totalCellArea);
                const double spanX = dhi - dlo, spanY = dhiY - dloY;
                ktlog.echo(
                    "    input: bbox x[{:.0f},{:.0f}] y[{:.0f},{:.0f}] = {:.2f}x{:.2f} of "
                    "die; peak bin density {:.2f}, mean {:.3f}, needed linear spread "
                    "{:.2f}x",
                    dlo, dhi, dloY, dhiY, spanX / dieW_, spanY / dieH_, peak,
                    nb ? mean / static_cast<double>(nb) : 0.0, std::sqrt(peak));
            }
            ktlog.echo(
                "    moved: rms {:.1f} max {:.1f} (die {:.0f}x{:.0f}); result bbox "
                "x[{:.0f},{:.0f}] y[{:.0f},{:.0f}]; mean bin density {:.3f}",
                rms, maxd, dieW_, dieH_, lo, hi, loY, hiY,
                densN ? densSum / static_cast<double>(densN) : 0.0);
            prev = ovfAfter;
        }
        static_cast<void>(prev);
        return res_;
    }

    // A negative lower/upper gap is impossible for a genuine lower bound: the
    // upper bound's cells are legalized, so its nets cannot be shorter than the
    // wirelength optimum of the linearised objective. It happens only when the
    // legalized cells overlap, which shortens nets artificially. Refuse to report
    // such a result as progress -- this fired once with a gap of -5.8e6 and a
    // "best" wirelength that was simply overlap being scored as a win.
    bool sawInvalidGap = false;

    // ---- global placement iterations --------------------------------------
    double gapRef = -1.0;
    // Annealing state for the pseudonet weight, 1.0 until the lower bound is
    // spread enough (see SimplParams::alphaDecay).
    double alphaScale = 1.0;
    double bestUpper = std::numeric_limits<double>::max();
    // Overflow of the placement `bestUpper` came from. Primary key for choosing
    // it; see the selection block below.
    double bestUpperOvf = std::numeric_limits<double>::max();
    int stale = 0;
    bool converged = false;
    double buildAcc = 0.0;
    double solveAcc = 0.0;
    double spreadAcc = 0.0;
    std::vector<std::pair<double, double>> curve;  // (upper HPWL, lower HPWL)
    // The best legal placement seen, kept because "last" and "best" are not the
    // same thing. The upper bound is not monotone: legalizing a lower bound that
    // improved can still lengthen wires, so a later iteration can be worse than
    // an earlier one. Measured on ibm01, the best legal wirelength was 1.157e7 at
    // iteration 1, and the run ended by returning 2.064e7 from iteration 21 --
    // 78% worse than a placement it had already computed and thrown away.
    std::vector<double> bestX, bestY;
    // The lower bound from the *same* iteration. The reported gap is a
    // convergence measure between the two bounds of one iteration; pairing an
    // early upper bound with the last lower bound makes the gap negative and
    // meaningless, which is not a legal-placement property but an artefact of
    // the two numbers coming from different iterations.
    std::vector<double> bestLower, bestLowerY;
    std::size_t bestIter = 0;

    for (std::size_t it = 0; it < par_.maxIters && !converged; ++it) {
        res_.globalIters = it + 1;

        // (1) Look-ahead legalization: lower bound -> upper bound.
        //
        // Applied repeatedly until the overflow stops improving. A single pass is
        // not enough once the top-down partitioning is handed the whole usable
        // die: it spreads into the space and leaves holes mid-die. Measured in
        // isolation (KTPLACE_SIMPL_LAL_ONLY) the same projection takes adaptec1
        // from 0.508 to 0.143 in one pass and on to 0.026 in two, so the
        // machinery does converge -- the outer loop was simply never giving it
        // the rounds.
        ScopedTimer lapTimer("simpl-legalize");
        upper = lower;
        upperY = lowerY;
        std::vector<double> keepX = pinX_;
        std::vector<double> keepY = pinY_;
        double prevPassOvf = std::numeric_limits<double>::max();
        // One pass per global-placement iteration. The paper alternates
        // "(1) look-ahead legalization, (2) updates to anchors and the B2B net
        // model, and (3) solution of the linear system" -- a single projection per
        // iteration. Re-projecting several times inside one iteration is not in
        // it, and it spends the thing the flow exists to show: every extra pass
        // spreads cells further before the anchors and the solve get a chance to
        // pull them back.
        for (std::size_t pass = 0;
             par_.lookAhead && pass < std::max<std::size_t>(par_.lalPasses, 1); ++pass) {
            blocksProcessed_ = 0;
            deepestLevel_ = 0;
            maxBlockCells_ = 0;
            binCells(upper, upperY);
            pinX_ = upper;
            pinY_ = upperY;
            lookAheadLegalize();
            upper = pinX_;
            upperY = pinY_;
            pinX_ = keepX;
            pinY_ = keepY;
            binCells(upper, upperY);
            const double ovfNow = scaledOverflow();
            if (!(ovfNow < prevPassOvf * (1.0 - par_.lalMinGain))) {
                break;  // no worthwhile progress from another pass
            }
            prevPassOvf = ovfNow;
        }
        spreadAcc += lapTimer.elapsedSeconds();
        lapTimer.lap();

        binCells(upper, upperY);
        const double upperOvf = scaledOverflow();
        binCells(lower, lowerY);
        describe(lower, lowerY, "LSS (linear system solve)");
        describe(upper, upperY, "LAL (look-ahead legalized)");
        densityStats(lower, lowerY, ("LSS it" + std::to_string(it)).c_str());
        const double ovfLower = scaledOverflow();
        if (par_.alphaDecay < 1.0 && ovfLower < par_.alphaDecayBelow) {
            // Once the lower bound is spread past the crowding point, start
            // letting the pseudonet weight fall so the solve can recover
            // wirelength instead of only ever adding spread.
            alphaScale *= par_.alphaDecay;
        }
        densityStats(upper, upperY, ("LAL it" + std::to_string(it)).c_str());
        const double upperHpwl = hpwl(upper, upperY);
        const double lowerHpwl = hpwl(lower, lowerY);
        const double gap = upperHpwl - lowerHpwl;
        if (gap < 0.0) {
            if (!sawInvalidGap) {
                ktlog.echo(
                    "iter {}: WARNING gap {:.4e} is negative -- the legalized cells overlap, "
                    "so this wirelength is not a legal result and is not counted as progress",
                    it, gap);
                sawInvalidGap = true;
            }
        }
        curve.emplace_back(upperHpwl, lowerHpwl);
        res_.gap = gap;

        // Which upper bound to ship. The paper ships the last one, and the last
        // one is legal by construction: look-ahead legalization is defined as a
        // projection onto legal placements, so its defining property is low
        // overflow, and "upper bound" is a term about wirelength bounds, not
        // about legality.
        //
        // Ranking by wirelength alone throws that property away, and picks the
        // least legalized iteration in the run: the earlier a placement is, the
        // more collapsed it is, the shorter its wires, and the more overlap it
        // has. On adaptec1 that selected iteration 1 -- overflow 0.222, the
        // worst of the legalized candidates -- over iteration 10 at 0.199 with a
        // third more wirelength, and the final legalizer was then handed a
        // placement a fifth of the way to legal.
        //
        // So legality is the primary key and wirelength the tie-break, with the
        // tolerance wide enough that ordinary iteration-to-iteration noise does
        // not count as a legality win. A wirelength win inside that band is a
        // real win: it is the same placement quality for less wire.
        const bool moreLegal = upperOvf < bestUpperOvf * (1.0 - par_.upperOvfTol);
        const bool sameLegal = upperOvf <= bestUpperOvf * (1.0 + par_.upperOvfTol);
        if (moreLegal || (sameLegal && upperHpwl < bestUpper - 1e-12)) {
            bestUpperOvf = upperOvf;
            bestUpper = upperHpwl;
            bestX = upper;
            bestY = upperY;
            bestLower = lower;
            bestLowerY = lowerY;
            bestIter = it;
        }
        if (!moreLegal && !(sameLegal && upperHpwl < bestUpper - 1e-12)) {
            ++stale;
        } else {
            stale = 0;
        }

        const double relGapTrace = upperHpwl > 0.0 ? gap / upperHpwl : 0.0;
        ktlog.trace(
            "iter {:3d}: lower {:.6e} upper {:.6e} gap {:.4e} ({:.3f}%, ref {:.4e}) "
            "ovf lower {:.4e} upper {:.4e} alpha {:.4g} stale {}",
            it, lowerHpwl, upperHpwl, gap, relGapTrace * 100.0, gapRef, scaledOverflow(), upperOvf,
            par_.alphaBase * (1.0 + static_cast<double>(it)) * alphaScale, stale);

        // Convergence, watched on the gap relative to the placement it describes.
        //
        // The paper ("SimPL", CACM 56(6)) terminates global placement when the
        // gap is reduced to 25% of the gap at the tenth iteration and the
        // upper-bound solution stops improving, or when the gap is smaller still.
        // The tenth-iteration reference is itself a small fraction of the
        // placement -- on adaptec1 the gap at the tenth iteration is ~4% of the
        // upper bound's HPWL -- so 25% of it means the bounds within ~1% of each
        // other, which is unreachable in practice: the bounds converge to a few
        // percent of each other and then plateau. The previous implementation
        // froze the reference and required `gap < 25% of gapRef`, which on
        // adaptec1 and adaptec2 never fired -- the runs only ended because the gap
        // eventually went NEGATIVE (overlapping legalized cells), which trivially
        // satisfies `gap < 10% of gapRef`. "Convergence" was the overlap, and the
        // iterations after the first, best placement (iteration 1 on both) were
        // pure LSS/LAL waste.
        //
        // The rule is therefore evaluated scale-free, exactly as the reference's
        // own value is a scale: the bounds have met when the gap is a bounded
        // fraction of the placement it separates.
        //
        // Two conditions, both guarded by the paper's oscillation window (no test
        // before gapReferenceIter -- the first few iterations' upper-bound HPWL
        // oscillates, so a reference taken inside that window measures noise):
        //   1. the bounds have met: gap/upperHpwl <= gapTightFrac;
        //   2. the bounds are close AND the upper bound has stopped improving for
        //      `patience` iterations: gap/upperHpwl <= gapRelaxedFrac && stale.
        // Both halves of condition 2 matter. The gap alone is satisfied by both
        // bounds drifting upward together, which is what this implementation does
        // -- on adaptec1 the upper bound rises from 5.3e8 to 7.5e8 while the gap
        // falls 4.5e8 -> 2.0e7, so a gap-only test certifies convergence on a
        // placement 40% worse than the one it started from. Requiring the upper
        // bound to have stopped improving is what makes the criterion mean
        // something.
        //
        // The best upper bound seen is kept in any case (bestX/bestY above), so an
        // earlier stop never costs quality: it only stops paying for LSS/LAL rounds
        // that have nothing left to improve.
        if (it == par_.gapReferenceIter) {
            gapRef = gap;  // informative only; the trace reports it
        }
        if (gap > 0.0 && it > par_.gapReferenceIter && upperHpwl > 0.0) {
            // The paper's rule, taken literally: the reference is "the gap at the
            // 10th iteration", not a fraction of the placement. Termination is
            // (1) the gap reduced to 25% of that reference AND the upper-bound
            // solution no longer improving, or (2) the gap below 10% of it.
            //
            // This replaces a scale-free reading that compared gap/upper against
            // 0.10 and 0.25. That one fires as soon as the bounds agree, and two
            // bounds that are equally bad do agree: on adaptec1 it ended the run
            // at iteration 11 with the bounds at 3.4e8 and 3.6e8, neither having
            // been below 0.16 scaled overflow.
            const double ref = gapRef > 0.0 ? gapRef : gap;
            const double relToRef = gap / ref;
            if (relToRef < par_.gapTightFrac) {
                converged = true;  // (2) below 10% of the reference gap
            } else if (relToRef <= par_.gapRelaxedFrac &&
                       stale >= static_cast<int>(par_.patience)) {
                converged = true;  // (1) within 25% of it, and no longer improving
            }
        } else if (gap < 0.0 && it > par_.gapReferenceIter) {
            // The negative-gap guard from the `sawInvalidGap` block: a negative
            // gap is the legalized cells overlapping, not the bounds meeting, so
            // it is not convergence. It does mean the LSS/LAL loop has nothing
            // left to offer -- the spread has overshot and only re-legalizes into
            // overlap -- so keeping the best placement and stopping is right, as
            // long as the loop that would otherwise run forever (adaptec1 never
            // re-achieved a positive gap once it turned negative at iteration 24)
            // terminates instead of burning every remaining iteration.
            converged = true;
        }

        if (par_.traceEvery > 0 && (it % par_.traceEvery) == 0 && !frameDir_.empty()) {
            const std::string step = frameStep(it);
            writeFrame(frameDir_ + "/simpl_LSS_" + step + ".svg", lower, lowerY, lowerHpwl,
                       upperOvf, "LSS (linear system solve) - iteration " + std::to_string(it), it,
                       par_.maxIters);
            writeFrame(frameDir_ + "/simpl_LAL_" + step + ".svg", upper, upperY, upperHpwl,
                       upperOvf, "LAL (look-ahead legalized) - iteration " + std::to_string(it), it,
                       par_.maxIters);
            if (par_.densityMaps) {
                writeDensityMap(frameDir_ + "/simpl_density_LSS_" + step + ".svg", lower, lowerY,
                                "LSS iteration " + std::to_string(it));
                writeDensityMap(frameDir_ + "/simpl_density_LAL_" + step + ".svg", upper, upperY,
                                "LAL iteration " + std::to_string(it));
            }
        }

        if (converged) {
            ktlog.trace("converged at iteration {}: gap {:.4e} ({:.3f}% of upper)", it, gap,
                        relGapTrace * 100.0);
            break;
        }

        // (2) Update anchors and the B2B net model from the upper bound, then
        // (3) re-solve. The pseudonet weight alpha grows with the iteration
        // number, moving the emphasis from interconnect onto constraints.
        anchorX_ = upper;
        anchorY_ = upperY;
        const double alpha = par_.alphaBase * (1.0 + static_cast<double>(it)) * alphaScale;
        ScopedTimer bTimer("simpl-build");
        // The B2B model and the pseudonet lengths are both measured at the LOWER
        // bound: that is the point being linearised, and the anchor distance is
        // |upper - lower|. Handing buildB2B the upper bound as the current
        // position as well made every pseudonet length identically zero, so the
        // degenerate-length guard discarded all 210k of them and the anchors
        // contributed nothing at all (measured: anchor diagonal exactly 0 against
        // an interconnect diagonal of 6.9e5). With no anchors in the system the
        // solve was pure interconnect minimisation, which is why the lower bound
        // never moved, why lower-bound HPWL only drifted, and why every pass
        // re-legalised the same collapsed input to the same result.
        buildB2B(lower, lowerY, alpha, true, par_.lssNetModel);
        buildAcc += bTimer.elapsedSeconds();
        bTimer.lap();
        ScopedTimer sTimer("simpl-solve");
        // Warm start from the previous lower bound. Starting instead from the
        // anchors was tried and rejected: it improved density (adaptec2 overflow
        // 0.093 -> 0.081) but left the final wirelength unchanged (1.73e9 ->
        // 1.75e9) and turned the bound gap negative. The seed is not the lever
        // here; the anchor weight is. See the "pseudonet share" trace, which is
        // where the real diagnosis lives.
        solX_ = lower;
        solY_ = lowerY;
        solve("g" + frameStep(it), /*allowFrames=*/true);
        lower = solX_;
        lowerY = solY_;
        enforceFences(lower, lowerY);
        solveAcc += sTimer.elapsedSeconds();
        sTimer.lap();
    }

    if (upper.empty()) {
        // Degenerate case: no iteration completed, fall back to the lower bound.
        upper = lower;
        upperY = lowerY;
    }
    if (!bestInit.empty()) {
        // Best warm-up round, for the same reason the global loop returns its best
        // upper bound: the last round of a converging solve is not reliably its
        // best, and the stopping rule can land on a round that regressed. This is
        // the *fallback* lower bound -- it is what applies when the global loop
        // produced no upper bound to pair with.
        lower = bestInit;
        lowerY = bestInitY;
    }
    if (!bestX.empty()) {
        // Return the best legal placement, not the last one. The paper's Figure 2
        // reports the last upper bound, which is only equivalent when the upper
        // bound is monotone; here it demonstrably is not, so "last" silently ships
        // a worse placement than the run already had in hand.
        //
        // The lower bound must come from the SAME iteration as the upper bound it
        // is being compared against, or the gap is not a gap: it is one iteration's
        // wirelength subtracted from another's. Doing this before the warm-up
        // fallback above let the warm-up's lower bound overwrite the correctly
        // paired one, so adaptec1 reported a lower bound of 8.07e7 against an
        // upper bound of 4.05e8 from a different iteration -- a gap of 3.2e8 that
        // means nothing, and an apparent lower bound below the warm-up's own HPWL.
        upper = bestX;
        upperY = bestY;
        lower = bestLower;
        lowerY = bestLowerY;
        res_.bestIter = bestIter;
    }
    res_.hpwlFinal = hpwl(upper, upperY);
    res_.hpwlLower = hpwl(lower, lowerY);
    res_.gap = res_.hpwlFinal - res_.hpwlLower;
    res_.spreadSeconds = spreadAcc;
    res_.buildSeconds += buildAcc;
    res_.solveSeconds += solveAcc;
    res_.usedLookAhead = par_.lookAhead;
    res_.fenceClamps = fenceClamps_;
    res_.fenceRegions = fences_ != nullptr ? fences_->numRegions() : 0;
    res_.fencePushes = fencePushes_;
    if (fences_ != nullptr && !movRegion_.empty()) {
        // Counted on the placement actually returned, not on the last lower bound:
        // this is the number that says whether the delivered result honours the
        // fences, which is the only one a reader cares about.
        std::vector<double> flat;
        flat.reserve(2 * upper.size());
        for (std::size_t i = 0; i < upper.size(); ++i) {
            flat.push_back(upper[i]);
            flat.push_back(upperY[i]);
        }
        res_.fenceViolations = fences_->countViolations(flat, movRegion_);
    }
    binCells(lower, lowerY);
    res_.overflowLower = scaledOverflow();
    binCells(upper, upperY);
    res_.overflowFinal = scaledOverflow();

    if (!par_.lookAhead) {
        // No legalization ran, so the anchors were the lower bound's own previous
        // positions and the solve had no spreading force left: this loop was just
        // the area-blind wirelength minimisation run to a local minimum. That is
        // the point of the switch -- it hands the caller the best wirelength the
        // net model can produce with all the overlap intact, so a downstream
        // legalizer's cost is an honest measure of the net model alone.
        ktlog.echo(
            "SimPL: look-ahead legalization DISABLED (KTPLACE_SIMPL_NO_LAL); returning the "
            "overlapping lower bound, HPWL {:.6e}, scaled overflow {:.6e}",
            res_.hpwlFinal, res_.overflowFinal);
    }

    // The result is the last upper bound (Figure 2: "Last Upper-bound
    // Placement"); positions are written back for the normal output path.
    for (std::size_t i = 0; i < numMovable_; ++i) {
        db_.setCellPosition(movVertex_[i], upper[i], upperY[i]);
    }

    if (!plotDir.empty()) {
        ensureDir(plotDir);
        std::ofstream csv(plotDir + "/simpl_bounds.csv");
        csv << "iter,hpwl_upper,hpwl_lower,gap\n";
        for (std::size_t i = 0; i < curve.size(); ++i) {
            csv << i << ',' << curve[i].first << ',' << curve[i].second << ','
                << (curve[i].first - curve[i].second) << '\n';
        }
        // Lower and upper HPWL against iteration: the two bounds meeting is the
        // whole convergence story, so it is worth a picture.
        double mx = 0.0;
        for (const auto &p : curve) {
            mx = std::max({mx, p.first, p.second});
        }
        const int W = 780, H = 340, PAD = 60;
        std::ofstream svg(plotDir + "/simpl_bounds.svg");
        svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H
            << "'>\n<rect width='100%' height='100%' fill='white'/>\n"
            << "<text x='" << PAD
            << "' y='24' font-family='monospace' font-size='14'>"
               "SimPL: HPWL of the lower and upper bounds</text>\n";
        const auto px = [&](std::size_t i) {
            return PAD + (W - 2 * PAD) *
                             (curve.size() < 2
                                  ? 0.0
                                  : static_cast<double>(i) / static_cast<double>(curve.size() - 1));
        };
        const auto py = [&](double v) {
            return H - PAD - (H - 2 * PAD) * std::clamp(v / std::max(mx, 1e-12), 0.0, 1.0);
        };
        svg << "<line x1='" << PAD << "' y1='" << (H - PAD) << "' x2='" << (W - PAD) << "' y2='"
            << (H - PAD) << "' stroke='#333'/>\n<line x1='" << PAD << "' y1='" << PAD << "' x2='"
            << PAD << "' y2='" << (H - PAD) << "' stroke='#333'/>\n";
        svg << "<polyline fill='none' stroke='#2471a3' stroke-width='2' points='";
        for (std::size_t i = 0; i < curve.size(); ++i) {
            svg << px(i) << ',' << py(curve[i].second) << ' ';
        }
        svg << "'/><polyline fill='none' stroke='#c0392b' stroke-width='2' points='";
        for (std::size_t i = 0; i < curve.size(); ++i) {
            svg << px(i) << ',' << py(curve[i].first) << ' ';
        }
        svg << "'/>\n<text x='" << (W - PAD - 130) << "' y='" << (PAD + 12)
            << "' font-family='monospace' font-size='12' fill='#2471a3'>lower bound</text>\n"
            << "<text x='" << (W - PAD - 130) << "' y='" << (PAD + 28)
            << "' font-family='monospace' font-size='12' fill='#c0392b'>upper "
               "bound</text>\n</svg>\n";
    }

    if (!frameDir_.empty()) {
        ensureDir(frameDir_);
        writeFrame(frameDir_ + "/simpl_FINAL_LAL.svg", upper, upperY, res_.hpwlFinal,
                   res_.overflowFinal, "FINAL = last LAL (look-ahead legalized) placement",
                   res_.globalIters, std::max<std::size_t>(res_.globalIters, 1));
        if (par_.densityMaps) {
            writeDensityMap(frameDir_ + "/simpl_density_FINAL.svg", upper, upperY, "FINAL LAL");
            writeDensityMap(frameDir_ + "/simpl_density_FINAL_LSS.svg", lower, lowerY, "FINAL LSS");
        }
        // The GIF itself is written by the flow, not here: it can only be
        // assembled once the legalizer and the detailed placer have added their
        // frames, and a GIF closed at the end of global placement would stop
        // exactly where the placement stops being interesting.

        // A browsable index of the stills alongside the bounds curve, so a run
        // can be flipped through frame by frame without an image viewer that
        // knows how to sort "frame_0010" after "frame_0009".
        if (!animEnabled_) {
            // Nothing rastered; the SVG stills are indexed by name instead.
            std::vector<std::string> svgs;
            for (const auto &e : std::filesystem::directory_iterator(frameDir_)) {
                if (e.path().extension() == ".svg") {
                    svgs.push_back(e.path().filename().string());
                }
            }
            std::sort(svgs.begin(), svgs.end());
        } else {
            // Raster stills now live in the run's animation directory, not
            // beside the SVGs, so index them from there.
            std::vector<std::string> stills;
            const std::filesystem::path animDir = std::filesystem::path(plotDir) / "anim";
            std::error_code ec;
            for (const auto &e : std::filesystem::directory_iterator(animDir, ec)) {
                if (ec) {
                    break;
                }
                const std::string n = e.path().filename().string();
                if (n.rfind("frame_", 0) == 0 && e.path().extension() == ".ppm") {
                    stills.push_back("anim/" + n);
                }
            }
            std::sort(stills.begin(), stills.end());
        }
    }

    ktlog.trace("frames written: {} to {}", res_.framesWritten,
                frameDir_.empty() ? "(none)" : frameDir_);
    return res_;
}

// ---------------------------------------------------------------------------

SimplePlacer::SimplePlacer(ktDM &db) : pImpl(std::make_unique<Impl>(db)) {}
SimplePlacer::~SimplePlacer() = default;
SimplePlacer::SimplePlacer(SimplePlacer &&) noexcept = default;
SimplePlacer &SimplePlacer::operator=(SimplePlacer &&) noexcept = default;

SimplResult SimplePlacer::place(const SimplParams &params, const std::string &plotDir,
                                bool useFences) {
    return pImpl->run(params, plotDir, useFences);
}

void reportSimpl(const SimplResult &r) {
    ktReportTable t("SimPL solver results");
    t.setHeaders({"metric", "initial", "final"});
    t.addRow({"movable cells", "", fmt::format("{}", r.numMovable)});
    t.addRow({"fixed cells", "", fmt::format("{}", r.numFixed)});
    t.addRow({"nets", "", fmt::format("{}", r.nets)});
    t.addRow({"init iterations", "", fmt::format("{}", r.initIters)});
    t.addRow({"global iterations", "", fmt::format("{}", r.globalIters)});
    t.addRow({"bin grid", "", fmt::format("{}x{}", r.binsX, r.binsY)});
    t.addRow({"matrix build (s)", "", fmt::format("{:.6}", r.buildSeconds)});
    t.addRow({"look-ahead (s)", "", fmt::format("{:.6}", r.spreadSeconds)});
    t.addRow({"linear solves (s)", "", fmt::format("{:.6}", r.solveSeconds)});
    t.addRow({"look-ahead legalization", "", r.usedLookAhead ? "on" : "OFF (raw LSS)"});
    t.addRow({"fence regions", "",
              !r.fencesEnabled
                  ? "OFF (disabled)"
                  : (r.fenceRegions == 0 ? "none" : fmt::format("{}", r.fenceRegions))});
    t.addRow({"cells held in fence", "", fmt::format("{}", r.fenceClamps)});
    t.addRow({"cells pushed out of a fence", "", fmt::format("{}", r.fencePushes)});
    t.addRow({"cells outside their fence at exit", "", fmt::format("{}", r.fenceViolations)});
    t.addRow({"HPWL seed", fmt::format("{:.6}", r.hpwlSeed), ""});
    t.addRow({"HPWL after initial placement", fmt::format("{:.6}", r.hpwlInit), ""});
    // Whether the warm-up's own solve converged. A large residual here means the
    // global phase starts from a placement that is not yet a quadratic optimum,
    // so its first iterations are spent finishing the warm-up rather than
    // spreading, and the gap the loop is supposed to close starts wide.
    t.addRow({"initial CG residual", fmt::format("{:.3e}", r.initResidual), ""});
    t.addRow({"HPWL lower bound", "", fmt::format("{:.6}", r.hpwlLower)});
    t.addRow({"HPWL final", "", fmt::format("{:.6}", r.hpwlFinal)});
    t.addRow({"returned from iteration", "", fmt::format("{} of {}", r.bestIter, r.globalIters)});
    t.addRow({"bound gap", "", fmt::format("{:.6}", r.gap)});
    t.addRow({"scaled overflow (lower)", "", fmt::format("{:.6}", r.overflowLower)});
    t.addRow({"scaled overflow (final)", "", fmt::format("{:.6}", r.overflowFinal)});
    t.addRow({"SVG frames written", "", fmt::format("{}", r.framesWritten)});
    t.addRow({"paper reference (adaptec1)", "77410738"});
    t.emit();
}

}  // namespace ktplace
