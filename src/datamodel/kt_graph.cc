// @file kt_graph.cc

#include "datamodel/kt_graph.h"

#include <stdexcept>

namespace ktplace {

const std::vector<std::size_t> Graph::kNoPins;

Graph::Graph() = default;
Graph::~Graph() = default;

std::size_t Graph::addCell(const std::string &name) {
    if (cellByName.find(name) != cellByName.end()) {
        throw std::runtime_error("Cell " + name + " already exists");
    }
    Vertex cell;
    cell.id = cells.size();
    cell.name = name;
    cells.push_back(std::move(cell));
    cellPinIds.emplace_back();
    cellByName[name] = cells.size() - 1;
    return cells.size() - 1;
}

bool Graph::hasCell(const std::string &name) const {
    return cellByName.find(name) != cellByName.end();
}

std::size_t Graph::getCellId(const std::string &name) const {
    const auto it = cellByName.find(name);
    if (it == cellByName.end()) {
        throw std::runtime_error("Vertex " + name + " not found");
    }
    return it->second;
}

const std::vector<Vertex> &Graph::getCells() const {
    return cells;
}

Vertex &Graph::getCell(std::size_t id) {
    if (id >= cells.size() || cells[id].id != id) {
        throw std::runtime_error("Invalid vertex ID");
    }
    return cells[id];
}

const Vertex &Graph::getCell(std::size_t id) const {
    if (id >= cells.size() || cells[id].id != id) {
        throw std::runtime_error("Invalid vertex ID");
    }
    return cells[id];
}

std::size_t Graph::getNumCells() const {
    return cells.size();
}

std::size_t Graph::addNet(const std::string &name, double weight) {
    if (netByName.find(name) != netByName.end()) {
        throw std::runtime_error("Net " + name + " already exists");
    }
    Net net;
    net.id = nets.size();
    net.name = name;
    net.weight = weight;
    nets.push_back(std::move(net));
    netPinIds.emplace_back();
    netByName[name] = nets.size() - 1;
    return nets.size() - 1;
}

bool Graph::hasNet(const std::string &name) const {
    return netByName.find(name) != netByName.end();
}

std::size_t Graph::getNetId(const std::string &name) const {
    const auto it = netByName.find(name);
    if (it == netByName.end()) {
        throw std::runtime_error("Net " + name + " not found");
    }
    return it->second;
}

const std::vector<Net> &Graph::getNets() const {
    return nets;
}

const Net &Graph::getNet(std::size_t id) const {
    if (id >= nets.size() || nets[id].id != id) {
        throw std::runtime_error("Invalid net ID");
    }
    return nets[id];
}

std::size_t Graph::getNumNets() const {
    return nets.size();
}

std::size_t Graph::addPin(std::size_t cellId, std::size_t netId, PinRole role, double offsetX,
                          double offsetY) {
    if (cellId >= cells.size()) {
        throw std::runtime_error("Vertex " + std::to_string(cellId) + " not found");
    }
    if (netId >= nets.size()) {
        throw std::runtime_error("Net " + std::to_string(netId) + " not found");
    }
    Pin pin;
    pin.id = pins.size();
    pin.cellId = cellId;
    pin.netId = netId;
    pin.role = role;
    pin.offsetX = offsetX;
    pin.offsetY = offsetY;
    pins.push_back(pin);
    netPinIds[netId].push_back(pins.size() - 1);
    cellPinIds[cellId].push_back(pins.size() - 1);
    return pins.size() - 1;
}

const std::vector<Pin> &Graph::getPins() const {
    return pins;
}

const Pin &Graph::getPin(std::size_t id) const {
    if (id >= pins.size() || pins[id].id != id) {
        throw std::runtime_error("Invalid pin ID");
    }
    return pins[id];
}

std::size_t Graph::getNumPins() const {
    return pins.size();
}

const std::vector<std::size_t> &Graph::getNetPins(std::size_t netId) const {
    if (netId >= netPinIds.size()) {
        throw std::runtime_error("Invalid net ID");
    }
    return netPinIds[netId];
}

const std::vector<std::size_t> &Graph::getCellPins(std::size_t cellId) const {
    if (cellId >= cellPinIds.size()) {
        throw std::runtime_error("Invalid vertex ID");
    }
    return cellPinIds[cellId];
}

void Graph::clear() {
    cells.clear();
    nets.clear();
    pins.clear();
    cellByName.clear();
    netByName.clear();
    netPinIds.clear();
    cellPinIds.clear();
}

}  // namespace ktplace
