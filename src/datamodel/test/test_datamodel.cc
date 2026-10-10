// @file test_datamodel.cc// Unit tests for ktDM and the placement graph


#define BOOST_TEST_MODULE ktplace_datamodel
#define BOOST_TEST_DYN_LINK
#include "datamodel/kt_dm.h"
#include "datamodel/kt_graph.h"

#include <boost/test/unit_test.hpp>
#include <ostream>
#include <stdexcept>

using namespace ktplace;

// Boost.Test prints the offending value when an assertion fails; scoped enums
// have no stream operator of their own, so provide one.
namespace ktplace {
std::ostream &operator<<(std::ostream &os, PinRole value) {
    return os << (value == PinRole::Driver ? "Driver" : "Receiver");
}
}  // namespace ktplace

// The database returns the id of everything it adds and marks it [[nodiscard]],
// so setup goes through these rather than discarding the result. A cell that
// silently failed to be added would otherwise leave the test asserting on a
// shorter netlist than the one it built, and passing.
std::size_t addCell(ktDM &db, const std::string &name, double x, double y,
                    bool isTerminal = false) {
    const std::size_t id = db.addCell(name, x, y, isTerminal);
    BOOST_TEST(id == db.getCellId(name));
    return id;
}

std::size_t addNet(ktDM &db, const std::string &name, double weight = 1.0) {
    const std::size_t id = db.addNet(name, weight);
    // Sequenced through a local on purpose. Written as one expression --
    // addNet(...) == getNetId(...) -- the two calls are unsequenced, and gcc
    // evaluates the right one first, so the lookup runs before the net exists and
    // throws "Vertex <name> not found". The test caught that.
    const std::size_t looked = db.getNetId(name);
    BOOST_TEST(id == looked);
    return id;
}

std::size_t addPin(ktDM &db, const std::string &cell, const std::string &net, double x, double y,
                   bool isInput) {
    const std::size_t before = db.getNumPins();
    const std::size_t id = db.addPin(cell, net, x, y, isInput);
    const std::size_t after = db.getNumPins();
    BOOST_TEST(id < after);
    BOOST_TEST(after == before + 1);
    return id;
}

std::size_t addSubrow(ktDM &db, std::size_t row, double x, double numSites) {
    const std::size_t before = db.getRows()[row].subrows.size();
    const std::size_t id = db.addSubrow(row, x, numSites);
    // The returned id indexes the row's subrows, not the rows, so it is checked
    // against the row it was added to rather than against getNumRows().
    BOOST_TEST(id == before);
    BOOST_TEST(db.getRows()[row].subrows.size() == before + 1);
    return id;
}

BOOST_AUTO_TEST_SUITE(ktDM_cells)


BOOST_AUTO_TEST_CASE(adds_cells_and_terminals) {
    ktDM db;
    BOOST_TEST(db.getNumCells() == 0);
    BOOST_TEST(db.getNumTerminals() == 0);

    addCell(db, "c0", 1.0, 2.0);
    addCell(db, "c1", 1.0, 2.0);
    addCell(db, "pad0", 1.0, 2.0, /*isTerminal=*/true);

    BOOST_TEST(db.getNumCells() == 3);
    BOOST_TEST(db.getNumTerminals() == 1);
    BOOST_TEST(db.hasCell("c0"));
    BOOST_TEST(!db.hasCell("nope"));
}

BOOST_AUTO_TEST_CASE(reports_geometry_of_a_cell) {
    ktDM db;
    const std::size_t id = addCell(db, "c0", 2.5, 4.0);
    const Graph &g = db.getGraph();
    const Vertex &v = g.getCell(id);
    BOOST_TEST(v.name == "c0");
    BOOST_TEST(v.width == 2.5);
    BOOST_TEST(v.height == 4.0);
    BOOST_TEST(!v.isTerminal);
}

BOOST_AUTO_TEST_CASE(rejects_unknown_cell) {
    ktDM db;
    // The database reports lookup failures as std::runtime_error.
    BOOST_CHECK_THROW((void)db.getCellId("missing"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(tracks_positions_and_fixed_flag) {
    ktDM db;
    addCell(db, "c0", 1.0, 1.0);
    db.setCellPosition("c0", 12.0, 34.0);
    const auto [x, y] = db.getCellPosition("c0");
    BOOST_TEST(x == 12.0);
    BOOST_TEST(y == 34.0);

    BOOST_TEST(!db.isCellFixed("c0"));
    db.setCellFixed("c0", true);
    BOOST_TEST(db.isCellFixed("c0"));
    db.setCellFixed("c0", false);
    BOOST_TEST(!db.isCellFixed("c0"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ktDM_nets)

BOOST_AUTO_TEST_CASE(two_pin_net_creates_two_edges) {
    ktDM db;
    addCell(db, "c0", 1.0, 1.0);
    addCell(db, "c1", 1.0, 1.0);
    addNet(db, "n0");
    addPin(db, "c0", "n0", 0.0, 0.0, true);
    addPin(db, "c1", "n0", 0.0, 0.0, false);

    BOOST_TEST(db.getNumNets() == 1);
    BOOST_TEST(db.getNumPins() == 2);
    BOOST_TEST(db.getGraph().getNumPins() == 2);
    BOOST_TEST(db.getNetPins(db.getNetId("n0")).size() == 2);
}

BOOST_AUTO_TEST_CASE(net_and_cell_pin_lists_are_consistent) {
    ktDM db;
    addCell(db, "c0", 1.0, 1.0);
    addNet(db, "n0");
    const std::size_t pin = addPin(db, "c0", "n0", 0.0, 0.0, true);
    BOOST_TEST(db.getCellPins(db.getCellId("c0")).size() == 1);
    BOOST_TEST(db.getNetPins(db.getNetId("n0")).front() == pin);
}

BOOST_AUTO_TEST_CASE(rejects_pins_on_unknown_vertices) {
    ktDM db;
    addCell(db, "c0", 1.0, 1.0);
    addNet(db, "n0");
    BOOST_CHECK_THROW(addPin(db, "ghost", "n0", 0.0, 0.0, true), std::runtime_error);
    BOOST_CHECK_THROW(addPin(db, "c0", "ghost", 0.0, 0.0, true), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(ktDM_rows_and_die)

BOOST_AUTO_TEST_CASE(counts_rows) {
    ktDM db;
    BOOST_TEST(db.getNumRows() == 0);
    // addRow takes (coordinate, height, sitewidth, sitespacing); the placeable
    // sites come from addSubrow, because a row with no subrow is not placeable.
    const std::size_t r0 = db.addRow(0.0, 10.0, 1.0, 1.0);
    const std::size_t r1 = db.addRow(10.0, 10.0, 1.0, 1.0);
    addSubrow(db, r0, 0.0, 100.0);
    addSubrow(db, r1, 0.0, 100.0);
    BOOST_TEST(db.getNumRows() == 2);

    // The row geometry the legalizer and detailed placer depend on.
    const std::vector<RowInfo> rows = db.getRows();
    BOOST_REQUIRE(rows.size() == 2);
    BOOST_TEST(rows[0].coordinate == 0.0);
    BOOST_TEST(rows[0].height == 10.0);
    BOOST_TEST(rows[0].pitch() == 1.0);
    BOOST_REQUIRE(rows[0].subrows.size() == 1);
    BOOST_TEST(rows[0].subrows[0].xlo() == 0.0);
    BOOST_TEST(rows[0].subrows[0].xhi(1.0) == 100.0);
    BOOST_TEST(rows[1].coordinate == 10.0);
}

BOOST_AUTO_TEST_CASE(round_trips_die_area) {
    ktDM db;
    db.setDieArea(0.0, 0.0, 100.0, 50.0);
    const auto [lo, hi] = db.getDieArea();
    BOOST_TEST(lo.first == 0.0);
    BOOST_TEST(lo.second == 0.0);
    BOOST_TEST(hi.first == 100.0);
    BOOST_TEST(hi.second == 50.0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(GraphOps)

BOOST_AUTO_TEST_CASE(a_pin_joins_a_cell_to_a_net) {
    Graph g;
    const std::size_t cell = g.addCell("c0");
    const std::size_t net = g.addNet("n0");
    const std::size_t pin = g.addPin(cell, net, PinRole::Driver, 1.5, 2.5);

    BOOST_TEST(g.getCellId("c0") == cell);
    BOOST_TEST(g.getNetId("n0") == net);
    BOOST_TEST(g.getPin(pin).cellId == cell);
    BOOST_TEST(g.getPin(pin).netId == net);
    BOOST_TEST(g.getPin(pin).role == PinRole::Driver);
    BOOST_TEST(g.getPin(pin).offsetX == 1.5);
    BOOST_TEST(g.getCellPins(cell).size() == 1);
    BOOST_TEST(g.getNetPins(net).size() == 1);
}

BOOST_AUTO_TEST_CASE(cell_and_net_ids_are_separate_spaces) {
    Graph g;
    g.addCell("c0");
    g.addNet("n0");
    // A cell and a net are numbered independently, so both can be 0 without
    // either being mistaken for the other.
    BOOST_TEST(g.getNumCells() == 1);
    BOOST_TEST(g.getNumNets() == 1);
    BOOST_TEST(g.getCellId("c0") == 0);
    BOOST_TEST(g.getNetId("n0") == 0);
}

BOOST_AUTO_TEST_CASE(clear_empties_the_graph) {
    Graph g;
    g.addCell("c0");
    g.addNet("n0");
    BOOST_TEST(g.getNumCells() == 1);
    BOOST_TEST(g.getNumNets() == 1);
    g.clear();
    BOOST_TEST(g.getNumCells() == 0);
    BOOST_TEST(g.getNumNets() == 0);
    BOOST_TEST(!g.hasCell("c0"));
    BOOST_TEST(!g.hasNet("n0"));
}

BOOST_AUTO_TEST_CASE(clear_resets_the_id_counters) {
    Graph g;
    g.addCell("c0");
    g.addNet("n0");
    g.clear();
    // Ids restart, otherwise a fresh design would inherit stale numbering.
    BOOST_TEST(g.addCell("again") == 0);
    BOOST_TEST(g.addNet("again") == 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_CASE(ktDM_clear_resets_everything) {
    ktDM db;
    addCell(db, "c0", 1.0, 1.0);
    addNet(db, "n0");
    addPin(db, "c0", "n0", 0.0, 0.0, true);
    const std::size_t row = db.addRow(0.0, 1.0, 1.0, 1.0);
    addSubrow(db, row, 0.0, 10.0);

    db.clear();

    BOOST_TEST(db.getNumCells() == 0);
    BOOST_TEST(db.getNumNets() == 0);
    BOOST_TEST(db.getNumPins() == 0);
    BOOST_TEST(db.getNumRows() == 0);
    BOOST_TEST(db.getRows().empty());
}
