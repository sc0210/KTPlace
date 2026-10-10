// @file bookshelfToKTAdaptor.cc// Implementation of Bookshelf format adapter// - Boosts Iostreams (gzip_decompressor) transparently decompresses .gz inputs// - oneTBB (parallel_for / blocked_range) parallelizes the node and net parsing


#include "adaptor/bookshelfToKTAdaptor.h"

#include "util/kt_log.h"
#include "util/kt_scopedTimer.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

// Boost.Iostreams - transparent gzip input
#include <boost/iostreams/device/file.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filtering_stream.hpp>

// oneTBB - parallel parsing
#include <oneapi/tbb/blocked_range.h>
#include <oneapi/tbb/parallel_for.h>

#ifdef _WIN32
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif

namespace ktplace {

namespace {
/// Case-insensitive token comparison, for .scl field names.
bool lowerEq(const std::string &a, const char *b) {
    if (a.size() != std::strlen(b)) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}


// Gzip-aware text file. If the path ends in ".gz" the stream is decompressed
// on the fly with Boost.Iostreams; otherwise it is read as plain text.
class InputTextFile {
public:
    explicit InputTextFile(const std::string &path) {
        // A file_source on a missing path still reports a "good" stream until
        // the first read, so the existence check has to happen here: otherwise
        // a missing input file looks like an empty one and parses as success.
        exists_ = std::filesystem::exists(path);
        if (path.size() > 3 && path.compare(path.size() - 3, 3, ".gz") == 0) {
            in_.push(boost::iostreams::gzip_decompressor());
            in_.push(boost::iostreams::file_source(path, std::ios::binary));
        } else {
            in_.push(boost::iostreams::file_source(path));
        }
    }

    std::istream &stream() {
        return in_;
    }
    explicit operator bool() const {
        return exists_ && static_cast<bool>(in_);
    }

private:
    boost::iostreams::filtering_istream in_;
    bool exists_ = false;
};

// Parsed node record produced by the parallel parsing phase.
struct NodeRec {
    std::string name;
    double width = 0.0;
    double height = 0.0;
    bool terminal = false;
    bool valid = false;
};

// Parsed pin record produced by the parallel parsing phase.
struct PinRec {
    std::string cellName;
    double offsetX = 0.0;
    double offsetY = 0.0;
    bool isInput = false;
};

// Net header information gathered during the serial pre-scan.
struct NetStub {
    std::string name;
    std::size_t degree = 0;
};

}  // namespace

// Helper to strip comments and whitespace
std::string BookshelfInputAdapter::stripComments(const std::string &line) {
    std::string result = line;
    size_t commentPos = result.find('#');
    if (commentPos != std::string::npos) {
        result = result.substr(0, commentPos);
    }
    // Trim whitespace
    result.erase(0, result.find_first_not_of(" \t\r\n"));
    result.erase(result.find_last_not_of(" \t\r\n") + 1);
    return result;
}

// Tokenize a line
std::vector<std::string> BookshelfInputAdapter::tokenize(const std::string &line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

BookshelfInputAdapter::BookshelfInputAdapter(std::unique_ptr<ktDM> database)
    : db(database ? std::move(database) : std::make_unique<ktDM>()) {}

BookshelfInputAdapter::~BookshelfInputAdapter() = default;

BookshelfInputAdapter::BookshelfInputAdapter(BookshelfInputAdapter &&) noexcept = default;
BookshelfInputAdapter &BookshelfInputAdapter::operator=(BookshelfInputAdapter &&) noexcept =
    default;

bool BookshelfInputAdapter::recognises(const std::string &dirPath) const {
    // The design's files are named after the directory holding them.
    const std::string stem = std::filesystem::path(dirPath).filename().string();
    if (stem.empty()) {
        return false;
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    return fs::exists((fs::path(dirPath) / (stem + ".nodes")), ec) ||
           fs::exists((fs::path(dirPath) / (stem + ".nodes.gz")), ec);
}

std::unique_ptr<ktDM> BookshelfInputAdapter::read(const std::string &dirPath) {
    return readFromDirectory(dirPath) ? releaseDM() : nullptr;
}

bool BookshelfInputAdapter::readFromDirectory(const std::string &dirPath) {
    // The design's files are named after the design, and the design after its
    // directory, so one name covers all five.
    const std::string baseName = std::filesystem::path(dirPath).filename().string();
    std::string nodesFile = dirPath + "/" + baseName + ".nodes";
    std::string netsFile = dirPath + "/" + baseName + ".nets";
    std::string plFile = dirPath + "/" + baseName + ".pl";
    std::string sclFile = dirPath + "/" + baseName + ".scl";
    std::string wtsFile = dirPath + "/" + baseName + ".wts";

    // Check for gzipped versions
    if (access((nodesFile + ".gz").c_str(), F_OK) == 0) {
        nodesFile += ".gz";
    }
    if (access((netsFile + ".gz").c_str(), F_OK) == 0) {
        netsFile += ".gz";
    }
    if (access((plFile + ".gz").c_str(), F_OK) == 0) {
        plFile += ".gz";
    }
    if (access((sclFile + ".gz").c_str(), F_OK) == 0) {
        sclFile += ".gz";
    }
    if (access((wtsFile + ".gz").c_str(), F_OK) == 0) {
        wtsFile += ".gz";
    }

    return readFromFiles(nodesFile, netsFile, plFile, sclFile, wtsFile);
}

bool BookshelfInputAdapter::readFromFiles(const std::string &nodesFile, const std::string &netsFile,
                                          const std::string &plFile, const std::string &sclFile,
                                          const std::string &wtsFile) {
    // Clear existing data
    db->clear();

    // Each file is timed on its own. Parsing dominates the load phase on a large
    // design, and the four files cost very different amounts: .nodes and .nets are
    // O(cells + pins) and slow, .scl and .pl are small and instant. When a load
    // takes minutes, "which file" is the first question, and the answer should not
    // require a profiler to get.
    //
    // Parse nodes file (required)
    {
        ScopedTimer timer("bookshelf-nodes");
        if (!parseNodesFile(nodesFile)) {
            ktlog.echo("cannot parse the nodes file: {}", nodesFile);
            return false;
        }
    }

    // Parse weights file (optional) BEFORE nets so net weights are honored
    if (!wtsFile.empty()) {
        ScopedTimer timer("bookshelf-wts");
        parseWtsFile(wtsFile);
    }

    // Parse nets file (required)
    {
        ScopedTimer timer("bookshelf-nets");
        if (!parseNetsFile(netsFile)) {
            ktlog.echo("cannot parse the nets file: {}", netsFile);
            return false;
        }
    }

    // Parse placement file (optional)
    if (!plFile.empty()) {
        ScopedTimer timer("bookshelf-pl");
        parsePlacementFile(plFile);
    }

    // Parse scl file (optional)
    if (!sclFile.empty()) {
        // Timed last, and separately, because it is the file that decides the
        // rows. If it is empty the design has no legal placement at all, which is
        // worth being able to confirm from the log rather than deduce.
        ScopedTimer timer("bookshelf-scl");
        parseSclFile(sclFile);
    }

    return true;
}

bool BookshelfInputAdapter::parseNodesFile(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        ktlog.echo("cannot open the nodes file: {}", filePath);
        return false;
    }

    // Read all lines once
    std::vector<std::string> lines;
    lines.reserve(1u << 18);
    std::string line;
    while (std::getline(file.stream(), line)) {
        lines.push_back(std::move(line));
    }

    // Serial pre-scan: locate the end of the header ("NumTerminals" line).
    std::size_t bodyStart = 0;
    while (bodyStart < lines.size()) {
        std::string stripped = stripComments(lines[bodyStart]);
        if (stripped.find("NumTerminals") != std::string::npos) {
            ++bodyStart;
            break;
        }
        ++bodyStart;
    }

    const std::size_t nNodes = lines.size() - bodyStart;
    if (nNodes == 0) {
        return true;
    }

    // Parallel phase: tokenize + validate every node line independently.
    std::vector<NodeRec> recs(nNodes);
    tbb::parallel_for(
        tbb::blocked_range<std::size_t>(0, nNodes), [&](const tbb::blocked_range<std::size_t> &r) {
            for (std::size_t i = r.begin(); i != r.end(); ++i) {
                std::string stripped = stripComments(lines[bodyStart + i]);
                if (stripped.empty())
                    continue;

                std::vector<std::string> tokens = tokenize(stripped);
                if (tokens.size() < 3) {
                    ktlog.warning("cannot parse node line {}: {}", bodyStart + i + 1, stripped);
                    continue;
                }

                NodeRec &rec = recs[i];
                try {
                    rec.width = std::stod(tokens[1]);
                    rec.height = std::stod(tokens[2]);
                    rec.terminal = (tokens.size() >= 4 && tokens[3] == "terminal");
                    rec.name = std::move(tokens[0]);
                    rec.valid = true;
                } catch (const std::exception &e) {
                    ktlog.warning("cannot parse node line {}: {} ({})", bodyStart + i + 1, stripped,
                                  e.what());
                }
            }
        });

    // Serial merge phase: insert records into the DB (Graph is not thread-safe).
    for (NodeRec &rec : recs) {
        if (!rec.valid)
            continue;
        try {
            const std::size_t id = db->addCell(rec.name, rec.width, rec.height, rec.terminal);
            // Terminals are die I/O pads: they are anchored on the die edge and
            // must never take part in spreading.
            if (rec.terminal) {
                db->setCellFixed(id, true);
            }
        } catch (const std::exception &e) {
            ktlog.echo("cannot add cell {}: {}", rec.name, e.what());
        }
    }

    return true;
}

bool BookshelfInputAdapter::parseNetsFile(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        ktlog.echo("cannot open the nets file: {}", filePath);
        return false;
    }

    // Read all lines once
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file.stream(), line)) {
        lines.push_back(std::move(line));
    }

    // Serial pre-scan: pull out "NetDegree" headers, count pins per net, and
    // record the global index of every pin line.
    std::vector<NetStub> netStubs;
    std::vector<std::size_t> pinsInNet;     // pins observed per net (serial order)
    std::vector<std::size_t> pinLineIndex;  // global line index of each pin
    bool inHeader = true;

    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::string stripped = stripComments(lines[i]);
        if (stripped.empty())
            continue;

        if (inHeader) {
            // Skip header lines: "UCLA nets 1.0", "NumNets : N", "NumPins : N"
            if (stripped.compare(0, 5, "UCLA") == 0 || stripped.compare(0, 7, "NumNets") == 0 ||
                stripped.compare(0, 7, "NumPins") == 0) {
                continue;
            }
            inHeader = false;
        }

        if (stripped.compare(0, 9, "NetDegree") == 0) {
            // Formats: "NetDegree <degree> <name>", "NetDegree : <degree> <name>",
            // or "NetDegree : <degree>" (name-less IBM style).
            const std::vector<std::string> tokens = tokenize(stripped);
            std::size_t nameIdx = 1;
            for (; nameIdx < tokens.size(); ++nameIdx) {
                if (tokens[nameIdx] != ":")
                    break;
            }
            if (tokens.size() < nameIdx + 1) {
                ktlog.warning("malformed NetDegree line {}: {}", i + 1, stripped);
                continue;
            }
            NetStub stub;
            stub.degree = std::stoull(tokens[nameIdx]);
            stub.name = (nameIdx + 1 < tokens.size()) ? tokens[nameIdx + 1] : "";
            netStubs.push_back(std::move(stub));
            pinsInNet.push_back(0);
            continue;
        }

        // Everything else after the header is a pin line belonging to the
        // current (last) net.
        if (!netStubs.empty()) {
            pinLineIndex.push_back(i);
            pinsInNet.back() += 1;
        }
    }

    // Prefix offsets: global pin ranges [pinStart[i], pinStart[i] + count)
    std::vector<std::size_t> pinStart(netStubs.size());
    std::size_t totalPins = 0;
    for (std::size_t i = 0; i < netStubs.size(); ++i) {
        pinStart[i] = totalPins;
        totalPins += std::min(pinsInNet[i], netStubs[i].degree);
    }

    // Parallel phase: parse each net's pins independently into disjoint
    // per-net buffers.
    std::vector<std::vector<PinRec>> netPins(netStubs.size());
    tbb::parallel_for(tbb::blocked_range<std::size_t>(0, netStubs.size()),
                      [&](const tbb::blocked_range<std::size_t> &r) {
                          for (std::size_t i = r.begin(); i != r.end(); ++i) {
                              auto &pins = netPins[i];
                              pins.reserve(std::min(pinsInNet[i], netStubs[i].degree));
                              const std::size_t beginIdx = pinStart[i];
                              const std::size_t count = std::min(pinsInNet[i], netStubs[i].degree);
                              for (std::size_t p = 0; p < count; ++p) {
                                  const std::size_t lineIdx = pinLineIndex[beginIdx + p];
                                  std::string stripped = stripComments(lines[lineIdx]);
                                  if (stripped.empty())
                                      continue;

                                  std::vector<std::string> tokens = tokenize(stripped);
                                  if (tokens.size() < 2)
                                      continue;

                                  PinRec rec;
                                  try {
                                      rec.cellName = std::move(tokens[0]);
                                      rec.isInput = (tokens[1] == "I");
                                      if (tokens.size() >= 5 && tokens[2] == ":") {
                                          rec.offsetX = std::stod(tokens[3]);
                                          rec.offsetY = std::stod(tokens[4]);
                                      }
                                  } catch (const std::exception &e) {
                                      ktlog.warning("cannot parse pin line {}: {} ({})",
                                                    lineIdx + 1, stripped, e.what());
                                      continue;
                                  }
                                  pins.push_back(std::move(rec));
                              }
                          }
                      });

    // Serial merge phase: create nets and pins in the DB.
    for (std::size_t i = 0; i < netStubs.size(); ++i) {
        // IBM-style net lists omit net names; synthesize a unique one so
        // name-less nets do not all collapse onto a single shared key.
        const std::string baseNetName = netStubs[i].name;
        std::string netName = baseNetName.empty() ? ("n" + std::to_string(i)) : baseNetName;
        double weight = 1.0;
        auto it = netWeights.find(baseNetName);
        if (it != netWeights.end()) {
            weight = it->second;
        }
        if (!db->hasNet(netName)) {
            (void)db->addNet(netName, weight);
        }
        for (const PinRec &pin : netPins[i]) {
            try {
                (void)db->addPin(pin.cellName, netName, pin.offsetX, pin.offsetY, pin.isInput);
            } catch (const std::exception &e) {
                ktlog.warning("cannot add pin: {}", e.what());
            }
        }
    }

    return true;
}

bool BookshelfInputAdapter::parsePlacementFile(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        // Placement file is optional
        return true;
    }

    bool inHeader = true;
    std::string line;
    while (std::getline(file.stream(), line)) {
        std::string stripped = stripComments(line);
        if (stripped.empty())
            continue;

        if (inHeader) {
            // Skip "UCLA pl 1.0" style header lines
            if (stripped.find("UCLA") != std::string::npos) {
                continue;
            }
            inHeader = false;
        }

        parsePlacementLine(stripped);
    }

    return true;
}

bool BookshelfInputAdapter::parsePlacementLine(const std::string &line) {
    // Format: "<cell_name> <x> <y> : <N|S|E|W|FN|FS|FE|FW>"
    // Or: "<cell_name> <x> <y>"
    std::vector<std::string> tokens = tokenize(line);
    if (tokens.size() < 3) {
        return false;
    }

    std::string cellName = tokens[0];
    double x = std::stod(tokens[1]);
    double y = std::stod(tokens[2]);

    bool fixed = false;
    if (tokens.size() >= 5 && tokens[3] == ":") {
        std::string orient = tokens[4];
        // Check if fixed (starts with 'F')
        fixed = (orient.size() > 0 && orient[0] == 'F');
    }
    // Bookshelf also spells immobility as a trailing "/FIXED" marker, which is
    // how most published .pl files mark the die pads.
    for (const std::string &token : tokens) {
        if (token == "/FIXED") {
            fixed = true;
        }
    }

    try {
        db->setCellPosition(cellName, x, y);
        if (fixed) {
            db->setCellFixed(cellName, true);
        }
    } catch (const std::exception &e) {
        // Cell might not exist yet, skip
        return false;
    }

    return true;
}

bool BookshelfInputAdapter::parseSclFile(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        // SCL file is optional
        return true;
    }

    std::string line;
    std::vector<std::string> currentRowTokens;
    bool inRow = false;

    while (std::getline(file.stream(), line)) {
        std::string stripped = stripComments(line);
        if (stripped.empty())
            continue;

        if (stripped.find("CoreRow") != std::string::npos) {
            inRow = true;
            currentRowTokens.clear();
            continue;
        }

        if (stripped == "End") {
            if (inRow) {
                parseSclRow(currentRowTokens);
                inRow = false;
            }
            continue;
        }

        if (inRow) {
            currentRowTokens.push_back(stripped);
        }
    }

    return true;
}

bool BookshelfInputAdapter::parseSclRow(const std::vector<std::string> &tokens) {
    double coordinate = 0.0;
    double height = 0.0;
    double sitewidth = 1.0;
    double sitespacing = 1.0;
    // A CoreRow block may carry several SubrowOrigin/NumSites pairs, one per
    // contiguous run of sites. Keeping only the last one silently discarded the
    // rest of the row, so a row interrupted by a macro was modelled as one span
    // and cells were placed across the blockage.
    struct Subrow {
        double originX;
        double numSites;
    };
    std::vector<Subrow> subrows;
    double pendingOrigin = 0.0;
    double pendingNumSites = 0.0;
    bool haveOrigin = false;

    // Parse row parameters from tokens (each token is one "key : value" line)
    for (const std::string &token : tokens) {
        const std::vector<std::string> parts = tokenize(token);
        if (parts.size() < 2)
            continue;

        // Field names are matched case-insensitively: the ISPD 2005 .scl files
        // write "NumSites" but the ICCAD 2004 ones write "Numsites", and a
        // case-sensitive match silently dropped every row of the latter.
        const std::string key = [&] {
            std::string k = parts[0];
            for (char &ch : k) {
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            }
            return k;
        }();
        // Skip an optional ":" separator to reach the value token
        std::size_t valIdx = 1;
        while (valIdx < parts.size() && parts[valIdx] == ":") {
            ++valIdx;
        }
        if (valIdx >= parts.size())
            continue;

        try {
            if (key == "coordinate") {
                coordinate = std::stod(parts[valIdx]);
            } else if (key == "height") {
                height = std::stod(parts[valIdx]);
            } else if (key == "sitewidth") {
                sitewidth = std::stod(parts[valIdx]);
            } else if (key == "sitespacing") {
                sitespacing = std::stod(parts[valIdx]);
            } else if (key == "subroworigin") {
                // Format: "SubrowOrigin : <x> NumSites : <count>". The x is the
                // subrow's first site and anchors the site grid; dropping it
                // left rows with no origin and no capacity.
                if (haveOrigin && pendingNumSites > 0.0) {
                    subrows.push_back(Subrow{pendingOrigin, pendingNumSites});
                }
                pendingOrigin = std::stod(parts[valIdx]);
                haveOrigin = true;
                pendingNumSites = 0.0;
                // Format: "SubrowOrigin : <value> NumSites : <count>"
                for (std::size_t p = 0; p + 1 < parts.size(); ++p) {
                    if (lowerEq(parts[p], "numsites")) {
                        std::size_t m = p + 1;
                        while (m < parts.size() && parts[m] == ":") {
                            ++m;
                        }
                        if (m < parts.size()) {
                            pendingNumSites = std::stod(parts[m]);
                        }
                        break;
                    }
                }
            }
        } catch (const std::exception &) {
            // Malformed row field; keep defaults
        }
    }

    if (haveOrigin && pendingNumSites > 0.0) {
        subrows.push_back(Subrow{pendingOrigin, pendingNumSites});
    }
    if (subrows.empty()) {
        return true;  // a row with no subrow has no placeable sites
    }
    const std::size_t rowId = db->addRow(coordinate, height, sitewidth, sitespacing);
    for (const Subrow &sr : subrows) {
        (void)db->addSubrow(rowId, sr.originX, sr.numSites);
    }

    return true;
}

bool BookshelfInputAdapter::parseWtsFile(const std::string &filePath) {
    InputTextFile file(filePath);
    if (!file) {
        // Weights file is optional
        return true;
    }

    std::string line;
    bool inHeader = true;
    while (std::getline(file.stream(), line)) {
        std::string stripped = stripComments(line);
        if (stripped.empty())
            continue;

        if (inHeader) {
            // Skip "UCLA wts 1.0" style header lines
            if (stripped.find("UCLA") != std::string::npos) {
                continue;
            }
            inHeader = false;
        }

        std::vector<std::string> tokens = tokenize(stripped);
        if (tokens.size() < 2)
            continue;

        try {
            std::string netName = tokens[0];
            double weight = std::stod(tokens[1]);
            netWeights[netName] = weight;
        } catch (const std::exception &) {
            // Not a "netname weight" line; skip
        }
    }

    return true;
}

}  // namespace ktplace
