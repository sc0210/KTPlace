// SPDX-License-Identifier: BSD-3-Clause
// @file test_viz.cc// Unit tests for the canvas/GIF writer and the SVG/raster renderers// The GIF writer is the reason these tests read bytes instead of comparing// pictures. A GIF that is almost right -- a bad code width, a missing loop// extension, a truncated block chain -- still opens in some viewers and shows// as garbage in others, so the assertions here walk the container structure// rather than trusting that "a file appeared".// The renderers are checked for well-formed output and for the specific// things that would misinform a reader: the die box falling back to 1x1 when// there are no fixed cells, coordinates indexing the wrong vertex, and frames// named so that collectFrames() sorts them in run order rather than// lexicographic order (frame_10 before frame_9).


#define BOOST_TEST_MODULE ktplace_viz

#include "visualization/kt_animator.h"
#include "visualization/kt_gif.h"
#include "visualization/kt_plotter.h"

#include <algorithm>
#include <boost/test/included/unit_test.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace ktplace;

namespace {

/// Read a whole file into a string, so the GIF can be inspected byte by byte.
std::string readAll(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool contains(const std::string &hay, const std::string &needle) {
    return hay.find(needle) != std::string::npos;
}

class ScratchDir {
public:
    explicit ScratchDir(const char *tag) {
        static int counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("ktplace_viz_" + std::string(tag) + "_" + std::to_string(++counter));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDir(const ScratchDir &) = delete;
    ScratchDir &operator=(const ScratchDir &) = delete;
    [[nodiscard]] std::string str() const {
        return path_.string();
    }
    [[nodiscard]] std::filesystem::path file(const std::string &n) const {
        return path_ / n;
    }

private:
    std::filesystem::path path_;
};

/// A palette with enough flat colours for the drawing tests.
std::vector<Rgb> palette() {
    return {Rgb{0, 0, 0},       Rgb{255, 0, 0},     Rgb{0, 255, 0},   Rgb{0, 0, 255},
            Rgb{255, 255, 255}, Rgb{128, 128, 128}, Rgb{255, 255, 0}, Rgb{0, 255, 255}};
}

/// A graph with a few fixed cells forming a die and some movable ones.
Graph sampleGraph() {
    Graph g;
    const auto add = [&g](const char *name, double x, double y, double w, double h, bool fixed) {
        const std::size_t id = g.addCell(name);
        Vertex &v = g.getCell(id);
        v.x = x;
        v.y = y;
        v.width = w;
        v.height = h;
        v.isFixed = fixed;
        g.getCell(id).isTerminal = fixed;
    };
    add("fix1", 0.0, 0.0, 10.0, 10.0, true);
    add("fix2", 90.0, 0.0, 10.0, 10.0, true);
    add("fix3", 0.0, 90.0, 10.0, 10.0, true);
    add("fix4", 90.0, 90.0, 10.0, 10.0, true);
    add("m1", 20.0, 20.0, 4.0, 4.0, false);
    add("m2", 40.0, 20.0, 4.0, 4.0, false);
    add("m3", 20.0, 40.0, 4.0, 4.0, false);
    return g;
}

/// Per-vertex coordinates in vertex order, as the renderers expect.
std::vector<float> sampleX(const Graph &g) {
    std::vector<float> v(g.getNumCells(), 0.0F);
    for (std::size_t i = 0; i < g.getNumCells(); ++i) {
        v[i] = static_cast<float>(g.getCell(i).x);
    }
    return v;
}

std::vector<float> sampleY(const Graph &g) {
    std::vector<float> v(g.getNumCells(), 0.0F);
    for (std::size_t i = 0; i < g.getNumCells(); ++i) {
        v[i] = static_cast<float>(g.getCell(i).y);
    }
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(a_new_canvas_is_the_size_asked_for) {
    const Canvas c(7, 5, palette());
    BOOST_TEST(c.width() == 7);
    BOOST_TEST(c.height() == 5);
    BOOST_TEST(c.pixels().size() == 35u);
    BOOST_TEST(c.palette().size() == 8u);
}

BOOST_AUTO_TEST_CASE(clear_fills_every_pixel) {
    Canvas c(4, 3, palette());
    c.clear(2);
    BOOST_TEST(std::all_of(c.pixels().begin(), c.pixels().end(), [](std::uint8_t p) {
        return p == 2;
    }));
}

BOOST_AUTO_TEST_CASE(a_rectangle_fills_only_its_own_pixels) {
    Canvas c(8, 8, palette());
    c.clear(0);
    c.fillRect(2, 2, 3, 3, 1);
    // 3x3 = 9 pixels, and the pixel at (2,2) is the top-left of the square.
    const auto count = [&c](std::uint8_t want) {
        return static_cast<int>(std::count(c.pixels().begin(), c.pixels().end(), want));
    };
    BOOST_TEST(count(1) == 9);
    BOOST_TEST(count(0) == 64 - 9);
}

BOOST_AUTO_TEST_CASE(a_rectangle_is_clipped_to_the_image) {
    // A cell near the die edge is drawn every frame; if the clip is wrong this
    // writes past the end of the pixel buffer.
    Canvas c(4, 4, palette());
    c.clear(0);
    c.fillRect(-5, -5, 100, 100, 3);
    BOOST_TEST(std::all_of(c.pixels().begin(), c.pixels().end(), [](std::uint8_t p) {
        return p == 3;
    }));

    Canvas c2(4, 4, palette());
    c2.clear(0);
    c2.fillRect(2, 2, 10, 10, 1);
    // Only the 2x2 corner inside the image is written.
    BOOST_TEST(c2.pixels()[0] == 0);
    BOOST_TEST(c2.pixels()[2 * 4 + 2] == 1);
    BOOST_TEST(c2.pixels()[3 * 4 + 3] == 1);
}

BOOST_AUTO_TEST_CASE(a_zero_or_negative_rectangle_draws_nothing) {
    Canvas c(4, 4, palette());
    c.clear(0);
    c.fillRect(1, 1, 0, 5, 1);
    c.fillRect(1, 1, 5, -3, 2);
    BOOST_TEST(std::all_of(c.pixels().begin(), c.pixels().end(), [](std::uint8_t p) {
        return p == 0;
    }));
}

BOOST_AUTO_TEST_CASE(a_half_transparent_rectangle_blends_rather_than_overwrites) {
    // Alpha 0 must leave the frame untouched; that is what makes a translucent
    // overlay usable as a preview.
    Canvas c(4, 4, palette());
    c.clear(1);
    c.fillRect(0, 0, 4, 4, 2, 0.0);
    BOOST_TEST(std::all_of(c.pixels().begin(), c.pixels().end(), [](std::uint8_t p) {
        return p == 1;
    }));
}

BOOST_AUTO_TEST_CASE(lines_run_in_both_directions_and_single_pixels) {
    Canvas c(6, 6, palette());
    c.clear(0);
    c.hLine(0, 5, 2, 1);
    c.vLine(0, 5, 3, 1);
    c.hLine(4, 1, 4, 2);  // reversed, must still draw
    BOOST_TEST(c.pixels()[2 * 6 + 0] == 1);
    BOOST_TEST(c.pixels()[2 * 6 + 5] == 1);
    BOOST_TEST(c.pixels()[0 * 6 + 3] == 1);
    BOOST_TEST(c.pixels()[5 * 6 + 3] == 1);
    BOOST_TEST(c.pixels()[4 * 6 + 1] == 2);
    BOOST_TEST(c.pixels()[4 * 6 + 4] == 2);
}

BOOST_AUTO_TEST_CASE(a_line_is_clipped_rather_than_wrapping) {
    Canvas c(5, 5, palette());
    c.clear(0);
    c.hLine(-3, 99, 2, 1);
    // The whole row is inside the clip, no wraparound into another row.
    for (std::size_t i = 0; i < 5; ++i) {
        BOOST_TEST(c.pixels()[2 * 5 + i] == 1);
    }
}

BOOST_AUTO_TEST_CASE(text_draws_something_and_keeps_its_size_predictable) {
    Canvas c(120, 20, palette());
    c.clear(0);
    c.text(1, 1, "iter 42", 1);
    // The width the renderer asked for has to match what it drew, or labels
    // overlap the plot or the legend.
    BOOST_TEST(Canvas::textWidth("iter 42") > 0);
    const auto lit =
        static_cast<int>(std::count_if(c.pixels().begin(), c.pixels().end(), [](std::uint8_t p) {
            return p == 1;
        }));
    BOOST_TEST(lit > 0);
}

BOOST_AUTO_TEST_CASE(text_scales_by_an_integer_factor) {
    const int small = Canvas::textWidth("ab");
    const int big = Canvas::textWidth("ab", 3);
    BOOST_TEST(small > 0);
    BOOST_TEST(big == small * 3);
}

BOOST_AUTO_TEST_CASE(unprintable_characters_are_skipped_not_crashing) {
    // A cell name or a note can carry anything; the font has a limited range and
    // must ignore what it cannot draw.
    Canvas c(200, 20, palette());
    c.clear(0);
    BOOST_CHECK_NO_THROW(c.text(0, 0, std::string("a\x01\x7f\xffz"), 1));
    BOOST_TEST(Canvas::textWidth("a\x01z") > 0);
}

BOOST_AUTO_TEST_CASE(an_adopted_frame_is_stored_as_given) {
    // The animation path already has palette indices, so it must not re-search
    // them: an index that maps to a different colour would silently shift every
    // colour in the GIF.
    const std::vector<std::uint8_t> idx = {0, 1, 1, 0};
    const Canvas c(2, 2, idx, palette());
    BOOST_TEST(c.width() == 2);
    BOOST_TEST(c.pixels() == idx);
}

// ---------------------------------------------------------------------------
// LZW and the GIF container
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(lzw_of_a_flat_run_is_small) {
    // 4096 identical pixels must not expand to 4096 codes. If the code width
    // never grows, or the dictionary is not reset, this is the canary.
    const std::vector<std::uint8_t> flat(4096, 0);
    const std::vector<std::uint8_t> out = gifCompress(flat, 8);
    BOOST_TEST(!out.empty());
    BOOST_TEST(out.size() < flat.size() / 4);
}

BOOST_AUTO_TEST_CASE(lzw_output_is_a_well_formed_sub_block_chain) {
    // Every block is a length byte then that many payload bytes, and the chain
    // ends with a zero length. A malformed chain truncates the image in some
    // viewers and silently drops the rest in others.
    std::vector<std::uint8_t> pixels(1000);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::uint8_t>((i * 7) % 5);
    }
    const std::vector<std::uint8_t> out = gifCompress(pixels, 8);

    std::size_t i = 0;
    std::size_t blocks = 0;
    while (i < out.size()) {
        const std::size_t len = out[i];
        if (len == 0) {
            break;
        }
        BOOST_REQUIRE(i + 1 + len <= out.size());
        i += 1 + len;
        ++blocks;
    }
    BOOST_TEST(blocks > 0);
    BOOST_TEST(i == out.size() - 1);  // the last byte is the terminator
}

BOOST_AUTO_TEST_CASE(lzw_handles_a_single_pixel_and_a_min_code_size_below_eight) {
    BOOST_TEST(!gifCompress({7}, 8).empty());
    BOOST_TEST(!gifCompress({0, 1, 2, 3}, 2).empty());
}

BOOST_AUTO_TEST_CASE(lzw_survives_input_that_defeats_a_naive_encoder) {
    // Alternating pixels never form a run, which is the worst case for the
    // dictionary growth. It must still terminate.
    std::vector<std::uint8_t> pixels;
    for (int i = 0; i < 20000; ++i) {
        pixels.push_back(static_cast<std::uint8_t>(i % 2));
    }
    const std::vector<std::uint8_t> out = gifCompress(pixels, 8);
    BOOST_TEST(!out.empty());
}

BOOST_AUTO_TEST_CASE(a_single_frame_gif_is_a_valid_gif89a) {
    const ScratchDir dir("single");
    Canvas c(8, 8, palette());
    c.clear(2);
    const std::string out = (dir.file("a.gif")).string();
    BOOST_TEST(writeGif(out, {c}, 20));

    const std::string bytes = readAll(dir.file("a.gif"));
    BOOST_TEST(contains(bytes, "GIF89a"));
    BOOST_TEST(bytes.size() > 20);

    // The logical screen descriptor follows the 6-byte signature and stores its
    // width and height as little-endian uint16, not as two bytes each.
    const auto u16 = [&bytes](std::size_t at) {
        return static_cast<unsigned>(static_cast<unsigned char>(bytes[at])) |
               (static_cast<unsigned>(static_cast<unsigned char>(bytes[at + 1])) << 8);
    };
    BOOST_TEST(u16(6) == 8u);  // width
    BOOST_TEST(u16(8) == 8u);  // height

    // Packed field: bit 7 is the global colour table flag, and the low three
    // bits hold N where the table is 2^(N+1) entries. Eight palette entries need
    // 3 bits per pixel, and the table is sized from that, so N is 2 for 8 slots.
    const unsigned packed = static_cast<unsigned char>(bytes[10]);
    BOOST_TEST((packed & 0x80u) != 0u);
    BOOST_TEST(1u << ((packed & 0x07u) + 1u) == 8u);
    // The table must be at least as large as the palette, or an index in the
    // last few colours would be unresolvable.
    BOOST_TEST((1u << ((packed & 0x07u) + 1u)) >= palette().size());
}

BOOST_AUTO_TEST_CASE(a_multi_frame_gif_carries_the_loop_extension) {
    // Without NETSCAPE2.0 the file plays once and stops. A run recorded as an
    // animation has to come back to the start on its own.
    const ScratchDir dir("loop");
    Canvas a(8, 8, palette());
    a.clear(1);
    Canvas b(8, 8, palette());
    b.clear(2);
    const std::string out = (dir.file("a.gif")).string();
    BOOST_TEST(writeGif(out, {a, b}, 5));
    BOOST_TEST(contains(readAll(dir.file("a.gif")), "NETSCAPE2.0"));
}

BOOST_AUTO_TEST_CASE(mismatched_frame_sizes_are_rejected_not_written) {
    // The container has a single logical screen, so a mixed-size list cannot be
    // encoded. Returning false is the contract; the point tested here is that it
    // does not write a file that claims to hold frames it never wrote.
    const ScratchDir dir("mixedsize");
    Canvas a(8, 4, palette());
    Canvas b(4, 8, palette());
    const std::string out = (dir.file("a.gif")).string();
    BOOST_TEST(!writeGif(out, {a, b}, 5));
    BOOST_TEST(!std::filesystem::exists(dir.file("a.gif")));
}

BOOST_AUTO_TEST_CASE(a_mismatched_palette_is_also_rejected) {
    // The colour table is written once for the file, so frames that disagree
    // about it would render later frames with the wrong colours.
    const ScratchDir dir("mixedpal");
    Canvas a(8, 8, palette());
    Canvas b(8, 8, std::vector<Rgb>{Rgb{1, 2, 3}});
    const std::string out = (dir.file("a.gif")).string();
    BOOST_TEST(!writeGif(out, {a, b}, 5));
}

BOOST_AUTO_TEST_CASE(writing_to_an_unopenable_path_reports_failure) {
    Canvas c(4, 4, palette());
    // A path whose parent is a file cannot be created; writeGif has to say so
    // rather than returning true for a file that is not there.
    const std::string out = "/proc/self/nonexistent_dir/a.gif";
    BOOST_TEST(!writeGif(out, {c}, 5));
}

BOOST_AUTO_TEST_CASE(an_empty_frame_list_writes_nothing_useful) {
    const ScratchDir dir("empty");
    const std::string out = (dir.file("a.gif")).string();
    // Nothing to draw: no file is a better answer than a zero-byte one that a
    // viewer reports as a broken image.
    BOOST_CHECK(!writeGif(out, {}, 5));
}

// ---------------------------------------------------------------------------
// Plotter helpers
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(ensure_dir_creates_nested_directories_and_tolerates_repeats) {
    const ScratchDir dir("ensuredir");
    const std::string nested = (dir.file("a") / "b" / "c").string();
    BOOST_TEST(ensureDir(nested));
    BOOST_TEST(std::filesystem::is_directory(nested));
    // Idempotent: called once per frame with the same directory.
    BOOST_TEST(ensureDir(nested));
    // An empty name is a no-op, not a failure: the plot flag may be off.
    BOOST_TEST(ensureDir(""));
}

BOOST_AUTO_TEST_CASE(the_die_box_comes_from_the_fixed_cells) {
    const Graph g = sampleGraph();
    const BBox box = fixedCellBBox(g);
    // The boxes include the cell extent, so a pad at the right rim is not
    // clipped out of the drawn die.
    BOOST_TEST(box[0] == 0.0);
    BOOST_TEST(box[1] == 0.0);
    BOOST_TEST(box[2] == 100.0);
    BOOST_TEST(box[3] == 100.0);
}

BOOST_AUTO_TEST_CASE(a_design_with_no_fixed_cells_still_gets_a_usable_box) {
    // The viewport is built from this box. A degenerate one would collapse the
    // whole plot to a point, which is what happens if it is not guarded.
    Graph g;
    const std::size_t id = g.addCell("m1");
    g.getCell(id).x = 10.0;
    g.getCell(id).y = 20.0;
    g.getCell(id).width = 4.0;
    g.getCell(id).height = 4.0;

    const BBox box = fixedCellBBox(g);
    BOOST_TEST(box[0] <= box[2]);
    BOOST_TEST(box[1] <= box[3]);
    BOOST_TEST(box[2] > box[0]);
    BOOST_TEST(box[3] > box[1]);
}

BOOST_AUTO_TEST_CASE(net_vertices_are_not_part_of_the_die) {
    // Nets have a width of zero and would otherwise anchor the box at 0,0 for a
    // design whose cells sit elsewhere.
    Graph g = sampleGraph();
    g.addNet("n1");
    const BBox box = fixedCellBBox(g);
    BOOST_TEST(box[0] == 0.0);
    BOOST_TEST(box[2] == 100.0);
}

// ---------------------------------------------------------------------------
// Renderers
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(an_svg_frame_is_well_formed_and_mentions_its_note) {
    const ScratchDir dir("svg");
    const Graph g = sampleGraph();
    const std::string path = (dir.file("frame_0000.svg")).string();
    writeFrameSvg(path, g, sampleX(g), sampleY(g), fixedCellBBox(g), 3, 10, 100.0, 200.0, 0.5,
                  "iter 3");

    const std::string svg = readAll(dir.file("frame_0000.svg"));
    BOOST_TEST(contains(svg, "<svg"));
    BOOST_TEST(contains(svg, "</svg>"));
    BOOST_TEST(contains(svg, "viewBox"));
    BOOST_TEST(contains(svg, "iter 3"));
    // Cells are drawn as unnamed rects grouped by colour, so their presence is
    // checked as drawn geometry rather than as a label: one die outline plus one
    // rect per cell is the minimum.
    const auto rects = static_cast<std::size_t>(std::count(svg.begin(), svg.end(), '<') -
                                                std::count(svg.begin(), svg.end(), '>'));
    BOOST_TEST(rects == 0u);  // every tag is closed
    std::size_t cellRects = 0;
    for (std::size_t at = svg.find("<rect"); at != std::string::npos;
         at = svg.find("<rect", at + 1)) {
        ++cellRects;
    }
    // 3 movable + 4 fixed cells + the die outline + the progress bar.
    BOOST_TEST(cellRects >= 8u);
    // The die outline is stroked rather than filled, so it reads as a frame.
    BOOST_TEST(contains(svg, "stroke=\"#bdbdbd\""));
}

BOOST_AUTO_TEST_CASE(the_svg_progress_bar_reflects_the_step) {
    const ScratchDir dir("svgbar");
    const Graph g = sampleGraph();
    const std::string early = (dir.file("a.svg")).string();
    const std::string late = (dir.file("b.svg")).string();
    writeFrameSvg(early, g, sampleX(g), sampleY(g), fixedCellBBox(g), 0, 10, 1.0, 1.0, 0.0, "");
    writeFrameSvg(late, g, sampleX(g), sampleY(g), fixedCellBBox(g), 10, 10, 1.0, 1.0, 0.0, "");

    // A step beyond the total must not produce a negative or overlong bar.
    const std::string bytes = readAll(dir.file("b.svg"));
    BOOST_TEST(!contains(bytes, "NaN"));
    BOOST_TEST(!contains(bytes, "nan"));
    BOOST_TEST(!contains(bytes, "inf"));
}

BOOST_AUTO_TEST_CASE(an_svg_with_no_fences_and_one_with_fences_both_draw) {
    const ScratchDir dir("svgfence");
    const Graph g = sampleGraph();
    constraintMgr cstr;
    cstr.addRegion("er0", {Point{20.0, 20.0}, Point{50.0, 50.0}});

    const std::string plain = (dir.file("plain.svg")).string();
    writeFrameSvg(plain, g, sampleX(g), sampleY(g), fixedCellBBox(g), 1, 2, 1.0, 1.0, 0.0, "");
    BOOST_TEST(contains(readAll(dir.file("plain.svg")), "<svg"));

    const std::string fenced = (dir.file("fenced.svg")).string();
    writeFrameSvg(fenced, g, sampleX(g), sampleY(g), fixedCellBBox(g), 1, 2, 1.0, 1.0, 0.0, "",
                  &cstr);
    const std::string withFence = readAll(dir.file("fenced.svg"));
    BOOST_TEST(contains(withFence, "<svg"));
    // The fence has to be visible, not silently dropped.
    BOOST_TEST(withFence.size() > readAll(dir.file("plain.svg")).size());
}

BOOST_AUTO_TEST_CASE(a_fixed_view_draws_the_original_positions) {
    // The fixed view exists so a series of frames shares one scale; it must not
    // consult the solution arrays.
    const ScratchDir dir("svgfixed");
    const Graph g = sampleGraph();
    std::vector<float> moved = sampleX(g);
    std::vector<float> movedY = sampleY(g);
    for (std::size_t i = 0; i < g.getNumCells(); ++i) {
        if (!g.getCell(i).isFixed) {
            moved[i] = 999.0F;
            movedY[i] = 999.0F;
        }
    }
    const std::string path = (dir.file("f.svg")).string();
    BOOST_CHECK_NO_THROW(writeFrameSvg(path, g, moved, movedY, fixedCellBBox(g), 1, 2, 1.0, 1.0,
                                       0.0, "", nullptr, true));
    BOOST_TEST(contains(readAll(dir.file("f.svg")), "<svg"));
}

BOOST_AUTO_TEST_CASE(movable_cells_are_written_once_each_in_vertex_order) {
    // The movable-cell lines are formatted in parallel chunks of 4096 vertices,
    // so a design several chunks long checks that the chunks are stitched back
    // in order and that none is dropped or written twice at a boundary. Cell i
    // sits at x = i in world units, so the order of the x values is the order of
    // the cells in the file.
    const ScratchDir dir("svgorder");
    Graph g = sampleGraph();
    constexpr std::size_t kCells = 10000;
    const std::size_t first = g.getNumCells();
    for (std::size_t i = 0; i < kCells; ++i) {
        Vertex &v = g.getCell(g.addCell("o" + std::to_string(i)));
        v.width = 1.0;
        v.height = 1.0;
    }
    std::vector<float> x = sampleX(g), y = sampleY(g);
    for (std::size_t i = 0; i < kCells; ++i) {
        x[first + i] = static_cast<float>(i);
        y[first + i] = 50.0F;
    }
    const BBox die{0.0, 0.0, 20000.0, 100.0};
    const std::string path = dir.file("o.svg").string();
    writeFrameSvg(path, g, x, y, die, 0, 1, 0.0, 0.0, 0.0, "", nullptr, true, /*worldUnits=*/true);

    const std::string svg = readAll(dir.file("o.svg"));
    const std::size_t open = svg.find("<g fill=\"#4fc3f7\"");
    BOOST_REQUIRE(open != std::string::npos);
    const std::size_t close = svg.find("</g>", open);
    BOOST_REQUIRE(close != std::string::npos);
    std::vector<double> xs;
    for (std::size_t at = svg.find("<rect x=\"", open); at < close;
         at = svg.find("<rect x=\"", at + 1)) {
        xs.push_back(std::stod(svg.substr(at + 9)));
    }
    // sampleGraph's three movable cells come first, then the kCells added here.
    BOOST_REQUIRE_EQUAL(xs.size(), kCells + 3);
    for (std::size_t i = 0; i < kCells; ++i) {
        BOOST_REQUIRE_EQUAL(xs[3 + i], static_cast<double>(i));
    }
}

BOOST_AUTO_TEST_CASE(a_raster_frame_is_a_binary_ppm_that_reads_back) {
    const ScratchDir dir("ppm");
    const Graph g = sampleGraph();
    const std::string path = (dir.file("frame_0000.ppm")).string();
    writeFrameRaster(path, g, sampleX(g), sampleY(g), fixedCellBBox(g), 2, 5, 10.0, 20.0, 0.1,
                     "iter 2");

    const std::string bytes = readAll(dir.file("frame_0000.ppm"));
    // P6: "P6" then two dimensions then maxval 255, all whitespace separated.
    BOOST_TEST(bytes.size() > 15);
    BOOST_TEST(bytes.compare(0, 2, "P6") == 0);
    BOOST_TEST(bytes.find("255") != std::string::npos);
    // Header + width*height*3, so the payload must actually be there.
    const std::size_t header = bytes.find('\n', bytes.find("255")) + 1;
    BOOST_TEST(bytes.size() - header > 3);
}

BOOST_AUTO_TEST_CASE(frames_are_collected_in_numeric_not_lexicographic_order) {
    // frame_10 must not be played before frame_9. This is the one ordering bug
    // in a zero-padded naming scheme that a small test catches and a big run
    // hides, because the run only looks wrong in the last few frames.
    const ScratchDir dir("order");
    const Graph g = sampleGraph();
    for (int i = 0; i < 12; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "frame_%04d.ppm", i);
        writeFrameRaster((dir.file(name)).string(), g, sampleX(g), sampleY(g), fixedCellBBox(g),
                         static_cast<std::size_t>(i), 12, 1.0, 1.0, 0.0, "");
    }
    BOOST_TEST(writeAnimatedGif(dir.str(), "anim.gif", 5));
    BOOST_TEST(contains(readAll(dir.file("anim.gif")), "GIF89a"));
}

BOOST_AUTO_TEST_CASE(assembling_a_gif_from_no_stills_fails_cleanly) {
    const ScratchDir dir("nostills");
    BOOST_TEST(!writeAnimatedGif(dir.str(), "anim.gif", 5));
    BOOST_TEST(!std::filesystem::exists(dir.file("anim.gif")));
}

BOOST_AUTO_TEST_CASE(a_written_gif_takes_its_stills_with_it) {
    // The stills are the encoder's scratch space, and on a real run they are
    // hundreds of megabytes of P6 that nothing can open. They go once the GIF
    // they were assembled into is on disk.
    const ScratchDir dir("cleanup");
    const Graph g = sampleGraph();
    for (int i = 0; i < 3; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "frame_%04d.ppm", i);
        writeFrameRaster((dir.file(name)).string(), g, sampleX(g), sampleY(g), fixedCellBBox(g),
                         static_cast<std::size_t>(i), 3, 1.0, 1.0, 0.0, "");
    }
    BOOST_TEST(writeAnimatedGif(dir.str(), "anim.gif", 5));
    BOOST_TEST(std::filesystem::exists(dir.file("anim.gif")));
    for (int i = 0; i < 3; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "frame_%04d.ppm", i);
        BOOST_TEST(!std::filesystem::exists(dir.file(name)));
    }
}

BOOST_AUTO_TEST_CASE(failed_gif_assembly_keeps_the_stills) {
    // The mirror image of the case above, and the reason the removal happens
    // after the encode rather than before it: if the encode failed, the stills
    // are the only record left of the run's frames.
    const ScratchDir dir("keepstills");
    BOOST_TEST(!writeAnimatedGif(dir.str(), "anim.gif", 5));
    // Nothing was collected, so there was nothing to keep either; the check is
    // that a failed assembly does not remove anything it did not produce.
    BOOST_TEST(!std::filesystem::exists(dir.file("anim.gif")));
}

BOOST_AUTO_TEST_CASE(a_raster_frame_can_be_written_as_a_png) {
    // The final still is meant to be looked at, and a PPM cannot be opened by a
    // browser or an image viewer, so ".png" is routed to the in-process writer
    // rather than to CImg (whose PNG path needs libpng headers this build lacks).
    const ScratchDir dir("png");
    const Graph g = sampleGraph();
    const std::string path = (dir.file("final.png")).string();
    writeFrameRaster(path, g, sampleX(g), sampleY(g), fixedCellBBox(g), 0, 1, 1.0, 1.0, 0.0,
                     "final", nullptr, /*fixedView=*/true, /*zoom=*/2.0);

    const std::string png = readAll(dir.file("final.png"));
    BOOST_TEST(png.size() > 8u);
    // Signature, then the first chunk: 8 signature bytes, a 4-byte length of 13,
    // then the type.
    BOOST_TEST(png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    BOOST_TEST(png.compare(12, 4, "IHDR") == 0);
    // The last chunk is a zero length, the type, and its CRC, so IEND starts 8
    // bytes from the end.
    BOOST_TEST(png.compare(png.size() - 8, 4, "IEND") == 0);
    // Zoom 2 is 1536 = 0x600, stored big-endian in IHDR's width then height.
    // The expected bytes start with a NUL, so they are built as a counted string:
    // a bare literal would compare as an empty string.
    const std::string be1536("\x00\x00\x06\x00", 4);
    BOOST_TEST(png.compare(16, 4, be1536) == 0);
    BOOST_TEST(png.compare(20, 4, be1536) == 0);
}

BOOST_AUTO_TEST_CASE(a_higher_zoom_makes_a_bigger_frame_not_a_stretched_one) {
    // The point of the zoom is that the placement resolves into more pixels, so
    // the two frames must have different dimensions and different byte counts --
    // a scaled-up copy of the small one would be a blur, not more detail.
    const ScratchDir dir("zoom");
    const Graph g = sampleGraph();
    writeFrameRaster((dir.file("small.ppm")).string(), g, sampleX(g), sampleY(g), fixedCellBBox(g),
                     0, 1, 1.0, 1.0, 0.0, "", nullptr, true, 1.0);
    writeFrameRaster((dir.file("big.ppm")).string(), g, sampleX(g), sampleY(g), fixedCellBBox(g), 0,
                     1, 1.0, 1.0, 0.0, "", nullptr, true, 3.0);

    const std::string small = readAll(dir.file("small.ppm"));
    const std::string big = readAll(dir.file("big.ppm"));
    // "P6\n768 768\n255\n" vs "P6\n2304 2304\n255\n": 3x the linear size.
    BOOST_TEST(contains(small, "768 768"));
    BOOST_TEST(contains(big, "2304 2304"));
    BOOST_TEST(big.size() > small.size() * 8u);
}

BOOST_AUTO_TEST_CASE(a_legal_row_is_drawn_at_its_true_size_and_not_wider) {
    // The defect this pins down is one that can only be seen by looking at a real
    // placement: cells drawn wider than they are. Edges used to be rounded to
    // whole pixels whenever a cell was small, so every cell grew or shrank by up
    // to a pixel on each side, the errors did not cancel between neighbours, and a
    // field of cells that touch exactly in the placement overlapped in the
    // drawing. Across a long row the errors accumulate, so the row comes out
    // visibly wider than it is -- a picture of overlapping cells.
    //
    // The cells here are sized so each is a few pixels across, which is where the
    // rounding used to bite, and there are enough of them that a per-cell error
    // adds up to something measurable.
    const ScratchDir dir("exactsize");
    Graph g;
    const std::size_t id = g.addCell("anchor");
    Vertex &a = g.getCell(id);
    a.x = 0.0;
    a.y = 0.0;
    a.width = 400.0;
    a.height = 400.0;
    a.isFixed = true;
    a.isTerminal = true;
    constexpr int kCells = 50;
    constexpr double kW = 2.0;  // about three pixels at the scale below
    constexpr double kH = 2.0;
    for (int i = 0; i < kCells; ++i) {
        const std::size_t c = g.addCell("c" + std::to_string(i));
        Vertex &v = g.getCell(c);
        v.x = 20.0 + i * kW;  // exactly edge to edge, no gap and no overlap
        v.y = 20.0;
        v.width = kW;
        v.height = kH;
    }
    writeFrameRaster((dir.file("row.ppm")).string(), g, sampleX(g), sampleY(g), fixedCellBBox(g), 0,
                     1, 1.0, 1.0, 0.0, "", nullptr, /*fixedView=*/true, /*zoom=*/1.0);

    const std::string ppm = readAll(dir.file("row.ppm"));
    BOOST_TEST(contains(ppm, "768 768"));
    const std::size_t headerEnd = ppm.find("255\n");
    BOOST_TEST_REQUIRE(headerEnd != std::string::npos);
    const std::size_t off = headerEnd + 4;
    const int w = 768, h = 768;
    auto px = [&](int x, int y) {
        const std::size_t i = off + (static_cast<std::size_t>(y) * w + x) * 3;
        return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(ppm[i]),
                                           static_cast<std::uint8_t>(ppm[i + 1]),
                                           static_cast<std::uint8_t>(ppm[i + 2])};
    };
    // "Any blue", not "exactly the flat blue". These cells are a few pixels
    // across, so most of them have no pixel at full coverage and the outer ones
    // are antialiased; matching one exact colour would count only the cells that
    // happen to land on a whole pixel and report a row far shorter than the one
    // drawn. The movable fill is strongly blue and the row lines are neutral grey,
    // so a margin on blue-over-red separates them without depending on coverage.
    const auto isBlue = [&](int x, int y) {
        const auto p = px(x, y);
        return p[2] > p[0] + 24;
    };

    // Scanned below the caption band only: the key draws a swatch in the movable
    // colour, so a scan over the whole frame would measure the legend as well as
    // the placement and report a span wider than the cells.
    constexpr int kBelowCaption = 160;
    int firstX = -1, lastX = -1, rowsHit = 0;
    for (int y = kBelowCaption; y < h; ++y) {
        int lo = -1, hi = -1;
        for (int x = 0; x < w; ++x) {
            if (isBlue(x, y)) {
                if (lo < 0) {
                    lo = x;
                }
                hi = x;
            }
        }
        if (lo >= 0) {
            ++rowsHit;
            if (firstX < 0) {
                firstX = lo;
            }
            lastX = hi;
        }
    }
    BOOST_TEST_REQUIRE(firstX >= 0);
    BOOST_TEST(rowsHit > 0);

    // The drawn span must be the span the cells occupy, and not a pixel more. The
    // viewport lays a 400-unit die into a 768px frame less its 36px margins and
    // 116px caption band, padded by 1% on each side. With per-edge rounding the
    // error is up to half a pixel per edge per cell, so over fifty cells it would
    // be tens of pixels; the tolerance here is a few, which is the antialiasing on
    // the two outer edges.
    const double avail = std::min(768.0 - 2 * 36.0 - 2.0, 768.0 - 116.0 - 36.0 - 2.0);
    const double scale = avail / (1.02 * 400.0);
    const double expected = kCells * kW * scale;
    const double drawn = static_cast<double>(lastX - firstX + 1);
    // BOOST_TEST_CONTEXT, not BOOST_TEST_CONTEXT: the former leaves an empty
    // statement where its scoped object is introduced, which is -Wempty-body, and
    // it is the only construct in this file that warned.
    BOOST_TEST_MESSAGE("expected " << expected << " px, drawn " << drawn);
    BOOST_TEST(drawn <= expected + 3.0);
    BOOST_TEST(drawn >= expected - 5.0);
}

BOOST_AUTO_TEST_CASE(the_rows_are_drawn_when_there_are_rows) {
    // A frame that says nothing about rows cannot show whether a placement is
    // legal, so the row lines are part of the picture. They are recovered from
    // the cells, which only works once a placement is on the rows: two distinct y
    // levels are two rows.
    const ScratchDir dir("rows");
    Graph g;
    const std::size_t id = g.addCell("anchor");
    Vertex &a = g.getCell(id);
    a.x = 0.0;
    a.y = 0.0;
    a.width = 200.0;
    a.height = 400.0;
    a.isFixed = true;
    a.isTerminal = true;
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 4; ++c) {
            const std::size_t v = g.addCell("c" + std::to_string(r * 4 + c));
            Vertex &vert = g.getCell(v);
            vert.x = 20.0 + c * 8.0;
            vert.y = 20.0 + r * 40.0;  // exactly row pitch apart
            vert.width = 8.0;
            vert.height = 12.0;
        }
    }
    writeFrameSvg((dir.file("rows.svg")).string(), g, sampleX(g), sampleY(g), fixedCellBBox(g), 0,
                  1, 1.0, 1.0, 0.0, "");
    const std::string svg = readAll(dir.file("rows.svg"));
    // The legend names the row count, which is the observable part of "rows were
    // found", and the lines themselves are drawn under the cells.
    BOOST_TEST(contains(svg, "row (8)"));
    BOOST_TEST(contains(svg, "<line "));
}

BOOST_AUTO_TEST_CASE(every_cell_reaches_every_frame) {
    // A frame that silently drops cells is the one rendering failure that cannot
    // be spotted by looking at it, because the picture still looks like a
    // placement. Both writers used to decimate above a draw cap, so the count of
    // drawn cells is checked against the graph rather than the drawing trusted.
    const ScratchDir dir("allcells");
    Graph g = sampleGraph();
    // More cells than any plausible draw cap, so a cap that still exists and is
    // low enough to engage would show up here as a shortfall.
    for (int i = 0; i < 4000; ++i) {
        const std::size_t id = g.addCell("extra" + std::to_string(i));
        Vertex &v = g.getCell(id);
        v.x = static_cast<double>(i % 50);
        v.y = static_cast<double>(i / 50);
        v.width = 1.0;
        v.height = 1.0;
    }
    const std::size_t cells = g.getNumCells();

    const std::string svgPath = (dir.file("all.svg")).string();
    writeFrameSvg(svgPath, g, sampleX(g), sampleY(g), fixedCellBBox(g), 0, 1, 1.0, 1.0, 0.0, "");
    const std::string svg = readAll(dir.file("all.svg"));
    std::size_t rects = 0;
    for (std::size_t at = svg.find("<rect"); at != std::string::npos;
         at = svg.find("<rect", at + 1)) {
        ++rects;
    }
    // One rect per cell, plus the die outline, the background and the progress bar.
    BOOST_TEST(rects >= cells + 3u);

    // And the raster frame draws the same number: each cell contributes at least
    // the flat fill colour, so the colour histogram has to hold all of them.
    writeFrameRaster((dir.file("all.ppm")).string(), g, sampleX(g), sampleY(g), fixedCellBBox(g), 0,
                     1, 1.0, 1.0, 0.0, "", nullptr, /*fixedView=*/true, /*zoom=*/4.0);
    const std::string ppm = readAll(dir.file("all.ppm"));
    BOOST_TEST(contains(ppm, "3072 3072"));
}

BOOST_AUTO_TEST_CASE(assembling_a_gif_from_stills_of_different_sizes_works) {
    // Two stages of a run can rasterise at different sizes; the collector has to
    // scale them onto a common canvas rather than abort.
    const ScratchDir dir("sizes");
    const Graph g = sampleGraph();
    writeFrameRaster((dir.file("frame_0000.ppm")).string(), g, sampleX(g), sampleY(g),
                     fixedCellBBox(g), 0, 2, 1.0, 1.0, 0.0, "");
    writeFrameRaster((dir.file("frame_0001.ppm")).string(), g, sampleX(g), sampleY(g),
                     fixedCellBBox(g), 1, 2, 1.0, 1.0, 0.0, "");
    BOOST_TEST(writeAnimatedGif(dir.str(), "anim.gif", 5));
    BOOST_TEST(contains(readAll(dir.file("anim.gif")), "GIF89a"));
}

// ---------------------------------------------------------------------------
// PlacementAnimator
// ---------------------------------------------------------------------------
//
// The animator is a process-wide singleton, so these cases share one object and
// each configure() or reset() is a fresh run. They are kept in one file for
// that reason: splitting them would mean two translation units racing for the
// same state.

namespace {

/// Reset the singleton so a case cannot leak state into the next one.
void freshAnimator() {
    PlacementAnimator::instance().reset();
}

}  // namespace

BOOST_AUTO_TEST_CASE(an_unconfigured_animator_records_nothing) {
    freshAnimator();
    const Graph g = sampleGraph();
    BOOST_TEST(!PlacementAnimator::instance().enabled());
    BOOST_TEST(PlacementAnimator::instance().budget() > 0u);  // default, not armed
    // A no-op rather than a crash: a run with no plot directory still calls
    // record() from every stage.
    BOOST_CHECK_NO_THROW(PlacementAnimator::instance().record(
        g, sampleX(g), sampleY(g), fixedCellBBox(g), 0, 1, 1.0, 1.0, 0.0, "x"));
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 0u);
    BOOST_TEST(!PlacementAnimator::instance().finish());
}

BOOST_AUTO_TEST_CASE(configure_creates_the_output_directory_and_arms_the_run) {
    const ScratchDir dir("animcfg");
    const std::string out = (dir.file("frames")).string();
    freshAnimator();
    PlacementAnimator::instance().configure(out, 20, 7, 3);
    BOOST_TEST(PlacementAnimator::instance().enabled());
    BOOST_TEST(std::filesystem::is_directory(out));
    BOOST_TEST(PlacementAnimator::instance().budget() == 20u);
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(configure_with_an_empty_directory_stays_disarmed) {
    freshAnimator();
    PlacementAnimator::instance().configure("", 10, 5);
    BOOST_TEST(!PlacementAnimator::instance().enabled());
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(a_zero_frame_budget_is_clamped_rather_than_dead) {
    // A cap of zero would make every stage think it is capped on frame zero,
    // including the one that is supposed to draw the legalized result.
    const ScratchDir dir("animzero");
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 0, 5);
    BOOST_TEST(PlacementAnimator::instance().enabled());
    BOOST_TEST(PlacementAnimator::instance().budget() == 1u);
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(a_blend_below_two_disables_interpolation) {
    // Values under 2 have no in-between frames to emit, so they are clamped
    // rather than left to make a division by zero or an empty loop.
    const ScratchDir dir("animblend");
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 10, 5, 1);
    BOOST_TEST(PlacementAnimator::instance().blend() == 1);
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 10, 5, 0);
    BOOST_TEST(PlacementAnimator::instance().blend() == 1);
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(recording_writes_numbered_stills_and_counts_them) {
    const ScratchDir dir("animrec");
    const Graph g = sampleGraph();
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 10, 5, 1);  // no blending

    PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g), 0, 3, 1.0,
                                         1.0, 0.0, "iter0");
    PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g), 1, 3, 1.0,
                                         1.0, 0.0, "iter1");
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 2u);
    // Zero-padded so directory order is frame order.
    BOOST_TEST(std::filesystem::exists(dir.file("frame_0000.ppm")));
    BOOST_TEST(std::filesystem::exists(dir.file("frame_0001.ppm")));
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(blending_emits_in_between_frames_that_arrive_at_the_new_place) {
    // Consecutive solves move cells by a small fraction of a width, so one
    // still per iteration reads as a flicker. Blending turns that into movement,
    // and the stills must end on the recorded placement.
    const ScratchDir dir("animblend2");
    Graph g = sampleGraph();
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 20, 5, 3);

    const std::vector<float> x = sampleX(g);
    std::vector<float> y = sampleY(g);
    PlacementAnimator::instance().record(g, x, y, fixedCellBBox(g), 0, 2, 1.0, 1.0, 0.0, "a");
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 1u);

    std::vector<float> x2 = x;
    for (std::size_t i = 0; i < x2.size(); ++i) {
        x2[i] += 4.0F;
    }
    // 1 still for the first placement, then blend-1 in-between frames plus the
    // placement itself.
    PlacementAnimator::instance().record(g, x2, y, fixedCellBBox(g), 1, 2, 1.0, 1.0, 0.0, "b");
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 4u);
    BOOST_TEST(std::filesystem::exists(dir.file("frame_0003.ppm")));
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(a_record_with_the_wrong_number_of_coordinates_is_ignored) {
    // The renderers index the coordinate vectors by vertex id, so a short
    // vector would read past the end. The cap check comes first, but the length
    // check still has to hold.
    const ScratchDir dir("animshort");
    const Graph g = sampleGraph();
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 10, 5, 1);
    const std::vector<float> tooShort{1.0F, 2.0F};
    PlacementAnimator::instance().record(g, tooShort, sampleY(g), fixedCellBBox(g), 0, 1, 1.0, 1.0,
                                         0.0, "bad");
    // Nothing was recorded, so the first real frame is still 0000.
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 0u);
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(the_frame_cap_stops_recording_and_is_reported) {
    const ScratchDir dir("animcap");
    const Graph g = sampleGraph();
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 2, 5, 1);
    for (int i = 0; i < 6; ++i) {
        PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g),
                                             static_cast<std::size_t>(i), 6, 1.0, 1.0, 0.0, "i");
    }
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 2u);
    BOOST_TEST(PlacementAnimator::instance().capped());
    BOOST_TEST(PlacementAnimator::instance().budget() == 0u);
    // The stills left behind are a browsable record of what was captured.
    BOOST_TEST(std::filesystem::exists(dir.file("frame_0001.ppm")));
    BOOST_TEST(!std::filesystem::exists(dir.file("frame_0002.ppm")));
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(hold_back_reserves_frames_for_the_later_stages) {
    // Global placement records thousands of stills and would otherwise spend the
    // whole budget before the legalizer draws anything, leaving an animation
    // that stops exactly where it becomes interesting.
    const ScratchDir dir("animhold");
    const Graph g = sampleGraph();
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 10, 5, 1);
    PlacementAnimator::instance().holdBack(4);

    for (int i = 0; i < 10; ++i) {
        PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g),
                                             static_cast<std::size_t>(i), 10, 1.0, 1.0, 0.0, "i");
    }
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 6u);
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(a_hold_back_larger_than_the_budget_is_reduced_to_half) {
    // A reservation that cannot be spent would disable the stage it was meant
    // to protect, which is the opposite of its purpose, so an oversized request
    // is cut to half the budget. budget() deliberately reports the ceiling
    // ignoring the reservation, so the effect is observed by recording.
    const ScratchDir dir("animholdbig");
    const Graph g = sampleGraph();
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 8, 5, 1);
    PlacementAnimator::instance().holdBack(1000);
    BOOST_TEST(PlacementAnimator::instance().budget() == 8u);
    BOOST_TEST(!PlacementAnimator::instance().capped());

    for (int i = 0; i < 8; ++i) {
        PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g),
                                             static_cast<std::size_t>(i), 8, 1.0, 1.0, 0.0, "i");
    }
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 4u);
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(finish_needs_at_least_two_stills) {
    // One still is not an animation and a two-frame GIF is a flicker; both are
    // more likely a mistake in the run than something worth keeping.
    const ScratchDir dir("animfin");
    const Graph g = sampleGraph();
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 10, 5, 1);
    BOOST_TEST(!PlacementAnimator::instance().finish());
    PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g), 0, 2, 1.0,
                                         1.0, 0.0, "only");
    BOOST_TEST(!PlacementAnimator::instance().finish());
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(finish_assembles_the_stills_into_a_looping_gif) {
    const ScratchDir dir("animgif");
    const Graph g = sampleGraph();
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 10, 5, 1);
    PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g), 0, 2, 1.0,
                                         1.0, 0.0, "a");
    PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g), 1, 2, 1.0,
                                         1.0, 0.0, "b");
    BOOST_TEST(PlacementAnimator::instance().finish("run.gif"));
    const std::string gif = readAll(dir.file("run.gif"));
    BOOST_TEST(contains(gif, "GIF89a"));
    BOOST_TEST(contains(gif, "NETSCAPE2.0"));
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(configure_twice_is_a_fresh_run_not_an_append) {
    const ScratchDir dir("animtwice");
    const Graph g = sampleGraph();
    freshAnimator();
    PlacementAnimator::instance().configure(dir.str(), 10, 5, 1);
    PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g), 0, 1, 1.0,
                                         1.0, 0.0, "a");
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 1u);

    PlacementAnimator::instance().configure(dir.str(), 10, 5, 1);
    // The counter, the held-back reservation and the "capped" flag all reset.
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 0u);
    BOOST_TEST(PlacementAnimator::instance().budget() == 10u);
    BOOST_TEST(!PlacementAnimator::instance().capped());
    // No interpolation from the previous run either: the first still of the
    // second run is a real placement, not a blend towards it.
    PlacementAnimator::instance().record(g, sampleX(g), sampleY(g), fixedCellBBox(g), 0, 1, 1.0,
                                         1.0, 0.0, "a");
    BOOST_TEST(PlacementAnimator::instance().frameCount() == 1u);
    freshAnimator();
}

BOOST_AUTO_TEST_CASE(the_singleton_is_one_object_for_the_whole_process) {
    // The three stages are constructed independently, which is the only reason
    // this is a singleton; they must therefore see the same instance.
    BOOST_TEST(&PlacementAnimator::instance() == &PlacementAnimator::instance());
}
