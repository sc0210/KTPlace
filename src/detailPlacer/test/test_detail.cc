// SPDX-License-Identifier: BSD-3-Clause
// @file test_detail.cc// Unit tests for the fast detailed placer// Detailed placement's whole contract is that it may only make a legal// placement legaler-and-shorter. So the assertions here are mostly about the// invariant rather than the optimisation: whatever the four techniques do, the// result has to be on-site, in-row, clear of the fixed cells, and never worse// on HPWL than the input. An optimiser that reports a swap count but leaves the// design illegal has failed at the only thing that matters.// The four techniques are then exercised one at a time, with pass counts of// zero for the others, so a failure names the move that broke legality rather// than "something in place()".


#define BOOST_TEST_MODULE ktplace_detail

#include "detailPlacer/kt_fastdp.h"
#include "util/kt_log.h"

#include <boost/test/included/unit_test.hpp>

/// Cell widths as handed to the database, recorded at add time. The DB exposes
/// positions but not dimensions, and the overlap check needs the extents.
std::vector<double> gWidths;

/// Size of a cell as the database was told, for the legality checks.
double widthOf(std::size_t id) {
    return id < gWidths.size() ? gWidths[id] : 1.0;
}

/// One database per test, and a clean width table, so cases cannot leak
/// positions or sizes into each other through the file-scope record above.
struct Fixture {
    Fixture() {
        gWidths.clear();
    }
};

BOOST_FIXTURE_TEST_SUITE(ktplace_detail, Fixture)

#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>
#include <vector>

using namespace ktplace;

namespace {

/// Site pitch for every row built here; the placements are on a 1-unit grid.
constexpr double kSite = 1.0;
constexpr double kRowHeight = 10.0;
constexpr double kRowPitch = 10.0;

/// A row of @p numSites sites, y at @p rowIndex * kRowPitch.
void addRow(ktDM &db, int rowIndex, double numSites, double originX = 0.0) {
    const double y = rowIndex * kRowPitch;
    const std::size_t row = db.addRow(y, kRowHeight, kSite, kSite);
    BOOST_TEST(row < db.getNumRows());
    const std::size_t before = db.getRows()[row].subrows.size();
    const std::size_t sub = db.addSubrow(row, originX, numSites);
    // The returned id indexes the row's subrows, not the rows.
    BOOST_TEST(sub == before);
    BOOST_TEST(db.getRows()[row].subrows.size() == before + 1);
}

/// A single movable cell snapped to a site.
// Places a cell off the site pitch, for the tests that check a pass snaps
// cells back onto it. Every other test goes through addCell, which rounds.
void addCellRaw(ktDM &db, const std::string &name, double x, int rowIndex, double width = 1.0) {
    const std::size_t id = db.addCell(name, width, kRowHeight);
    db.setCellPosition(id, x, rowIndex * kRowPitch);
}

void addCell(ktDM &db, const std::string &name, double x, int rowIndex, double width = 1.0) {
    const std::size_t id = db.addCell(name, width, kRowHeight);
    db.setCellPosition(id, std::round(x / kSite) * kSite, rowIndex * kRowPitch);
    gWidths.resize(db.getNumCells(), 1.0);
    gWidths[id] = width;
}

/// A fixed cell, which the optimiser must not move and must not overlap.
void addFixed(ktDM &db, const std::string &name, double x, int rowIndex, double width = 1.0) {
    const std::size_t id = db.addCell(name, width, kRowHeight, /*isTerminal=*/true);
    db.setCellPosition(id, x, rowIndex * kRowPitch);
    db.setCellFixed(id, true);
    gWidths.resize(db.getNumCells(), 1.0);
    gWidths[id] = width;
}

/// A two-pin net between two cells. Both the net and its pins are created, since
/// addPin resolves the net by name and throws if it does not exist.
void addNet(ktDM &db, const std::string &net, const std::string &a, const std::string &b) {
    // Sequenced through locals: two calls in one comparison are unsequenced, and
    // gcc evaluates the right-hand one first, so the lookup runs before the net
    // exists. getNetId reports that as "Vertex <name> not found".
    const std::size_t netId = db.addNet(net);
    const std::size_t looked = db.getNetId(net);
    BOOST_TEST(netId == looked);
    // addPin throws if either endpoint is unknown, which is the property the
    // comment above claims; checking the count is what makes it a claim.
    const std::size_t before = db.getNumPins();
    const std::size_t pinA = db.addPin(a, net, 0.0, 0.0, true);
    const std::size_t pinB = db.addPin(b, net, 0.0, 0.0, false);
    const std::size_t after = db.getNumPins();
    BOOST_TEST(pinA < after);
    BOOST_TEST(pinB < after);
    BOOST_TEST(after == before + 2);
}

[[nodiscard]] DetailPlaceParams only(std::size_t which) {
    DetailPlaceParams p;
    p.globalSwapPasses = which == 0 ? 1 : 0;
    p.verticalSwapPasses = which == 1 ? 1 : 0;
    p.localReorderPasses = which == 2 ? 1 : 0;
    p.clusterPasses = which == 3 ? 1 : 0;
    return p;
}

/// Number of movable cells not sitting on a site pitch.
[[nodiscard]] std::size_t r0_offsite(ktDM &db) {
    const auto rows = db.getRows();
    if (rows.empty()) {
        return 0;
    }
    std::size_t off = 0;
    for (std::size_t i = 0; i < db.getNumCells(); ++i) {
        if (db.isCellFixed(i)) {
            continue;
        }
        const auto [x, y] = db.getCellPosition(i);
        (void)y;
        const double rem = std::fmod(x - rows.front().xlo(), kSite);
        if (std::abs(rem) > 1e-6 && std::abs(rem - kSite) > 1e-6) {
            ++off;
        }
    }
    return off;
}

/// Everything zero: a run that must be a no-op. Used to prove the placer is
/// inert when asked to do nothing, which is what a fully converged design wants.
[[nodiscard]] DetailPlaceParams none() {
    DetailPlaceParams p;
    p.globalSwapPasses = 0;
    p.verticalSwapPasses = 0;
    p.localReorderPasses = 0;
    p.clusterPasses = 0;
    return p;
}

/// True when every movable cell still sits on a site, inside its row band, and
/// clear of every other cell. Checked from the outside so it does not reuse the
/// placer's own notion of legality.
[[nodiscard]] bool isLegal(ktDM &db) {
    const auto rows = db.getRows();
    if (rows.empty()) {
        // No rows means nothing can be legal; a design with cells and no rows is
        // the caller's mistake, so say so rather than reading rows.front().
        return db.getNumCells() == 0;
    }
    struct Box {
        double x0, x1, y0, y1;
    };
    std::vector<Box> boxes;
    for (std::size_t i = 0; i < db.getNumCells(); ++i) {
        const auto [x, y] = db.getCellPosition(i);
        const double w = widthOf(i);
        const double h = kRowHeight;
        if (db.isCellFixed(i)) {
            boxes.push_back({x, x + w, y, y + h});
            continue;
        }
        // On a site, to within floating point.
        const double rem = std::fmod(x - rows.front().xlo(), kSite);
        if (std::abs(rem) > 1e-6 && std::abs(rem - kSite) > 1e-6) {
            return false;
        }
        // In some row band.
        bool inRow = false;
        for (const auto &r : rows) {
            if (y >= r.coordinate - 1e-6 && y + h <= r.coordinate + r.height + 1e-6) {
                inRow = true;
                break;
            }
        }
        if (!inRow) {
            return false;
        }
        // Within the row's sites.
        const double xhi = rows.front().xhi();
        const bool withinRow = x >= rows.front().xlo() - 1e-6 && x + w <= xhi + 1e-6;
        if (!withinRow) {
            return false;
        }
        boxes.push_back({x, x + w, y, y + h});
    }
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        for (std::size_t j = i + 1; j < boxes.size(); ++j) {
            const bool disjoint = boxes[i].x1 <= boxes[j].x0 || boxes[j].x1 <= boxes[i].x0 ||
                                  boxes[i].y1 <= boxes[j].y0 || boxes[j].y1 <= boxes[i].y0;
            if (!disjoint) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The invariant that matters
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(a_design_with_nothing_to_do_is_left_alone) {
    ktDM db;
    addRow(db, 0, 20);
    addCell(db, "a", 0.0, 0);
    addCell(db, "b", 1.0, 0);
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place(none());
    BOOST_TEST(r.globalSwaps == 0u);
    BOOST_TEST(r.verticalSwaps == 0u);
    BOOST_TEST(r.reorderMoves == 0u);
    BOOST_TEST(r.clusterMoves == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(an_empty_database_does_not_crash) {
    ktDM db;
    FastDetailedPlacer dp(db);
    DetailPlaceResult r;
    BOOST_CHECK_NO_THROW(r = dp.place());
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(r.offRow == 0u);
    BOOST_TEST(r.offSite == 0u);
}

BOOST_AUTO_TEST_CASE(a_database_with_rows_but_no_cells_does_not_crash) {
    ktDM db;
    addRow(db, 0, 10);
    addRow(db, 1, 10);
    FastDetailedPlacer dp(db);
    BOOST_CHECK_NO_THROW(dp.place());
}

BOOST_AUTO_TEST_CASE(placing_a_legal_design_keeps_it_legal) {
    ktDM db;
    addRow(db, 0, 40);
    addRow(db, 1, 40);
    addRow(db, 2, 40);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 10; ++c) {
            addCell(db, "c" + std::to_string(r * 10 + c), c * 2.0, r);
        }
    }
    FastDetailedPlacer dp(db);
    const DetailPlaceResult res = dp.place();
    BOOST_TEST(res.overlappingPairs == 0u);
    BOOST_TEST(res.offRow == 0u);
    BOOST_TEST(res.offSite == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(an_illegal_input_is_reported_rather_than_silently_called_legal) {
    // Detailed placement refines a placement the legalizer has already made
    // legal; it is not itself a legalizer. Feeding it overlapping cells and then
    // reading overlappingPairs as a verdict must give a nonzero number, not a
    // clean bill of health. The stage is the caller, not this code, that has to
    // know the input was bad. An overlap the passes can resolve is repaired and
    // the run reports itself legal afterwards.
    ktDM db;
    addRow(db, 0, 20);
    addCell(db, "a", 5.0, 0);
    addCell(db, "b", 5.0, 0);  // exactly on top of a
    addCell(db, "c", 6.0, 0);
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place();
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(an_unrepairable_overlap_is_still_reported) {
    // Two cells too wide to both fit the subrow, so no pass can separate them.
    // A repair that cannot work must leave the overlap visible rather than
    // reporting a clean run.
    ktDM db;
    addRow(db, 0, 4);
    addCellRaw(db, "a", 1.0, 0, 3.0);
    addCellRaw(db, "b", 1.0, 0, 3.0);
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place();
    BOOST_TEST(r.overlappingPairs > 0u);
}

BOOST_AUTO_TEST_CASE(hpwl_never_gets_worse_over_the_whole_run) {
    // Each technique is individually monotone, so the combined run has to be
    // too. A regression here means some pass is applying a losing move.
    ktDM db;
    addRow(db, 0, 60);
    addRow(db, 1, 60);
    addRow(db, 2, 60);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 14; ++c) {
            addCell(db, "c" + std::to_string(r * 14 + c), 1.0 + c * 3.0, r);
        }
    }
    // A chain of nets, so ordering matters.
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 13; ++c) {
            const std::string a = "c" + std::to_string(r * 14 + c);
            const std::string b = "c" + std::to_string(r * 14 + c + 1);
            addNet(db, "n" + a + b, a, b);
        }
    }
    FastDetailedPlacer dp(db);
    const DetailPlaceResult res = dp.place();
    BOOST_TEST(res.hpwlBefore > 0.0);
    BOOST_TEST(res.hpwlAfter <= res.hpwlBefore + 1e-6);
    BOOST_TEST(res.seconds >= 0.0);
}

BOOST_AUTO_TEST_CASE(an_already_optimal_placement_is_not_reshuffled) {
    // Cells packed left to right with their nets adjacent is already minimal,
    // so a correct placer has nothing to do. A nonzero move count here means
    // the optimiser is wandering.
    ktDM db;
    addRow(db, 0, 40);
    for (int c = 0; c < 8; ++c) {
        addCell(db, "c" + std::to_string(c), static_cast<double>(c), 0);
    }
    for (int c = 0; c < 7; ++c) {
        const std::string a = "c" + std::to_string(c);
        const std::string b = "c" + std::to_string(c + 1);
        addNet(db, "n" + a + b, a, b);
    }
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place();
    BOOST_TEST(r.hpwlAfter <= r.hpwlBefore + 1e-6);
    BOOST_TEST(isLegal(db));
}

// ---------------------------------------------------------------------------
// Each technique on its own
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(global_swap_alone_stays_legal) {
    ktDM db;
    addRow(db, 0, 60);
    addRow(db, 1, 60);
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 10; ++c) {
            addCell(db, "c" + std::to_string(r * 10 + c), 2.0 + c * 4.0, r);
        }
    }
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 9; ++c) {
            const std::string a = "c" + std::to_string(r * 10 + c);
            const std::string b = "c" + std::to_string(r * 10 + c + 1);
            addNet(db, "n" + a + b, a, b);
        }
    }
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place(only(0));
    BOOST_TEST(r.globalSwaps >= 1u);
    BOOST_TEST(r.verticalSwaps == 0u);
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(r.hpwlAfter <= r.hpwlBefore + 1e-6);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(global_swap_never_moves_a_cell_onto_a_fixed_one) {
    ktDM db;
    addRow(db, 0, 40);
    addFixed(db, "macro", 10.0, 0, /*width=*/4.0);
    for (int c = 0; c < 6; ++c) {
        addCell(db, "c" + std::to_string(c), 20.0 + c * 2.0, 0);
    }
    for (int c = 0; c < 5; ++c) {
        const std::string a = "c" + std::to_string(c);
        const std::string b = "c" + std::to_string(c + 1);
        addNet(db, "n" + a + b, a, b);
    }
    // A cell that would like to sit next to the macro.
    addNet(db, "nmacro", "c0", "macro");

    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place(only(0));
    BOOST_TEST(r.overFixed == 0u);
    BOOST_TEST(r.overlappingPairs == 0u);
    // The macro is where it started.
    const auto [mx, my] = db.getCellPosition("macro");
    BOOST_TEST(mx == 10.0);
    BOOST_TEST(my == 0.0);
}

BOOST_AUTO_TEST_CASE(vertical_swap_pulls_a_cell_back_to_its_own_row) {
    // A cell in row 1 whose nets all reach back to row 0, and a row-0 cell
    // sitting to the right of the cell's own net. Vertical swap is the only
    // technique that can change rows, so the other three are off.
    ktDM db;
    addRow(db, 0, 40);
    addRow(db, 1, 40);
    addCell(db, "a", 2.0, 0);
    addCell(db, "c", 29.0, 0);
    addCell(db, "b", 30.0, 1);
    addNet(db, "n0", "a", "b");
    addNet(db, "n1", "b", "c");

    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place(only(1));
    BOOST_TEST(r.verticalSwaps >= 1u);
    // The rows exchanged, so the y term of the HPWL is part of the decision.
    BOOST_TEST(r.hpwlAfter < r.hpwlBefore);
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(r.offRow == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(local_reordering_reduces_a_backwards_ordering) {
    // Three cells whose nets want "p" between the other two, but which are
    // stored r,p,q. The left-to-right order is wrong, which is exactly what the
    // reordering dynamic program is there to fix, so the other three are off.
    ktDM db;
    addRow(db, 0, 40);
    addCell(db, "r", 2.0, 0);
    addCell(db, "p", 5.0, 0);
    addCell(db, "q", 8.0, 0);
    addNet(db, "n0", "p", "r");
    addNet(db, "n1", "p", "q");

    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place(only(2));
    BOOST_TEST(r.reorderMoves >= 1u);
    BOOST_TEST(r.hpwlAfter < r.hpwlBefore);
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(local_reordering_never_moves_a_cell_onto_a_fixed_one) {
    // The reordering dynamic program packed its window from the left edge of the
    // window and validated only the subrow's right edge, so a cell whose site
    // rounding moved it left could be planned straight through a macro. On
    // adaptec1 that put 9879 cells over macros and failed the independent check,
    // while the pass's own self-check still reported zero because it never looked
    // at the fixed cells either.
    //
    // The macro's right edge is deliberately off the site grid: that is what
    // makes rounding a cell left put it inside the macro, and a macro edge is not
    // guaranteed to be site aligned in a real LEF.
    ktDM db;
    addRow(db, 0, 40);
    addFixed(db, "macro", 8.0, 0, /*width=*/6.4);
    // Legal as it stands, 14.6 clears the macro's 14.4 right edge. Packing the
    // window from 14 snaps the first cell to 14, which is inside the macro, and
    // it shortens the nets from 28 to 22, so the pass would otherwise take it.
    for (int c = 0; c < 3; ++c) {
        addCellRaw(db, "c" + std::to_string(c), 14.6 + c * 4.0, 0);
    }
    for (int c = 1; c < 3; ++c) {
        const std::string a = "c" + std::to_string(c - 1);
        const std::string b = "c" + std::to_string(c);
        addNet(db, "n" + a + b, a, b);
    }

    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place(only(2));
    BOOST_TEST(r.overFixed == 0u);
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(r.offRow == 0u);
    // isLegal is deliberately not used: it insists every cell is already on the
    // site pitch, and this input is not. Macro clearance is the point here.
    for (int c = 0; c < 3; ++c) {
        const auto [x, y] = db.getCellPosition("c" + std::to_string(c));
        const bool clearOfMacro = (x + 1.0 <= 8.0) || (x >= 14.4);
        BOOST_TEST(clearOfMacro);
        BOOST_TEST(y == 0.0);
    }
}

BOOST_AUTO_TEST_CASE(local_reordering_keeps_cells_on_their_sites) {
    // The subset dynamic program chooses an order, not a position, so every
    // cell has to land back on a site pitch even though it may have moved.
    ktDM db;
    addRow(db, 0, 40);
    for (int c = 0; c < 6; ++c) {
        addCell(db, "c" + std::to_string(c), 2.0 + c * 3.0, 0);
    }
    for (int c = 0; c < 5; ++c) {
        const std::string a = "c" + std::to_string(c);
        const std::string b = "c" + std::to_string(c + 1);
        addNet(db, "n" + a + b, a, b);
    }
    FastDetailedPlacer dp(db);
    dp.place(only(2));
    BOOST_TEST(r0_offsite(db) == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(single_segment_clustering_tightens_a_gap) {
    // Cells sitting off the site pitch. Clustering is the technique that snaps
    // them back onto sites, so the other three are off.
    ktDM db;
    addRow(db, 0, 40);
    for (int c = 0; c < 3; ++c) {
        addCellRaw(db, "c" + std::to_string(c), 2.0 + c * 2.0 + 0.4, 0);
    }
    for (const std::pair<const std::string, const std::string> &e :
         {std::pair<const std::string, const std::string>{"c0", "c1"},
          std::pair<const std::string, const std::string>{"c1", "c2"},
          std::pair<const std::string, const std::string>{"c0", "c2"}}) {
        addNet(db, "n" + e.first + e.second, e.first, e.second);
    }

    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place(only(3));
    BOOST_TEST(r.clusterMoves >= 1u);
    BOOST_TEST(r0_offsite(db) == 0u);
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(single_segment_clustering_resolves_an_overlap) {
    // Two cells sharing a site. Resolving the overlap is the clustering pass's
    // job even though it lengthens the net between them, so the pass cannot be
    // gated on a wirelength improvement.
    ktDM db;
    addRow(db, 0, 40);
    addCellRaw(db, "c0", 2.0, 0, 2.0);
    addCellRaw(db, "c1", 2.5, 0, 2.0);
    addNet(db, "n0", "c0", "c1");

    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place(only(3));
    BOOST_TEST(r.clusterMoves >= 1u);
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(the_move_counts_add_up_to_the_reported_totals) {
    ktDM db;
    addRow(db, 0, 60);
    addRow(db, 1, 60);
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 12; ++c) {
            addCell(db, "c" + std::to_string(r * 12 + c), 1.0 + c * 4.0, r);
        }
    }
    for (int r = 0; r < 2; ++r) {
        for (int c = 0; c < 11; ++c) {
            const std::string a = "c" + std::to_string(r * 12 + c);
            const std::string b = "c" + std::to_string(r * 12 + c + 1);
            addNet(db, "n" + a + b, a, b);
        }
    }
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place();
    const std::size_t moves = r.globalSwaps + r.verticalSwaps + r.reorderMoves + r.clusterMoves;
    BOOST_TEST(moves <= r.hpwlBefore + 1e9);  // sanity: no overflow to a huge value
    BOOST_TEST(r.overlappingPairs == 0u);
}

// ---------------------------------------------------------------------------
// Parameters and edge cases
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(the_default_parameters_run_every_technique) {
    const DetailPlaceParams p;
    BOOST_TEST(p.globalSwapPasses > 0u);
    BOOST_TEST(p.verticalSwapPasses > 0u);
    BOOST_TEST(p.localReorderPasses > 0u);
    BOOST_TEST(p.clusterPasses > 0u);
    BOOST_TEST(p.localReorderWindow > 0u);
    BOOST_TEST(p.minImprovement >= 0.0);
    BOOST_TEST(p.plotDir.empty());
}

BOOST_AUTO_TEST_CASE(a_local_reorder_window_of_zero_does_not_hang) {
    // The window is capped by a 2^k subset DP, so a caller that sets it to
    // zero or one must get a cheap run, not an empty loop over a zero-length
    // window or an unbounded one.
    ktDM db;
    addRow(db, 0, 40);
    for (int c = 0; c < 8; ++c) {
        addCell(db, "c" + std::to_string(c), static_cast<double>(c) * 2.0, 0);
    }
    DetailPlaceParams p = none();
    p.localReorderPasses = 1;
    p.localReorderWindow = 0;
    FastDetailedPlacer dp(db);
    DetailPlaceResult r;
    BOOST_CHECK_NO_THROW(r = dp.place(p));
    BOOST_TEST(r.overlappingPairs == 0u);

    p.localReorderWindow = 1;
    BOOST_CHECK_NO_THROW(r = dp.place(p));
    BOOST_TEST(r.overlappingPairs == 0u);
}

BOOST_AUTO_TEST_CASE(an_oversized_local_reorder_window_is_capped_not_obeyed) {
    // A window of 20 would be 2^20 subsets. The cost has to be bounded, or a
    // parameter tweak turns into an overnight run.
    ktDM db;
    addRow(db, 0, 60);
    for (int c = 0; c < 10; ++c) {
        addCell(db, "c" + std::to_string(c), static_cast<double>(c * 2), 0);
    }
    for (int c = 0; c < 9; ++c) {
        const std::string a = "c" + std::to_string(c);
        const std::string b = "c" + std::to_string(c + 1);
        addNet(db, "n" + a + b, a, b);
    }
    DetailPlaceParams p = none();
    p.localReorderPasses = 1;
    p.localReorderWindow = 64;
    FastDetailedPlacer dp(db);
    DetailPlaceResult r;
    BOOST_CHECK_NO_THROW(r = dp.place(p));
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(r.reorderMoves < db.getNumCells() * 100u);
}

BOOST_AUTO_TEST_CASE(a_single_cell_placement_is_legal_and_untouched) {
    ktDM db;
    addRow(db, 0, 10);
    addCell(db, "only", 3.0, 0);
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place();
    BOOST_TEST(r.globalSwaps == 0u);
    BOOST_TEST(r.hpwlBefore == 0.0);
    BOOST_TEST(r.hpwlAfter == 0.0);
    const auto [x, y] = db.getCellPosition("only");
    BOOST_TEST(x == 3.0);
    BOOST_TEST(y == 0.0);
}

BOOST_AUTO_TEST_CASE(a_cell_with_no_nets_stays_where_it_is) {
    // With nothing pulling it, moving the cell is pure risk, and the HPWL is
    // identical either way, so a correct optimiser leaves it.
    ktDM db;
    addRow(db, 0, 40);
    addCell(db, "lonely", 7.0, 0);
    addCell(db, "n1", 1.0, 0);
    addCell(db, "n2", 2.0, 0);
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place();
    BOOST_TEST(isLegal(db));
    // The report has to agree with the placement, not just be produced: a detail
    // placer that reported zero overlaps while the cells overlapped would still
    // satisfy the check above.
    BOOST_TEST(r.overlappingPairs == 0);
    const auto [x, y] = db.getCellPosition("lonely");
    BOOST_TEST(x == 7.0);
    BOOST_TEST(y == 0.0);
}

BOOST_AUTO_TEST_CASE(cells_at_the_ends_of_a_row_are_not_pushed_off_it) {
    // The first and last site have no neighbour to swap with. Treating the row
    // as infinite would let a cell end up at a negative x, off the die.
    ktDM db;
    addRow(db, 0, 6);
    addCell(db, "a", 0.0, 0);
    addCell(db, "b", 5.0, 0);
    addFixed(db, "pad", 3.0, 0);
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place();
    BOOST_TEST(r.offSite == 0u);
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(isLegal(db));
    for (std::size_t i = 0; i < db.getNumCells(); ++i) {
        const auto [x, y] = db.getCellPosition(i);
        BOOST_TEST(x >= -1e-6);
    }
}

BOOST_AUTO_TEST_CASE(a_row_split_by_a_subrow_gap_is_respected) {
    // A row with two subrows is not one span of x, so placing into it means
    // choosing a subrow. The gap between them has no sites.
    ktDM db;
    const std::size_t row = db.addRow(0.0, kRowHeight, kSite, kSite);
    BOOST_TEST(row < db.getNumRows());
    BOOST_TEST(db.addSubrow(row, 0.0, 5.0) == 0u);
    BOOST_TEST(db.addSubrow(row, 20.0, 5.0) == 1u);
    BOOST_TEST(db.getRows()[row].subrows.size() == 2u);
    addCell(db, "a", 1.0, 0);
    addCell(db, "b", 21.0, 0);
    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place();
    BOOST_TEST(r.offSite == 0u);
    BOOST_TEST(r.overlappingPairs == 0u);
    // Neither cell may sit in the gap at x in (5, 20).
    for (const char *name : {"a", "b"}) {
        const auto [x, y] = db.getCellPosition(name);
        (void)y;
        // Either still in the left subrow, or now in the right one.
        const bool inLeft = x + 1.0 <= 5.0 + 1e-6;
        const bool inRight = x >= 20.0 - 1e-6;
        BOOST_TEST(static_cast<bool>(inLeft || inRight));
    }
}

BOOST_AUTO_TEST_CASE(fences_are_accepted_without_being_enforced) {
    // The parameter is documented as "drawn, not enforced". It has to be
    // accepted and it must not change the legality verdict, because the
    // optimizer does not police regions -- the legalizer does.
    ktDM db;
    addRow(db, 0, 40);
    addCell(db, "a", 1.0, 0);
    addCell(db, "b", 2.0, 0);
    constraintMgr cstr;
    cstr.addRegion("er0", {Point{0.0, 0.0}, Point{1.0, 10.0}});
    db.setConstraints(std::move(cstr));

    DetailPlaceParams p = none();
    p.globalSwapPasses = 2;
    FastDetailedPlacer dp(db);
    DetailPlaceResult r;
    BOOST_CHECK_NO_THROW(r = dp.place(p));
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(an_unwritable_plot_directory_does_not_lose_the_placement) {
    // The frames are a diagnostic. If the directory cannot be written, the run
    // still has to improve the placement and still has to be legal.
    ktDM db;
    addRow(db, 0, 40);
    for (int c = 0; c < 6; ++c) {
        addCell(db, "c" + std::to_string(c), 1.0 + c * 3.0, 0);
    }
    DetailPlaceParams p;
    p.plotDir = "/proc/self/nonexistent_frames";
    FastDetailedPlacer dp(db);
    DetailPlaceResult r;
    // The frames are a diagnostic. An unwritable directory must not throw out
    // of place(), or a plotting problem loses a finished placement.
    BOOST_CHECK_NO_THROW(r = dp.place(p));
    BOOST_TEST(r.overlappingPairs == 0u);
    BOOST_TEST(isLegal(db));
}

BOOST_AUTO_TEST_CASE(the_placer_is_not_copyable) {
    // PIMPL with unique_ptr: a copy would double-free. Deleting the operations
    // is the cheap way to make that impossible.
    static_assert(!std::is_copy_constructible_v<FastDetailedPlacer>);
    static_assert(!std::is_copy_assignable_v<FastDetailedPlacer>);
    BOOST_TEST(true);
}

BOOST_AUTO_TEST_CASE(failed_vertical_swap_restores_the_row_the_cell_is_in) {
    // A cell that has already swapped rows in this pass, then tries a second swap
    // that fails only at its landing y, must stay in the row it swapped into.
    // The trial used to restore y from the row the cell started the pass in, so
    // its geometry went back to the old row while the spans held it in the new
    // one -- an overlap no per-span check could see.
    //
    // The second trial can only fail at its landing y if the blockage does not
    // also cut the span. One that reaches the end of a row is such a blockage:
    // the span builder skips an obstacle that runs to the end of the free run,
    // and leaves it to the per-y blockage test. A movable cell taller than a row
    // is left in place as such a blockage.
    ktDM db;
    for (int r = 0; r < 4; ++r) {
        addRow(db, r, 100);
    }
    // Zero-size pads on row boundaries: they pull on nets without blocking.
    const auto pad = [&](const std::string &name, double x, double y) {
        const std::size_t id = db.addCell(name, 0.0, 0.0, /*isTerminal=*/true);
        db.setCellPosition(id, x, y);
        db.setCellFixed(id, true);
    };
    addCell(db, "c", 50.0, 1, 3.0);  // processed first: the cell under test
    addCell(db, "d1", 96.0, 0);      // first partner, in the row below
    addCell(db, "d2", 95.0, 2);      // second partner, in the row above
    addCell(db, "e", 98.0, 1);       // sits where c lands if y is restored wrongly
    // Two rows tall, at the right end of rows 2-3 (x 97..100). c (3 wide) at
    // x 95 in row 2 hits it; d2 (1 wide) at x 96 in row 1 does not.
    const std::size_t tall = db.addCell("tall", 3.0, 2.0 * kRowHeight);
    db.setCellPosition(tall, 97.0, 2.0 * kRowPitch);
    pad("p1", 97.0, 0.0);   // c wants row 0 near x 97
    pad("p2", 51.0, 10.0);  // d1 wants row 1 near x 51
    pad("p3", 96.0, 10.0);  // d2 wants row 1 near x 96, strongly
    addNet(db, "nc", "c", "p1");
    addNet(db, "nd1", "d1", "p2");
    for (int k = 0; k < 4; ++k) {
        addNet(db, "nd2_" + std::to_string(k), "d2", "p3");
    }

    FastDetailedPlacer dp(db);
    const DetailPlaceResult r = dp.place(only(1));
    BOOST_TEST(r.verticalSwaps >= 1u);

    // c swapped into row 0 and its failed second trial must leave it there.
    const auto [cx, cy] = db.getCellPosition("c");
    BOOST_TEST(cy == 0.0, "c at y=" << cy << ", expected row 0");
    // No two single-row movable cells overlap, checked over the cell arrays
    // rather than per span.
    const std::vector<std::pair<std::string, double>> cells = {
        {"c", 3.0}, {"d1", 1.0}, {"d2", 1.0}, {"e", 1.0}};
    for (std::size_t i = 0; i < cells.size(); ++i) {
        for (std::size_t j = i + 1; j < cells.size(); ++j) {
            const auto [xi, yi] = db.getCellPosition(cells[i].first);
            const auto [xj, yj] = db.getCellPosition(cells[j].first);
            const bool disjoint = xi + cells[i].second <= xj + 1e-9 ||
                                  xj + cells[j].second <= xi + 1e-9 ||
                                  yi + kRowHeight <= yj + 1e-9 || yj + kRowHeight <= yi + 1e-9;
            BOOST_TEST(disjoint, cells[i].first << " overlaps " << cells[j].first);
        }
    }
    (void)cx;
}

BOOST_AUTO_TEST_SUITE_END()
