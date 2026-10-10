// @file lefdefToKTAdaptor.cc// Implementation of the LEF/DEF format adapter// Parses the contest-style LEF/DEF benchmark format:// - LEF MACRO blocks provide cell dimensions (microns) and pin// locations/directions.// - The DEF file provides the die area, row sites, the instance list// (components) with their placement status, the I/O pads (pins), and// the flat signal netlist (nets).// DEF coordinates already use the DEF unit scale (UNITS DISTANCE MICRONS);// LEF dimensions, which are in microns, are scaled by that factor so both// live in the same coordinate frame.


#include "adaptor/lefdefToKTAdaptor.h"

#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

// Boost.Iostreams - transparent gzip input
#include <boost/iostreams/device/file.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>

namespace ktplace {

namespace {

// Gzip-aware text file. If the path ends in ".gz" the stream is decompressed
// on the fly with Boost.Iostreams; otherwise it is read as plain text.
class InputTextFile {
public:
    explicit InputTextFile(const std::string &path) {
        const bool gz = path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0;
        if (gz) {
            in.push(boost::iostreams::gzip_decompressor());
            in.push(boost::iostreams::file_source(path, std::ios::binary));
        } else {
            in.push(boost::iostreams::file_source(path));
        }
    }

    std::istream &stream() {
        return in;
    }
    explicit operator bool() const {
        return static_cast<bool>(in);
    }

private:
    boost::iostreams::filtering_istream in;
};

// Try to parse a double, returning false on failure (avoids stod exceptions
// hitting the caller).
bool tryDouble(const std::string &s, double &out) {
    try {
        std::size_t used = 0;
        out = std::stod(s, &used);
        return used > 0;
    } catch (const std::exception &) {
        return false;
    }
}

}  // namespace

std::vector<std::string> LefDefInputAdapter::tokenize(const std::string &line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string tok;
    while (iss >> tok) {
        tokens.push_back(tok);
    }
    return tokens;
}

std::string LefDefInputAdapter::sanitizeName(const std::string &name) {
    std::string out = name;
    if (!out.empty() && out[0] == '\\') {
        out.erase(0, 1);  // DEF escape prefix
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (c == '/' || c == '.' || c == '[' || c == ']' || c == '\\' || c == '(' || c == ')' ||
            c == '{' || c == '}') {
            out[i] = '_';
        }
    }
    return out;
}

LefDefInputAdapter::LefDefInputAdapter(std::unique_ptr<ktDM> database)
    : db(database ? std::move(database) : std::make_unique<ktDM>()) {}

LefDefInputAdapter::~LefDefInputAdapter() = default;

LefDefInputAdapter::LefDefInputAdapter(LefDefInputAdapter &&) noexcept = default;
LefDefInputAdapter &LefDefInputAdapter::operator=(LefDefInputAdapter &&) noexcept = default;

bool LefDefInputAdapter::recognises(const std::string &dirPath) const {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::directory_iterator it(dirPath, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (fs::path(name).extension() == ".def") {
            return true;
        }
    }
    return false;
}

std::unique_ptr<ktDM> LefDefInputAdapter::read(const std::string &dirPath) {
    if (!readFromDirectory(dirPath)) {
        return nullptr;
    }
    std::unique_ptr<ktDM> database = releaseDM();
    // The fences go into the database, so the adapter does not have to outlive the
    // placement it constrains.
    database->setConstraints(std::move(fences));
    return database;
}

bool LefDefInputAdapter::readFromDirectory(const std::string &dirPath) {
    namespace fs = std::filesystem;

    std::string defFile;
    std::vector<std::string> lefFiles;
    std::error_code ec;
    fs::directory_iterator it(dirPath, fs::directory_options::skip_permission_denied, ec);
    const fs::directory_iterator end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const std::string lower = [&]() {
            std::string l = name;
            std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return l;
        }();
        if ((lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".def") == 0) ||
            (lower.size() >= 7 && lower.compare(lower.size() - 7, 7, ".def.gz") == 0)) {
            if (defFile.empty() || lower.find("floorplan") != std::string::npos) {
                defFile = it->path().string();
            }
        }
        if ((lower.size() >= 4 && lower.compare(lower.size() - 4, 4, ".lef") == 0) ||
            (lower.size() >= 7 && lower.compare(lower.size() - 7, 7, ".lef.gz") == 0)) {
            lefFiles.push_back(it->path().string());
        }
    }
    if (ec) {
        ktlog.echo("cannot scan the LEF/DEF directory {}", dirPath);
        return false;
    }
    if (defFile.empty()) {
        ktlog.echo("no .def file found in {}", dirPath);
        return false;
    }
    return readFromFiles(defFile, lefFiles);
}

bool LefDefInputAdapter::readFromFiles(const std::string &defFile,
                                       const std::vector<std::string> &lefFiles) {
    // Timed per format for the same reason as Bookshelf: a LEF/DEF load is
    // dominated by whichever file is larger, and the LEF files in particular are
    // numerous and individually small, so a single "lefdef" number would hide
    // which side of the load is slow.
    {
        ScopedTimer timer("lefdef-units");
        // DEF coordinate scale must be known before LEF micron sizes are used.
        peekDefUnits(defFile);
    }

    if (!lefFiles.empty()) {
        ScopedTimer timer("lefdef-lef");
        for (const std::string &lef : lefFiles) {
            if (!parseLefFile(lef)) {
                return false;
            }
        }
    }

    ScopedTimer timer("lefdef-def");
    return parseDefFile(defFile);
}

void LefDefInputAdapter::peekDefUnits(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        return;
    }
    unitsPerMicron = 1.0;
    std::string line;
    while (std::getline(file.stream(), line)) {
        if (line.find("UNITS") == std::string::npos && line.find("MICRONS") == std::string::npos) {
            continue;
        }
        const auto tokens = tokenize(line);
        for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
            if (tokens[i] == "MICRONS") {
                double v = 1.0;
                if (tryDouble(tokens[i + 1], v) && v > 0.0) {
                    unitsPerMicron = v;
                }
                return;
            }
        }
        return;
    }
}

bool LefDefInputAdapter::parseLefFile(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        ktlog.echo("cannot open the LEF file {}", filePath);
        return false;
    }

    std::string line;
    bool inMacro = false;
    std::string macro;
    MacroRec rec;
    bool inPin = false;
    std::string pin;
    bool inSite = false;
    bool inPropDefs = false;
    std::string site;
    std::size_t lineNum = 0;

    while (std::getline(file.stream(), line)) {
        ++lineNum;
        const auto tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }

        // PROPERTYDEFINITIONS is metadata this parser has no use for, but it
        // cannot simply be walked past: it contains lines that begin with MACRO
        // (property *types* are named after what they describe), and taking one
        // of those for a macro definition puts the parser into macro mode. The
        // block then closes with "END PROPERTYDEFINITIONS", whose name does not
        // match the macro it thinks it is inside, so macro mode never ends -- and
        // every later top-level statement in the file, SITE included, is skipped
        // because they are all guarded on !inMacro.
        //
        // That is not hypothetical: it is why the ISPD 2015 designs came out with
        // no site height at all, which gave every DEF row a height of zero, which
        // left the placer's density grid with no available area and made the
        // overflow, the density term and the legalizer's region growth all read
        // from an empty map.
        if (inPropDefs) {
            if (tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "PROPERTYDEFINITIONS") {
                inPropDefs = false;
            }
            continue;
        }
        if (tokens[0] == "PROPERTYDEFINITIONS") {
            inPropDefs = true;
            continue;
        }

        if (!inMacro && !inSite) {
            if (tokens[0] == "MACRO" && tokens.size() >= 2) {
                inMacro = true;
                inPin = false;
                macro = sanitizeName(tokens[1]);
                rec = MacroRec();
            } else if (tokens[0] == "SITE" && tokens.size() >= 2) {
                inSite = true;
                site = tokens[1];
            } else if (tokens[0] == "UNITS") {
                // LEF may carry its own UNITS block; harmless to skip.
            }
            continue;
        }

        if (inSite) {
            if (tokens[0] == "SIZE" && tokens.size() >= 4) {
                double w = 0.0, h = 0.0;
                if (tryDouble(tokens[1], w)) {
                    siteWidthMicrons = w;
                }
                if (tryDouble(tokens[3], h)) {
                    siteHeightMicrons = h;
                }
            } else if (tokens[0] == "END") {
                inSite = false;
                site.clear();
            }
            continue;
        }

        if (tokens[0] == "SIZE" && tokens.size() >= 4) {
            double w = 1.0, h = 1.0;
            if (tryDouble(tokens[1], w)) {
                rec.widthMicrons = w;
            }
            if (tryDouble(tokens[3], h)) {
                rec.heightMicrons = h;
            }
        } else if (tokens[0] == "PIN" && tokens.size() >= 2) {
            inPin = true;
            pin = sanitizeName(tokens[1]);
            rec.pinExists[pin] = true;
        } else if (tokens[0] == "DIRECTION" && inPin && tokens.size() >= 2) {
            const std::string &d = tokens[1];
            rec.pinIsInput[pin] = (d != "OUTPUT");
        } else if (tokens[0] == "RECT" && inPin && tokens.size() >= 5) {
            double x1 = 0.0, y1 = 0.0;
            tryDouble(tokens[1], x1);
            tryDouble(tokens[2], y1);
            rec.pinOffset[pin] = {x1, y1};
        } else if (tokens[0] == "END") {
            if (tokens.size() >= 2) {
                if (inPin && sanitizeName(tokens[1]) == pin) {
                    inPin = false;
                    pin.clear();
                } else if (sanitizeName(tokens[1]) == macro || tokens[1] == macro) {
                    macros[macro] = rec;
                    inMacro = false;
                }
            }
        }
    }
    return true;
}

bool LefDefInputAdapter::parseDefFile(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        ktlog.echo("cannot open the DEF file {}", filePath);
        return false;
    }

    std::string line;
    std::size_t lineNum = 0;
    int section = 0;  // 0=none, 1=COMPONENTS, 2=PINS, 3=NETS, 4=REGIONS, 5=GROUPS

    // REGIONS/GROUPS entries wrap over several lines, so tokens accumulate
    // until the terminating ';'.
    std::vector<std::string> regionTokens;
    std::vector<std::string> groupTokens;
    std::size_t numRegions = 0;
    std::size_t numGroupedCells = 0;
    // (region name, instance prefix) pairs, applied once the netlist is loaded
    // because GROUPS comes before COMPONENTS in the file.
    std::vector<std::pair<std::string, std::string>> pendingGroups;

    // Component entry under construction ("- inst macro" + continuation lines).
    std::string compName;
    std::string macroName;
    bool compSeen = false;  // inside a component entry
    bool compPlaced = false;
    bool compFixed = false;
    double compX = 0.0, compY = 0.0;

    // Pin entry under construction ("- pinName" + continuation lines).
    std::string pinName;
    bool pinSeen = false;
    bool pinPlaced = false;
    double pinX = 0.0, pinY = 0.0;

    // Nets: assembled token stream for the section.
    std::vector<std::string> netTokens;

    const auto flushComponent = [&]() {
        if (!compSeen) {
            return;
        }
        compSeen = false;
        const std::string cname = sanitizeName(compName);
        auto mIt = macros.find(sanitizeName(macroName));
        double w = unitsPerMicron, h = unitsPerMicron;
        if (mIt != macros.end()) {
            w = mIt->second.widthMicrons * unitsPerMicron;
            h = mIt->second.heightMicrons * unitsPerMicron;
        }
        (void)db->addCell(cname, w, h, false);
        if (compPlaced) {
            db->setCellPosition(cname, compX, compY);
        }
        if (compFixed) {
            db->setCellFixed(cname, true);
        }
        instMacro[cname] = sanitizeName(macroName);
    };

    const auto flushPin = [&]() {
        if (!pinSeen) {
            return;
        }
        pinSeen = false;
        const std::string pname = sanitizeName(pinName);
        auto mIt = macros.find(pname);
        double w = unitsPerMicron, h = unitsPerMicron;
        if (mIt != macros.end()) {
            w = mIt->second.widthMicrons * unitsPerMicron;
            h = mIt->second.heightMicrons * unitsPerMicron;
        } else {
            w = std::max(unitsPerMicron, 1.0);
            h = std::max(unitsPerMicron, 1.0);
        }
        (void)db->addCell(pname, w, h, true);  // I/O pads are terminals
        if (pinPlaced) {
            db->setCellPosition(pname, pinX, pinY);
        }
    };

    while (std::getline(file.stream(), line)) {
        ++lineNum;
        const auto tokens = tokenize(line);
        if (tokens.empty()) {
            continue;
        }

        // ---- Row / die / units lines (only valid outside sections) ----
        if (tokens[0] == "ROW" && tokens.size() >= 13) {
            double x = 0.0, y = 0.0, numX = 0.0, dx = 200.0, dy = 0.0;
            tryDouble(tokens[3], x);
            tryDouble(tokens[4], y);
            // ROW name site X Y orient DO numX BY numY STEP dx dy
            if (tokens.size() > 7) {
                tryDouble(tokens[7], numX);
            }
            for (std::size_t i = 8; i + 1 < tokens.size(); ++i) {
                if (tokens[i] == "STEP") {
                    tryDouble(tokens[i + 1], dx);
                    if (i + 2 < tokens.size()) {
                        tryDouble(tokens[i + 2], dy);
                    }
                }
            }
            // Many contest DEFs describe a single strip per ROW line with
            // "STEP <siteWidth> 0"; fall back to the LEF site height for the
            // row height in that case.
            double rowH = dy > 0.0 ? dy : siteHeightMicrons * unitsPerMicron;
            double siteW = dx > 0.0 ? dx : siteWidthMicrons * unitsPerMicron;
            if (siteW <= 0.0) {
                siteW = 1.0;
            }
            // A DEF ROW may carry several "DO n BY m STEP dx dy" segments, and
            // each is a separate contiguous run of sites, i.e. a subrow. Only
            // reading the first one modelled a row that is interrupted as one
            // unbroken span.
            struct Seg {
                double numX, dx, dy;
            };
            std::vector<Seg> segs;
            for (std::size_t i = 6; i + 1 < tokens.size(); ++i) {
                if (tokens[i] != "DO") {
                    continue;
                }
                Seg seg{0.0, dx, dy};
                tryDouble(tokens[i + 1], seg.numX);
                for (std::size_t j = i; j + 1 < tokens.size(); ++j) {
                    if (tokens[j] != "STEP") {
                        continue;
                    }
                    tryDouble(tokens[j + 1], seg.dx);
                    if (j + 2 < tokens.size()) {
                        tryDouble(tokens[j + 2], seg.dy);
                    }
                    i = j;
                    break;
                }
                segs.push_back(seg);
            }
            if (segs.empty()) {
                segs.push_back(Seg{numX, dx, dy});
            }
            const double h = segs.front().dy > 0.0 ? segs.front().dy : rowH;
            const std::size_t rowId = db->addRow(y, h, siteW, siteW);
            double ox = x;
            for (const Seg &sg : segs) {
                if (sg.numX > 0.0) {
                    (void)db->addSubrow(rowId, ox, sg.numX);
                    ox += sg.numX * siteW;
                }
            }
            continue;
        }
        if (tokens[0] == "DIEAREA") {
            // DIEAREA ( x0 y0 ) ( x1 y1 ) ;  -- collect the coordinates in
            // order rather than indexing blindly, since the parentheses are
            // separate tokens.
            std::vector<double> box;
            for (std::size_t i = 1; i < tokens.size() && box.size() < 4; ++i) {
                double value = 0.0;
                if (tryDouble(tokens[i], value)) {
                    box.push_back(value);
                }
            }
            if (box.size() < 4) {
                continue;  // malformed; keep whatever die area we had
            }
            double x0 = box[0];
            double y0 = box[1];
            double x1 = box[2];
            double y1 = box[3];
            if (x1 <= x0 || y1 <= y0) {
                x1 = std::max(x1, x0 + 1.0);
                y1 = std::max(y1, y0 + 1.0);
            }
            db->setDieArea(x0, y0, x1, y1);
            continue;
        }

        // ---- REGIONS: "- name ( x y ) ( x y ) ... + TYPE FENCE ;" ----
        if (section == 4) {
            for (const std::string &tok : tokens) {
                if (tok != ";") {
                    regionTokens.push_back(tok);
                    continue;
                }
                // Entry complete: "- <name> ( x y ) ( x y ) ... + TYPE FENCE".
                // Taking the numeric tokens in order yields the vertices; the
                // name is the first token after the leading '-'.
                if (regionTokens.size() >= 2) {
                    const std::string name = regionTokens[1];
                    std::vector<double> coords;
                    coords.reserve(regionTokens.size());
                    for (std::size_t i = 1; i < regionTokens.size(); ++i) {
                        double value = 0.0;
                        if (tryDouble(regionTokens[i], value)) {
                            coords.push_back(value);
                        }
                    }
                    std::vector<Point> polygon;
                    polygon.reserve(coords.size() / 2);
                    for (std::size_t i = 0; i + 1 < coords.size(); i += 2) {
                        polygon.push_back(Point{coords[i], coords[i + 1]});
                    }
                    if (fences.addRegion(name, std::move(polygon)) != constraintMgr::kNoRegion) {
                        ++numRegions;
                    }
                }
                regionTokens.clear();
            }
            if (tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "REGIONS") {
                regionTokens.clear();
                section = 0;
            }
            continue;
        }

        // ---- GROUPS: "- regionName instancePattern" then "+ REGION region ;" ----
        if (section == 5) {
            for (const std::string &tok : tokens) {
                if (tok == ";") {
                    // "- <region> <pattern>... + REGION <region>" -- a group may
                    // list several name patterns, e.g. "- er0 h0c/* h0a/* h0/*".
                    if (groupTokens.size() >= 2) {
                        const std::string regionName = sanitizeName(groupTokens[0]);
                        for (std::size_t i = 1; i < groupTokens.size(); ++i) {
                            std::string pattern = groupTokens[i];
                            if (!pattern.empty() && pattern.back() == '*') {
                                pattern.pop_back();
                            }
                            if (!pattern.empty()) {
                                pendingGroups.emplace_back(regionName, sanitizeName(pattern));
                            }
                        }
                    }
                    groupTokens.clear();
                } else if (tok != "-" && tok != "+" && tok != "REGION") {
                    groupTokens.push_back(tok);
                }
            }
            if (tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "GROUPS") {
                groupTokens.clear();
                section = 0;
            }
            continue;
        }

        // ---- Section transitions ----
        if (tokens[0] == "REGIONS") {
            section = 4;
            regionTokens.clear();
            continue;
        }
        if (tokens[0] == "GROUPS") {
            section = 5;
            groupTokens.clear();
            continue;
        }
        if (tokens[0] == "COMPONENTS") {
            section = 1;
            continue;
        }
        if (section == 1 && tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "COMPONENTS") {
            flushComponent();
            flushPin();
            section = 0;
            continue;
        }
        if (tokens[0] == "PINS") {
            flushComponent();
            flushPin();
            section = 2;
            continue;
        }
        if (section == 2 && tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "PINS") {
            flushPin();
            section = 0;
            continue;
        }
        if (tokens[0] == "NETS") {
            flushPin();
            netTokens.clear();
            section = 3;
            continue;
        }
        if (section == 3 && tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "NETS") {
            section = 0;
            continue;
        }
        if (tokens[0] == "END" && tokens.size() >= 2 && tokens[1] == "DESIGN") {
            flushComponent();
            flushPin();
            section = 0;
            continue;
        }

        // ---- COMPONENTS section ----
        if (section == 1) {
            if (tokens[0] == "-" && tokens.size() >= 3) {
                flushComponent();
                compName = tokens[1];
                macroName = tokens[2];
                compSeen = true;
                compPlaced = false;
                compFixed = false;
                compX = 0.0;
                compY = 0.0;
                // Same-line "+ ..." properties (e.g. "+ FIXED ( x y ) N ;")
                for (std::size_t i = 3; i < tokens.size(); ++i) {
                    if (tokens[i] == "PLACED" || tokens[i] == "FIXED") {
                        std::size_t c = i + 1;
                        if (c < tokens.size() && tokens[c] == "(") {
                            ++c;
                        }
                        double tx = 0.0, ty = 0.0;
                        if (c + 1 < tokens.size() && tryDouble(tokens[c], tx) &&
                            tryDouble(tokens[c + 1], ty)) {
                            compPlaced = true;
                            compFixed = (tokens[i] == "FIXED");
                            compX = tx;
                            compY = ty;
                        }
                    } else if (tokens[i] == "UNPLACED") {
                        compPlaced = false;
                    }
                }
            } else if (tokens[0] == "+") {
                if (tokens.size() >= 2 && tokens[1] == "PLACED" && tokens.size() >= 6) {
                    compPlaced = true;
                    tryDouble(tokens[3], compX);
                    tryDouble(tokens[4], compY);
                } else if (tokens.size() >= 2 && tokens[1] == "FIXED" && tokens.size() >= 6) {
                    compPlaced = true;
                    compFixed = true;
                    tryDouble(tokens[3], compX);
                    tryDouble(tokens[4], compY);
                } else if (tokens.size() >= 2 && tokens[1] == "UNPLACED") {
                    compPlaced = false;
                }
            }
            continue;
        }

        // ---- PINS section ----
        if (section == 2) {
            if (tokens[0] == "-" && tokens.size() >= 2) {
                flushPin();
                pinName = tokens[1];
                pinSeen = true;
                pinPlaced = false;
                pinX = 0.0;
                pinY = 0.0;
                for (std::size_t i = 2; i < tokens.size(); ++i) {
                    if (tokens[i] == "PLACED" || tokens[i] == "FIXED") {
                        std::size_t c = i + 1;
                        if (c < tokens.size() && tokens[c] == "(") {
                            ++c;
                        }
                        double tx = 0.0, ty = 0.0;
                        if (c + 1 < tokens.size() && tryDouble(tokens[c], tx) &&
                            tryDouble(tokens[c + 1], ty)) {
                            pinPlaced = true;
                            pinX = tx;
                            pinY = ty;
                        }
                    }
                }
            } else if (tokens[0] == "+" && pinSeen) {
                if (tokens.size() >= 2 && (tokens[1] == "PLACED" || tokens[1] == "FIXED") &&
                    tokens.size() >= 6) {
                    pinPlaced = true;
                    tryDouble(tokens[3], pinX);
                    tryDouble(tokens[4], pinY);
                }
            }
            continue;
        }

        // ---- NETS section ----
        if (section == 3) {
            // A DEF net can span multiple lines; accumulate raw tokens and
            // split them into nets at '-' markers and ';' terminators.
            netTokens.insert(netTokens.end(), tokens.begin(), tokens.end());
            continue;
        }
    }

    flushComponent();
    flushPin();

    // ---- Process the accumulated NETS token stream ----
    std::string netName;
    std::vector<NetPinRef> pins;
    bool haveNet = false;
    bool inGroup = false;
    std::vector<std::string> group;

    const auto resolveGroup = [&]() {
        if (group.empty()) {
            return;
        }
        NetPinRef ref;
        if (!group.empty() && group[0] == "PIN") {
            ref.ioPin = sanitizeName(group[1]);
        } else if (group.size() >= 2) {
            ref.inst = sanitizeName(group[0]);
            ref.pin = sanitizeName(group[1]);
        }
        group.clear();
        pins.push_back(std::move(ref));
    };

    const auto flushNet = [&]() {
        if (!haveNet) {
            return;
        }
        haveNet = false;
        std::string name = netName;
        netName.clear();
        if (name.empty()) {
            return;
        }
        // The graph keys every vertex (cells AND nets) by name globally, but
        // in DEF every I/O pad's net is named after the pad itself.  Disambiguate
        // the net so the pad cell keeps its own name.
        while (db->hasCell(name)) {
            name += "__NET";
        }
        if (!db->hasNet(name)) {
            (void)db->addNet(name, 1.0);
        }
        for (const NetPinRef &ref : pins) {
            std::string cell;
            double offsetX = 0.0, offsetY = 0.0;
            bool isInput = false;
            if (!ref.inst.empty()) {
                cell = ref.inst;
                auto it = instMacro.find(cell);
                if (it != instMacro.end()) {
                    auto mIt = macros.find(it->second);
                    if (mIt != macros.end()) {
                        const MacroRec &mr = mIt->second;
                        auto pOff = mr.pinOffset.find(ref.pin);
                        if (pOff != mr.pinOffset.end()) {
                            offsetX = pOff->second.first * unitsPerMicron;
                            offsetY = pOff->second.second * unitsPerMicron;
                        }
                        auto pIn = mr.pinIsInput.find(ref.pin);
                        if (pIn != mr.pinIsInput.end()) {
                            isInput = pIn->second;
                        }
                    }
                }
            } else {
                cell = ref.ioPin;
            }
            if (cell.empty() || !db->hasCell(cell)) {
                continue;
            }
            try {
                (void)db->addPin(cell, name, offsetX, offsetY, isInput);
            } catch (const std::exception &e) {
                ktlog.warning("DEF net '{}' pin '{}' skipped: {}", name, cell, e.what());
            }
        }
        pins.clear();
    };

    for (const std::string &tok : netTokens) {
        if (tok == "-") {
            // New net: finalise the previous one.
            flushNet();
            haveNet = true;
            inGroup = false;
        } else if (tok == ";") {
            flushNet();
            inGroup = false;
        } else if (tok == "(") {
            inGroup = true;
            group.clear();
        } else if (tok == ")") {
            inGroup = false;
            resolveGroup();
        } else if (tok == "+") {
            // Ignore any + continuations reaching the stream (none expected).
        } else if (inGroup) {
            group.push_back(tok);
        } else if (haveNet) {
            // Net name tokens up to the first '(' group.
            if (!netName.empty()) {
                netName += "_";
            }
            netName += tok;
        }
    }
    flushNet();

    // GROUPS is written before COMPONENTS, so the instance -> region mapping
    // can only be applied now that every cell vertex exists.
    for (const auto &[regionName, pattern] : pendingGroups) {
        int regionId = constraintMgr::kNoRegion;
        for (std::size_t i = 0; i < fences.numRegions(); ++i) {
            if (fences.region(static_cast<int>(i))->name == regionName) {
                regionId = static_cast<int>(i);
                break;
            }
        }
        if (regionId == constraintMgr::kNoRegion) {
            continue;
        }
        const std::size_t assigned = fences.assignByPrefix(regionId, pattern, db->getGraph());
        numGroupedCells += assigned;
    }

    if (numRegions > 0) {
        ktlog.echo("regions: {} fence(s), {} instance(s) constrained", numRegions, numGroupedCells);
    }

    return true;
}

}  // namespace ktplace
