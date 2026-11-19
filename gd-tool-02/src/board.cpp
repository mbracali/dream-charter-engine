// gd-tool-02: the boards that show a chart while the stems play: the Guitar Hero-style
// highway (in perspective), the vocal lane and the whole-song overview strip.
#include <algorithm>
#include <cmath>

#include "app.h"

SDL_Color App::laneColor(int lane, bool drums) {
    static const SDL_Color frets[5] = {{0x3c, 0xb3, 0x4a, 255}, {0xd9, 0x3b, 0x3b, 255}, {0xe8, 0xc5, 0x2c, 255},
                                       {0x3b, 0x7d, 0xd9, 255}, {0xe8, 0x84, 0x2c, 255}};
    if (!drums) return frets[std::clamp(lane, 0, 4)];
    static const SDL_Color pads[5] = {{0xe8, 0x84, 0x2c, 255}, {0xd9, 0x3b, 0x3b, 255}, {0xe8, 0xc5, 0x2c, 255},
                                      {0x3b, 0x7d, 0xd9, 255}, {0x3c, 0xb3, 0x4a, 255}};
    return pads[std::clamp(lane, 0, 4)];
}

const std::vector<ChartNote>* App::partNotes(const std::string& part, int diff) const {
    auto it = chart_.parts.find(part);
    if (it == chart_.parts.end() || diff < 0 || diff > 3) return nullptr;
    return &it->second[size_t(diff)];
}

void App::drawHighway(const SDL_FRect& a, const std::vector<ChartNote>* notes, bool drums, float now) {
    const float L = 1.8f, D = 2.4f, back = 0.07f;  // seconds ahead, depth, seconds below the strike line
    const float bottomW = std::min(a.w * 0.84f, 600 * u_), cx = a.x + a.w / 2;
    const float strikeY = a.y + a.h - 40 * u_, horizonY = a.y + 6 * u_;
    const float span = strikeY - horizonY, farScale = 1.f / (1.f + D);
    auto at = [&](float dt, float& scale, float& y) {
        const float d = dt / L * D;
        scale = 1.f / (1.f + d);
        y = strikeY - span * (1.f - scale) / (1.f - farScale);
    };
    const int lanes = drums ? 4 : 5;
    const float laneW = bottomW / float(lanes);
    auto laneX = [&](int idx, float scale) { return cx + (float(idx) - float(lanes - 1) / 2.f) * laneW * scale; };

    float s0, y0, s1, y1;
    at(-back, s0, y0);
    at(L, s1, y1);
    {
        SDL_FPoint q[4] = {{cx - bottomW / 2 * s1, y1}, {cx + bottomW / 2 * s1, y1}, {cx + bottomW / 2 * s0, y0}, {cx - bottomW / 2 * s0, y0}};
        SDL_FColor far = shape::fcolor({0x10, 0x0e, 0x12, 255}), near = shape::fcolor({0x26, 0x22, 0x29, 255});
        SDL_FColor cs[4] = {far, far, near, near};
        shape::quad(renderer_, q, cs);
    }
    for (int i = 0; i <= lanes; ++i) {
        const float off = (float(i) - float(lanes) / 2.f) * laneW;
        const bool rail = i == 0 || i == lanes;
        const SDL_FColor c0 = shape::fcolor(rail ? kSand : kMuted, rail ? 0.9f : 0.45f), c1 = shape::fcolor(rail ? kSand : kMuted, 0.05f);
        const float w0 = (rail ? 3.f : 1.2f) * u_;
        SDL_FPoint q[4] = {{cx + off * s1 - w0 * s1 / 2, y1}, {cx + off * s1 + w0 * s1 / 2, y1},
                           {cx + off * s0 + w0 * s0 / 2, y0}, {cx + off * s0 - w0 * s0 / 2, y0}};
        SDL_FColor cs[4] = {c1, c1, c0, c0};
        shape::quad(renderer_, q, cs);
    }
    auto fade = [&](float dt) { return std::clamp((1.f - dt / L) / 0.25f, 0.f, 1.f); };
    auto beatLine = [&](float t, bool bar) {
        const float dt = t - now;
        if (dt < -back || dt > L) return;
        float s, y;
        at(dt, s, y);
        const float h = (bar ? 3.f : 1.3f) * u_ * s, half = bottomW / 2 * s;
        const SDL_FColor c = shape::fcolor(bar ? kSand : kMuted, (bar ? 0.55f : 0.35f) * fade(dt));
        SDL_FPoint q[4] = {{cx - half, y - h / 2}, {cx + half, y - h / 2}, {cx + half, y + h / 2}, {cx - half, y + h / 2}};
        SDL_FColor cs[4] = {c, c, c, c};
        shape::quad(renderer_, q, cs);
    };
    for (auto& [t, bar] : chart_.beats) beatLine(t, bar);
    for (auto& [t, name] : chart_.sections) {
        const float dt = t - now;
        if (dt < 0 || dt > L * 0.85f) continue;
        float s, y;
        at(dt, s, y);
        const float tw = textW(name, 1.25f * u_);
        const float lx = cx - bottomW / 2 * s - tw - 14 * u_;
        if (lx > a.x) text(lx, y - 5 * u_, name, kSand, 1.25f * u_);
    }

    std::array<bool, 5> lit{};
    if (notes) {
        auto first = std::lower_bound(notes->begin(), notes->end(), now - 12.f, [](const ChartNote& n, float t) { return n.t < t; });
        auto last = std::lower_bound(first, notes->end(), now + L, [](const ChartNote& n, float t) { return n.t < t; });
        for (auto it = first; it != last; ++it) {
            const ChartNote& n = *it;
            if (n.t <= now + 0.03f && (n.t >= now - 0.09f || (n.sustain > 0 && now <= n.t + n.sustain)))
                lit[size_t(std::clamp(n.lane, 0, 4))] = true;
            if (n.sustain <= 0 || drums) continue;
            const float a0 = std::max(n.t - now, -back), a1 = std::min(n.t + n.sustain - now, L);
            if (a1 <= a0) continue;
            float sa, ya, sb, yb;
            at(a0, sa, ya);
            at(a1, sb, yb);
            const SDL_Color c = n.star ? kStar : laneColor(n.lane, false);
            const float wa = laneW * 0.11f * sa, wb = laneW * 0.11f * sb, xa = laneX(n.lane, sa), xb = laneX(n.lane, sb);
            SDL_FPoint q[4] = {{xb - wb, yb}, {xb + wb, yb}, {xa + wa, ya}, {xa - wa, ya}};
            SDL_FColor cs[4] = {shape::fcolor(c, 0.2f), shape::fcolor(c, 0.2f), shape::fcolor(c, 0.85f), shape::fcolor(c, 0.85f)};
            shape::quad(renderer_, q, cs);
        }
        for (auto it = last; it != first;) {
            --it;
            const ChartNote& n = *it;
            const float dt = n.t - now;
            if (dt < -back || dt > L) continue;
            float s, y;
            at(dt, s, y);
            const float f = fade(dt);
            const SDL_Color c = n.star ? kStar : laneColor(n.lane, drums);
            if (drums && n.lane == 0) {  // kick: a bar across the neck
                const float half = bottomW / 2 * s * 0.96f, h = 7 * u_ * s;
                SDL_FRect bar{cx - half, y - h / 2, half * 2, h};
                fill({bar.x, bar.y + h * 0.4f, bar.w, bar.h}, kInk, Uint8(150 * f));
                fill(bar, c, Uint8(255 * f));
                fill({bar.x, bar.y, bar.w, h * 0.3f}, kCream, Uint8(90 * f));
                continue;
            }
            const int idx = drums ? n.lane - 1 : n.lane;
            drawGem(laneX(idx, s), y, laneW * 0.34f * s, c, f, n.hopo, n.cymbal);
        }
    }
    for (int i = 0; i < lanes; ++i) {
        const int lane = drums ? i + 1 : i;
        const SDL_Color c = laneColor(lane, drums);
        const float x = laneX(i, 1.f), rx = laneW * 0.36f, ry = rx * 0.42f;
        if (lit[size_t(lane)]) shape::ellipse(renderer_, x, strikeY, rx * 1.7f, ry * 2.2f, shape::fcolor(c, 0.22f));
        shape::ellipse(renderer_, x, strikeY + ry * 0.3f, rx * 1.05f, ry * 1.05f, shape::fcolor(kInk));
        shape::ellipse(renderer_, x, strikeY, rx, ry, shape::fcolor(c, lit[size_t(lane)] ? 1.f : 0.75f));
        shape::ellipse(renderer_, x, strikeY, rx * 0.72f, ry * 0.72f, shape::fcolor(lit[size_t(lane)] ? c : SDL_Color{0x1a, 0x17, 0x1c, 255}));
        if (lit[size_t(lane)]) shape::ellipse(renderer_, x, strikeY - ry * 0.2f, rx * 0.4f, ry * 0.35f, shape::fcolor(kCream, 0.7f));
    }
    if (drums && lit[0]) fill({cx - bottomW / 2, strikeY + 12 * u_, bottomW, 3 * u_}, laneColor(0, true));
}

void App::drawGem(float x, float y, float rx, SDL_Color c, float alpha, bool hopo, bool cymbal) {
    const float ry = rx * 0.45f;
    auto darker = [](SDL_Color k) { return SDL_Color{Uint8(k.r * 0.55f), Uint8(k.g * 0.55f), Uint8(k.b * 0.55f), 255}; };
    shape::ellipse(renderer_, x, y + ry * 0.55f, rx * 1.04f, ry, shape::fcolor(kInk, 0.55f * alpha));
    shape::ellipse(renderer_, x, y + ry * 0.22f, rx, ry, shape::fcolor(darker(c), alpha));
    if (cymbal) {
        shape::ellipse(renderer_, x, y, rx, ry * 0.8f, shape::fcolor(kCream, 0.85f * alpha));
        shape::ellipse(renderer_, x, y, rx * 0.82f, ry * 0.62f, shape::fcolor(c, alpha));
        return;
    }
    shape::ellipse(renderer_, x, y, rx * 0.94f, ry * 0.86f, shape::fcolor(c, alpha));
    shape::ellipse(renderer_, x, y - ry * 0.3f, rx * 0.55f, ry * 0.32f, shape::fcolor(kCream, 0.3f * alpha));
    if (hopo) shape::ellipse(renderer_, x, y, rx * 0.42f, ry * 0.42f, shape::fcolor(kCream, 0.95f * alpha));
    else shape::ellipse(renderer_, x, y, rx * 0.3f, ry * 0.3f, shape::fcolor(kInk, 0.6f * alpha));
}

// Vocals: the pitch of each syllable over time, the lyrics under it.
void App::drawVocalBoard(const SDL_FRect& a, float now) {
    SDL_FRect lane{a.x + 10 * u_, a.y + a.h * 0.1f, a.w - 20 * u_, a.h * 0.55f};
    fill(lane, {0x12, 0x10, 0x14, 255});
    frame(lane, kMuted);
    const float past = 1.2f, future = 4.8f, nowX = lane.x + lane.w * past / (past + future);
    const float pps = lane.w / (past + future);
    int lo = 48, hi = 72;
    if (!chart_.vocals.empty()) {
        lo = 127;
        hi = 0;
        for (auto& v : chart_.vocals) {
            lo = std::min(lo, v.pitch);
            hi = std::max(hi, v.pitch);
        }
        lo -= 2;
        hi += 2;
    }
    auto pY = [&](int p) { return lane.y + lane.h * (1.f - float(p - lo) / float(std::max(1, hi - lo))); };
    for (auto& [t, bar] : chart_.beats) {
        const float x = nowX + (t - now) * pps;
        if (x < lane.x || x > lane.x + lane.w) continue;
        fill({x, lane.y, u_, lane.h}, bar ? kMuted : kBar);
    }
    for (auto& [t0, t1] : chart_.phrases) {
        const float x = nowX + (t1 - now) * pps;
        if (x >= lane.x && x <= lane.x + lane.w) fill({x, lane.y, 2 * u_, lane.h}, kDim, 120);
    }
    float lastLyricEnd = -1e9f;
    int activePitch = -1;
    for (auto& v : chart_.vocals) {
        const float x0 = nowX + (v.t - now) * pps, x1 = nowX + (v.t + v.dur - now) * pps;
        if (x1 < lane.x || x0 > lane.x + lane.w) continue;
        const bool active = now >= v.t && now <= v.t + v.dur;
        if (active) activePitch = v.pitch;
        const float y = pY(v.pitch), h = 10 * u_;
        const float cx0 = std::max(x0, lane.x), cx1 = std::min(x1, lane.x + lane.w);
        const SDL_Color c = active ? kCream : stemColor("vocals");
        fill({cx0, y - h / 2, std::max(2 * u_, cx1 - cx0), h}, c, x1 < nowX ? 110 : 255);
        const float tw = textW(v.lyric, 1.5f * u_);
        if (x0 >= lastLyricEnd + 6 * u_ && x0 >= lane.x && x0 + tw <= lane.x + lane.w) {
            text(x0, lane.y + lane.h + 14 * u_, v.lyric, active ? kCream : (x1 < nowX ? kMuted : kSand), 1.5f * u_);
            lastLyricEnd = x0 + tw;
        }
    }
    fill({nowX - u_, lane.y - 6 * u_, 2 * u_, lane.h + 12 * u_}, kCream);
    if (activePitch >= 0) shape::ellipse(renderer_, nowX, pY(activePitch), 8 * u_, 8 * u_, shape::fcolor(kCream));
}

// The whole song: where the notes are (density), the sections, the playhead. Click to seek.
void App::drawOverview(const SDL_FRect& r, const std::vector<ChartNote>* notes, bool vocals, const std::string& part) {
    fill(r, kInk);
    const float dur = chart_.duration > 0 ? chart_.duration : float(player_.duration());
    if (dur <= 0) return;
    const int cols = std::max(1, int(r.w / (3 * u_)));
    std::vector<int> bins(size_t(cols), 0);
    auto add = [&](float t) {
        int i = int(t / dur * float(cols));
        if (i >= 0 && i < cols) ++bins[size_t(i)];
    };
    if (vocals) for (auto& v : chart_.vocals) add(v.t);
    else if (notes) for (auto& n : *notes) add(n.t);
    const int peak = std::max(1, *std::max_element(bins.begin(), bins.end()));
    const SDL_Color c = stemColor(part);
    for (int i = 0; i < cols; ++i) {
        if (!bins[size_t(i)]) continue;
        const float h = (r.h - 6 * u_) * float(bins[size_t(i)]) / float(peak);
        fill({r.x + float(i) * 3 * u_, r.y + r.h - 3 * u_ - h, 2 * u_, h}, c, 170);
    }
    for (auto& [t, name] : chart_.sections) fill({r.x + t / dur * r.w, r.y, u_, 6 * u_}, kOxblood);
    const float px = r.x + float(player_.playhead()) / dur * r.w;
    fill({px - u_, r.y - 3 * u_, 2 * u_, r.h + 6 * u_}, kCream);
    frame(r, kBar);
    hit(r, [this, r, dur](float x, float) {
        player_.seek(double((x - r.x) / r.w * dur));
        followPlayhead();
    });
}
