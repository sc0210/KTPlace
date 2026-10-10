// @file test_datamodel.cc// Unit tests for PlacementDB and the placement graph


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
std::ostream &operator<<(std::ostream &os, VertexType value) {
    return os << (value == VertexType::Cell ? "Cell" : "Net");
}

std::ostream &operator<<(std::ostream &os, PinDirection value) {
    return os << (value == PinDirection::Input ? "Input" : "Output");
}
}  // namespace ktplace

BOOST_AUTO_TEST_SUITE(PlacementDB_cells)

BOOST_AUTO_TEST_CASE(adds_cells_and_terminals) {
    PlacementDB db;
    BOOST_TEST(db.getNumCells() == 0);
    BOOST_TEST(db.getNumTerminals() == 0);

    (void)db.addCell("c0", 1.0, 2.0);
    (void)db.addCell("c1", 1.0, 2.0);
    (void)db.addCell("pad0", 1.0, 2.0, /*isTerminal=*/true);

    BOOST_TEST(db.getNumCells() == 3);
    BOOST_TEST(db.getNumTerminals() == 1);
    BOOST_TEST(db.hasCell("c0"));
    BOOST_TEST(!db.hasCell("nope"));
}

BOOST_AUTO_TEST_CASE(reports_geometry_of_a_cell) {
    PlacementDB db;
    const std::size_t id = db.addCell("c0", 2.5, 4.0);
    const Graph &g = db.getGraph();
    const Vertex &v = g.getVertex(id);
    BOOST_TEST(v.name == "c0");
    BOOST_TEST(v.width == 2.5);
    BOOST_TEST(v.height == 4.0);
    BOOST_TEST(v.type == VertexType::Cell);
    BOOST_TEST(!v.isTerminal);
}

BOOST_AUTO_TEST_CASE(rejects_unknown_cell) {
    PlacementDB db;
    // The database reports lookup failures as std::runtime_error.
    BOOST_CHECK_THROW((void)db.getCellId("missing"), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(tracks_positions_and_fixed_flag) {
    PlacementDB db;
    (void)db.addCell("c0", 1.0, 1.0);
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

BOOST_AUTO_TEST_SUITE(PlacementDB_nets)

BOOST_AUTO_TEST_CASE(two_pin_net_creates_two_edges) {
    PlacementDB db;
    (void)db.addCell("c0", 1.0, 1.0);
    (void)db.addCell("c1", 1.0, 1.0);
    (void)db.addNet("n0");
    (void)db.addPin("c0", "n0", 0.0, 0.0, true);
    (void)db.addPin("c1", "n0", 0.0, 0.0, false);

    BOOST_TEST(db.getNumNets() == 1);
    BOOST_TEST(db.getNumPins() == 2);
    BOOST_TEST(db.getGraph().getNumEdges() == 2);
    BOOST_TEST(db.getNetPins(db.getNetId("n0")).size() == 2);
}

BOOST_AUTO_TEST_CASE(net_and_cell_pin_lists_are_consistent) {
    PlacementDB db;
    (void)db.addCell("c0", 1.0, 1.0);
    (void)db.addNet("n0");
    const std::size_t pin = db.addPin("c0", "n0", 0.0, 0.0, true);
    BOOST_TEST(db.getCellPins(db.getCellId("c0")).size() == 1);
    BOOST_TEST(db.getNetPins(db.getNetId("n0")).front() == pin);
}

BOOST_AUTO_TEST_CASE(rejects_pins_on_unknown_vertices) {
    PlacementDB db;
    (void)db.addCell("c0", 1.0, 1.0);
    (void)db.addNet("n0");
    BOOST_CHECK_THROW((void)db.addPin("ghost", "n0", 0.0, 0.0, true), std::runtime_error);
    BOOST_CHECK_THROW((void)db.addPin("c0", "ghost", 0.0, 0.0, true), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(PlacementDB_rows_and_die)

BOOST_AUTO_TEST_CASE(counts_rows) {
    PlacementDB db;
    BOOST_TEST(db.getNumRows() == 0);
    // addRow takes (coordinate, height, sitewidth, sitespacing); the placeable
    // sites come from addSubrow, because a row with no subrow is not placeable.
    const std::size_t r0 = db.addRow(0.0, 10.0, 1.0, 1.0);
    const std::size_t r1 = db.addRow(10.0, 10.0, 1.0, 1.0);
    (void)db.addSubrow(r0, 0.0, 100.0);
    (void)db.addSubrow(r1, 0.0, 100.0);
    BOOST_TEST(db.getNumRows() == 2);

    // The row geometry the legalizer and detailed placer depend on.
    const std::vector<PlacementDB::RowInfo> rows = db.getRows();
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
    PlacementDB db;
    db.setDieArea(0.0, 0.0, 100.0, 50.0);
    const auto [lo, hi] = db.getDieArea();
    BOOST_TEST(lo.first == 0.0);
    BOOST_TEST(lo.second == 0.0);
    BOOST_TEST(hi.first == 100.0);
    BOOST_TEST(hi.second == 50.0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(GraphOps)

BOOST_AUTO_TEST_CASE(directs_edges_from_cell_to_net) {
    Graph g;
    const std::size_t cell = g.addVertex(VertexType::Cell, "c0");
    const std::size_t net = g.addVertex(VertexType::Net, "n0");
    const std::size_t edge = g.addEdge(cell, net, PinDirection::Output);

    BOOST_TEST(g.getVertexId("c0") == cell);
    BOOST_TEST(g.getVertexType(net) == VertexType::Net);
    BOOST_TEST(g.getEdge(edge).source == cell);
    BOOST_TEST(g.getEdge(edge).target == net);
    BOOST_TEST(g.getOutEdges(cell).size() == 1);
    BOOST_TEST(g.getInEdges(net).size() == 1);
}

BOOST_AUTO_TEST_CASE(clear_empties_the_graph) {
    Graph g;
    g.addVertex(VertexType::Cell, "c0");
    g.addVertex(VertexType::Net, "n0");
    BOOST_TEST(g.getNumVertices() == 2);
    g.clear();
    BOOST_TEST(g.getNumVertices() == 0);
    BOOST_TEST(!g.hasVertex("c0"));
}

BOOST_AUTO_TEST_CASE(clear_resets_the_id_counters) {
    Graph g;
    g.addVertex(VertexType::Cell, "c0");
    g.clear();
    // Ids restart, otherwise a fresh design would inherit stale numbering.
    BOOST_TEST(g.addVertex(VertexType::Cell, "again") == 0);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_CASE(PlacementDB_clear_resets_everything) {
    PlacementDB db;
    (void)db.addCell("c0", 1.0, 1.0);
    (void)db.addNet("n0");
    (void)db.addPin("c0", "n0", 0.0, 0.0, true);
    const std::size_t row = db.addRow(0.0, 1.0, 1.0, 1.0);
    (void)db.addSubrow(row, 0.0, 10.0);

    db.clear();

    BOOST_TEST(db.getNumCells() == 0);
    BOOST_TEST(db.getNumNets() == 0);
    BOOST_TEST(db.getNumPins() == 0);
    BOOST_TEST(db.getNumRows() == 0);
    BOOST_TEST(db.getRows().empty());
    const auto [cells, nets] = db.getStats();
    BOOST_TEST(cells == 0);
    BOOST_TEST(nets == 0);
}
