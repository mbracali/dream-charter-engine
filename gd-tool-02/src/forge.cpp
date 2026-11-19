#include "forge.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

bool ForgePreview::load(const std::filesystem::path& file) {
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
            ss >> t >> bar;
            beats.push_back({t, bar != 0});
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
            ss >> part >> diff >> n.t >> n.sustain >> n.lane >> flags;
            n.hopo = flags.find('h') != std::string::npos;
            n.star = flags.find('s') != std::string::npos;
            n.cymbal = flags.find('c') != std::string::npos;
            if (diff >= 0 && diff < 4) parts[part][size_t(diff)].push_back(n);
        } else if (kind == "VOCAL") {
            VocalNote v;
            ss >> v.t >> v.dur >> v.pitch;
            std::getline(ss, v.lyric);
            if (!v.lyric.empty()) v.lyric = v.lyric.substr(1);
            vocals.push_back(v);
        } else if (kind == "PHRASE") {
            float a, b;
            ss >> a >> b;
            phrases.push_back({a, b});
        }
    }
    for (auto& [part, diffs] : parts)
        for (auto& notes : diffs)
            std::stable_sort(notes.begin(), notes.end(), [](auto& a, auto& b) { return a.t < b.t; });
    std::sort(vocals.begin(), vocals.end(), [](auto& a, auto& b) { return a.t < b.t; });
    loaded = true;
    return true;
}

int ForgePreview::count(const std::string& part, int diff) const {
    if (part == "vocals") return int(vocals.size());
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
