// @file kt_solutionMgr.cc

#include "datamodel/kt_solutionMgr.h"

#include <algorithm>
#include <limits>

namespace ktplace {

double netlistHPWL(const Graph &graph, const std::vector<double> &x, const std::vector<double> &y) {
    double total = 0.0;
    const std::size_t nn = graph.getNumNets();
    for (std::size_t n = 0; n < nn; ++n) {
        const std::vector<std::size_t> &pins = graph.getNetPins(n);
        if (pins.size() < 2) {
            continue;
        }
        double xlo = std::numeric_limits<double>::max();
        double xhi = -std::numeric_limits<double>::max();
        double ylo = std::numeric_limits<double>::max();
        double yhi = -std::numeric_limits<double>::max();
        for (const std::size_t pinId : pins) {
            const Pin &pin = graph.getPin(pinId);
            const Vertex &cell = graph.getCell(pin.cellId);
            // The cell's far edge, not just the pin: the offset says where the pin
            // sits on the cell, and the wire ends at the cell's other side.
            xlo = std::min(xlo, x[pin.cellId] + pin.offsetX);
            xhi = std::max(xhi, x[pin.cellId] + pin.offsetX + cell.width);
            ylo = std::min(ylo, y[pin.cellId] + pin.offsetY);
            yhi = std::max(yhi, y[pin.cellId] + pin.offsetY + cell.height);
        }
        total += (xhi - xlo) + (yhi - ylo);
    }
    return total;
}

solutionMgr solutionMgr::fromGraph(const Graph &graph) {
    solutionMgr s(graph.getNumCells());
    const std::size_t nv = graph.getNumCells();
    for (std::size_t v = 0; v < nv; ++v) {
        const Vertex &vert = graph.getCell(v);
        s.x_[v] = vert.x;
        s.y_[v] = vert.y;
    }
    return s;
}

void solutionMgr::commitTo(Graph &graph) const {
    const std::size_t nv = graph.getNumCells();
    const std::size_t n = std::min(nv, x_.size());
    for (std::size_t v = 0; v < n; ++v) {
        Vertex &vert = graph.getCell(v);
        vert.x = x_[v];
        vert.y = y_[v];
    }
}

}  // namespace ktplace
