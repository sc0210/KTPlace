// @file kt_solutionMgr.h
// The one mutable part of a design, and the two views a stage works from.

#pragma once

#include "datamodel/kt_graph.h"

#include <cstddef>
#include <vector>

namespace ktplace {

/// Half-perimeter wirelength of a placement, computed from a netlist and the
/// positions that go with it.
///
/// A free function taking the graph and the coordinates, because that is what a
/// stage actually has: it is working on its own positions and has not committed
/// them to the design yet. There were three HPWL implementations before this --
/// ktDM::hpwl() over the committed placement, SimplePlacer::Impl::hpwl() over its
/// own arrays, and FastDP::segmentHpwl() over a cell subset -- and this is the
/// shape the plotter already takes (writeFrameSvg(path, g, x, y, ...)).
///
/// @p x and @p y are indexed by vertex id and must cover every vertex. Entries
/// for net vertices are ignored.
[[nodiscard]] double netlistHPWL(const Graph &graph, const std::vector<double> &x,
                                 const std::vector<double> &y);

/// A placement a stage produced and ktDM will accept: the positions of every
/// vertex, indexed by vertex id.
///
/// This is what a stage produces and what a stage consumes, and the only part of
/// a design that changes between stages. Everything else about a vertex -- its
/// name, type, width, height, whether it is fixed, which net it is on -- is
/// fixed once parsed.
///
/// It is a value type, not a view of the design. A stage builds one, moves cells
/// around in it, and hands it back; nothing writes to the design until the flow
/// calls ktDM::setPlacementSolution(). That is the whole point: a stage cannot
/// leave the design half-updated, and a stage can be tested against a
/// solutionMgr without a ktDM at all.
class solutionMgr {
public:
    solutionMgr() = default;

    /// Sized for a graph of @p numVertices vertices. Vertices are indexed by id,
    /// so this is getNumVertices(), not the number of cells -- nets occupy ids
    /// too, and leaving them zero is what keeps the indexing identical to the
    /// graph's.
    explicit solutionMgr(std::size_t numVertices) {
        resize(numVertices);
    }

    /// Resize, zero-filling. The positions of a design being read are the design's
    /// own; this is how a Solution is seeded from them.
    void resize(std::size_t numVertices) {
        x_.assign(numVertices, 0.0);
        y_.assign(numVertices, 0.0);
    }

    /// The positions currently in the design, for a graph of this shape.
    [[nodiscard]] static solutionMgr fromGraph(const Graph &graph);

    /// Write these positions into @p graph's vertices.
    ///
    /// The only path from a stage back to the design. A vertex that is not a cell
    /// keeps the net data it already had: a Solution carries no net information
    /// and must not clear any.
    void commitTo(Graph &graph) const;

    [[nodiscard]] double x(std::size_t vertexId) const {
        return x_.at(vertexId);
    }
    [[nodiscard]] double y(std::size_t vertexId) const {
        return y_.at(vertexId);
    }
    void setX(std::size_t vertexId, double x) {
        x_.at(vertexId) = x;
    }
    void setY(std::size_t vertexId, double y) {
        y_.at(vertexId) = y;
    }

    [[nodiscard]] const std::vector<double> &xs() const {
        return x_;
    }
    [[nodiscard]] const std::vector<double> &ys() const {
        return y_;
    }

    [[nodiscard]] std::size_t size() const {
        return x_.size();
    }

private:
    std::vector<double> x_;
    std::vector<double> y_;
};

}  // namespace ktplace
