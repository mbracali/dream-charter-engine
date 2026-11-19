// gd-tool-02 Forge: the chart preview written by python/forge.py
// (forge/preview.txt), and the shapes the board is drawn with.
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct ChartNote {
    float t = 0, sustain = 0;
    int lane = 0;  // guitar/bass/keys: 0 green .. 4 orange; drums: 0 kick, 1 red, 2 yellow, 3 blue, 4 green
    bool hopo = false, star = false, cymbal = false;
};

struct VocalNote {
    float t = 0, dur = 0;
    int pitch = 60;
    std::string lyric;
};

struct ForgePreview {
    bool loaded = false;
    float duration = 0;
    std::vector<std::pair<float, bool>> beats;  // time, first beat of a bar
    std::vector<std::pair<float, std::string>> sections;
    std::map<std::string, std::array<std::vector<ChartNote>, 4>> parts;  // part -> Easy..Expert, by time
    std::vector<VocalNote> vocals;
    std::vector<std::pair<float, float>> phrases;

    bool load(const std::filesystem::path& file);
    void clear() { *this = ForgePreview(); }
    // Chords count once (notes at the same time).
    int count(const std::string& part, int diff) const;
};

namespace shape {

SDL_FColor fcolor(SDL_Color c, float alpha = 1.f);
void ellipse(SDL_Renderer* r, float cx, float cy, float rx, float ry, SDL_FColor c, int segments = 28);
// A quad from four corners (clockwise), each with its own colour (gradients).
void quad(SDL_Renderer* r, const SDL_FPoint p[4], const SDL_FColor c[4]);
void triangle(SDL_Renderer* r, SDL_FPoint a, SDL_FPoint b, SDL_FPoint c, SDL_FColor color);

}  // namespace shape
