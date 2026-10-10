// @file kt_simpl.h// SimPL global placement (Kim, Lee & Markov, CACM 56(6):105-113, 2013)// Self-contained force-directed global placement. Unlike the electrostatic// placers (ePlace/RePlAce) there is no potential solve here; the spreading// information comes from a *legalized* placement, which is what makes the// method both fast and its convergence provable.// The flow keeps two placements and lets them meet:// lower bound  the solution of a sparse linear system, which may overlap// upper bound  a roughly legal placement produced by look-ahead legalization// The upper bound is computed first each iteration and then used two ways: its// locations become *fixed anchors* wired to their cells by artificial// two-pin pseudonets, and it becomes the linearization point for the net model.// The lower bound is then re-solved with those anchors in the system, which// drags it toward feasibility. Because the pseudonet weight grows with the// iteration number, the emphasis moves steadily from interconnect reduction to// constraint satisfaction, and convergence is guaranteed rather than hoped for.// Three pieces carry the method, and each is implemented here as described:// - BOUND2BOUND (B2B) net model. A p-pin net is decomposed *for the current// placement* into edges between the extreme (min and max) pins and every// other pin, each of weight w / ((k-1) * |distance|). For that placement the// quadratic objective then equals the net's bounding-box length exactly, so// the solve is a faithful first-order model of HPWL. The catch is that the// model is placement-dependent, so the graph is rebuilt every iteration --// the "B2B Graph Update" box in Figure 2 of the paper.// - LOOK-AHEAD LEGALIZATION (Algorithm 1). Overfull grid bins are clustered by// BFS; each cluster is grown to a minimal containing rectangle of legal// density; that rectangle is recursively split by a cell-area cutline and a// whitespace cutline, and cells are redistributed by per-stripe linear// scaling. Scaling factors differ per stripe, which is where the// "nonlinearity" comes from, and that is what removes overlap while// preserving relative placement.// - PSEUDONETS. Fixed zero-area anchors at the upper-bound locations, with// weight alpha / distance and alpha = 0.01 * (1 + iteration). Because a fixed// endpoint contributes only to the diagonal, these also improve the// conditioning of the Jacobi-preconditioned CG solve.// References: M.-C. Kim, D.-J. Lee, I. L. Markov, "SimPL: an algorithm for// placing VLSI circuits", CACM 56(6), 105-113, 2013 (journal version: IEEE// TCAD 31(1):50-60, 2012). The B2B model is from P. Spindler et al.,// "Kraftwerk2", IEEE TCAD 27(8), 2008.


#pragma once

#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"

#include <cstddef>
#include <memory>
#include <string>

namespace ktplace {

/// Tunables. Defaults follow the paper: target density g = 1.0, five to seven
/// initial-placement iterations, 33-45 global-placement iterations on ISPD 2005.
struct SimplParams {
    // --- initial placement (ignores areas and overlaps entirely) ------------
    /// Alternate B2B rebuild and CG solve until HPWL stops improving. The paper
    /// reports 5-7 iterations being sufficient.
    /// Hard ceiling on warm-up iterations. A safety net, not the stopping rule.
    /// Upper bound on the initial-placement rounds of Section 4.1.
    ///
    /// Seven, which is what the paper reports for every benchmark size: it
    /// alternates the quadratic solve with a B2B graph rebuild, and the rebuild
    /// is the point, because the net model is placement-dependent and a second
    /// solve on the same graph buys very little. This was 1, on the reasoning that
    /// the warm-up result is thrown away. It is not thrown away -- it seeds the
    /// global-placement loop, and the first anchors are the first look-ahead
    /// legalization of it, so its shape is what gets locked in. The paper says as
    /// much: this step "notably impacts the final result, as it can determine the
    /// overall shape of the final placement solutions". Measured on adaptec1,
    /// one round gives 4.054e+08 and seven give 3.559e+08.
    std::size_t initMaxIters = 7;

    /// Stop the warm-up when a round improves HPWL by less than this fraction.
    /// The paper's Section 4.1 says only "until HPWL stops improving", which as
    /// written means any non-improvement at all ends it -- and a quadratic solve
    /// alternates improvements with tiny regressions, so that rule fires on noise
    /// and stops the warm-up early. A relative floor plus patience is the same
    /// test with the noise taken out.
    double initTolFrac = 5e-3;
    /// Consecutive rounds below initTolFrac before the warm-up is called done.
    /// One, because the warm-up is meant to be rough, and because the star model
    /// is placement-independent: once its solve has converged, a further round
    /// re-solves the same system (adaptec1: zero CG iterations from round 2 on),
    /// and on a fenced design it only re-solves the fence clamping. Measured with
    /// every round run (this was 0): rounds 3-7 changed adaptec1's final HPWL not
    /// at all (4.219e+08 for 2, 3 and 7 rounds) and cost mgc_superblue16_a ~100 s.
    /// Applies to the star model only; a B2B warm-up runs every round.
    /// 0 disables the early exit entirely: every round in initMaxIters runs.
    std::size_t initPatience = 1;

    // --- look-ahead legalization --------------------------------------------
    /// Run look-ahead legalization. Turning this off returns the raw lower bound,
    /// so the only spreading is whatever the anchors achieve and the caller is
    /// left to legalize (Abacus). It is a diagnostic, not a configuration: the
    /// paper's upper bound IS a legalized placement, so there is no such thing as
    /// a finished SimPL result without it. What it does isolate is how much of
    /// the lower bound's wirelength survives a real legalizer, which is the
    /// measurement needed to tell a bad net model from a bad legalizer.
    bool lookAhead = true;
    /// Maximum allowed bin density, g. The paper's ISPD 2005 runs use 1.0.
    double densityLimit = 1.0;
    /// Bins per axis for the density grid. 0 selects automatically.
    std::size_t binsX = 0;
    std::size_t binsY = 0;
    /// Number of look-ahead legalization passes applied to the lower bound each
    /// outer iteration. One pass is not enough once the top-down partitioning is
    /// given the whole die: it spreads into the space and leaves holes mid-die.
    /// The isolated measurement (KTPLACE_SIMPL_LAL_ONLY) reached 0.026 overflow
    /// in two passes on adaptec1, so the machinery converges -- the loop just
    /// never gave it the rounds. Passes stop early once the overflow stops
    /// improving.
    std::size_t lalPasses = 1;
    /// Stop the internal passes once the overflow improves by less than this
    /// relative amount.
    double lalMinGain = 0.01;
    /// Relative band within which two upper bounds count as equally legalized,
    /// so the shorter one wins. Ranking the run's upper bounds by wirelength
    /// alone selects the least legalized iteration in it, because the more
    /// collapsed a placement is the shorter its wires and the more it overlaps;
    /// this is how wide "more legalized" has to be before wirelength decides.
    /// 0 makes overflow the only key, which on adaptec1 costs ~20% wirelength
    /// chasing overflow differences that do not survive legalization.
    double upperOvfTol = 0.05;
    /// Recursion cut-off from Algorithm 1 line 8: blocks at this depth stop
    /// being split.
    std::size_t maxLevel = 10;
    /// Algorithm 1 line 8 "Area(B) is small enough". The paper does not give a
    /// value; blocks holding no more than this many cells are not split. A
    /// single cell cannot overlap anything, so 1 is always safe.
    std::size_t minCellsToSplit = 4;
    /// A stripe is subdivided further while its available area exceeds this
    /// fraction of the region's available area (1/10 in the paper).
    double stripeAreaFraction = 0.1;
    /// A cluster whose own rectangle already holds at least this fraction of the
    /// movable cell area means the placement is globally collapsed, not locally
    /// overfull, so the top-down partitioning is given the whole usable die instead
    /// of the minimal legal-density rectangle. Without this the legalizer confines
    /// a collapsed placement to the ~54% of the die its cells strictly need and
    /// never uses the rest -- measured as a fully empty left quarter and top third,
    /// at a large wirelength cost because the I/O pads ring the die.
    double globalClusterFrac = 0.5;

    // --- pseudonets ---------------------------------------------------------
    /// alpha = alphaBase * (1 + iteration number).
    double alphaBase = 0.01;
    /// Optional annealing of the pseudonet weight. 1.0 (the default) never anneals,
    /// which is the paper's "increasing weights of pseudonets" schedule.
    ///
    /// The schedule is the suspected cause of a specific failure: because alpha
    /// only ever grows and it is the *only* spreading force in the solve, nothing
    /// in the loop ever reduces spreading pressure, so once the lower bound has
    /// been pulled far enough out, wirelength can only get worse. Measured on
    /// ibm01, the upper bound lands at ~2.1e7 for every alphaBase from 0.001 to
    /// 0.3 while a legal placement of 1.05e7 existed at iteration 2, at the same
    /// density -- so the damage is not the magnitude of alpha but its having no
    /// downward phase.
    ///
    /// With alphaDecay set, alpha is multiplied by this factor on every iteration
    /// after the lower bound's overflow drops below alphaDecayBelow, so spreading
    /// is ramped up while the placement is crowded and then relaxed to let
    /// wirelength be recovered. 1.0 disables it.
    double alphaDecay = 1.0;
    /// Lower-bound overflow below which alphaDecay starts to apply.
    double alphaDecayBelow = 0.40;

    /// The pseudonet weight law. The paper's Figure 6 labels a pseudonet
    /// "weight = alpha/Length", and AMF-Placer (ICCAD 2021) states it
    /// independently: "the weight of a pseudo net is calculated by dividing a
    /// global factor alpha by the movement distance of the corresponding
    /// instance in last optimization". Taken literally that is a constant-force
    /// L1 penalty whose restoring force does not grow with distance, which would
    /// give the linear solver permanent freedom to keep optimising wirelength for
    /// a cell the legalizer moved a long way.
    ///
    /// It is also far too weak to spread anything, and that is measured rather
    /// than argued. On adaptec1 the per-cell interconnect diagonal is 0.164 and
    /// a lower bound sits ~1000 units from its legal anchor, so
    ///     w = alpha/length -> per-cell anchor weight 1.0e-5, i.e. 6.2e-05 of
    ///                           the interconnect diagonal at iteration 0
    ///     w = alpha       -> per-cell anchor weight 0.01,   i.e. 6.1e-02, and
    ///                           the lower bound's overflow falls 0.548 -> 0.308
    ///                           over two iterations instead of staying at 0.508
    /// Parity between 0.01*(1+iteration) and 0.164 arrives around iteration 15,
    /// which is inside the paper's 26-35 global-placement iterations. So the
    /// figure's Length must be a normalised length of order 1, in which case
    /// alpha/Length reduces to alpha up to a constant the published schedule is
    /// already calibrated against.
    enum class PseudonetLaw {
        ConstantStiffness,  ///< w = alpha -- the paper's law, as a quadratic surrogate
        InverseLength,      ///< w = alpha / max(distance, floor) -- not the paper's
    };

    // Which quadratic model a net is expanded into for a solve.// Star and B2B are two approximations of the same HPWL, and which is better// depends on the state of the placement being approximated -- which is why// they are chosen per stage rather than globally.// B2B (Spindler et al.) wires each net's extreme pins to each other and to// every internal pin, with weight 1/length. It is placement-dependent, so the// matrix has to be rebuilt as the placement moves, and it preserves the// bounding box rather than the ordering: for cells that are still piled on// top of each other it is a poor model of what the net will actually cost.// Star wires every pin to a free virtual centre. The centre is minimised out// analytically and what remains is a clique of weight w/k, so it builds the// same matrix with no position dependence at all. A clique is the better// approximation while cells are unspread, because it is the one that keeps// the ordering of the pins, and it needs no rebuild.// So: star for the initial placement, which is the unspread solve, and B2B for// the global-placement solves, which approximate an already-spread placement// and need the bounding box to track the spreading.

    enum class NetModel {
        Star,  ///< clique of weight w/k; weights independent of the placement
        B2B,   ///< extremes plus extremes-to-internal, weighted 1/length
    };

    /// Net model for the initial placement, and for the global-placement solves.
    ///
    /// Star for the first, B2B for the second; see NetModel for why they differ.
    /// The initial placement is the solve that decides the shape everything else is
    /// built on, and a clique holds the ordering of cells that are still piled up
    /// better than a bounding box does. The global-placement solves approximate an
    /// already-spread placement, where the bounding box is the better model and
    /// the paper uses it.
    ///
    /// Measured on adaptec1, and worth reading before trusting the first half of
    /// that: the star initial placement is BETTER -- 6.59e+07 against 8.07e+07 for
    /// B2B, against the paper's 4.48e+07 -- and the finished placement is WORSE,
    /// 5.63e+08 against 3.56e+08. The two are consistent, and the reason is the
    /// look-ahead legalizer rather than either model. A tighter initial placement
    /// has a higher peak density, and the legalizer spreads until every bin is at
    /// the density limit, so it pushes the cells further to reach it. The
    /// over-spreading is the known gap against the paper (see the README), and it
    /// is large enough to swallow a 19% improvement in the solve that precedes it.
    /// Once that is fixed, the better initial placement is what should be wanted
    /// here, so this stays the default.
    ///
    /// More iterations do not help the star model, and the reason is structural: its
    /// weights do not depend on the placement, so the matrix is identical on every
    /// round and the iteration count is a CG tolerance and nothing else. B2B
    /// rebuilds each round from the new positions, which is why the paper alternates
    /// solve and rebuild there and why initMaxIters matters for it.
    /// How far a stripe spreads the cells assigned to it, in the nonlinear scaling
    /// of Section 4.2 step (iv).
    ///
    /// The paper says cell locations within each stripe are "linearly scaled from
    /// current locations" and does not give the factor. Measured on adaptec1, every
    /// reading of it that actually scales is worse than not scaling at all:
    ///
    ///   mode   final HPWL   it0 spread   worst overfull bin after LAL
    ///   tight   2.462e+08      10.25x            219x
    ///   both    2.438e+08       8.07x            --
    ///   none    2.251e+08       7.87x             41x
    ///
    /// against the paper's 7.74e+07. So None is the default: the stripe assignment
    /// and the clamp to the assigned stripe are kept, and the scaling is not.
    ///
    /// This is not a legality trade. All three produce a placement the checker
    /// passes with zero overlapping pairs, zero cells off row, off site or over a
    /// macro, because the Abacus legalization that follows the global placement is
    /// what actually legalizes -- the look-ahead pass only has to hand it
    /// something shaped sensibly. And None leaves the least extreme local
    /// crowding behind (41x rather than 219x), so it is not buying wirelength with
    /// overlap. What scaling buys is a more even fill, and that is what costs the
    /// wirelength: spreading a stripe moves cells away from the net clusters that
    /// put them there.
    ///
    /// The other two are kept because they are the paper's step and the question
    /// deserves to be answerable without a code change.
    enum class StripeScale {
        Tight,  ///< scale only when over capacity; never spreads a sparse stripe
        Both,   ///< sqrt(room/assigned) either way
        None,   ///< assign and clamp only; no scaling
        Fill,   ///< sort by position, then greedily pack with minimal movement
    };

    /// How the cells assigned to a stripe are placed inside it, which is the
    /// last step of Algorithm 1's "nonlinear scaling" and the only place the
    /// nonlinearity comes from: different stripes get different factors, so the
    /// method is not a uniform scaling of the whole region.
    ///
    /// Both is the default, and it is what the paper describes -- "cell locations
    /// within each stripe are linearly scaled from current locations". None, which
    /// disables the step entirely, was the default until now because it gave the
    /// shortest wirelength, and that comparison was confounded: the upper bound
    /// was being chosen by wirelength alone, which selects the *least* legalized
    /// iteration in the run (the more collapsed a placement is, the shorter its
    /// wires), so "None wins" was measuring a selection artefact. With the
    /// selection fixed, Both beats None on wirelength on ibm01 by 18% and on
    /// legality on both designs -- overflow 0.224 against 0.276 on ibm01, 0.169
    /// against 0.206 on adaptec1 -- and costs 4% of wirelength on adaptec1.
    StripeScale stripeScaleMode = StripeScale::Both;

    NetModel initNetModel = NetModel::Star;
    NetModel lssNetModel = NetModel::B2B;
    // The paper, on this, verbatim: "we control cell movement and iteration
    // convergence by multiplying each pseudonet weight by an additional factor
    // alpha > 0 computed as alpha = 0.01 x (1 + Iteration_Number) ... The relevant
    // constraint requires that each cell be placed over its anchor, and the
    // (Manhattan) distance between their locations is the penalty for violating
    // the constraint."
    //
    // So the anchor term is alpha times the *Manhattan distance to the anchor* --
    // an L1 penalty, whose gradient is a force of constant magnitude alpha. The
    // weight is therefore alpha, not alpha over the distance: alpha/d is not the
    // paper's law and, as a quadratic surrogate for an L1 penalty, it is the wrong
    // shape as well. This had briefly been changed to InverseLength on the reading
    // of a "weight = alpha/Length" note in the source; that note describes no
    // equation in this paper, and the text above is the actual statement.
    //
    // alpha = 0.01 x (1 + it) is unchanged and already matches the paper exactly.
    // KTPLACE_SIMPL_PSEUDONET=inverse selects the other law for comparison.
    PseudonetLaw pseudonetLaw = PseudonetLaw::ConstantStiffness;

    // --- convergence --------------------------------------------------------
    /// The paper's rule, expressed scale-free. Verbatim the paper says "Global
    /// placement continues until (1) the gap is reduced to 25% of the gap at the
    /// 10th iteration and upper-bound solution stops improving or (2) the gap is
    /// smaller [than 10% of it]." The reference, "the gap at the 10th iteration",
    /// is itself already a small fraction of the placement -- on adaptec1 that gap
    /// is ~4% of the upper bound's HPWL -- so "25% of the gap" means the bounds
    /// within ~1% of each other, which never occurs short of the gap going negative
    /// through overlap. Measuring the gap as a fraction of the placement it
    /// describes (gap / upperHpwl) is the same rule with the scale taken out.
    ///
    /// Iteration 10 is still honoured, as the *window*, not the reference: upper
    /// bound HPWL oscillates for the first four to seven iterations, so no
    /// convergence test is evaluated before gapReferenceIter. That is what keeps a
    /// lucky early dip from ending the run.
    ///
    /// Both halves of the rule are required before the relaxed test fires. The gap
    /// alone is satisfied by both bounds drifting upward together, which is what
    /// this implementation did: on adaptec1 the upper bound rose from 5.3e8 to
    /// 7.5e8 while the gap fell 4.5e8 -> 2.0e7, so a gap-only test certified
    /// convergence on a placement 40% worse than the one it started from.
    /// Requiring the upper bound to have stopped improving (patience) is what makes
    /// the criterion mean something.
    ///
    /// Defaults are fractions of the upper bound's HPWL.
    double gapRelaxedFrac = 0.25;       ///< (1) gap within 25% of the reference, and stale
    double gapTightFrac = 0.10;         ///< (2) gap below 10% of the reference: converged
    std::size_t gapReferenceIter = 10;  ///< oscillation window; first test at it+1
    /// Upper-bound non-improving iterations tolerated once the gap test is met --
    /// the paper's "stops improving", which is what the oscillation note warns about.
    std::size_t patience = 5;

    // --- solver -------------------------------------------------------------
    // CG budget. The paper's claim is that, with preconditioning, the iteration
    // count grows no faster than log n -- about 18 for 2e5 cells -- and that one
    // placement iteration costs O(n log^2 n). A 200-iteration cap is not part of
    // the method.
    std::size_t cgMaxIter = 60;
    // Relative residual. This is deliberately loose. The B2B objective is a
    // first-order linearisation of HPWL that is exact only at the point it was
    // built, so the solve is there to supply a descent direction, not an exact
    // minimiser; and B2B's 1/length weights span orders of magnitude, which
    // leaves Jacobi CG stalling near 1e-3 no matter how long it runs. Measured
    // with a 1e-6 target: every solve used all 200 iterations and still ended
    // between 3e-4 and 1.4e-2, i.e. the budget bought nothing.
    double cgTol = 1e-3;

    // --- run ----------------------------------------------------------------
    std::size_t maxIters = 100;
    /// Write an SVG frame every N global-placement iterations (0 = only the
    /// final frame). Requires a snapshot directory. One, because the animation is
    /// mostly about watching global placement converge: with this off, the GIF was
    /// almost entirely legalization and detailed placement, which are the two
    /// stages that move the fewest cells.
    std::size_t traceEvery = 1;
    /// Write a cell-placement SVG frame every N conjugate-gradient iterations
    /// inside each linear solve (0 = off). A full run produces thousands of
    /// ~4 MB frames, so enable this only for short debug runs
    /// (KTPLACE_SIMPL_CG_EVERY=1 with a small KTPLACE_SIMPL_ITERS).
    std::size_t cgEvery = 0;
    /// Cadence inside the warm-up solves, which run a handful of times and are
    /// otherwise invisible in the animation: with the global phase's cadence the
    /// opening of the run contributes no frames at all.
    std::size_t cgEveryInit = 1;
    /// Write a bin-density heat map next to every placement frame. This is the
    /// view that shows whether the lower bound is actually spreading.
    bool densityMaps = true;
    /// Fixed seed for the uniform initial placement, so runs are reproducible.
    std::uint64_t seed = 20240607;

    /// Where the starting placement comes from.
    enum class StartPlacement {
        /// Use the design's own placement when it looks like a real one, and fall
        /// back to the paper's uniform seed otherwise. Not the default: measured
        /// on dma, which does ship a placement at 4.33e8, adopting it and then
        /// running the loop ends at 4.13e9, so the loop still destroys a good
        /// input. Enable it once the loop stops regressing real placements.
        Auto,
        /// The paper's Figure 2: a uniformly distributed placement, then the
        /// area-blind quadratic initial placement of Section 4.1. Correct when
        /// the design ships no usable placement.
        Uniform,
        /// Trust the design's own placement unconditionally.
        Input,
    };
    /// Uniform, the paper's behaviour, is the default: the ISPD 2005 benchmarks
    /// ship every movable cell at the origin, so there is nothing there to adopt,
    /// and adopting a real placement currently makes the result worse.
    StartPlacement start = StartPlacement::Uniform;

    /// Minimum fraction of movable cells that must lie inside the die for the
    /// input placement to count as usable under StartPlacement::Auto.
    double minInputInsideFrac = 0.9;
};

/// Outcome of a run.
struct SimplResult {
    std::size_t numMovable = 0;
    std::size_t numFixed = 0;
    std::size_t nets = 0;

    /// Iterations actually run, initial placement included.
    std::size_t initIters = 0;
    std::size_t globalIters = 0;

    std::size_t binsX = 0;
    std::size_t binsY = 0;

    /// HPWL of the placement the design shipped with, when one was adopted.
    double hpwlInput = 0.0;
    /// Whether the input placement was adopted instead of a uniform seed.
    bool usedInputPlacement = false;
    /// HPWL of the uniform seed, of the last lower bound, and of the returned
    /// placement. The paper's result is the upper bound.
    ///
    /// The returned placement is the *best* upper bound seen, not the last: the
    /// upper bound is not monotone across iterations, so the two differ, and
    /// returning the last one can ship a materially worse placement than the run
    /// already computed. bestIter says which iteration it came from.
    double hpwlSeed = 0.0;
    /// HPWL after the warm-up, and how far its last conjugate-gradient solve
    /// actually got. The two say whether the warm-up produced a placement worth
    /// starting from, or one the global phase still has to undo.
    double hpwlInit = 0.0;
    double initResidual = 0.0;
    double hpwlLower = 0.0;
    double hpwlFinal = 0.0;
    /// Iteration the returned upper bound came from, 0-based.
    std::size_t bestIter = 0;
    /// Whether look-ahead legalization ran. When false, hpwlFinal IS hpwlLower
    /// and the placement is overlapping.
    bool usedLookAhead = false;
    /// Final gap, hpwlFinal - hpwlLower.
    double gap = 0.0;

    /// Fence accounting. All zero for an unconstrained design, which is how a
    /// caller can tell "no fences" from "fences present and ignored".
    std::size_t fenceClamps = 0;
    std::size_t fencePushes = 0;
    std::size_t fenceViolations = 0;
    // Fences the design carried, and whether enforcement was on. Reported with
    // the rest so the table can say "none" against "OFF (disabled)".
    std::size_t fenceRegions = 0;
    bool fencesEnabled = true;

    /// Scaled overflow per bin (Figure 7) of the final lower bound and of the
    /// returned placement.
    double overflowLower = 0.0;
    double overflowFinal = 0.0;

    double spreadSeconds = 0.0;
    double buildSeconds = 0.0;
    double solveSeconds = 0.0;

    /// Placement frames (LSS/LAL/CG/density) actually written.
    std::size_t framesWritten = 0;
};

// The solver's own summary, emitted from the component that produced the
// numbers rather than by the caller that ran it.
void reportSimpl(const SimplResult &res);

class SimplePlacer {
public:
    explicit SimplePlacer(ktDM &db);
    ~SimplePlacer();

    SimplePlacer(const SimplePlacer &) = delete;
    SimplePlacer &operator=(const SimplePlacer &) = delete;
    SimplePlacer(SimplePlacer &&) noexcept;
    SimplePlacer &operator=(SimplePlacer &&) noexcept;

    // Run global placement. The fences are the design's own, read from the
    // database; enforceFences turns them off for a run that wants to measure what
    // they cost.
    SimplResult place(const SimplParams &params = {}, const std::string &plotDir = "",
                      bool useFences = true);

private:
    class Impl;
    std::unique_ptr<Impl> pImpl;
};

}  // namespace ktplace
