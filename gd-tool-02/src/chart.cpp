#include "chart.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
const char* const kPartOrder[] = {"guitar", "bass", "drums"};
}

bool Chart::load(const std::filesystem::path& file) {
    clear();
    std::ifstream in(file);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string kind;
        ss >> kind;
        if (kind == "SONG") {
            ss >> duration;
        } else if (kind == "BEAT") {
            float t;
            int bar;
            if (ss >> t >> bar) beats.push_back({t, bar != 0});
        } else if (kind == "SECTION") {
            float t;
            ss >> t;
            std::string name;
            std::getline(ss, name);
            sections.push_back({t, name.empty() ? name : name.substr(1)});
        } else if (kind == "NOTE") {
            std::string part, flags;
            int diff;
            ChartNote n;
            if (!(ss >> part >> diff >> n.t >> n.sustain >> n.lane >> flags)) continue;
            n.hopo = flags.find('h') != std::string::npos;
            n.star = flags.find('s') != std::string::npos;
            n.cymbal = flags.find('c') != std::string::npos;
            if (diff >= 0 && diff < 4) parts[part][size_t(diff)].push_back(n);
        } else if (kind == "VOCAL") {
            VocalNote v;
            if (!(ss >> v.t >> v.dur >> v.pitch)) continue;
            std::getline(ss, v.lyric);
            if (!v.lyric.empty()) v.lyric = v.lyric.substr(1);
            vocals.push_back(v);
        } else if (kind == "PHRASE") {
            float a, b;
            if (ss >> a >> b) phrases.push_back({a, b});
        }
    }
    sort();
    loaded = true;
    return true;
}

void Chart::sort() {
    for (auto& [part, diffs] : parts)
        for (auto& notes : diffs)
            std::stable_sort(notes.begin(), notes.end(), [](auto& a, auto& b) { return a.t < b.t || (a.t == b.t && a.lane < b.lane); });
    std::stable_sort(vocals.begin(), vocals.end(), [](auto& a, auto& b) { return a.t < b.t; });
}

bool Chart::save(const std::filesystem::path& file) const {
    const std::filesystem::path tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary);
        if (!out) return false;
        char buf[256];
        std::snprintf(buf, sizeof buf, "SONG %.3f\n", duration);
        out << buf;
        for (auto& [t, bar] : beats) {
            std::snprintf(buf, sizeof buf, "BEAT %.4f %d\n", t, bar ? 1 : 0);
            out << buf;
        }
        for (auto& [t, name] : sections) {
            std::snprintf(buf, sizeof buf, "SECTION %.4f ", t);
            out << buf << name << "\n";
        }
        std::vector<std::string> order(std::begin(kPartOrder), std::end(kPartOrder));
        for (auto& [part, _] : parts)
            if (std::find(order.begin(), order.end(), part) == order.end()) order.push_back(part);
        for (auto& part : order) {
            auto it = parts.find(part);
            if (it == parts.end()) continue;
            for (int d = 0; d < 4; ++d)
                for (auto& n : it->second[size_t(d)]) {
                    std::string flags = std::string(n.hopo ? "h" : "") + (n.star ? "s" : "") + (n.cymbal ? "c" : "");
                    std::snprintf(buf, sizeof buf, "NOTE %s %d %.4f %.4f %d %s\n", part.c_str(), d, n.t, n.sustain, n.lane,
                                  flags.empty() ? "-" : flags.c_str());
                    out << buf;
                }
        }
        for (auto& v : vocals) {
            std::snprintf(buf, sizeof buf, "VOCAL %.4f %.4f %d", v.t, v.dur, v.pitch);
            out << buf;
            std::string lyric = v.lyric;
            std::replace(lyric.begin(), lyric.end(), '\n', ' ');
            if (!lyric.empty()) out << " " << lyric;
            out << "\n";
        }
        for (auto& [a, b] : phrases) {
            std::snprintf(buf, sizeof buf, "PHRASE %.4f %.4f\n", a, b);
            out << buf;
        }
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

int Chart::count(const std::string& part, int diff) const {
    if (part == "vocals") return vocals.empty() ? -1 : int(vocals.size());
    auto it = parts.find(part);
    if (it == parts.end() || diff < 0 || diff > 3) return -1;
    int n = 0;
    float last = -1;
    for (auto& note : it->second[size_t(diff)])
        if (note.t != last) {
            ++n;
            last = note.t;
        }
    return n;
}

bool Chart::hasPart(const std::string& part) const {
    if (part == "vocals") return !vocals.empty();
    auto it = parts.find(part);
    if (it == parts.end()) return false;
    for (auto& d : it->second)
        if (!d.empty()) return true;
    return false;
}

float Chart::beatLength(float t) const {
    if (beats.size() < 2) return 0.5f;
    auto it = std::upper_bound(beats.begin(), beats.end(), t, [](float v, auto& b) { return v < b.first; });
    size_t i = it == beats.begin() ? 0 : size_t(it - beats.begin()) - 1;
    if (i + 1 >= beats.size()) i = beats.size() - 2;
    return std::max(0.05f, beats[i + 1].first - beats[i].first);
}

// Grid positions: beats[i] + k/perBeat * (beats[i+1] - beats[i]); beyond the beats, the
// last beat length repeats.
float Chart::snap(float t, int perBeat) const {
    perBeat = std::max(1, perBeat);
    if (beats.size() < 2) {
        const float step = 0.5f / float(perBeat);
        return std::max(0.f, std::round(t / step) * step);
    }
    auto it = std::upper_bound(beats.begin(), beats.end(), t, [](float v, auto& b) { return v < b.first; });
    size_t i = it == beats.begin() ? 0 : size_t(it - beats.begin()) - 1;
    const float len = beatLength(t), base = beats[i].first;
    const float k = std::round((t - base) / len * float(perBeat));
    return std::max(0.f, base + k / float(perBeat) * len);
}

float Chart::stepFrom(float t, int perBeat, int steps) const {
    float x = snap(t, perBeat);
    for (int i = 0; i < std::abs(steps); ++i) {
        const float len = beatLength(steps > 0 ? x + 1e-4f : x - 1e-4f) / float(std::max(1, perBeat));
        x = snap(x + (steps > 0 ? len : -len), perBeat);
    }
    return std::max(0.f, x);
}

namespace shape {

SDL_FColor fcolor(SDL_Color c, float alpha) {
    return {c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f * alpha};
}

void ellipse(SDL_Renderer* r, float cx, float cy, float rx, float ry, SDL_FColor c, int segments) {
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    v.push_back({{cx, cy}, c, {0, 0}});
    for (int i = 0; i <= segments; ++i) {
        float a = float(i) / float(segments) * 6.2831853f;
        v.push_back({{cx + std::cos(a) * rx, cy + std::sin(a) * ry}, c, {0, 0}});
    }
    for (int i = 1; i <= segments; ++i) {
        idx.push_back(0);
        idx.push_back(i);
        idx.push_back(i + 1);
    }
    SDL_RenderGeometry(r, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
}

void ring(SDL_Renderer* r, float cx, float cy, float rx, float ry, float thick, SDL_FColor c, int segments) {
    std::vector<SDL_Vertex> v;
    std::vector<int> idx;
    for (int i = 0; i <= segments; ++i) {
        float a = float(i) / float(segments) * 6.2831853f, ca = std::cos(a), sa = std::sin(a);
        v.push_back({{cx + ca * rx, cy + sa * ry}, c, {0, 0}});
        v.push_back({{cx + ca * (rx - thick), cy + sa * (ry - thick)}, c, {0, 0}});
    }
    for (int i = 0; i < segments; ++i) {
        const int a = 2 * i;
        idx.insert(idx.end(), {a, a + 1, a + 2, a + 1, a + 3, a + 2});
    }
    SDL_RenderGeometry(r, nullptr, v.data(), int(v.size()), idx.data(), int(idx.size()));
}

void quad(SDL_Renderer* r, const SDL_FPoint p[4], const SDL_FColor c[4]) {
    SDL_Vertex v[4];
    for (int i = 0; i < 4; ++i) v[i] = {p[i], c[i], {0, 0}};
    const int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(r, nullptr, v, 4, idx, 6);
}

void triangle(SDL_Renderer* r, SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FColor color) {
    SDL_Vertex v[3] = {{a, color, {0, 0}}, {b, color, {0, 0}}, {c, color, {0, 0}}};
    SDL_RenderGeometry(r, nullptr, v, 3, nullptr, 0);
}

}  // namespace shape
