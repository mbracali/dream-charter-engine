// gd-tool-02 Fine tuning: a piano-roll editor for the forged chart (forge/chart.txt).
// Guitar / bass / keys / drums: five lanes over time, notes snapped to the beat grid;
// add (click), select, move (drag), sustain (drag the tail end, or Alt+drag), delete
// (right click / Delete), HOPO / Star Power / cymbal (H / S / C). Vocals: pitch rows over
// time; move, re-pitch, resize, and edit each syllable's text. Undo / redo; every change
// is saved to chart.txt (Export reads it). A highway on the right previews the result.
#include <algorithm>
#include <cmath>

#include "app.h"
#include "text_util.h"

namespace {

const char* const kFretNames[5] = {"Green", "Red", "Yellow", "Blue", "Orange"};
const char* const kDrumNames[5] = {"Kick", "Red", "Yellow", "Blue", "Green"};
const char* const kNoteNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
const int kSnaps[] = {1, 2, 4, 3, 6, 8};  // per beat: 1/4, 1/8, 1/16, triplet 8ths, triplet 16ths, 1/32
const char* const kSnapLabels[] = {"4", "8", "16", "8T", "16T", "32"};

constexpr float kLabelW = 100;  // lane-name column (x u_)

}  // namespace

std::vector<ChartNote>* App::editNotes() {
    if (tuneVocals()) return nullptr;
    auto it = chart_.parts.find(tunePart_);
    if (it == chart_.parts.end()) return nullptr;
    return &it->second[size_t(std::clamp(tuneDiff_, 0, 3))];
}

float App::rollX(float t) const { return rollRect_.x + kLabelW * u_ + (t - viewStart_) * pps_ * u_; }
float App::rollT(float x) const { return viewStart_ + (x - rollRect_.x - kLabelW * u_) / (pps_ * u_); }

int App::laneAtY(float y) const {
    const float rowH = rollRect_.h / 5;
    return std::clamp(int((y - rollRect_.y) / rowH), 0, 4);
}

float App::pitchY(int pitch) const {
    const float rowH = rollRect_.h / float(pitchHi_ - pitchLo_ + 1);
    return rollRect_.y + (float(pitchHi_ - pitch) + 0.5f) * rowH;
}

int App::pitchAtY(float y) const {
    const float rowH = rollRect_.h / float(pitchHi_ - pitchLo_ + 1);
    return std::clamp(pitchHi_ - int((y - rollRect_.y) / rowH), pitchLo_, pitchHi_);
}

int App::noteAt(float x, float y) const {
    auto* self = const_cast<App*>(this);
    const auto* notes = self->editNotes();
    if (!notes) return -1;
    const int lane = laneAtY(y);
    const float rowH = rollRect_.h / 5, rad = std::min(rowH * 0.34f, 13 * u_) + 3 * u_;
    int best = -1;
    float bestD = 1e9f;
    for (size_t i = 0; i < notes->size(); ++i) {
        const ChartNote& n = (*notes)[i];
        if (n.lane != lane) continue;
        const float nx = rollX(n.t), ex = rollX(n.t + n.sustain);
        float d = std::abs(x - nx);
        if (n.sustain > 0 && x >= nx && x <= ex + 6 * u_) d = 0;  // on the tail
        if (d <= rad && d < bestD) {
            bestD = d;
            best = int(i);
        }
    }
    return best;
}

int App::vocalAt(float x, float y) const {
    const float rowH = rollRect_.h / float(pitchHi_ - pitchLo_ + 1), h = std::max(rowH * 0.9f, 8 * u_);
    for (size_t i = 0; i < chart_.vocals.size(); ++i) {
        const VocalNote& v = chart_.vocals[i];
        const float x0 = rollX(v.t), x1 = std::max(rollX(v.t + v.dur), x0 + 6 * u_), py = pitchY(v.pitch);
        if (x >= x0 - 3 * u_ && x <= x1 + 3 * u_ && std::abs(y - py) <= h / 2 + 3 * u_) return int(i);
    }
    return -1;
}

void App::selectNote(int index) {
    sel_ = index;
    if (tuneVocals() && sel_ >= 0 && sel_ < int(chart_.vocals.size())) player_.seek(chart_.vocals[size_t(sel_)].t);
}

void App::pushUndo() {
    undo_.push_back(chart_);
    if (undo_.size() > 200) undo_.erase(undo_.begin());
    redo_.clear();
}

void App::undo() {
    if (undo_.empty()) return;
    redo_.push_back(chart_);
    chart_ = undo_.back();
    undo_.pop_back();
    sel_ = -1;
    chartChanged();
    syncOffsets();
    status_ = "Undone.";
}

void App::redo() {
    if (redo_.empty()) return;
    undo_.push_back(chart_);
    chart_ = redo_.back();
    redo_.pop_back();
    sel_ = -1;
    chartChanged();
    syncOffsets();
    status_ = "Redone.";
}

void App::chartChanged() {
    chartDirty_ = true;
    if (drag_ == Drag::None) saveChart();
}

void App::revertChart() {
    Project* p = currentProject();
    if (!p) return;
    Chart forged;
    if (!forged.load(p->dir / "forge" / "forged.txt")) {
        status_ = "No forged copy to go back to (forge again).";
        return;
    }
    pushUndo();
    forged.offsetMs = chart_.offsetMs;  // forged.txt has the offsets the forge applied
    chart_ = forged;
    sel_ = -1;
    chartChanged();
    setValue(*p, "forge", "tuned", "no");
    status_ = "Back to the forged chart (" + kCmd + "Z brings your edits back).";
}

void App::deleteSelection() {
    if (sel_ < 0) return;
    if (tuneVocals()) {
        if (sel_ >= int(chart_.vocals.size())) return;
        pushUndo();
        chart_.vocals.erase(chart_.vocals.begin() + sel_);
    } else {
        auto* notes = editNotes();
        if (!notes || sel_ >= int(notes->size())) return;
        pushUndo();
        notes->erase(notes->begin() + sel_);
    }
    sel_ = -1;
    chartChanged();
}

void App::toggleFlag(char flag) {
    auto* notes = editNotes();
    if (!notes || sel_ < 0 || sel_ >= int(notes->size())) return;
    ChartNote& n = (*notes)[size_t(sel_)];
    const bool drums = tunePart_ == "drums";
    if (flag == 'c' && (!drums || n.lane < 2)) return;
    if (flag == 'h' && drums) return;
    pushUndo();
    // A flag belongs to the whole chord (notes at the same time).
    const float t = n.t;
    const bool on = flag == 'h' ? !n.hopo : flag == 's' ? !n.star : !n.cymbal;
    for (auto& m : *notes) {
        if (std::abs(m.t - t) > 1e-4f) continue;
        if (flag == 'h') m.hopo = on;
        else if (flag == 's') m.star = on;
        else if (flag == 'c' && &m == &n) m.cymbal = on;
    }
    chartChanged();
}

void App::followPlayhead() {
    if (rollRect_.w <= 0) return;
    const float t = float(player_.playhead()), span = (rollRect_.w - kLabelW * u_) / (pps_ * u_);
    if (t < viewStart_ || t > viewStart_ + span * 0.85f) viewStart_ = std::max(0.f, t - span * 0.15f);
}

void App::tuneWheel(float dx, float dy, bool zoom) {
    if (zoom) {
        const float anchor = rollT(mouseX_);
        pps_ = std::clamp(pps_ * (dy > 0 ? 1.15f : 1 / 1.15f), 30.f, 1500.f);
        viewStart_ = std::max(0.f, anchor - (mouseX_ - rollRect_.x - kLabelW * u_) / (pps_ * u_));
        return;
    }
    const float step = 120.f / pps_;  // ~120 px per notch
    viewStart_ = std::max(0.f, viewStart_ - (dy - dx) * step);
    if (player_.playing()) follow_ = false;
}

void App::tuneMouseDown(float x, float y, Uint8 button, bool shift) {
    (void)shift;
    if (inside(rulerRect_, x, y)) {  // the ruler: seek / scrub
        player_.seek(std::max(0.f, rollT(x)));
        drag_ = Drag::Scrub;
        return;
    }
    if (x < rollRect_.x + kLabelW * u_) return;
    const bool alt = SDL_GetModState() & SDL_KMOD_ALT;
    dragX_ = x;
    dragY_ = y;
    dragPushed_ = false;
    if (tuneVocals()) {
        const int i = vocalAt(x, y);
        if (button == SDL_BUTTON_RIGHT) {
            if (i >= 0) {
                sel_ = i;
                deleteSelection();
            }
            return;
        }
        if (i >= 0) {
            sel_ = i;
            const VocalNote& v = chart_.vocals[size_t(i)];
            dragT0_ = v.t;
            dragV0_ = v.dur;
            dragLane0_ = v.pitch;
            drag_ = std::abs(x - rollX(v.t + v.dur)) <= 6 * u_ || alt ? Drag::Resize : Drag::Move;
            return;
        }
        // A new syllable at the click, one grid step long; type its text.
        pushUndo();
        dragPushed_ = true;
        VocalNote v;
        v.t = chart_.snap(rollT(x), snap_);
        v.dur = std::max(0.08f, chart_.stepFrom(v.t, snap_, 1) - v.t);
        v.pitch = pitchAtY(y);
        chart_.vocals.push_back(v);
        chart_.sort();
        for (size_t k = 0; k < chart_.vocals.size(); ++k)
            if (chart_.vocals[k].t == v.t && chart_.vocals[k].pitch == v.pitch) sel_ = int(k);
        dragT0_ = v.t;
        dragV0_ = v.dur;
        dragLane0_ = v.pitch;
        drag_ = Drag::Move;
        chartChanged();
        focusVocalLyric();
        dragPushed_ = false;
        return;
    }
    auto* notes = editNotes();
    if (!notes) return;
    const int i = noteAt(x, y);
    if (button == SDL_BUTTON_RIGHT) {
        if (i >= 0) {
            sel_ = i;
            deleteSelection();
        }
        return;
    }
    if (i >= 0) {
        sel_ = i;
        const ChartNote& n = (*notes)[size_t(i)];
        dragT0_ = n.t;
        dragV0_ = n.sustain;
        dragLane0_ = n.lane;
        const bool onTailEnd = n.sustain > 0 && std::abs(x - rollX(n.t + n.sustain)) <= 7 * u_;
        drag_ = (onTailEnd || alt) && tunePart_ != "drums" ? Drag::Sustain : Drag::Move;
        return;
    }
    // A new note at the click.
    ChartNote n;
    n.t = chart_.snap(rollT(x), snap_);
    n.lane = laneAtY(y);
    for (auto& m : *notes)
        if (std::abs(m.t - n.t) < 1e-3f && m.lane == n.lane) return;  // already there
    // Join the chord's flags (HOPO / Star Power) when it lands on one.
    for (auto& m : *notes)
        if (std::abs(m.t - n.t) < 1e-3f) {
            n.hopo = m.hopo;
            n.star = m.star;
        }
    if (tunePart_ == "drums" && n.lane >= 2) n.cymbal = n.lane != 3;  // a fresh hit: yellow/green cymbals, blue tom
    pushUndo();
    notes->push_back(n);
    chart_.sort();
    for (size_t k = 0; k < notes->size(); ++k)
        if (std::abs((*notes)[k].t - n.t) < 1e-5f && (*notes)[k].lane == n.lane) sel_ = int(k);
    dragT0_ = n.t;
    dragV0_ = 0;
    dragLane0_ = n.lane;
    drag_ = Drag::Move;
    dragPushed_ = true;
    chartChanged();
}

void App::tuneMouseMove(float x, float y) {
    if (drag_ == Drag::Scrub) {
        player_.seek(std::max(0.f, rollT(x)));
        return;
    }
    if (sel_ < 0) return;
    const float dt = (x - dragX_) / (pps_ * u_);
    if (tuneVocals()) {
        if (sel_ >= int(chart_.vocals.size())) return;
        VocalNote v = chart_.vocals[size_t(sel_)];
        if (drag_ == Drag::Move) {
            v.t = chart_.snap(std::max(0.f, dragT0_ + dt), snap_);
            v.pitch = std::clamp(dragLane0_ + (pitchAtY(y) - pitchAtY(dragY_)), 36, 84);
        } else if (drag_ == Drag::Resize) {
            v.dur = std::max(0.05f, chart_.snap(v.t + std::max(0.f, dragV0_ + dt), snap_) - v.t);
        }
        const VocalNote& cur = chart_.vocals[size_t(sel_)];
        if (v.t == cur.t && v.pitch == cur.pitch && v.dur == cur.dur) return;
        if (!dragPushed_) {
            pushUndo();
            dragPushed_ = true;
        }
        chart_.vocals[size_t(sel_)] = v;
        chart_.sort();
        for (size_t k = 0; k < chart_.vocals.size(); ++k)
            if (chart_.vocals[k].t == v.t && chart_.vocals[k].pitch == v.pitch && chart_.vocals[k].lyric == v.lyric) sel_ = int(k);
        chartChanged();
        return;
    }
    auto* notes = editNotes();
    if (!notes || sel_ >= int(notes->size())) return;
    ChartNote n = (*notes)[size_t(sel_)];
    if (drag_ == Drag::Move) {
        n.t = chart_.snap(std::max(0.f, dragT0_ + dt), snap_);
        n.lane = std::clamp(dragLane0_ + (laneAtY(y) - laneAtY(dragY_)), 0, 4);
    } else if (drag_ == Drag::Sustain) {
        const float end = chart_.snap(std::max(n.t, rollT(x)), snap_);
        n.sustain = end - n.t < chart_.beatLength(n.t) / 3.f ? 0.f : end - n.t;  // shorter than ~1/3 beat: a tap
    }
    const ChartNote& cur = (*notes)[size_t(sel_)];
    if (n.t == cur.t && n.lane == cur.lane && n.sustain == cur.sustain) return;
    for (size_t k = 0; k < notes->size(); ++k)  // never two notes on one spot
        if (int(k) != sel_ && std::abs((*notes)[k].t - n.t) < 1e-3f && (*notes)[k].lane == n.lane) return;
    if (!dragPushed_) {
        pushUndo();
        dragPushed_ = true;
    }
    if (tunePart_ == "drums" && n.lane < 2) n.cymbal = false;
    (*notes)[size_t(sel_)] = n;
    chart_.sort();
    for (size_t k = 0; k < notes->size(); ++k)
        if (std::abs((*notes)[k].t - n.t) < 1e-5f && (*notes)[k].lane == n.lane) sel_ = int(k);
    chartChanged();
}

void App::tuneMouseUp() {
    drag_ = Drag::None;
    if (focus_ != Focus::VocalLyric) dragPushed_ = false;
    saveChart();
}

bool App::tuneKey(SDL_Keycode key, SDL_Keymod mod) {
    const bool cmd = mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL), shift = mod & SDL_KMOD_SHIFT;
    if (cmd) {
        if (key == SDLK_Z) shift ? redo() : undo();
        else if (key == SDLK_Y) redo();
        else return false;
        return true;
    }
    if (key == SDLK_DELETE || key == SDLK_BACKSPACE) {
        deleteSelection();
        return true;
    }
    if (tuneVocals()) {
        if (sel_ < 0 || sel_ >= int(chart_.vocals.size())) return false;
        VocalNote& v = chart_.vocals[size_t(sel_)];
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) {
            focusVocalLyric();
            dragPushed_ = false;
            return true;
        }
        if (key == SDLK_UP || key == SDLK_DOWN) {
            pushUndo();
            v.pitch = std::clamp(v.pitch + (key == SDLK_UP ? 1 : -1), 36, 84);
            chartChanged();
            return true;
        }
        if (key == SDLK_LEFT || key == SDLK_RIGHT) {
            pushUndo();
            v.t = chart_.stepFrom(v.t, snap_, key == SDLK_RIGHT ? 1 : -1);
            const float t = v.t;
            const int pitch = v.pitch;
            chart_.sort();
            for (size_t k = 0; k < chart_.vocals.size(); ++k)
                if (chart_.vocals[k].t == t && chart_.vocals[k].pitch == pitch) sel_ = int(k);
            chartChanged();
            return true;
        }
        return false;
    }
    auto* notes = editNotes();
    if (key == SDLK_H) toggleFlag('h');
    else if (key == SDLK_S) toggleFlag('s');
    else if (key == SDLK_C) toggleFlag('c');
    else if (!notes || sel_ < 0 || sel_ >= int(notes->size())) return false;
    else if (key == SDLK_UP || key == SDLK_DOWN || key == SDLK_LEFT || key == SDLK_RIGHT) {
        ChartNote n = (*notes)[size_t(sel_)];
        if (key == SDLK_UP) n.lane = std::max(0, n.lane - 1);
        else if (key == SDLK_DOWN) n.lane = std::min(4, n.lane + 1);
        else n.t = chart_.stepFrom(n.t, snap_, key == SDLK_RIGHT ? 1 : -1);
        for (size_t k = 0; k < notes->size(); ++k)
            if (int(k) != sel_ && std::abs((*notes)[k].t - n.t) < 1e-3f && (*notes)[k].lane == n.lane) return true;
        pushUndo();
        (*notes)[size_t(sel_)] = n;
        chart_.sort();
        for (size_t k = 0; k < notes->size(); ++k)
            if (std::abs((*notes)[k].t - n.t) < 1e-5f && (*notes)[k].lane == n.lane) sel_ = int(k);
        chartChanged();
    } else return false;
    return true;
}

// ---- drawing ------------------------------------------------------------------------------------

void App::drawTune(Project& p, const SDL_FRect& r) {
    if (chartFor_ != p.id) loadChart(p);
    if (stemsFor_ != p.id) loadStems(p);
    auto parts = chartParts();
    if (!parts.empty() && std::find(parts.begin(), parts.end(), tunePart_) == parts.end()) tunePart_ = parts[0];
    if (player_.playing() && follow_) followPlayhead();

    // Row 1: part, difficulty, grid | undo, redo, revert | next.
    float x = r.x;
    const std::string before = tunePart_;
    x += partTabs(x, r.y, parts, tunePart_) + 12 * u_;
    if (tunePart_ != before) unfocus();
    if (!tuneVocals()) {
        x += segmented(x, r.y, {"Easy", "Medium", "Hard", "Expert"}, tuneDiff_, [this](int i) {
                 tuneDiff_ = i;
                 sel_ = -1;
             }) + 12 * u_;
    }
    int snapIdx = 2;
    for (int i = 0; i < 6; ++i)
        if (kSnaps[i] == snap_) snapIdx = i;
    text(x, r.y + 11 * u_, "Grid 1/", kDim, 1.5f * u_);
    x += textW("Grid 1/", 1.5f * u_) + 8 * u_;
    std::vector<std::string> snaps(std::begin(kSnapLabels), std::end(kSnapLabels));
    if (x + segmentedW(snaps) <= r.x + r.w) segmented(x, r.y, snaps, snapIdx, [this](int i) { snap_ = kSnaps[i]; });

    // Row 2: transport, zoom, follow | undo, redo, revert | the selected note.
    const float y2 = r.y + 44 * u_;
    x = r.x + transport(r.x, y2) + 16 * u_;
    x += button(x, y2, "-", [this] { pps_ = std::max(30.f, pps_ / 1.3f); }, true, false, 34 * u_) + 4 * u_;
    x += button(x, y2, "+", [this] { pps_ = std::min(1500.f, pps_ * 1.3f); }, true, false, 34 * u_) + 8 * u_;
    x += button(x, y2, "Follow", [this] { follow_ = !follow_; if (follow_) followPlayhead(); }, true, follow_, 34 * u_) + 16 * u_;
    x += button(x, y2, "Undo", [this] { undo(); }, !undo_.empty(), false, 34 * u_) + 4 * u_;
    x += button(x, y2, "Redo", [this] { redo(); }, !redo_.empty(), false, 34 * u_) + 4 * u_;
    x += button(x, y2, "Revert to forged", [this] { revertChart(); }, true, false, 34 * u_) + 16 * u_;
    // The selected note and its flags.
    std::string info = "Click: add  -  drag: move  -  right click: delete  -  " + kCmd + "Z: undo";
    if (tuneVocals() && sel_ >= 0 && sel_ < int(chart_.vocals.size())) {
        const VocalNote& v = chart_.vocals[size_t(sel_)];
        info = clockTenths(v.t) + "  " + kNoteNames[v.pitch % 12] + std::to_string(v.pitch / 12 - 1) + "  \"" + v.lyric + "\"";
    } else if (auto* notes = editNotes(); notes && sel_ >= 0 && sel_ < int(notes->size())) {
        const ChartNote& n = (*notes)[size_t(sel_)];
        const bool drums = tunePart_ == "drums";
        const char* const* names = drums ? kDrumNames : kFretNames;
        const int lane = std::clamp(n.lane, 0, 4);
        info = clockTenths(n.t) + "  " + names[lane];
        if (!drums) x += button(x, y2, "HOPO (H)", [this] { toggleFlag('h'); }, true, n.hopo, 34 * u_) + 4 * u_;
        x += button(x, y2, "Star (S)", [this] { toggleFlag('s'); }, true, n.star, 34 * u_) + 4 * u_;
        if (drums && n.lane >= 2) x += button(x, y2, "Cymbal (C)", [this] { toggleFlag('c'); }, true, n.cymbal, 34 * u_) + 4 * u_;
        x += 8 * u_;
    }
    text(x, y2 + 11 * u_, info, kDim, 1.25f * u_, r.x + r.w - x);

    // The roll (left) and a highway preview (right).
    const float top = y2 + 50 * u_, overviewH = 34 * u_, lyricH = tuneVocals() ? 40 * u_ : 0;
    const float previewW = r.w > 980 * u_ ? std::min(320 * u_, r.w * 0.26f) : 0;
    const std::string next = "Next: Export song  >";
    const float nextW = buttonW(next);
    button(r.x + r.w - nextW, r.y + r.h - overviewH, next, [this] { saveChart(); setScreen(Screen::Export); }, true, true, overviewH);
    const float rollW = r.w - (previewW > 0 ? previewW + 16 * u_ : 0);
    rulerRect_ = {r.x + kLabelW * u_, top, rollW - kLabelW * u_, 24 * u_};
    rollRect_ = {r.x, top + 26 * u_, rollW, r.y + r.h - top - 26 * u_ - overviewH - 12 * u_ - lyricH};
    if (tuneVocals()) drawVocalRoll(rollRect_);
    else drawRoll(rollRect_);
    if (tuneVocals()) {  // the selected syllable's text
        const float ly = rollRect_.y + rollRect_.h + 8 * u_;
        text(r.x, ly + 9 * u_, "Syllable", kDim, 1.5f * u_);
        SDL_FRect field{r.x + 100 * u_, ly, std::min(360 * u_, rollW - 100 * u_), 28 * u_};
        const bool has = sel_ >= 0 && sel_ < int(chart_.vocals.size());
        const bool focused = focus_ == Focus::VocalLyric;
        fill(field, kInk);
        frame(field, focused ? kOxblood : kMuted);
        const std::string shown = has ? chart_.vocals[size_t(sel_)].lyric : "select a note";
        text(field.x + 8 * u_, field.y + 8 * u_, shown, has ? kCream : kMuted, 1.5f * u_, field.w - 16 * u_);
        if (focused && has && (SDL_GetTicks() / 500) % 2 == 0)
            fill({field.x + 8 * u_ + textW(shown, 1.5f * u_) + u_, field.y + 5 * u_, 2 * u_, field.h - 10 * u_}, kCream);
        if (has) hit(field, [this](float, float) { focusVocalLyric(); dragPushed_ = false; });
        text(field.x + field.w + 12 * u_, ly + 9 * u_, "Enter: edit  -  Tab: next syllable  -  \"-\" joins a word, \"+\" slides", kMuted, 1.25f * u_,
             rollW - field.w - 112 * u_);
    }
    const auto* notes = tuneVocals() ? nullptr : partNotes(tunePart_, tuneDiff_);
    drawOverview({r.x, r.y + r.h - overviewH, previewW > 0 ? rollW : rollW - nextW - 12 * u_, overviewH}, notes, tuneVocals(), tunePart_);
    if (previewW > 0) {
        SDL_FRect pv{r.x + r.w - previewW, top, previewW, r.y + r.h - top - overviewH - 12 * u_};
        fill(pv, kPanel);
        boardRect_ = pv;
        const float now = float(player_.playhead());
        if (tuneVocals()) drawVocalBoard({pv.x, pv.y + 10 * u_, pv.w, pv.h - 20 * u_}, now);
        else drawHighway({pv.x, pv.y + 8 * u_, pv.w, pv.h - 56 * u_}, notes, tunePart_ == "drums", now);
    }
}

// The grid shared by both rolls: beats, bars (numbered on the ruler), sections, playhead.
static void drawGrid(App& a, const SDL_FRect& r) {
    const float x0 = r.x + kLabelW * a.u_, x1 = r.x + r.w;
    const float t0 = a.rollT(x0), t1 = a.rollT(x1);
    a.fill(a.rulerRect_, kBar);
    int bar = 0;
    for (size_t i = 0; i < a.chart_.beats.size(); ++i) {
        const auto& [t, isBar] = a.chart_.beats[i];
        if (isBar) ++bar;
        if (t < t0 - 2 || t > t1) continue;
        const float x = a.rollX(t);
        // Grid subdivisions up to the next beat.
        const float next = i + 1 < a.chart_.beats.size() ? a.chart_.beats[i + 1].first : t + a.chart_.beatLength(t);
        const float subW = (a.rollX(next) - x) / float(a.snap_);
        if (subW > 6 * a.u_)
            for (int k = 1; k < a.snap_; ++k) {
                const float sx = x + subW * float(k);
                if (sx >= x0 && sx <= x1) a.fill({sx, r.y, std::max(1.f, a.u_ * 0.5f), r.h}, kBar, 140);
            }
        if (x < x0 || x > x1) continue;
        a.fill({x, r.y, isBar ? 2 * a.u_ : a.u_, r.h}, isBar ? kMuted : kBar, isBar ? 255 : 255);
        if (isBar) {
            a.fill({x, a.rulerRect_.y, a.u_, a.rulerRect_.h}, kMuted);
            a.text(x + 4 * a.u_, a.rulerRect_.y + 3 * a.u_, std::to_string(bar), kDim, 1.1f * a.u_);
        }
    }
    for (auto& [t, name] : a.chart_.sections) {
        if (t < t0 - 10 || t > t1) continue;
        const float x = std::max(x0, a.rollX(t));
        a.fill({x, a.rulerRect_.y + a.rulerRect_.h - 4 * a.u_, 3 * a.u_, 4 * a.u_}, kOxblood);
        a.text(x + 6 * a.u_, a.rulerRect_.y + 13 * a.u_, name, kSand, a.u_, x1 - x - 8 * a.u_);
    }
    const float px = a.rollX(float(a.player_.playhead()));
    if (px >= x0 && px <= x1) {
        a.fill({px - a.u_, a.rulerRect_.y, 2 * a.u_, a.rulerRect_.h + r.h + 2 * a.u_}, kCream);
        shape::triangle(a.renderer_, {px - 6 * a.u_, a.rulerRect_.y}, {px + 6 * a.u_, a.rulerRect_.y}, {px, a.rulerRect_.y + 8 * a.u_},
                        shape::fcolor(kCream));
    }
}

void App::drawRoll(const SDL_FRect& r) {
    auto* notes = editNotes();
    const bool drums = tunePart_ == "drums";
    const float rowH = r.h / 5, x0 = r.x + kLabelW * u_;
    fill(r, kPanel);
    for (int lane = 0; lane < 5; ++lane) {
        const float y = r.y + float(lane) * rowH;
        if (lane % 2) fill({x0, y, r.w - kLabelW * u_, rowH}, {0x1b, 0x18, 0x1d, 255});
        const SDL_Color c = laneColor(lane, drums);
        shape::ellipse(renderer_, r.x + 14 * u_, y + rowH / 2, 6 * u_, 6 * u_, shape::fcolor(c));
        text(r.x + 26 * u_, y + rowH / 2 - 6 * u_, (drums ? kDrumNames : kFretNames)[lane], kSand, 1.25f * u_, kLabelW * u_ - 30 * u_);
    }
    fill({x0 - u_, r.y, u_, r.h}, kMuted);
    drawGrid(*this, r);
    if (!notes) {
        const std::string a = "This part has no " + std::vector<std::string>{"Easy", "Medium", "Hard", "Expert"}[size_t(tuneDiff_)] + " chart.";
        text(x0 + 20 * u_, r.y + r.h / 2, a, kSand, 1.5f * u_);
        return;
    }
    const float t0 = rollT(x0) - 8, t1 = rollT(r.x + r.w);
    const float rad = std::min(rowH * 0.34f, 13 * u_);
    auto first = std::lower_bound(notes->begin(), notes->end(), t0, [](const ChartNote& n, float t) { return n.t < t; });
    // Tails, then gems.
    for (auto it = first; it != notes->end() && it->t <= t1; ++it) {
        if (it->sustain <= 0) continue;
        const float y = r.y + (float(it->lane) + 0.5f) * rowH, xa = std::max(x0, rollX(it->t)), xb = std::min(r.x + r.w, rollX(it->t + it->sustain));
        if (xb <= xa) continue;
        const SDL_Color c = it->star ? kStar : laneColor(it->lane, drums);
        fill({xa, y - rowH * 0.1f, xb - xa, rowH * 0.2f}, c, 170);
        fill({xb - 3 * u_, y - rowH * 0.18f, 3 * u_, rowH * 0.36f}, c);  // the end handle
    }
    for (auto it = first; it != notes->end() && it->t <= t1; ++it) {
        const float x = rollX(it->t);
        if (x < x0 - rad || x > r.x + r.w + rad) continue;
        const float y = r.y + (float(it->lane) + 0.5f) * rowH;
        const SDL_Color c = laneColor(it->lane, drums);
        const bool sel = int(it - notes->begin()) == sel_;
        if (it->star) shape::ellipse(renderer_, x, y, rad + 4 * u_, rad + 4 * u_, shape::fcolor(kStar, 0.9f));
        if (sel) shape::ellipse(renderer_, x, y, rad + (it->star ? 7 : 4) * u_, rad + (it->star ? 7 : 4) * u_, shape::fcolor(kCream));
        shape::ellipse(renderer_, x, y, rad, rad, shape::fcolor(kInk));
        shape::ellipse(renderer_, x, y, rad - 1.5f * u_, rad - 1.5f * u_, shape::fcolor(c));
        if (it->cymbal) {  // a cymbal: a bright rim and a flat top
            shape::ring(renderer_, x, y, rad - 1.5f * u_, rad - 1.5f * u_, 2.5f * u_, shape::fcolor(kCream, 0.9f));
            fill({x - rad * 0.6f, y - u_, rad * 1.2f, 2 * u_}, kCream, 200);
        } else if (it->hopo) {
            shape::ellipse(renderer_, x, y, rad * 0.45f, rad * 0.45f, shape::fcolor(kCream));
        } else {
            shape::ellipse(renderer_, x, y, rad * 0.3f, rad * 0.3f, shape::fcolor(kInk, 0.55f));
        }
    }
    // Where a click would add a note.
    if (drag_ == Drag::None && inside(r, mouseX_, mouseY_) && mouseX_ > x0 && overlay_ == Overlay::None && noteAt(mouseX_, mouseY_) < 0) {
        const float t = chart_.snap(rollT(mouseX_), snap_);
        const int lane = laneAtY(mouseY_);
        shape::ellipse(renderer_, rollX(t), r.y + (float(lane) + 0.5f) * rowH, rad, rad, shape::fcolor(laneColor(lane, drums), 0.3f));
    }
}

void App::drawVocalRoll(const SDL_FRect& r) {
    // The pitch range: the song's notes, at least an octave and a half.
    int lo = 127, hi = 0;
    for (auto& v : chart_.vocals) {
        lo = std::min(lo, v.pitch);
        hi = std::max(hi, v.pitch);
    }
    if (lo > hi) {
        lo = 55;
        hi = 72;
    }
    lo = std::max(36, lo - 3);
    hi = std::min(84, hi + 3);
    while (hi - lo < 18) {
        if (lo > 36) --lo;
        if (hi - lo < 18 && hi < 84) ++hi;
        if (lo == 36 && hi == 84) break;
    }
    pitchLo_ = lo;
    pitchHi_ = hi;
    const float x0 = r.x + kLabelW * u_, rowH = r.h / float(hi - lo + 1);
    fill(r, kPanel);
    for (int p = lo; p <= hi; ++p) {
        const float y = pitchY(p) - rowH / 2;
        const bool black = std::string(kNoteNames[p % 12]).size() > 1;
        fill({x0, y, r.w - kLabelW * u_, rowH}, black ? SDL_Color{0x13, 0x11, 0x15, 255} : kPanel);
        fill({r.x + 30 * u_, y, kLabelW * u_ - 32 * u_, rowH - u_}, black ? kInk : kDim, black ? 255 : 90);
        if (p % 12 == 0 || rowH > 12 * u_)
            text(r.x + 4 * u_, y + rowH / 2 - 4 * u_, std::string(kNoteNames[p % 12]) + std::to_string(p / 12 - 1), p % 12 == 0 ? kSand : kMuted, u_);
        if (p % 12 == 0) fill({x0, y + rowH - u_, r.w - kLabelW * u_, u_}, kMuted, 120);
    }
    drawGrid(*this, r);
    for (auto& [a, b] : chart_.phrases) {  // phrase ends
        const float x = rollX(b);
        if (x > x0 && x < r.x + r.w) fill({x, r.y, 2 * u_, r.h}, kDim, 90);
    }
    const float h = std::max(rowH * 0.9f, 8 * u_);
    const SDL_Color c = stemColor("vocals");
    for (size_t i = 0; i < chart_.vocals.size(); ++i) {
        const VocalNote& v = chart_.vocals[i];
        const float xa = rollX(v.t);
        float xb = std::max(rollX(v.t + v.dur), xa + 6 * u_);
        if (xb < x0 || xa > r.x + r.w) continue;
        const float y = pitchY(v.pitch), cx0 = std::max(xa, x0);
        const bool clipped = xb > r.x + r.w;
        if (clipped) xb = r.x + r.w;
        const bool sel = int(i) == sel_;
        if (sel) fill({cx0 - 2 * u_, y - h / 2 - 2 * u_, xb - cx0 + 4 * u_, h + 4 * u_}, kCream);
        fill({cx0, y - h / 2, xb - cx0, h}, v.lyric.empty() || v.lyric == "+" ? kDim : c);
        if (!clipped) fill({xb - 3 * u_, y - h / 2, 3 * u_, h}, kInk, 120);  // the resize edge
        if (xa >= x0) text(xa + 2 * u_, y - h / 2 - 12 * u_, v.lyric.empty() ? "?" : v.lyric, sel ? kCream : kSand, 1.25f * u_,
                           std::max(30 * u_, xb - xa + 40 * u_));
    }
    if (chart_.vocals.empty()) text(x0 + 20 * u_, r.y + r.h / 2, "No vocals. Click to add a syllable.", kSand, 1.5f * u_);
}
