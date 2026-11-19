// gd-tool-02: the small YAML subset our files use (project.yaml, settings.yaml):
// comment lines, then sections of scalar values:
//
//   song:
//     title: "Canção"      <- double-quoted (JSON escapes) or plain scalars
//     year: 2024
//
// Unknown sections and keys are kept, in order, when the file is saved again.
#pragma once

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class YamlDoc {
public:
    struct Section {
        std::string name;
        std::vector<std::pair<std::string, std::string>> values;
    };

    bool load(const std::filesystem::path& file);
    // Writes through a temporary file, so a crash never leaves half a file.
    bool save(const std::filesystem::path& file) const;

    std::string get(const std::string& section, const std::string& key) const;
    void set(const std::string& section, const std::string& key, const std::string& value);
    // Adds the section/key (empty) if missing, keeping the order given.
    void ensure(const std::string& section, const std::string& key);

    std::vector<std::string> header;  // leading "# ..." lines
    std::vector<Section> sections;
};
