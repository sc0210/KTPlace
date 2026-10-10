// SPDX-License-Identifier: BSD-3-Clause
// @file test_constraint.cc// Unit tests for the placement-region (fence) constraints// A fence is the one part of the data model where a "correct" answer is easy to// get subtly wrong, so these tests pin the geometric contract rather than just// calling each accessor once: rectangles come in pairs, a region is a union and// not an outline, containment includes the boundary, and escaping must land// outside *every* fence, not just the one that caught the point.


#define BOOST_TEST_MODULE ktplace_constraint

#include "constraint/kt_constraintMgr.h"
#include "util/kt_log.h"

#include <boost/test/included/unit_test.hpp>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

/// Build a corner list for a single rectangle.
std::vector<Point> rect(double loX, double loY, double hiX, double hiY) {
    return {Point{loX, loY}, Point{hiX, hiY}};
}

/// A manager with one rectangle fence at [0,100] x [0,100].
constraintMgr singleFence() {
    constraintMgr mgr;
    mgr.addRegion("er0", rect(0.0, 0.0, 100.0, 100.0));
    return mgr;
}

}  // namespace

BOOST_AUTO_TEST_CASE(a_fence_becomes_region_zero) {
    constraintMgr mgr;
    BOOST_TEST(mgr.numRegions() == 0u);
    BOOST_TEST(mgr.region(0) == nullptr);

    const int id = mgr.addRegion("er0", rect(0.0, 0.0, 100.0, 200.0));
    BOOST_TEST(id == 0);
    BOOST_TEST(mgr.numRegions() == 1u);

    const Region *r = mgr.region(0);
    BOOST_REQUIRE(r != nullptr);
    BOOST_TEST(r->name == "er0");
    BOOST_TEST(r->rects.size() == 1u);
    BOOST_TEST(r->area == 20000.0);
    // Bounding box is what contains() rejects against, so it must be set even
    // though the region has a single rectangle.
    BOOST_TEST(r->minX == 0.0);
    BOOST_TEST(r->minY == 0.0);
    BOOST_TEST(r->maxX == 100.0);
    BOOST_TEST(r->maxY == 200.0);
}

BOOST_AUTO_TEST_CASE(an_out_of_range_region_id_has_no_region) {
    constraintMgr mgr = singleFence();
    BOOST_TEST(mgr.region(-1) == nullptr);
    BOOST_TEST(mgr.region(1) == nullptr);
    BOOST_TEST(mgr.region(9999) == nullptr);
    BOOST_TEST(mgr.region(0) != nullptr);
}

BOOST_AUTO_TEST_CASE(corners_are_normalised_whichever_way_round_they_arrive) {
    // DEF point pairs are documented as lower-left/upper-right, but taking them
    // as unordered costs nothing and avoids a silently inverted fence.
    constraintMgr mgr;
    mgr.addRegion("er0", {Point{100.0, 200.0}, Point{0.0, 0.0}});

    const Region *r = mgr.region(0);
    BOOST_REQUIRE(r != nullptr);
    BOOST_TEST(r->area == 20000.0);
    BOOST_TEST(r->rects.front().lo.x == 0.0);
    BOOST_TEST(r->rects.front().lo.y == 0.0);
    BOOST_TEST(r->rects.front().hi.x == 100.0);
    BOOST_TEST(r->rects.front().hi.y == 200.0);
}

BOOST_AUTO_TEST_CASE(an_odd_trailing_corner_is_dropped_not_misread) {
    // A dangling corner cannot form a rectangle. Reading it as a rectangle with
    // a missing half would invent geometry, so the pair is simply not made.
    constraintMgr mgr;
    mgr.addRegion("er0", {Point{0.0, 0.0}, Point{10.0, 10.0}, Point{20.0, 20.0}});

    const Region *r = mgr.region(0);
    BOOST_REQUIRE(r != nullptr);
    BOOST_TEST(r->rects.size() == 1u);
    BOOST_TEST(r->area == 100.0);
}

BOOST_AUTO_TEST_CASE(a_region_with_no_usable_rectangle_is_rejected) {
    constraintMgr mgr;
    BOOST_TEST(mgr.addRegion("empty", {}) == constraintMgr::kNoRegion);
    // Zero area and a single corner are both unusable.
    BOOST_TEST(mgr.addRegion("degenerate", rect(5.0, 5.0, 5.0, 5.0)) == constraintMgr::kNoRegion);
    BOOST_TEST(mgr.addRegion("flat", {Point{0.0, 0.0}, Point{10.0, 0.0}}) ==
               constraintMgr::kNoRegion);
    BOOST_TEST(mgr.numRegions() == 0u);
}

BOOST_AUTO_TEST_CASE(containment_includes_the_boundary) {
    const constraintMgr mgr = singleFence();
    // The lower-left and upper-right corners are inside, not outside: a cell
    // snapped exactly to a fence edge is legal and must not be pushed out.
    BOOST_TEST(mgr.contains(0, 50.0, 50.0));
    BOOST_TEST(mgr.contains(0, 0.0, 0.0));
    BOOST_TEST(mgr.contains(0, 100.0, 100.0));
    BOOST_TEST(mgr.contains(0, 0.0, 100.0));
    BOOST_TEST(!mgr.contains(0, -0.001, 50.0));
    BOOST_TEST(!mgr.contains(0, 50.0, 100.001));
}

BOOST_AUTO_TEST_CASE(an_unconstrained_cell_is_always_legal) {
    // kNoRegion means "not fenced", so it must not report a violation for a
    // position that happens to be outside every fence.
    const constraintMgr mgr = singleFence();
    BOOST_TEST(mgr.contains(constraintMgr::kNoRegion, 1e9, 1e9));
    // An id that is in range of the manager but not of its region list is not
    // silently treated as unconstrained.
    BOOST_TEST(!mgr.contains(7, 1e9, 1e9));
}

BOOST_AUTO_TEST_CASE(a_region_is_the_union_of_its_rectangles_not_an_outline) {
    // Two rectangles with a gap between them. Treating the region as one
    // polygon would call the gap legal; the ISPD fences are genuinely
    // disconnected, so the gap has to be outside.
    constraintMgr mgr;
    mgr.addRegion("er0", {Point{0.0, 0.0}, Point{10.0, 10.0}, Point{20.0, 0.0}, Point{30.0, 10.0}});

    BOOST_TEST(mgr.contains(0, 5.0, 5.0));
    BOOST_TEST(mgr.contains(0, 25.0, 5.0));
    BOOST_TEST(!mgr.contains(0, 15.0, 5.0));

    const Region *r = mgr.region(0);
    BOOST_REQUIRE(r != nullptr);
    BOOST_TEST(r->rects.size() == 2u);
    // Areas sum; they do not overlap here so the sum is the union's area.
    BOOST_TEST(r->area == 200.0);
    // The bounding box spans the gap, which is why contains() still has to
    // fall through to the per-rectangle test.
    BOOST_TEST(r->minX == 0.0);
    BOOST_TEST(r->maxX == 30.0);
}

BOOST_AUTO_TEST_CASE(clamping_leaves_a_legal_position_alone) {
    const constraintMgr mgr = singleFence();
    double x = 42.0;
    double y = 42.0;
    mgr.clampToRegion(0, x, y);
    BOOST_TEST(x == 42.0);
    BOOST_TEST(y == 42.0);
}

BOOST_AUTO_TEST_CASE(clamping_pulls_a_position_inside) {
    const constraintMgr mgr = singleFence();
    double x = 500.0;
    double y = 20.0;
    mgr.clampToRegion(0, x, y);
    BOOST_TEST(x == 100.0);  // only x is out of range
    BOOST_TEST(y == 20.0);

    double u = -5.0;
    double v = -5.0;
    mgr.clampToRegion(0, u, v);
    BOOST_TEST(u == 0.0);
    BOOST_TEST(v == 0.0);
}

BOOST_AUTO_TEST_CASE(clamping_chooses_the_nearest_rectangle_of_a_split_region) {
    // The nearest rectangle is the one on the same side, not simply the first.
    constraintMgr mgr;
    mgr.addRegion("er0",
                  {Point{0.0, 0.0}, Point{10.0, 10.0}, Point{100.0, 0.0}, Point{110.0, 10.0}});

    double x = 99.0;
    double y = 5.0;
    mgr.clampToRegion(0, x, y);
    BOOST_TEST(x == 100.0);
    BOOST_TEST(y == 5.0);
    BOOST_TEST(mgr.contains(0, x, y));
}

BOOST_AUTO_TEST_CASE(clamping_an_unknown_region_is_a_no_op) {
    const constraintMgr mgr = singleFence();
    double x = 1e6;
    double y = 1e6;
    mgr.clampToRegion(constraintMgr::kNoRegion, x, y);
    BOOST_TEST(x == 1e6);
    BOOST_TEST(y == 1e6);
    mgr.clampToRegion(42, x, y);
    BOOST_TEST(x == 1e6);
    BOOST_TEST(y == 1e6);
}

BOOST_AUTO_TEST_CASE(push_out_escapes_a_fence_and_reports_the_move) {
    const constraintMgr mgr = singleFence();
    double x = 50.0;
    double y = 50.0;
    const bool moved = mgr.pushOutOfRegions(x, y, 0.0, 0.0, 100.0, 100.0);
    BOOST_TEST(moved);
    BOOST_TEST(!mgr.contains(0, x, y));
}

BOOST_AUTO_TEST_CASE(push_out_leaves_a_clear_position_untouched) {
    const constraintMgr mgr = singleFence();
    double x = 500.0;
    double y = 500.0;
    BOOST_TEST(!mgr.pushOutOfRegions(x, y, 0.0, 0.0, 1000.0, 1000.0));
    BOOST_TEST(x == 500.0);
    BOOST_TEST(y == 500.0);
}

BOOST_AUTO_TEST_CASE(push_out_escapes_abutting_fences_without_ping_ponging) {
    // The bug this guards against: stepping to the edge of the rectangle that
    // caught the point lands inside the neighbour, so the escape oscillates
    // between the two and never terminates. The exit has to be checked against
    // every fence, not just the host rectangle.
    constraintMgr mgr;
    mgr.addRegion("er0", rect(0.0, 0.0, 10.0, 10.0));
    mgr.addRegion("er1", rect(10.0, 0.0, 20.0, 10.0));

    double x = 5.0;
    double y = 5.0;
    const bool moved = mgr.pushOutOfRegions(x, y, 0.0, 0.0, 100.0, 100.0);
    BOOST_TEST(moved);
    // The contract is only that the point ends up outside *every* fence, which
    // is the property that keeps the escape from oscillating. Which side it
    // takes is the algorithm's choice: here the right-hand exit is blocked by
    // er1 and the left-hand one is off-die, so upward is the only legal way out.
    BOOST_TEST(!mgr.contains(0, x, y));
    BOOST_TEST(!mgr.contains(1, x, y));
    BOOST_TEST(x >= 0.0);
    BOOST_TEST(x <= 100.0);
    BOOST_TEST(y >= 0.0);
    BOOST_TEST(y <= 100.0);
}

BOOST_AUTO_TEST_CASE(push_out_prefers_an_escape_that_stays_on_the_die) {
    // A fence hugging the die edge: the nearest side is off-die, and taking it
    // would be undone by whatever clamps the cell back on-chip.
    constraintMgr mgr;
    mgr.addRegion("er0", rect(0.0, 0.0, 20.0, 100.0));

    double x = 10.0;
    double y = 50.0;
    mgr.pushOutOfRegions(x, y, 0.0, 0.0, 100.0, 100.0);
    BOOST_TEST(x > 20.0);
    BOOST_TEST(x <= 100.0);
    BOOST_TEST(y == 50.0);
}

BOOST_AUTO_TEST_CASE(push_out_with_no_fences_at_all_changes_nothing) {
    const constraintMgr mgr;
    double x = 5.0;
    double y = 5.0;
    BOOST_TEST(!mgr.pushOutOfRegions(x, y, 0.0, 0.0, 100.0, 100.0));
    BOOST_TEST(x == 5.0);
    BOOST_TEST(y == 5.0);
}

BOOST_AUTO_TEST_CASE(assign_by_prefix_stamps_matching_cells_only) {
    Graph graph;
    graph.addCell("eh0/a");
    graph.addCell("eh0/b");
    graph.addCell("eh1/a");
    graph.addCell("other");
    // A net vertex whose name also matches must not be stamped: only cells
    // carry a region.
    graph.addNet("eh0/net");

    constraintMgr mgr = singleFence();
    const std::size_t assigned = mgr.assignByPrefix(0, "eh0/", graph);
    BOOST_TEST(assigned == 2u);

    BOOST_TEST(graph.getCellId("eh0/a") != static_cast<std::size_t>(-1));
    BOOST_TEST(graph.getCell(graph.getCellId("eh0/a")).regionId == 0);
    BOOST_TEST(graph.getCell(graph.getCellId("eh0/b")).regionId == 0);
    BOOST_TEST(graph.getCell(graph.getCellId("eh1/a")).regionId == constraintMgr::kNoRegion);
    BOOST_TEST(graph.getCell(graph.getCellId("other")).regionId == constraintMgr::kNoRegion);
    // A net is not a cell and so cannot carry a region: the two are numbered
    // separately, and asking for the net as a cell is an error rather than a hit.
    BOOST_CHECK_THROW(graph.getCellId("eh0/net"), std::runtime_error);
    BOOST_TEST(graph.getNetId("eh0/net") != static_cast<std::size_t>(-1));

    const Region *r = mgr.region(0);
    BOOST_REQUIRE(r != nullptr);
    BOOST_TEST(r->cellCount == 2u);
}

BOOST_AUTO_TEST_CASE(the_first_matching_group_wins) {
    // A cell claimed by two overlapping groups must end up in exactly one, or
    // the two legalizers would each think they own it.
    Graph graph;
    graph.addCell("eh0/a");
    graph.addCell("eh0/b");

    constraintMgr mgr;
    mgr.addRegion("er0", rect(0.0, 0.0, 10.0, 10.0));
    mgr.addRegion("er1", rect(20.0, 0.0, 30.0, 10.0));

    BOOST_TEST(mgr.assignByPrefix(0, "eh0/", graph) == 2u);
    BOOST_TEST(mgr.assignByPrefix(1, "eh0/", graph) == 0u);
    BOOST_TEST(graph.getCell(graph.getCellId("eh0/a")).regionId == 0);
    BOOST_TEST(mgr.region(1)->cellCount == 0u);
}

BOOST_AUTO_TEST_CASE(assign_rejects_an_unusable_region_or_prefix) {
    Graph graph;
    graph.addCell("eh0/a");

    constraintMgr mgr = singleFence();
    BOOST_TEST(mgr.assignByPrefix(constraintMgr::kNoRegion, "eh0/", graph) == 0u);
    BOOST_TEST(mgr.assignByPrefix(9, "eh0/", graph) == 0u);  // out of range
    BOOST_TEST(mgr.assignByPrefix(0, "", graph) == 0u);      // empty prefix
    BOOST_TEST(graph.getCell(graph.getCellId("eh0/a")).regionId == constraintMgr::kNoRegion);
}

BOOST_AUTO_TEST_CASE(a_prefix_longer_than_the_name_cannot_match) {
    // compare() on a short string must not read past its end or match
    // everything.
    Graph graph;
    graph.addCell("a");

    constraintMgr mgr = singleFence();
    BOOST_TEST(mgr.assignByPrefix(0, "abcdef", graph) == 0u);
}

BOOST_AUTO_TEST_CASE(violations_are_counted_over_the_shorter_of_the_inputs) {
    constraintMgr mgr = singleFence();
    // Two positions, only the first of which is outside the fence.
    const std::vector<double> positions{500.0, 500.0, 50.0, 50.0};
    const std::vector<int> regions{0, 0};
    BOOST_TEST(mgr.countViolations(positions, regions) == 1u);

    // Unconstrained positions never count, whatever they are.
    const std::vector<int> free{-1, -1};
    BOOST_TEST(mgr.countViolations(positions, free) == 0u);
}

BOOST_AUTO_TEST_CASE(a_missing_region_id_list_truncates_the_count) {
    // The region id array is per movable; a short one means the caller has not
    // filled the solver buffers yet. Counting past its end would be a read of
    // stale memory, so the shorter length wins.
    constraintMgr mgr = singleFence();
    const std::vector<double> positions{500.0, 500.0, 500.0, 500.0};
    const std::vector<int> regions{0};
    BOOST_TEST(mgr.countViolations(positions, regions) == 1u);
}

BOOST_AUTO_TEST_CASE(a_region_with_many_rectangles_reports_its_full_area) {
    constraintMgr mgr;
    mgr.addRegion("er0", {Point{0.0, 0.0}, Point{10.0, 10.0}, Point{0.0, 20.0}, Point{10.0, 30.0},
                          Point{0.0, 40.0}, Point{10.0, 50.0}});
    const Region *r = mgr.region(0);
    BOOST_REQUIRE(r != nullptr);
    BOOST_TEST(r->rects.size() == 3u);
    BOOST_TEST(r->area == 300.0);
    BOOST_TEST(r->minY == 0.0);
    BOOST_TEST(r->maxY == 50.0);
    // The gap at y in (10, 20) is outside the union.
    BOOST_TEST(!mgr.contains(0, 5.0, 15.0));
    BOOST_TEST(mgr.contains(0, 5.0, 25.0));
}

BOOST_AUTO_TEST_CASE(regions_accessor_exposes_what_was_added) {
    constraintMgr mgr;
    mgr.addRegion("er0", rect(0.0, 0.0, 10.0, 10.0));
    mgr.addRegion("er1", rect(20.0, 0.0, 30.0, 10.0));
    BOOST_REQUIRE(mgr.regions().size() == 2u);
    BOOST_TEST(mgr.regions()[0].name == "er0");
    BOOST_TEST(mgr.regions()[1].name == "er1");
}
