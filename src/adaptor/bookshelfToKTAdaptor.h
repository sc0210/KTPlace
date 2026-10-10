// @file bookshelfToKTAdaptor.h
// Bookshelf format adapter

#pragma once

#include "adaptor/kt_inputReader.h"
#include "datamodel/kt_dm.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ktplace {

// Parses Bookshelf files (.nodes, .nets, .pl, .scl, .wts), named after the
// directory they sit in, into the internal ktDM.
class BookshelfInputAdapter final : public InputReader {
public:
    explicit BookshelfInputAdapter(std::unique_ptr<ktDM> db = nullptr);

    ~BookshelfInputAdapter() override;

    // Copy semantics (deleted)
    BookshelfInputAdapter(const BookshelfInputAdapter &) = delete;
    BookshelfInputAdapter &operator=(const BookshelfInputAdapter &) = delete;

    // Move semantics
    BookshelfInputAdapter(BookshelfInputAdapter &&) noexcept;
    BookshelfInputAdapter &operator=(BookshelfInputAdapter &&) noexcept;

    [[nodiscard]] bool recognises(const std::string &dirPath) const override;
    [[nodiscard]] std::unique_ptr<ktDM> read(const std::string &dirPath) override;
    [[nodiscard]] std::string_view formatName() const override {
        return "bookshelf";
    }

    // Read Bookshelf format from directory

    [[nodiscard]] bool readFromDirectory(const std::string &dirPath);

    // Read Bookshelf format from individual files

    [[nodiscard]] bool readFromFiles(const std::string &nodesFile, const std::string &netsFile,
                                     const std::string &plFile = "",
                                     const std::string &sclFile = "",
                                     const std::string &wtsFile = "");

    // Get the ktDM object

    [[nodiscard]] ktDM &getDM() {
        return *db;
    }
    [[nodiscard]] const ktDM &getDM() const {
        return *db;
    }

    // Release ownership of the ktDM

    [[nodiscard]] std::unique_ptr<ktDM> releaseDM() {
        return std::move(db);
    }

private:
    // Helper methods for parsing individual files
    bool parseNodesFile(const std::string &filePath);
    bool parseNetsFile(const std::string &filePath);
    bool parsePlacementFile(const std::string &filePath);
    bool parseSclFile(const std::string &filePath);
    bool parseWtsFile(const std::string &filePath);

    // Helper methods for parsing lines
    bool parsePlacementLine(const std::string &line);
    bool parseSclRow(const std::vector<std::string> &tokens);

    // Utility methods
    std::string stripComments(const std::string &line);
    std::vector<std::string> tokenize(const std::string &line);

    // Internal state
    std::unique_ptr<ktDM> db;
    std::unordered_map<std::string, double> netWeights;  // From .wts file
};

}  // namespace ktplace
