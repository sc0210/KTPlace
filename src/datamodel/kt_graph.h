// @file kt_graph.h
// The netlist: what is in the design, and what is connected to what.

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace ktplace {

/// Which end of a net a pin sits at.
enum class PinRole {
    Driver,    ///< the cell drives the net
    Receiver,  ///< the cell receives it
};

/// A cell: a box that occupies space. Either a standard cell, which placement
/// may move, or a macro or an I/O pad, which it may not.
class Vertex {
public:
    std::size_t id = 0;
    std::string name;

    double width = 0.0;
    double height = 0.0;
    double x = 0.0;
    double y = 0.0;

    bool isTerminal = false;  ///< an I/O pad: fixed, and on the die boundary
    bool isFixed = false;     ///< a macro, or a pad

    /// The placement region this cell is fenced into (constraintMgr), or -1.
    int regionId = -1;
};

/// A net: the signal its pins share.
class Net {
public:
    std::size_t id = 0;
    std::string name;
    double weight = 1.0;
};

/// One end of a net: where it meets a cell, and which way it points.
class Pin {
public:
    std::size_t id = 0;
    std::size_t cellId = 0;
    std::size_t netId = 0;
    PinRole role = PinRole::Receiver;

    /// Where on the cell the pin sits, relative to the cell's lower-left corner.
    double offsetX = 0.0;
    double offsetY = 0.0;
};

/// The netlist: cells, nets, and the pins joining them.
///
/// Three lists rather than one graph of mixed vertices, so that a cell is a
/// thing that occupies space and a net is a thing that does not, and neither is
/// asked to carry the other's fields.
class Graph {
public:
    Graph();
    ~Graph();

    Graph(const Graph &) = delete;
    Graph &operator=(const Graph &) = delete;

    // Movable, so a test or a stage can hand a built graph back by value.
    Graph(Graph &&) noexcept = default;
    Graph &operator=(Graph &&) noexcept = default;

    // Cells
    [[nodiscard]] std::size_t addCell(const std::string &name);
    [[nodiscard]] bool hasCell(const std::string &name) const;
    [[nodiscard]] std::size_t getCellId(const std::string &name) const;
    [[nodiscard]] const std::vector<Vertex> &getCells() const;
    [[nodiscard]] Vertex &getCell(std::size_t id);
    [[nodiscard]] const Vertex &getCell(std::size_t id) const;
    [[nodiscard]] std::size_t getNumCells() const;

    // Nets
    [[nodiscard]] std::size_t addNet(const std::string &name, double weight = 1.0);
    [[nodiscard]] bool hasNet(const std::string &name) const;
    [[nodiscard]] std::size_t getNetId(const std::string &name) const;
    [[nodiscard]] const std::vector<Net> &getNets() const;
    [[nodiscard]] const Net &getNet(std::size_t id) const;
    [[nodiscard]] std::size_t getNumNets() const;

    // Pins
    [[nodiscard]] std::size_t addPin(std::size_t cellId, std::size_t netId, PinRole role,
                                     double offsetX, double offsetY);
    [[nodiscard]] const std::vector<Pin> &getPins() const;
    [[nodiscard]] const Pin &getPin(std::size_t id) const;
    [[nodiscard]] std::size_t getNumPins() const;

    /// Pin ids on a net, in the order they were added.
    [[nodiscard]] const std::vector<std::size_t> &getNetPins(std::size_t netId) const;

    /// Pin ids on a cell, in the order they were added.
    [[nodiscard]] const std::vector<std::size_t> &getCellPins(std::size_t cellId) const;

    void clear();

private:
    std::vector<Vertex> cells;
    std::vector<Net> nets;
    std::vector<Pin> pins;

    std::unordered_map<std::string, std::size_t> cellByName;
    std::unordered_map<std::string, std::size_t> netByName;

    /// pin ids per net, and per cell
    std::vector<std::vector<std::size_t>> netPinIds;
    std::vector<std::vector<std::size_t>> cellPinIds;

    static const std::vector<std::size_t> kNoPins;
};

}  // namespace ktplace
