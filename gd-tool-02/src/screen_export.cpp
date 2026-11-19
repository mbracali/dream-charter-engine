// gd-tool-02 Export song: python/export_song.py writes a Clone Hero / YARG song folder per
// target (GH1, GH2/GH3/Aerosmith, Full band; docs/product_spec.md) into the songs folder
// chosen in Settings (default: PROJECT/export/<format>). The Additive menu (gd-tool-01)
// is shown, disabled, until that tool defines its format.
#include <algorithm>

#include "app.h"

namespace {

struct TargetDef {
    const char* id;
    const char* title;
    const char* setlist;
    std::vector<std::string> parts;
};
const TargetDef kTargets[] = {
    {"gh1", "Guitar Hero 1", "Guitar only (GH1 setlist)", {"guitar"}},
    {"gh2", "GH2 / GH3 / Aerosmith", "Guitar + bass (co-op)", {"guitar", "bass"}},
    {"full", "Full band", "Every part (GH4+, RB, YARG)", {"guitar", "bass", "drums", "vocals", "keys"}},
};

}  // namespace

bool App::targetOn(const Project& p, const std::string& target) const {
    std::string t = p.doc.get("export", "targets");
    if (t.empty()) t = "full";
    return ("," + t + ",").find("," + target + ",") != std::string::npos;
}

void App::startExport(const std::string& format) {
    Project* p = currentProject();
    if (!p || busy(*p)) return;
    saveChart();
    std::string targets;
    for (auto& t : kTargets)
        if (targetOn(*p, t.id)) targets += (targets.empty() ? "" : ",") + std::string(t.id);
    if (targets.empty()) {
        status_ = "Choose at least one target.";
        return;
    }
    const fs::path dest = exportDir(format);
    std::error_code ec;
    fs::create_directories(dest, ec);
    jobs_.push_back({p->dir, JobKind::Export, {"--dest", dest.string(), "--targets", targets, "--format", format}, format});
    exported_.clear();
    status_ = p->title() + ": exporting to " + dest.string();
}

void App::drawExport(Project& p, const SDL_FRect& r) {
    if (chartFor_ != p.id) loadChart(p);
    float y = r.y;
    text(r.x, y, "Choose the games, then where to send the song.", kDim, 1.75f * u_);
    y += 36 * u_;

    // The targets.
    const float gap = 16 * u_, cw = (r.w - 2 * gap) / 3, ch = 210 * u_;
    for (int i = 0; i < 3; ++i) {
        const TargetDef& t = kTargets[i];
        const std::string id = t.id;
        SDL_FRect card{r.x + float(i) * (cw + gap), y, cw, ch};
        const bool on = targetOn(p, id);
        fill(card, on ? kHover : kPanel);
        if (on) frame(card, kOxblood);
        else if (hovered(card)) frame(card, kMuted);
        checkbox({card.x + 16 * u_, card.y + 18 * u_, 18 * u_, 18 * u_}, on);
        text(card.x + 46 * u_, card.y + 19 * u_, t.title, on ? kCream : kSand, 1.5f * u_, card.w - 60 * u_);
        text(card.x + 46 * u_, card.y + 42 * u_, t.setlist, kDim, 1.25f * u_, card.w - 60 * u_);
        float ly = card.y + 72 * u_;
        std::vector<std::string> stems;
        for (auto& part : t.parts) {
            const bool has = part == "vocals" ? !chart_.vocals.empty() : chart_.hasPart(part);
            if (part == "keys" && !has) continue;  // keys only when charted
            const SDL_Color c = stemColor(part);
            shape::ellipse(renderer_, card.x + 52 * u_, ly + 6 * u_, 5 * u_, 5 * u_, shape::fcolor(c, has ? 1 : 0.3f));
            std::string label = part;
            label[0] = char(std::toupper(label[0]));
            text(card.x + 66 * u_, ly, label, has ? kCream : kMuted, 1.5f * u_);
            const std::string note = has ? std::to_string(chart_.count(part, 3)) + (part == "vocals" ? " syllables" : " notes") : "not charted";
            text(card.x + card.w - 16 * u_ - textW(note, 1.25f * u_), ly + 2 * u_, note, has ? kDim : kOxblood, 1.25f * u_);
            if (has) stems.push_back(part + ".ogg");
            ly += 22 * u_;
        }
        stems.push_back("song.ogg");
        std::string files;
        for (auto& s : stems) files += (files.empty() ? "" : " + ") + s;
        float fy = card.y + ch - 40 * u_;
        for (auto& l : wrap(files, size_t((card.w - 32 * u_) / (8 * 1.1f * u_)))) {
            text(card.x + 16 * u_, fy, l, kMuted, 1.1f * u_);
            fy += 14 * u_;
        }
        hit(card, [this, id, on](float, float) {
            Project* q = currentProject();
            if (!q) return;
            std::string list;
            for (auto& tt : kTargets) {
                const bool v = tt.id == id ? !on : targetOn(*q, tt.id);
                if (v) list += (list.empty() ? "" : ",") + std::string(tt.id);
            }
            setValue(*q, "export", "targets", list.empty() ? "none" : list);
        });
    }
    y += ch + 26 * u_;

    // Where: the songs folders (Settings), and the buttons.
    const bool exporting = jobFor(p, JobKind::Export);
    struct Dest {
        const char* format;
        const char* name;
        DialogKind dialog;
    };
    const Dest dests[] = {{"clonehero", "Clone Hero", DialogKind::CloneHeroDir}, {"yarg", "YARG", DialogKind::YargDir}};
    for (auto& d : dests) {
        SDL_FRect row{r.x, y, r.w, 56 * u_};
        fill(row, kPanel);
        const std::string format = d.format;
        const float bw = std::max(buttonW("Export to Clone Hero"), buttonW("Export to YARG"));
        button(row.x + 14 * u_, row.y + 12 * u_, std::string("Export to ") + d.name, [this, format] { startExport(format); },
               !busy(p), true, 32 * u_);
        const fs::path dir = exportDir(format);
        const bool custom = format == "yarg" ? !yargDir_.empty() : !cloneHeroDir_.empty();
        const float tx = row.x + bw + 34 * u_;
        text(tx, row.y + 12 * u_, custom ? std::string(d.name) + " songs folder" : "In the project. Change... sends it to " + std::string(d.name),
             kDim, 1.25f * u_, row.w - bw - 200 * u_);
        text(tx, row.y + 30 * u_, dir.string(), kCream, 1.5f * u_, row.w - bw - 200 * u_);
        const DialogKind dk = d.dialog;
        const float chW = buttonW("Change...");
        button(row.x + row.w - chW - 14 * u_, row.y + 12 * u_, "Change...", [this, dk] { showDialog(dk); }, true, false, 32 * u_);
        y += 64 * u_;
    }
    {
        SDL_FRect row{r.x, y, r.w, 48 * u_};
        fill(row, kPanel);
        button(row.x + 14 * u_, row.y + 9 * u_, "Send to the Additive menu", nullptr, false, false, 30 * u_);
        const float ax = row.x + buttonW("Send to the Additive menu") + 34 * u_;
        text(ax, row.y + 19 * u_, "Coming with gd-tool-01 (Additive music menu).", kMuted, 1.25f * u_, row.x + row.w - ax - 10 * u_);
        y += 64 * u_;
    }

    // Progress, then the result.
    if (exporting) {
        const bool running = activeFor(p, JobKind::Export);
        drawDisc(r.x + 34 * u_, y + 34 * u_, 30 * u_, true);
        progressBar({r.x + 84 * u_, y + 16 * u_, std::min(560 * u_, r.w - 100 * u_), 14 * u_}, running ? jobProgress_ : -1, !running);
        text(r.x + 84 * u_, y + 42 * u_, running ? jobStatus_ : "Waiting...", kDim, 1.5f * u_, r.w - 100 * u_);
        return;
    }
    std::vector<std::string> folders = exported_;
    if (folders.empty() && !p.doc.get("export", "last_folder").empty()) folders.push_back(p.doc.get("export", "last_folder"));
    if (!folders.empty()) {
        text(r.x, y, exported_.empty() ? "LAST EXPORT  (" + p.doc.get("export", "exported_at").substr(0, 16) + ")" : "EXPORTED", kGood, 1.5f * u_);
        y += 24 * u_;
        for (auto& f : folders) {
            if (y + 30 * u_ > r.y + r.h) break;
            const fs::path folder = f;
            const float bw = buttonW("Show");
            button(r.x, y, "Show", [this, folder] { reveal(folder); }, fs::exists(folder), false, 26 * u_);
            text(r.x + bw + 12 * u_, y + 8 * u_, f, kCream, 1.25f * u_, r.w - bw - 12 * u_);
            y += 32 * u_;
        }
    }
}
