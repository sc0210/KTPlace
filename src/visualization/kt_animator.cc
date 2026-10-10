// @file kt_animator.cc// Implementation of PlacementAnimator


#include "visualization/kt_animator.h"

#include "util/kt_log.h"

#include <utility>

namespace ktplace {

namespace {
/// Zero-padded so directory order is frame order. Matches the GIF writer's
/// expectation of "frame_NNNN.ppm".
std::string frameName(std::size_t n) {
    std::string s = std::to_string(n);
    while (s.size() < 4) {
        s.insert(s.begin(), '0');
    }
    return "frame_" + s + ".ppm";
}
}  // namespace

PlacementAnimator &PlacementAnimator::instance() {
    // Function-local static: constructed on first use, so a run that never
    // animates does not pay for it, and destroyed in reverse order at exit.
    static PlacementAnimator animator;
    return animator;
}

void PlacementAnimator::configure(const std::string &outDir, std::size_t maxFrames, int delayCs,
                                  int blend, double zoom) {
    dir_ = outDir;
    maxFrames_ = maxFrames > 0 ? maxFrames : 1;
    delayCs_ = delayCs;
    blend_ = blend > 1 ? blend : 1;
    zoom_ = zoom >= 1.0 ? zoom : 1.0;
    frame_ = 0;
    havePrev_ = false;
    prevX_.clear();
    prevY_.clear();
    held_ = 0;
    capped_ = false;
    enabled_ = !outDir.empty();
    if (enabled_) {
        std::error_code ec;
        std::filesystem::create_directories(dir_, ec);
        enabled_ = !ec;
    }
}

void PlacementAnimator::reset() {
    dir_.clear();
    frame_ = 0;
    offered_ = 0;
    stride_ = 1;
    havePrev_ = false;
    prevX_.clear();
    prevY_.clear();
    held_ = 0;
    capped_ = false;
    enabled_ = false;
}

void PlacementAnimator::holdBack(std::size_t n) {
    // Never reserve more than exists; a reservation that cannot be spent would
    // silently disable the stage it was meant to protect.
    held_ = n < maxFrames_ ? n : maxFrames_ / 2;
    capped_ = false;
}

void PlacementAnimator::record(const Graph &g, const std::vector<float> &x,
                               const std::vector<float> &y, const BBox &die, std::size_t step,
                               std::size_t total, double hpwl, double hpwlInitial, double resid,
                               const std::string &note, const constraintMgr *constraints,
                               bool mandatory) {
    const std::size_t limit = held_ < maxFrames_ ? maxFrames_ - held_ : maxFrames_;
    // A mandatory frame is one per global-placement iteration, or the legalizer's
    // and the detail placer's own: sparse by construction. It is exempt from both
    // the stage budget and the thinning stride, because the dense per-CG-iteration
    // frames are what fill the budget and what gets thinned. Without the exemption
    // the whole LSS/LAL sequence was dropped from the GIF -- 12 frames out of
    // several hundred offered, so the stride skipped essentially all of them.
    if (!enabled_ || (!mandatory && frame_ >= limit)) {
        if (enabled_ && !capped_) {
            capped_ = true;
            ktlog.echo(
                "animation: this stage used its {} frame allowance; later stages may not be "
                "represented. Raise KTPLACE_ANIM_MAX_FRAMES, or lower KTPLACE_SIMPL_CG_EVERY. "
                "The per-iteration SVGs are unaffected.",
                limit);
        }
        return;
    }
    // Every producer supplies one coordinate per vertex; a short vector would
    // index past the end inside the renderer.
    if (x.size() != g.getNumCells() || y.size() != g.getNumCells()) {
        return;
    }
    const auto emit = [&](const std::vector<float> &ix, const std::vector<float> &iy) {
        // Subsample rather than stop. Every CG iteration of every round is offered,
        // and once the byte budget is spent the spacing widens, so the animation
        // covers the whole run -- warm-up, LSS, LAL, legalization, detail placement
        // -- instead of exhausting itself in the first rounds and ending before
        // legalization begins.
        ++offered_;
        // Mandatory frames bypass the stride. One frame per global-placement
        // iteration is only 12 of the several hundred offered once every
        // conjugate-gradient iteration is offered, so the subsampling that keeps
        // the warm-up affordable was dropping the entire LSS and LAL sequence --
        // the part of the run the animation most needs to show. Per-iteration
        // frames are the sparse ones by construction, so they are kept and the
        // dense ones are thinned instead.
        if (!mandatory && stride_ > 1 && ((offered_ - 1) % stride_) != 0) {
            return true;
        }
        // Exempt for the same reason: the stage budget is reached long before the
        // warm-up's several hundred offered frames are, so without the exemption
        // the per-iteration frames still lost to the dense ones.
        if (frame_ >= limit && !mandatory) {
            stride_ *= 2;
            return true;
        }
        const std::string path = (dir_ / frameName(frame_)).string();
        // The caller's constraints, not nullptr: a run with fences drew them in
        // the per-iteration SVGs and then dropped them from the GIF, so the one
        // picture that gets looked at was missing the regions the placement was
        // required to respect.
        writeFrameRaster(path, g, ix, iy, die, step, total, hpwl, hpwlInitial, resid, note,
                         constraints, /*fixedView=*/true, zoom_);
        ++frame_;
        return frame_ < limit;
    };

    // The in-between frames first, so the animation arrives at this placement
    // rather than snapping to it.
    if (havePrev_ && blend_ > 1 && prevX_.size() == x.size()) {
        std::vector<float> bx(x.size()), by(y.size());
        for (int k = 1; k < blend_; ++k) {
            if (frame_ >= limit) {
                break;
            }
            const double t = static_cast<double>(k) / static_cast<double>(blend_);
            for (std::size_t v = 0; v < x.size(); ++v) {
                bx[v] = static_cast<float>(prevX_[v] + t * (x[v] - prevX_[v]));
                by[v] = static_cast<float>(prevY_[v] + t * (y[v] - prevY_[v]));
            }
            if (!emit(bx, by)) {
                break;
            }
        }
    }
    emit(x, y);

    prevX_ = x;
    prevY_ = y;
    havePrev_ = true;
}

bool PlacementAnimator::finish(const std::string &gifName) const {
    if (!enabled_) {
        return false;
    }
    // One still is not an animation, and a two-frame GIF is a flicker; both are
    // more likely to be a mistake in the run than something worth writing.
    if (frame_ < 2) {
        return false;
    }
    return writeAnimatedGif(dir_.string(), gifName, delayCs_);
}

}  // namespace ktplace
