#include "yaml_doc.h"

#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

namespace {

void appendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r");
    size_t b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::string parseDoubleQuoted(const std::string& s) {
    std::string out;
    for (size_t i = 1; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') break;
        if (c != '\\' || i + 1 >= s.size()) {
            out += c;
            continue;
        }
        char e = s[++i];
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case '0': out += '\0'; break;
            case 'u': {
                unsigned cp = unsigned(std::stoul(s.substr(i + 1, 4), nullptr, 16));
                i += 4;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                    unsigned lo = unsigned(std::stoul(s.substr(i + 3, 4), nullptr, 16));
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 6;
                }
                appendUtf8(out, cp);
                break;
            }
            default: out += e; break;  // \" \\ \/
        }
    }
    return out;
}

std::string parseValue(std::string raw) {
    raw = trim(raw);
    if (raw.empty()) return "";
    if (raw[0] == '"') return parseDoubleQuoted(raw);
    if (raw[0] == '\'') {
        std::string out;
        for (size_t i = 1; i < raw.size(); ++i) {
            if (raw[i] == '\'') {
                if (i + 1 < raw.size() && raw[i + 1] == '\'') {
                    out += '\'';
                    ++i;
                } else {
                    break;
                }
            } else {
                out += raw[i];
            }
        }
        return out;
    }
    size_t hash = raw.find(" #");
    if (hash != std::string::npos) raw = trim(raw.substr(0, hash));
    if (raw == "~" || raw == "null") return "";
    return raw;
}

std::string quote(const std::string& v) {
    static const std::regex plain(R"(^-?[0-9]+(\.[0-9]+)?$|^(true|false)$)");
    if (std::regex_match(v, plain)) return v;
    std::string out = "\"";
    for (unsigned char c : v) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\t': out += "\\t"; break;
            case '\r': out += "\\r"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += char(c);
                }
        }
    }
    return out + "\"";
}

}  // namespace

bool YamlDoc::load(const std::filesystem::path& file) {
    std::ifstream in(file);
    if (!in) return false;
    header.clear();
    sections.clear();
    std::string line;
    bool body = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string t = trim(line);
        if (t.empty() || t[0] == '#') {
            if (!body && !t.empty()) header.push_back(line);
            continue;
        }
        body = true;
        size_t colon = t.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(t.substr(0, colon));
        if (line[0] != ' ' && line[0] != '\t') {
            sections.push_back({key, {}});
        } else if (!sections.empty()) {
            sections.back().values.push_back({key, parseValue(t.substr(colon + 1))});
        }
    }
    return true;
}

bool YamlDoc::save(const std::filesystem::path& file) const {
    std::ostringstream out;
    for (auto& h : header) out << h << '\n';
    if (!header.empty()) out << '\n';
    for (auto& s : sections) {
        out << s.name << ":\n";
        for (auto& [k, v] : s.values) out << "  " << k << ": " << quote(v) << '\n';
        out << '\n';
    }
    std::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << out.str();
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

std::string YamlDoc::get(const std::string& section, const std::string& key) const {
    for (auto& s : sections)
        if (s.name == section)
            for (auto& [k, v] : s.values)
                if (k == key) return v;
    return "";
}

void YamlDoc::set(const std::string& section, const std::string& key, const std::string& value) {
    ensure(section, key);
    for (auto& s : sections)
        if (s.name == section)
            for (auto& [k, v] : s.values)
                if (k == key) v = value;
}

void YamlDoc::ensure(const std::string& section, const std::string& key) {
    for (auto& s : sections)
        if (s.name == section) {
            for (auto& kv : s.values)
                if (kv.first == key) return;
            s.values.push_back({key, ""});
            return;
        }
    sections.push_back({section, {{key, ""}}});
}
