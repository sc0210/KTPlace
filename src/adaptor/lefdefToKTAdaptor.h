// @file lefdefToKTAdaptor.h
// LEF/DEF format adapter
//
// Parses industry-standard LEF (physical library) and DEF (design) files into
// the internal ktDM. Supports the ISPD / ICCAD placement-contest style
// inputs (floorplan.def + cells.lef + tech.lef + design.v), where every standard
// cell is "UNPLACED", macros and I/O pads are placed/fixed, and net connectivity
// comes from the DEF NETS section.

#pragma once

#include "adaptor/kt_inputReader.h"
#include "constraint/kt_constraintMgr.h"
#include "datamodel/kt_dm.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ktplace {

// - cells.lef     -> cell sizes, pin locations/directions (micron)
// - tech.lef      -> ignored (no MACROs)
// - floorplan.def -> die area, rows, placed/fixed macros, I/O pads, and the flat
//                    component netlist
//
// DEF coordinate units (UNITS DISTANCE MICRONS) are respected: LEF sizes, which
// are in microns, are scaled into the DEF coordinate frame.
class LefDefInputAdapter final : public InputReader {
public:
    explicit LefDefInputAdapter(std::unique_ptr<ktDM> db = nullptr);

    ~LefDefInputAdapter() override;

    // Copy semantics (deleted)
    LefDefInputAdapter(const LefDefInputAdapter &) = delete;
    LefDefInputAdapter &operator=(const LefDefInputAdapter &) = delete;

    // Move semantics
    LefDefInputAdapter(LefDefInputAdapter &&) noexcept;
    LefDefInputAdapter &operator=(LefDefInputAdapter &&) noexcept;

    [[nodiscard]] bool recognises(const std::string &dirPath) const override;
    [[nodiscard]] std::unique_ptr<ktDM> read(const std::string &dirPath) override;
    [[nodiscard]] std::string_view formatName() const override {
        return "lefdef";
    }

    // Read the LEF/DEF files from a directory

    [[nodiscard]] bool readFromDirectory(const std::string &dirPath);

    // Read LEF/DEF format from explicit files

    [[nodiscard]] bool readFromFiles(const std::string &defFile,
                                     const std::vector<std::string> &lefFiles);

    // Get the ktDM object

    [[nodiscard]] ktDM &getDM() {
        return *db;
    }
    [[nodiscard]] const ktDM &getDM() const {
        return *db;
    }

    // Release ownership of the ktDM

    // Placement region ("fence") constraints read from the DEF.// Empty when the design declares no REGIONS/GROUPS.

    [[nodiscard]] const constraintMgr &getConstraints() const {
        return fences;
    }

    [[nodiscard]] std::unique_ptr<ktDM> releaseDM() {
        return std::move(db);
    }

private:
    // Per-macro record gathered from the LEF MACRO blocks.
    struct MacroRec {
        double widthMicrons = 1.0;
        double heightMicrons = 1.0;
        std::unordered_map<std::string, bool> pinIsInput;  // pin name -> isInput
        std::unordered_map<std::string, std::pair<double, double>>
            pinOffset;  // pin -> (x,y) microns
        std::unordered_map<std::string, bool> pinExists;
    };

    // A single pin reference inside a DEF net: either an instance pin
    // (inst != "") or an I/O pad (ioPin != "").
    struct NetPinRef {
        std::string inst;   // instance name (sanitised)
        std::string pin;    // cell pin name
        std::string ioPin;  // I/O pad name when this is a pin-level ref
    };

    bool parseLefFile(const std::string &filePath);
    bool parseDefFile(const std::string &filePath);
    void peekDefUnits(const std::string &filePath);

    std::vector<std::string> static tokenize(const std::string &line);
    std::string static sanitizeName(const std::string &name);

    // Internal state
    std::unique_ptr<ktDM> db;
    std::unordered_map<std::string, MacroRec> macros;        // LEF macro -> record
    std::unordered_map<std::string, std::string> instMacro;  // DEF inst -> macro
    double unitsPerMicron = 1.0;                             // DEF UNITS DISTANCE MICRONS
    double siteWidthMicrons = 0.0;                           // LEF SITE core size (micron)
    double siteHeightMicrons = 0.0;
    constraintMgr fences;  ///< REGIONS/GROUPS from the DEF
};

}  // namespace ktplace
