// @file kt_inputReader.h
// Reading a design into a ktDM

#pragma once

#include "datamodel/kt_dm.h"

#include <memory>
#include <string>
#include <string_view>

namespace ktplace {

// One input format.
//
// The flow chooses an adapter and then sees only this. Which format a directory
// holds, and what that format's files mean, stay inside the adapter: adding a
// format means adding a class and one line to makeInputReader, and touching
// neither the flow nor anything downstream of it.
class InputReader {
public:
    virtual ~InputReader() = default;

    // Whether dirPath holds input this format recognises. Used only to choose an
    // adapter, so it must not read the design.
    [[nodiscard]] virtual bool recognises(const std::string &dirPath) const = 0;

    // Reads the design and hands over its database, or returns nullptr if the
    // design could not be read. Anything the design declared beyond the netlist
    // -- LEF/DEF fences, for one -- is installed into that database before it is
    // returned, so the reader has no reason to outlive the call.
    [[nodiscard]] virtual std::unique_ptr<ktDM> read(const std::string &dirPath) = 0;

    [[nodiscard]] virtual std::string_view formatName() const = 0;

    // Placement constraints the format carried. Null for a format that has none.
    [[nodiscard]] virtual const constraintMgr *constraints() const {
        return nullptr;
    }
};

// The adapter for dirPath, chosen by what the directory contains. Null if no
// format recognises it.
[[nodiscard]] std::unique_ptr<InputReader> makeInputReader(const std::string &dirPath);

}  // namespace ktplace
