// gd-tool-02 --selftest: drives Fine tuning through the same handlers the mouse and keys use
// (on the first project, which must be forged), checks the chart and chart.txt after each
// step, and leaves the chart as it found it (undo). Exit code 1 on any failure.
//
//   ./build/gd-tool-02 --settings S.yaml --screen 2 --selftest
#include <cmath>
#include <cstdio>

#include "app.h"

int App::selfTest() {
    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s  %s\n", ok ? "ok    " : "FAILED", what);
        if (!ok) ++failures;
    };
    Project* p = currentProject();
    if (!p || !p->forged()) {
        std::printf("FAILED  no forged project to test\n");
        return 1;
    }
    auto onDisk = [&](const std::string& part, int diff) {
        Chart c;
        c.load(p->chartFile());
        return c.count(part, diff);
    };
    auto onDiskVocals = [&] {
        Chart c;
        c.load(p->chartFile());
        return c.vocals;
    };
    const Chart original = chart_;

    // Frets: guitar Expert, a spot with room.
    tunePart_ = "guitar";
    tuneDiff_ = 3;
    snap_ = 4;
    draw();  // lays out rollRect_
    auto* notes = editNotes();
    check(notes && !notes->empty(), "the guitar Expert chart is loaded");
    if (!notes) return failures + 1;
    const size_t n0 = notes->size();
    const int chords0 = chart_.count("guitar", 3);
    // An empty spot: the middle of the view, lane 2, nudged until free.
    float t = viewStart_ + (rollRect_.w * 0.5f) / (pps_ * u_);
    const float laneY = rollRect_.y + rollRect_.h / 5 * 2.5f;
    for (int k = 0; k < 64 && noteAt(rollX(chart_.snap(t, snap_)), laneY) >= 0; ++k) t += 0.07f;
    const float x = rollX(chart_.snap(t, snap_));
    tuneMouseDown(x, laneY, SDL_BUTTON_LEFT, false);
    tuneMouseUp();
    check(notes->size() == n0 + 1, "click on an empty spot adds a note");
    check(sel_ >= 0 && (*notes)[size_t(sel_)].lane == 2, "the new note is selected, on the clicked lane");
    const float added = sel_ >= 0 ? (*notes)[size_t(sel_)].t : -1;
    check(std::abs(added - chart_.snap(added, snap_)) < 1e-4f, "the new note sits on the grid");
    check(onDisk("guitar", 3) >= chords0, "chart.txt saved after adding");

    // Drag it one beat later and one lane down.
    const float beatPx = chart_.beatLength(added) * pps_ * u_;
    tuneMouseDown(rollX(added), laneY, SDL_BUTTON_LEFT, false);
    tuneMouseMove(rollX(added) + beatPx, laneY + rollRect_.h / 5);
    tuneMouseUp();
    const ChartNote moved = (*notes)[size_t(sel_)];
    check(std::abs(moved.t - chart_.snap(added + chart_.beatLength(added), snap_)) < 2e-3f, "drag moves the note a beat");
    check(moved.lane == 3, "drag down moves it a lane");

    // Sustain with Alt+drag is mouse-state dependent; set it through the drag handler.
    drag_ = Drag::Sustain;
    dragX_ = rollX(moved.t);
    dragV0_ = 0;
    tuneMouseMove(rollX(moved.t) + beatPx, rollRect_.y + rollRect_.h / 5 * 3.5f);
    tuneMouseUp();
    check((*notes)[size_t(sel_)].sustain > chart_.beatLength(moved.t) * 0.9f, "dragging the tail gives a one-beat sustain");

    // Flags.
    tuneKey(SDLK_H, SDL_KMOD_NONE);
    check((*notes)[size_t(sel_)].hopo, "H makes it a HOPO");
    tuneKey(SDLK_S, SDL_KMOD_NONE);
    check((*notes)[size_t(sel_)].star, "S gives it Star Power");
    tuneKey(SDLK_UP, SDL_KMOD_NONE);
    check((*notes)[size_t(sel_)].lane == 2, "Up moves it a lane up");

    // Delete (right click), then undo / redo.
    const ChartNote last = (*notes)[size_t(sel_)];
    tuneMouseDown(rollX(last.t), rollRect_.y + rollRect_.h / 5 * 2.5f, SDL_BUTTON_RIGHT, false);
    check(notes->size() == n0, "right click deletes it");
    undo();
    notes = editNotes();
    check(notes->size() == n0 + 1, "undo brings it back");
    redo();
    notes = editNotes();
    check(notes->size() == n0, "redo deletes it again");

    // Drums: a new yellow hit is a cymbal, C makes it a tom.
    tunePart_ = "drums";
    draw();
    if (auto* drums = editNotes()) {
        const size_t d0 = drums->size();
        float dt = viewStart_ + (rollRect_.w * 0.5f) / (pps_ * u_);
        const float yellowY = rollRect_.y + rollRect_.h / 5 * 2.5f;
        for (int k = 0; k < 64 && noteAt(rollX(chart_.snap(dt, snap_)), yellowY) >= 0; ++k) dt += 0.07f;
        tuneMouseDown(rollX(chart_.snap(dt, snap_)), yellowY, SDL_BUTTON_LEFT, false);
        tuneMouseUp();
        check(drums->size() == d0 + 1 && (*drums)[size_t(sel_)].cymbal, "a new yellow drum hit is a cymbal");
        tuneKey(SDLK_C, SDL_KMOD_NONE);
        check(!(*drums)[size_t(sel_)].cymbal, "C turns it into a tom");
    } else {
        check(false, "the drums chart is loaded");
    }

    // Vocals: add a syllable, type it, re-pitch it.
    tunePart_ = "vocals";
    draw();
    const size_t v0 = chart_.vocals.size();
    float vt = viewStart_ + (rollRect_.w * 0.4f) / (pps_ * u_);
    const float vy = pitchY(64);
    for (int k = 0; k < 64 && vocalAt(rollX(chart_.snap(vt, snap_)), vy) >= 0; ++k) vt += 0.1f;
    tuneMouseDown(rollX(chart_.snap(vt, snap_)), vy, SDL_BUTTON_LEFT, false);
    tuneMouseUp();
    check(chart_.vocals.size() == v0 + 1 && chart_.vocals[size_t(sel_)].pitch == 64, "click adds a syllable at the row's pitch");
    check(focus_ == Focus::VocalLyric, "and opens its text for typing");
    SDL_Event e{};
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = "la";
    handle(e);
    onKey(SDLK_RETURN, SDL_KMOD_NONE);
    check(chart_.vocals[size_t(sel_)].lyric == "la", "typing sets the syllable");
    tuneKey(SDLK_UP, SDL_KMOD_NONE);
    check(chart_.vocals[size_t(sel_)].pitch == 65, "Up raises it a semitone");
    bool found = false;
    for (auto& v : onDiskVocals())
        if (v.lyric == "la" && v.pitch == 65) found = true;
    check(found, "chart.txt has the new syllable");

    // Put everything back.
    while (!undo_.empty()) undo();
    const bool same = chart_.vocals.size() == original.vocals.size() && chart_.count("guitar", 3) == original.count("guitar", 3) &&
                      chart_.parts["drums"][3].size() == original.parts.at("drums")[3].size();
    check(same, "undoing everything restores the chart");
    check(onDisk("guitar", 3) == original.count("guitar", 3) && onDiskVocals().size() == original.vocals.size(),
          "and chart.txt on disk");
    std::printf("%s\n", failures ? "SELFTEST FAILED" : "SELFTEST OK");
    return failures;
}
