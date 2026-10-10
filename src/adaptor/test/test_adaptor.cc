// @file test_adaptor.cc// Unit tests for the Bookshelf and LEF/DEF input adapters// Every test writes a tiny synthetic design into a scratch directory, so the// suite needs no benchmark data on disk.


#define BOOST_TEST_MODULE ktplace_adaptor
#define BOOST_TEST_DYN_LINK
#include "adaptor/bookshelfToKTAdaptor.h"
#include "adaptor/lefdefToKTAdaptor.h"
#include "datamodel/kt_graph.h"

#include <boost/test/unit_test.hpp>
#include <filesystem>
#include <fstream>
#include <string>

using namespace ktplace;
namespace fs = std::filesystem;

namespace {

/// A scratch directory that removes itself when the test ends.
class ScratchDir {
public:
    explicit ScratchDir(const std::string &tag) {
        static int counter = 0;
        path =
            fs::temp_directory_path() / ("ktplace_test_" + tag + "_" + std::to_string(++counter));
        fs::create_directories(path);
    }
    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;

    [[nodiscard]] std::string str() const {
        return path.string();
    }
    [[nodiscard]] fs::path file(const std::string &name) const {
        return path / name;
    }

    void write(const std::string &name, const std::string &content) const {
        fs::create_directories(file(name).parent_path());
        std::ofstream out(file(name));
        BOOST_REQUIRE(out.is_open());
        out << content;
    }

private:
    fs::path path;
};

/// Two movable cells, one I/O pad, one two-pin net, one row.
constexpr const char *kNodes = R"(UCLA nodes 1.0

NumNodes : 3
NumTerminals : 1

	c0	1.0	2.0
	c1	3.0	2.0
	pad0	1.0	2.0	terminal
)";

constexpr const char *kNets = R"(UCLA nets 1.0

NumNets : 1
NumPins : 3

NetDegree : 3	n0
	c0 I	: 0.0	0.0
	c1 O	: 0.0	0.0
	pad0 I	: 0.0	0.0
)";

constexpr const char *kPl = R"(UCLA pl 1.0

c0	0.0	0.0 : N
c1	4.0	0.0 : N
pad0	0.0	0.0 : N /FIXED
)";

constexpr const char *kScl = R"(UCLA scl 1.0

NumRows : 1

CoreRow Horizontal
  Coordinate   :  0
  Height       :  2
  Sitewidth    :  1
  Sitespacing  :  1
  SubrowOrigin :  0 NumSites :  10
End
)";

/// Minimal LEF/DEF pair: one fixed macro, one placed standard cell, one pad.
constexpr const char *kTechLef = R"(VERSION 5.8 ;
UNITS
  DATABASE MICRONS 1000 ;
END UNITS
SITE core
  CLASS CORE ;
  SIZE 0.100 BY 0.900 ;
END core
END LIBRARY
)";

constexpr const char *kCellsLef = R"(VERSION 5.8 ;
MACRO NSTD
  CLASS CORE ;
  SIZE 0.200 BY 0.900 ;
  PIN A
    DIRECTION INPUT ;
    PORT
      RECT 0.000 0.000 0.050 0.100 ;
    END
  END A
END NSTD
MACRO MACRO1
  CLASS BLOCK ;
  SIZE 10.000 BY 20.000 ;
  PIN A
    DIRECTION INPUT ;
    PORT
      RECT 0.000 0.000 0.100 0.100 ;
    END
  END A
END MACRO1
END LIBRARY
)";

constexpr const char *kFloorplanDef = R"(VERSION 5.8 ;
UNITS DISTANCE MICRONS 1000 ;
DIEAREA ( 0 0 ) ( 10000 20000 ) ;
ROW row0 core 0 0 N DO 10 BY 1 STEP 100 0 ;
COMPONENTS 2 ;
	- u1 NSTD + PLACED ( 1000 2000 ) N ;
	- m1 MACRO1 + FIXED ( 5000 5000 ) N ;
END COMPONENTS
PINS 1 ;
	- p1 + NET n0 + PLACED ( 0 0 ) N ;
END PINS
NETS 1 ;
	- n0 ( u1 A ) ( PIN p1 ) ;
END NETS
END DESIGN
)";

}  // namespace

BOOST_AUTO_TEST_SUITE(Bookshelf)

BOOST_AUTO_TEST_CASE(reads_a_minimal_design_from_a_directory) {
    const ScratchDir dir("bs_dir");
    dir.write("tiny/tiny.nodes", kNodes);
    dir.write("tiny/tiny.nets", kNets);
    dir.write("tiny/tiny.pl", kPl);
    dir.write("tiny/tiny.scl", kScl);

    BookshelfInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromDirectory(dir.file("tiny").string()));

    ktDM &db = adapter.getDM();
    BOOST_TEST(db.getNumCells() == 3);
    BOOST_TEST(db.getNumTerminals() == 1);
    BOOST_TEST(db.getNumNets() == 1);
    BOOST_TEST(db.getNumPins() == 3);
    BOOST_TEST(db.getNumRows() == 1);
    BOOST_TEST(db.hasCell("c0"));
    BOOST_TEST(db.hasCell("pad0"));
}

BOOST_AUTO_TEST_CASE(reads_cell_geometry_and_terminal_flag) {
    const ScratchDir dir("bs_geom");
    dir.write("tiny/tiny.nodes", kNodes);
    dir.write("tiny/tiny.nets", kNets);

    BookshelfInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromFiles(dir.file("tiny/tiny.nodes").string(),
                                        dir.file("tiny/tiny.nets").string()));

    ktDM &db = adapter.getDM();
    const Graph &g = db.getGraph();
    const Vertex &c1 = g.getCell(db.getCellId("c1"));
    BOOST_TEST(c1.width == 3.0);
    BOOST_TEST(c1.height == 2.0);
    BOOST_TEST(!c1.isTerminal);
    BOOST_TEST(g.getCell(db.getCellId("pad0")).isTerminal);
}

BOOST_AUTO_TEST_CASE(placement_file_marks_pads_fixed) {
    const ScratchDir dir("bs_fixed");
    dir.write("tiny/tiny.nodes", kNodes);
    dir.write("tiny/tiny.nets", kNets);
    dir.write("tiny/tiny.pl", kPl);

    BookshelfInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromFiles(dir.file("tiny/tiny.nodes").string(),
                                        dir.file("tiny/tiny.nets").string(),
                                        dir.file("tiny/tiny.pl").string()));

    ktDM &db = adapter.getDM();
    // The pad is anchored, the standard cell is free to move.
    BOOST_TEST(db.isCellFixed("pad0"));
    BOOST_TEST(!db.isCellFixed("c0"));
}

BOOST_AUTO_TEST_CASE(terminals_are_fixed_even_without_a_pl_file) {
    const ScratchDir dir("bs_term");
    dir.write("tiny/tiny.nodes", kNodes);
    dir.write("tiny/tiny.nets", kNets);

    BookshelfInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromFiles(dir.file("tiny/tiny.nodes").string(),
                                        dir.file("tiny/tiny.nets").string()));

    ktDM &db = adapter.getDM();
    // "terminal" in .nodes alone makes a pad immovable.
    BOOST_TEST(db.isCellFixed("pad0"));
    BOOST_TEST(!db.isCellFixed("c0"));
}

BOOST_AUTO_TEST_CASE(reads_row_structure) {
    const ScratchDir dir("bs_rows");
    dir.write("tiny/tiny.nodes", kNodes);
    dir.write("tiny/tiny.nets", kNets);
    dir.write("tiny/tiny.scl", kScl);

    BookshelfInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromFiles(dir.file("tiny/tiny.nodes").string(),
                                        dir.file("tiny/tiny.nets").string(), "",
                                        dir.file("tiny/tiny.scl").string()));

    BOOST_TEST(adapter.getDM().getNumRows() == 1);
}

BOOST_AUTO_TEST_CASE(fails_cleanly_on_a_missing_directory) {
    BookshelfInputAdapter adapter;
    BOOST_TEST(!adapter.readFromDirectory("/nonexistent/ktplace/path"));
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(LefDef)

BOOST_AUTO_TEST_CASE(reads_macros_cells_pads_and_nets) {
    const ScratchDir dir("ld_basic");
    dir.write("tech.lef", kTechLef);
    dir.write("cells.lef", kCellsLef);
    dir.write("floorplan.def", kFloorplanDef);

    LefDefInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromDirectory(dir.str()));

    ktDM &db = adapter.getDM();
    // Two components plus the pad.
    BOOST_TEST(db.getNumCells() == 3);
    BOOST_TEST(db.getNumTerminals() == 1);
    BOOST_TEST(db.getNumNets() == 1);
    BOOST_TEST(db.hasCell("m1"));
    BOOST_TEST(db.hasCell("u1"));
}

BOOST_AUTO_TEST_CASE(scales_lef_microns_by_the_def_units) {
    const ScratchDir dir("ld_units");
    dir.write("tech.lef", kTechLef);
    dir.write("cells.lef", kCellsLef);
    dir.write("floorplan.def", kFloorplanDef);

    LefDefInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromDirectory(dir.str()));

    ktDM &db = adapter.getDM();
    const Graph &g = db.getGraph();
    // SIZE 10.000 BY 20.000 microns at 1000 units per micron.
    const Vertex &macro = g.getCell(db.getCellId("m1"));
    BOOST_TEST(macro.width == 10000.0);
    BOOST_TEST(macro.height == 20000.0);
    // SIZE 0.200 BY 0.900 microns.
    const Vertex &std = g.getCell(db.getCellId("u1"));
    BOOST_TEST(std.width == 200.0);
    BOOST_TEST(std.height == 900.0);
}

BOOST_AUTO_TEST_CASE(keeps_macro_and_placed_component_coordinates) {
    const ScratchDir dir("ld_pos");
    dir.write("tech.lef", kTechLef);
    dir.write("cells.lef", kCellsLef);
    dir.write("floorplan.def", kFloorplanDef);

    LefDefInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromDirectory(dir.str()));

    ktDM &db = adapter.getDM();
    // "+ FIXED ( 5000 5000 )" is the component origin, not its centre.
    const auto [mx, my] = db.getCellPosition("m1");
    BOOST_TEST(mx == 5000.0);
    BOOST_TEST(my == 5000.0);
    BOOST_TEST(db.isCellFixed("m1"));

    const auto [ux, uy] = db.getCellPosition("u1");
    BOOST_TEST(ux == 1000.0);
    BOOST_TEST(uy == 2000.0);
    BOOST_TEST(!db.isCellFixed("u1"));
}

BOOST_AUTO_TEST_CASE(reads_die_area_and_rows) {
    const ScratchDir dir("ld_die");
    dir.write("tech.lef", kTechLef);
    dir.write("cells.lef", kCellsLef);
    dir.write("floorplan.def", kFloorplanDef);

    LefDefInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromDirectory(dir.str()));

    ktDM &db = adapter.getDM();
    const auto [lo, hi] = db.getDieArea();
    BOOST_TEST(lo.first == 0.0);
    BOOST_TEST(lo.second == 0.0);
    BOOST_TEST(hi.first == 10000.0);
    BOOST_TEST(hi.second == 20000.0);
    // ROW ... STEP 100 0 carries no row height, so the LEF SITE size is used.
    BOOST_TEST(db.getNumRows() == 1);
}

BOOST_AUTO_TEST_CASE(renames_def_components_containing_slashes) {
    const ScratchDir dir("ld_sanitize");
    dir.write("tech.lef", kTechLef);
    dir.write("cells.lef", kCellsLef);
    dir.write("floorplan.def", R"(VERSION 5.8 ;
UNITS DISTANCE MICRONS 1000 ;
COMPONENTS 1 ;
	- u/1 NSTD + PLACED ( 0 0 ) N ;
END COMPONENTS
END DESIGN
)");

    LefDefInputAdapter adapter;
    BOOST_REQUIRE(adapter.readFromDirectory(dir.str()));
    // "/" is not usable as a graph key, so it becomes "_".
    BOOST_TEST(adapter.getDM().hasCell("u_1"));
}

BOOST_AUTO_TEST_CASE(fails_cleanly_without_a_def_file) {
    const ScratchDir dir("ld_empty");
    dir.write("tech.lef", kTechLef);
    LefDefInputAdapter adapter;
    BOOST_TEST(!adapter.readFromDirectory(dir.str()));
}

BOOST_AUTO_TEST_SUITE_END()
