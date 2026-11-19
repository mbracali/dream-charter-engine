// gd-tool-02 Song forge: python/forge.py separates the stems (if needed) and charts the
// song with the sheets of Song home; the disc spins meanwhile. Then the stems (mixer)
// and the charts (highway) can be previewed, and the forge settings changed.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>

#include "app.h"

std::vector<std::string> App::chartParts() const {
    std::vector<std::string> out;
    for (const char* p : {"guitar", "bass", "drums"})  // never keys (R25)
        if (chart_.hasPart(p)) out.push_back(p);
    const Project* p = nullptr;
    for (auto& q : projects_)
        if (q.id == current_) p = &q;
    if (!chart_.vocals.empty() || (p && showVocals(*p))) out.push_back("vocals");  // R23 / R29
    return out;
}

std::string App::drumStyle(const Project& p) const {
    const std::string s = p.doc.get("forge", "drums_style");
    return s == "heard" || s == "locked" ? s : "steady";
}

std::string App::riffStyle(const Project& p) const { return p.doc.get("forge", "riffs_style") == "heard" ? "heard" : "steady"; }

std::string App::sourceOf(const Project& p, const std::string& part) const {
    const std::string s = p.doc.get("forge", part + "_source");
    return s.empty() ? part : s;
}

// Everything the charts depend on, to tell when they are out of date.
std::string App::forgeSignature(const Project& p) const {
    std::string sig = "genre=" + p.doc.get("forge", "genre") + ";drums=" + drumStyle(p) + ";riffs=" + riffStyle(p) +
                      ";guitar=" + sourceOf(p, "guitar") + ";bass=" + sourceOf(p, "bass") +
                      ";lyrics=" + lyricsChoice(p) + ";absent=" + p.doc.get("sheets", "guitar_absent") + p.doc.get("sheets", "bass_absent") +
                      p.doc.get("sheets", "drums_absent");
    for (const char* box : {"guitar", "bass", "drums"}) {
        sig += std::string(";") + box + ":";
        for (auto& f : sheetFiles(p, box))
            if (f.used) sig += f.name + ",";
    }
    return sig;
}

void App::startForge() {
    Project* p = currentProject();
    if (!p || jobFor(*p, JobKind::Forge)) return;
    if (busy(*p)) {
        status_ = p->title() + ": wait for the current job to finish.";
        return;
    }
    saveChart();
    jobs_.push_back({p->dir, JobKind::Forge, {}, forgeSignature(*p)});
    p->doc.set("forge", "status", "running");
    p->dirty = true;
    saveProject(*p);
    jobStatus_ = "Waiting...";
    jobProgress_ = -1;
    status_ = p->title() + ": forging.";
}

// The genre presets of dream-charter-engine (ai_charter/genres/*.toml).
const std::vector<std::pair<std::string, std::string>>& App::genres() {
    if (genres_.empty()) {
        genres_.push_back({"", "Auto"});
        std::vector<std::pair<std::string, std::string>> found;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(toolRoot() / ".." / "ai_charter" / "genres", ec)) {
            if (e.path().extension() != ".toml") continue;
            std::string name = e.path().stem().string(), line;
            std::ifstream in(e.path());
            while (std::getline(in, line))
                if (line.rfind("name = \"", 0) == 0) {
                    name = line.substr(8, line.rfind('"') - 8);
                    break;
                }
            static const std::map<std::string, std::string> english = {
                {"acustico", "Acoustic / Ballad"}, {"eletronica", "Electronic / Dance"}, {"blues_jazz", "Blues / Jazz"},
                {"mpb", "MPB / Bossa Nova / Samba"}, {"reggae", "Reggae / Ska"}, {"sertanejo", "Sertanejo / Country"}};
            auto it = english.find(e.path().stem().string());
            found.push_back({e.path().stem().string(), it != english.end() ? it->second : name});
        }
        std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.second < b.second; });
        genres_.insert(genres_.end(), found.begin(), found.end());
    }
    return genres_;
}

float App::partTabs(float x, float y, const std::vector<std::string>& parts, std::string& selected) {
    std::vector<std::string> labels;
    int sel = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        std::string l = parts[i];
        l[0] = char(std::toupper(l[0]));
        labels.push_back(l);
        if (parts[i] == selected) sel = int(i);
    }
    if (labels.empty()) return 0;
    return segmented(x, y, labels, sel, [&selected, parts, this](int i) {
        selected = parts[size_t(i)];
        sel_ = -1;
    });
}

void App::drawForgeScreen(Project& p, const SDL_FRect& r) {
    if (jobFor(p, JobKind::Forge)) return drawForgeProgress(p, r);
    if (!p.forged()) {
        // Not forged yet (or it failed): what will happen, the settings and the button.
        const float cx = r.x + r.w * 0.3f, radius = std::min(r.w * 0.2f, r.h * 0.3f);
        drawDisc(cx, r.y + r.h * 0.42f, radius, false);
        const float x = r.x + r.w * 0.56f, w = r.w * 0.44f;
        float y = r.y + r.h * 0.12f;
        const bool failed = p.doc.get("forge", "status") == "failed";
        text(x, y, failed ? "The forge failed" : "Ready to forge", kCream, 3 * u_);
        y += 46 * u_;
        if (failed) {
            for (auto& l : wrap(p.doc.get("forge", "error"), size_t(w / (8 * 1.5f * u_)))) {
                text(x, y, l, kOxblood, 1.5f * u_);
                y += 20 * u_;
            }
            y += 10 * u_;
        }
        for (auto& l : wrap("The forge separates the song into stems (Demucs, a few minutes the first time), then "
                            "charts guitar, bass, drums and vocals with the sheets you gave on Song home.",
                            size_t(w / (8 * 1.5f * u_)))) {
            text(x, y, l, kDim, 1.5f * u_);
            y += 20 * u_;
        }
        const std::string why = forgeBlocker(p);
        if (!why.empty()) text(x, r.y + r.h - 90 * u_, why, kOxblood, 1.25f * u_, w);
        bigButton({x, r.y + r.h - 66 * u_, std::min(w, 380 * u_), 56 * u_}, failed ? "TRY AGAIN" : "FORGE SONG", [this] { forgeSong(); },
                  why.empty());
        return;
    }
    if (chartFor_ != p.id) loadChart(p);
    if (stemsFor_ != p.id) loadStems(p);

    // Toolbar: transport | Band / One part / Difficulties, Mixer | Next.
    float tx = r.x + transport(r.x, r.y) + 24 * u_;
    tx += segmented(tx, r.y, {"Band", "One part", "Difficulties"}, forgeView_, [this](int i) { forgeView_ = i; }) + 12 * u_;
    std::string next = "Next: Fine tuning  >";
    if (tx + buttonW("Mixer") >= r.x + r.w - buttonW(next) - 12 * u_) next = "Fine tuning >";  // narrow: keep Mixer
    if (tx + buttonW("Mixer") < r.x + r.w - buttonW(next) - 12 * u_)
        button(tx, r.y, "Mixer", [this] { showPanels_ = !showPanels_; }, true, showPanels_, 34 * u_);
    button(r.x + r.w - buttonW(next), r.y, next, [this] { setScreen(Screen::Tune); }, true, true, 34 * u_);
    const float top = r.y + 50 * u_;
    if (!showPanels_) {  // the charts get the whole width
        drawPreview(p, {r.x, top, r.w, r.y + r.h - top});
    } else {  // the stems mixer on the left
        const float leftW = std::min(420 * u_, r.w * 0.34f);
        drawMixer({r.x, top, leftW, r.y + r.h - top});
        drawPreview(p, {r.x + leftW + 20 * u_, top, r.w - leftW - 20 * u_, r.y + r.h - top});
    }
    drawDropdown();
    drawOffsetPopover();
}

void App::drawForgeProgress(Project& p, const SDL_FRect& r) {
    const bool running = activeFor(p, JobKind::Forge);
    const float cx = r.x + r.w / 2, radius = std::min(r.w * 0.2f, r.h * 0.28f);
    drawDisc(cx, r.y + r.h * 0.36f, radius, true);
    float y = r.y + r.h * 0.36f + radius + 34 * u_;
    const std::string t = running ? "Forging " + p.title() : "Waiting for the job before it...";
    text(cx - std::min(textW(t, 2.5f * u_), r.w) / 2, y, t, kCream, 2.5f * u_, r.w);
    y += 42 * u_;
    const float bw = std::min(620 * u_, r.w - 80 * u_);
    progressBar({cx - bw / 2, y, bw, 16 * u_}, running ? jobProgress_ : -1, !running || jobProgress_ < 0);
    if (running && jobProgress_ >= 0) {
        const std::string pc = std::to_string(int(jobProgress_ * 100)) + "%";
        text(cx + bw / 2 + 12 * u_, y + 2 * u_, pc, kSand, 1.5f * u_);
    }
    y += 32 * u_;
    text(cx - std::min(textW(jobStatus_, 1.5f * u_), bw) / 2, y, running ? jobStatus_ : "", kDim, 1.5f * u_, bw);
    y += 30 * u_;
    const std::string el = running ? "elapsed " + clockText(double(SDL_GetTicks() - busySince_) / 1000.0) : "";
    text(cx - textW(el, 1.25f * u_) / 2, y, el, kMuted, 1.25f * u_);
}

void App::drawMixer(const SDL_FRect& r) {
    fill(r, kPanel);
    text(r.x + 14 * u_, r.y + 12 * u_, "STEMS", kSand, 1.5f * u_);
    const bool has = !player_.empty();
    float bx = r.x + r.w - 12 * u_ - buttonW("Mute all");
    button(bx, r.y + 6 * u_, "Mute all", [this] { player_.setAllMuted(true, false); }, has, false, 26 * u_);
    bx -= buttonW("All on") + 6 * u_;
    button(bx, r.y + 6 * u_, "All on", [this] { player_.setAllMuted(false, true); }, has, false, 26 * u_);
    const auto& stems = player_.stems();
    const float top = r.y + 42 * u_, rowH = std::min(44 * u_, (r.h - 48 * u_) / float(std::max<size_t>(1, stems.size())));
    const float labelW = 150 * u_;
    const double dur = player_.duration(), now = player_.playhead();
    for (size_t i = 0; i < stems.size(); ++i) {
        const Stem& s = *stems[i];
        SDL_FRect row{r.x + 8 * u_, top + float(i) * rowH, r.w - 16 * u_, rowH - 4 * u_};
        const SDL_Color c = stemColor(s.name);
        fill(row, kBar);
        fill({row.x, row.y, 4 * u_, row.h}, s.muted ? kMuted : c);
        const std::string shown = s.name == "other" ? "rest" : s.name;  // other + keys: the rest of the mix
        text(row.x + 12 * u_, row.y + (row.h - 12 * u_) / 2, shown, s.muted ? kMuted : kCream, 1.5f * u_, labelW - 70 * u_);
        bool soloed = !s.muted;
        for (size_t k = 0; k < stems.size(); ++k)
            if (k != i && !stems[k]->muted) soloed = false;
        const float bs = 20 * u_, by = row.y + (row.h - bs) / 2;
        SDL_FRect mb{row.x + labelW - 2 * bs - 12 * u_, by, bs, bs}, sb{row.x + labelW - bs - 6 * u_, by, bs, bs};
        fill(mb, s.muted ? kOxblood : (hovered(mb) ? kHover : kPanel));
        fill(sb, soloed ? kSand : (hovered(sb) ? kHover : kPanel));
        text(mb.x + (bs - 12 * u_) / 2, by + (bs - 12 * u_) / 2, "M", kCream, 1.5f * u_);
        text(sb.x + (bs - 12 * u_) / 2, by + (bs - 12 * u_) / 2, "S", soloed ? kInk : kCream, 1.5f * u_);
        hit(mb, [this, i](float, float) { player_.toggleMute(i); });
        hit(sb, [this, i, soloed](float, float) { soloed ? player_.setAllMuted(false, true) : player_.solo(i); });
        SDL_FRect wave{row.x + labelW, row.y + 2 * u_, row.w - labelW - 4 * u_, row.h - 4 * u_};
        color(s.muted ? kMuted : c, s.muted ? 120 : 220);
        const float mid = wave.y + wave.h / 2, amp = wave.h / 2;
        const int cols = std::max(1, int(wave.w / u_));
        for (int x = 0; x < cols; ++x) {
            size_t b0 = size_t(x) * s.peaks.size() / size_t(cols);
            size_t b1 = std::max(b0 + 1, size_t(x + 1) * s.peaks.size() / size_t(cols));
            float pk = 0;
            for (size_t b = b0; b < b1 && b < s.peaks.size(); ++b) pk = std::max(pk, s.peaks[b]);
            const float hh = std::max(0.5f, pk * amp), px = wave.x + float(x) * u_;
            SDL_RenderLine(renderer_, px, mid - hh, px, mid + hh);
        }
        if (dur > 0) fill({wave.x + float(now / dur) * wave.w, wave.y, std::max(1.f, u_), wave.h}, kCream);
        hit(wave, [this, wave](float x, float) { player_.seek(double((x - wave.x) / wave.w) * player_.duration()); });
    }
    if (stems.empty()) text(r.x + 14 * u_, top + 6 * u_, "No stems yet.", kMuted, 1.5f * u_);
}

void App::drawPreview(Project& p, const SDL_FRect& r) {
    (void)p;
    fill(r, kPanel);
    auto parts = chartParts();
    if (forgeView_ == 2) return drawDifficulties(r);
    if (forgeView_ == 0) {
        text(r.x + 14 * u_, r.y + 20 * u_, "THE BAND", kSand, 1.75f * u_);
        {  // R28: the stems no charted part uses ("other", keys, absent parts)
            const auto rest = restOfMix(p);
            bool on = false;
            for (size_t i : rest) on = on || !player_.stems()[i]->muted;
            const float cx = r.x + 14 * u_ + textW("THE BAND", 1.75f * u_) + 30 * u_;
            SDL_FRect cb{cx, r.y + 18 * u_, 16 * u_, 16 * u_};
            checkbox(cb, on);
            const std::string l = "Rest of the mix";
            text(cb.x + 24 * u_, cb.y + 3 * u_, l, rest.empty() ? kMuted : on ? kCream : kDim, 1.25f * u_);
            if (!rest.empty())
                hit({cb.x - 4 * u_, cb.y - 6 * u_, textW(l, 1.25f * u_) + 34 * u_, 28 * u_}, [this, rest, on](float, float) {
                    for (size_t i : rest)
                        if (player_.stems()[i]->muted == on) player_.toggleMute(i);
                });
        }
        const std::vector<std::string> diffs = {"Easy", "Medium", "Hard", "Expert"};
        segmented(r.x + r.w - 12 * u_ - segmentedW(diffs), r.y + 10 * u_, diffs, previewDiff_, [this](int i) { previewDiff_ = i; });
        const float overviewH = 30 * u_;
        SDL_FRect area{r.x, r.y + 54 * u_, r.w, r.h - 54 * u_ - overviewH - 22 * u_};
        boardRect_ = area;
        drawBandView(area);
        const std::string first = parts.empty() || parts[0] == "vocals" ? "guitar" : parts[0];
        drawOverview({r.x + 12 * u_, r.y + r.h - overviewH - 10 * u_, r.w - 24 * u_, overviewH}, partNotes(first, previewDiff_), false, first);
        return;
    }
    if (!parts.empty() && std::find(parts.begin(), parts.end(), previewPart_) == parts.end()) previewPart_ = parts[0];
    const bool vocals = previewPart_ == "vocals";
    const float tabsW = partTabs(r.x + 12 * u_, r.y + 10 * u_, parts, previewPart_);
    const std::vector<std::string> diffs = {"Easy", "Medium", "Hard", "Expert"};
    const bool twoRows = tabsW + segmentedW(diffs) + 40 * u_ > r.w;  // narrow: difficulty under the parts
    if (!vocals) {
        const float dx = twoRows ? r.x + 12 * u_ : r.x + r.w - 12 * u_ - segmentedW(diffs);
        segmented(dx, r.y + (twoRows ? 50 : 10) * u_, diffs, previewDiff_, [this](int i) { previewDiff_ = i; });
    }
    const float overviewH = 30 * u_;
    const float head = (twoRows ? 94 : 54) * u_;
    SDL_FRect area{r.x, r.y + head, r.w, r.h - head - overviewH - 22 * u_};
    boardRect_ = area;
    const float now = float(player_.playhead());
    const auto* notes = vocals ? nullptr : partNotes(previewPart_, previewDiff_);
    if (vocals) drawVocalBoard(area, now);
    else drawHighway(area, notes, previewPart_ == "drums", now);
    const int n = chart_.count(previewPart_, previewDiff_);
    const std::string cnt = n < 0 ? "not charted" : std::to_string(n) + (vocals ? " syllables" : " notes");
    text(r.x + 14 * u_, area.y + 4 * u_, cnt, kDim, 1.25f * u_);
    drawOverview({r.x + 12 * u_, r.y + r.h - overviewH - 10 * u_, r.w - 24 * u_, overviewH}, notes, vocals, previewPart_);
    if (parts.empty()) {
        const std::string a = "The forge made no charts. Check the stems and forge again.";
        text(r.x + (r.w - textW(a, 1.5f * u_)) / 2, r.y + r.h / 2, a, kSand, 1.5f * u_);
    }
}

// Every part at once, like a Rock Band screen: the vocals across the top (when the song has
// lyrics), then one highway per part (guitar, bass, drums) side by side, all on one playhead.
// Over each: its sound on / off and its Chart offset (sync).
void App::drawBandView(const SDL_FRect& r) {
    const float now = float(player_.playhead());
    Project* p = currentProject();
    std::vector<std::string> lanes;
    for (const char* part : {"guitar", "bass", "drums"})
        if (chart_.hasPart(part) || (p && !partAbsent(*p, part))) lanes.push_back(part);
    // A part's sound: the stem it is made from, on or off (R22).
    auto soundBox = [&](float x, float y, const std::string& part) {
        const std::string stem = part == "drums" || part == "vocals" || !p ? part : sourceOf(*p, part);
        int index = -1;
        for (size_t i = 0; i < player_.stems().size(); ++i)
            if (player_.stems()[i]->name == stem) index = int(i);
        SDL_FRect cb{x, y, 16 * u_, 16 * u_};
        const bool on = index >= 0 && !player_.stems()[size_t(index)]->muted;
        checkbox(cb, on);
        if (index >= 0) hit({cb.x - 4 * u_, cb.y - 4 * u_, cb.w + 8 * u_, cb.h + 8 * u_}, [this, index](float, float) {
                player_.toggleMute(size_t(index));
            });
        return cb.w + 10 * u_;
    };
    // Chart offset (R27): opens the sync popover under the button.
    auto offsetButton = [&](float right, float y, const std::string& part, bool narrow = false) {
        const int ms = p ? std::atoi(p->doc.get("forge", part + "_offset_ms").c_str()) : 0;
        const std::string what = narrow ? "Offset" : "Chart offset";
        const std::string label = ms ? what + " " + std::string(ms > 0 ? "+" : "") + std::to_string(ms) + (narrow ? "" : " ms") : what;
        const float ts = 1.1f * u_, w = textW(label, ts) + 16 * u_;
        SDL_FRect b{right - w, y, w, 20 * u_};
        const bool open = offsetPart_ == part;
        fill(b, open ? kOxblood : hovered(b) ? kHover : kBar);
        frame(b, open ? kOxblood : kMuted);
        text(b.x + 8 * u_, b.y + 6 * u_, label, ms ? kCream : kSand, ts);
        hit(b, [this, part, b](float, float) {
            if (offsetPart_ == part) {
                offsetPart_.clear();
                return;
            }
            offsetPart_ = part;
            offsetAnchor_ = b;
        });
        if (open) offsetAnchor_ = b;
        return w;
    };
    float y = r.y;
    if (p && (showVocals(*p) || !chart_.vocals.empty())) {  // R29: no lyrics, no vocals track
        const float vh = std::min(r.h * 0.26f, 190 * u_);
        SDL_FRect strip{r.x + 8 * u_, y, r.w - 16 * u_, vh};
        fill(strip, {0x13, 0x11, 0x15, 255});
        fill({strip.x, strip.y, 3 * u_, 22 * u_}, stemColor("vocals"));
        const float sx = strip.x + 10 * u_ + soundBox(strip.x + 10 * u_, strip.y + 3 * u_, "vocals");
        text(sx, strip.y + 6 * u_, chart_.vocals.empty() ? "Vocals" : "Vocals  " + std::to_string(chart_.vocals.size()) + " syllables",
             kSand, 1.25f * u_);
        if (!chart_.vocals.empty()) offsetButton(strip.x + strip.w - 8 * u_, strip.y + 1 * u_, "vocals");
        drawVocalBoard({strip.x, strip.y + 14 * u_, strip.w, strip.h - 14 * u_}, now);
        if (chart_.vocals.empty()) {
            const std::string a = "No vocals came out of the lyrics: check them on Song home and forge again.";
            text(strip.x + (strip.w - textW(a, 1.25f * u_)) / 2, strip.y + strip.h / 2, a, kMuted, 1.25f * u_);
        }
        y += vh + 8 * u_;
    }
    if (lanes.empty()) return;
    const float gap = 10 * u_, w = (r.w - gap * float(lanes.size() + 1)) / float(lanes.size());
    for (size_t i = 0; i < lanes.size(); ++i) {
        const std::string& part = lanes[i];
        const float x = r.x + gap + float(i) * (w + gap);
        std::string label = part;
        label[0] = char(std::toupper(label[0]));
        const int n = chart_.count(part, previewDiff_);
        const std::string count = n < 0 ? "not charted" : std::to_string(n) + " notes";
        fill({x, y, 3 * u_, 22 * u_}, stemColor(part));
        const float lx = x + 10 * u_ + soundBox(x + 10 * u_, y + 3 * u_, part);
        text(lx, y + 6 * u_, label, kCream, 1.5f * u_);
        float right = x + w - 4 * u_;
        const bool narrow = w < lx - x + textW(label, 1.5f * u_) + textW("Chart offset +100 ms", 1.1f * u_) + 40 * u_;
        if (n >= 0) right -= offsetButton(right, y + 1 * u_, part, narrow) + 8 * u_;
        const float cx = lx + textW(label, 1.5f * u_) + 12 * u_;
        if (right - textW(count, 1.1f * u_) > cx) text(right - textW(count, 1.1f * u_), y + 8 * u_, count, n < 0 ? kMuted : kDim, 1.1f * u_);
        drawHighway({x, y + 26 * u_, w, r.y + r.h - y - 26 * u_}, partNotes(part, previewDiff_), part == "drums", now);
    }
}

// R29: a vocals track only when the song has lyrics (typed, a file; or a chart with vocals).
bool App::showVocals(const Project& p) const { return lyricsChoice(p) != "none"; }

// Stems no charted part plays: other, keys, and the stems of absent / uncharted parts.
std::vector<size_t> App::restOfMix(const Project& p) const {
    std::vector<std::string> used;
    for (const char* part : {"guitar", "bass"})
        if (chart_.hasPart(part)) used.push_back(sourceOf(p, part));
    if (chart_.hasPart("drums")) used.push_back("drums");
    if (!chart_.vocals.empty()) used.push_back("vocals");
    std::vector<size_t> out;
    for (size_t i = 0; i < player_.stems().size(); ++i) {
        const std::string& name = player_.stems()[i]->name;
        if (name != "mix" && std::find(used.begin(), used.end(), name) == used.end()) out.push_back(i);
    }
    return out;
}

// Move a part's chart (every difficulty; the vocals with their phrases) by `seconds`.
void App::shiftChart(const std::string& part, float seconds) {
    if (seconds == 0) return;
    pushUndo();
    if (part == "vocals") {
        for (auto& v : chart_.vocals) v.t = std::max(0.f, v.t + seconds);
        for (auto& [a, b] : chart_.phrases) {
            a = std::max(0.f, a + seconds);
            b = std::max(0.f, b + seconds);
        }
    } else if (auto it = chart_.parts.find(part); it != chart_.parts.end()) {
        for (auto& diff : it->second)
            for (auto& n : diff) n.t = std::max(0.f, n.t + seconds);
    }
    chart_.sort();
    chart_.offsetMs[part] += int(std::lround(seconds * 1000));
    chartChanged();
    syncOffsets();
}

// project.yaml follows the chart's offsets (after a shift, an undo or a redo).
void App::syncOffsets() {
    Project* p = currentProject();
    if (!p || chartFor_ != p->id) return;
    for (const char* part : {"guitar", "bass", "drums", "vocals"}) {
        auto it = chart_.offsetMs.find(part);
        const int ms = it == chart_.offsetMs.end() ? 0 : it->second;
        setValue(*p, "forge", std::string(part) + "_offset_ms", ms ? std::to_string(ms) : "");
    }
}

// The song's audio against every chart (positive: the audio plays later). Export pads / trims.
void App::setSongOffset(int ms) {
    Project* p = currentProject();
    if (!p) return;
    ms = std::clamp(ms, -5000, 5000);
    setValue(*p, "forge", "audio_offset_ms", ms ? std::to_string(ms) : "");
    player_.setOffset(ms / 1000.0);
}

// The sync popover under a part's Chart offset button. Playback goes on while it is open,
// so the notes can be nudged until they land on the sound.
void App::drawOffsetPopover() {
    Project* p = currentProject();
    if (offsetPart_.empty() || !p) return;
    std::string label = offsetPart_;
    label[0] = char(std::toupper(label[0]));
    const float w = 560 * u_, h = 196 * u_;
    int ww, wh;
    SDL_GetCurrentRenderOutputSize(renderer_, &ww, &wh);
    SDL_FRect box{std::min(offsetAnchor_.x + offsetAnchor_.w - w, float(ww) - w - 8 * u_), offsetAnchor_.y + offsetAnchor_.h + 6 * u_, w, h};
    box.x = std::max(8 * u_, box.x);
    offsetRect_ = box;
    fill({box.x + 4 * u_, box.y + 5 * u_, box.w, box.h}, kInk, 180);
    fill(box, kPanel);
    frame(box, kMuted);
    fill({box.x, box.y, box.w, 3 * u_}, kOxblood);
    hit(box, [](float, float) {});  // clicks inside stay inside
    float y = box.y + 14 * u_;
    const float x = box.x + 16 * u_;
    text(x, y, "Sync " + label, kCream, 1.75f * u_);
    y += 28 * u_;
    segmented(x, y, {label + " chart", "Whole song audio"}, offsetMode_, [this](int i) { offsetMode_ = i; }, 28 * u_);
    y += 40 * u_;
    const int songMs = std::atoi(p->doc.get("forge", "audio_offset_ms").c_str());
    const int chartMs = std::atoi(p->doc.get("forge", offsetPart_ + "_offset_ms").c_str());
    const int value = offsetMode_ == 0 ? chartMs : songMs;
    const std::string v = (value > 0 ? "+" : "") + std::to_string(value) + " ms";
    text(x, y + 4 * u_, v, value ? kCream : kDim, 2.5f * u_);
    // Steps: chart later (+) moves the notes later; song later delays the audio.
    float bx = x + 130 * u_;
    for (int step : {-100, -10, -1, 1, 10, 100}) {
        const std::string l = (step > 0 ? "+" : "") + std::to_string(step);
        bx += button(bx, y, l, [this, step] {
                  Project* q = currentProject();
                  if (!q) return;
                  if (offsetMode_ == 0) shiftChart(offsetPart_, step / 1000.f);
                  else setSongOffset(std::atoi(q->doc.get("forge", "audio_offset_ms").c_str()) + step);
              }, true, false, 28 * u_) + 4 * u_;
    }
    y += 40 * u_;
    text(x, y, offsetMode_ == 0 ? "+ moves the " + offsetPart_ + " notes later, - earlier."
                                : "+ plays the audio later than every chart, - earlier.",
         kDim, 1.1f * u_, w - 32 * u_);
    y += 18 * u_;
    text(x, y, "Left / Right: 10 ms, Shift: 1 ms. Keep playing to hear it.", kMuted, 1.1f * u_, w - 32 * u_);
    y += 24 * u_;
    button(x, y, "Reset", [this] {
        Project* q = currentProject();
        if (!q) return;
        if (offsetMode_ == 0) shiftChart(offsetPart_, -std::atoi(q->doc.get("forge", offsetPart_ + "_offset_ms").c_str()) / 1000.f);
        else setSongOffset(0);
    }, value != 0, false, 26 * u_);
    button(box.x + box.w - 16 * u_ - buttonW("Done"), y, "Done", [this] { offsetPart_.clear(); }, true, true, 26 * u_);
}

// One instrument, every difficulty side by side (Easy .. Expert), on one playhead (R34).
void App::drawDifficulties(const SDL_FRect& r) {
    std::vector<std::string> parts;
    for (const char* part : {"guitar", "bass", "drums"})
        if (chart_.hasPart(part)) parts.push_back(part);
    if (parts.empty()) {
        const std::string a = "No instrument charts. Forge again.";
        text(r.x + (r.w - textW(a, 1.5f * u_)) / 2, r.y + r.h / 2, a, kSand, 1.5f * u_);
        return;
    }
    if (std::find(parts.begin(), parts.end(), previewPart_) == parts.end()) previewPart_ = parts[0];
    partTabs(r.x + 12 * u_, r.y + 10 * u_, parts, previewPart_);
    const std::string hint = "Every difficulty of one part, side by side";
    text(r.x + r.w - textW(hint, 1.25f * u_) - 14 * u_, r.y + 22 * u_, hint, kMuted, 1.25f * u_);
    const float now = float(player_.playhead()), overviewH = 30 * u_, top = r.y + 54 * u_;
    const float gap = 10 * u_, w = (r.w - gap * 5) / 4, h = r.y + r.h - top - overviewH - 22 * u_;
    boardRect_ = {r.x, top, r.w, h};
    const char* names[4] = {"Easy", "Medium", "Hard", "Expert"};
    for (int d = 0; d < 4; ++d) {
        const float x = r.x + gap + float(d) * (w + gap);
        const int n = chart_.count(previewPart_, d);
        fill({x, top, 3 * u_, 22 * u_}, stemColor(previewPart_));
        text(x + 10 * u_, top + 6 * u_, names[d], d == 3 ? kCream : kSand, 1.5f * u_);
        const std::string cnt = n < 0 ? "-" : std::to_string(n) + " notes";
        text(x + w - textW(cnt, 1.1f * u_) - 4 * u_, top + 8 * u_, cnt, kDim, 1.1f * u_);
        drawHighway({x, top + 26 * u_, w, h - 26 * u_}, partNotes(previewPart_, d), previewPart_ == "drums", now);
    }
    drawOverview({r.x + 12 * u_, r.y + r.h - overviewH - 10 * u_, r.w - 24 * u_, overviewH}, partNotes(previewPart_, 3), false,
                 previewPart_);
}
