// gd-tool-02 Song home: the song, its music sheets (one box each for guitar, bass, drums
// and lyrics) and FORGE SONG. Sheets are copied to PROJECT/sheets/<box>/; which ones the
// forge uses is project.yaml sheets:<box>_use ("" all, "none", or "a.gp5|b.txt").
// python/sheets.py reads the same layout.
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>

#include "app.h"
#include "fields.h"
#include "text_util.h"

namespace {

std::string readHead(const fs::path& file, size_t max = 1 << 20) {
    std::ifstream in(file, std::ios::binary);
    std::string data(max, '\0');
    in.read(data.data(), std::streamsize(max));
    data.resize(size_t(in.gcount()));
    return data;
}

std::string readAll(const fs::path& file) { return readHead(file, 1 << 20); }

// "%20" -> " " in a file:// URL.
std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) && std::isxdigit((unsigned char)s[i + 2])) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// A file path from a pasted line: "/path/x.gp5", "file:///path/x.gp5" or "~/x.gp5".
fs::path pastedPath(std::string line) {
    line = trim(line);
    if (line.rfind("file://", 0) == 0) line = urlDecode(line.substr(7));
    if (!line.empty() && line[0] == '~')
        if (const char* home = SDL_getenv("HOME")) line = std::string(home) + line.substr(1);
    if (line.size() > 1 && line.front() == '"' && line.back() == '"') line = line.substr(1, line.size() - 2);
    return line;
}

}  // namespace

// Same rule as python/ascii_tab.py is_ascii_tab: at least 3 lines of a short label, a bar
// line (| or :), then a body of 4+ characters that is at least 40% dashes.
bool isAsciiTab(const std::string& text) {
    int count = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        size_t i = line.find_first_not_of(" \t");
        size_t j = i;
        while (j != std::string::npos && j < line.size() && (std::isalnum((unsigned char)line[j]) || line[j] == '#') && j - i < 6) ++j;
        if (i != std::string::npos && j > i && std::isalpha((unsigned char)line[i])) {
            while (j < line.size() && line[j] == ' ') ++j;
            if (j < line.size() && (line[j] == '|' || line[j] == ':')) {
                std::string body = trim(line.substr(j + 1));
                const size_t dashes = size_t(std::count(body.begin(), body.end(), '-'));
                if (body.size() >= 4 && float(dashes) >= float(body.size()) * 0.4f) ++count;
            }
        }
        start = end + 1;
    }
    return count >= 3;
}

namespace {

std::string joinNames(const std::vector<std::string>& names) {
    std::string out;
    for (auto& n : names) out += (out.empty() ? "" : "|") + n;
    return out;
}

}  // namespace

std::string App::sheetKind(const fs::path& file, const std::string& box) {
    const std::string ext = lower(file.extension().string());
    if (box == "lyrics") return ext == ".txt" || ext == ".lrc" ? "lyrics" : "";
    static const std::set<std::string> tabs = {".gp3", ".gp4", ".gp5", ".gpx", ".gp", ".gp6", ".gp7", ".gp8"};
    if (tabs.count(ext)) return "tab";
    if (ext == ".mid" || ext == ".midi" || ext == ".kar") {
        // A game chart has PART tracks (track names are plain text in the file).
        const std::string head = readHead(file);
        for (const char* t : {"PART GUITAR", "PART BASS", "PART DRUMS", "PART KEYS"})
            if (head.find(t) != std::string::npos) return "chart";
        return "score";
    }
    if (ext == ".chart") return "chart";
    if (ext == ".txt" || ext == ".tab") return isAsciiTab(readAll(file)) ? "texttab" : ext == ".txt" ? "chords" : "";
    return "";
}

std::string App::kindLabel(const std::string& kind) {
    if (kind == "tab") return "Guitar Pro tab";
    if (kind == "score") return "MIDI score";
    if (kind == "chart") return "chart, used as-is";
    if (kind == "chords") return "chord sheet";
    if (kind == "texttab") return "text tab";
    if (kind == "lyrics") return "lyrics";
    return "not usable";
}

std::vector<SheetFile> App::sheetFiles(const Project& p, const std::string& box) const {
    std::vector<SheetFile> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(p.dir / "sheets" / box, ec)) {
        const std::string name = e.path().filename().string();
        if (!e.is_regular_file() || name.empty() || name[0] == '.') continue;
        out.push_back({name, sheetKind(e.path(), box), true});
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    const std::string use = p.doc.get("sheets", box + "_use");
    if (box != "lyrics" && !use.empty()) {
        const std::string all = "|" + use + "|";
        for (auto& f : out) f.used = use != "none" && all.find("|" + f.name + "|") != std::string::npos;
    }
    for (auto& f : out)
        if (f.kind.empty()) f.used = false;
    return out;
}

void App::addSheet(Project& p, const std::string& box, const fs::path& file) {
    const std::string kind = sheetKind(file, box);
    if (kind.empty()) {
        status_ = file.filename().string() + " can't go in the " + box + " box (" +
                  (box == "lyrics" ? "use a .txt or .lrc" : "use Guitar Pro, MIDI, .chart or a chord sheet .txt") + ").";
        return;
    }
    const fs::path dir = p.dir / "sheets" / box;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path dest = dir / file.filename();
    if (!fs::equivalent(file, dest, ec)) fs::copy_file(file, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        status_ = "Could not copy " + file.filename().string() + ": " + ec.message();
        return;
    }
    const std::string name = file.filename().string();
    if (box == "lyrics") {
        setValue(p, "sheets", "lyrics_use", name);  // the newest file is the one used
    } else {
        std::string use = p.doc.get("sheets", box + "_use");
        if (use == "none") setValue(p, "sheets", box + "_use", name);
        else if (!use.empty() && ("|" + use + "|").find("|" + name + "|") == std::string::npos) setValue(p, "sheets", box + "_use", use + "|" + name);
    }
    status_ = name + " added to " + box + " (" + kindLabel(kind) + ").";
    logEvent("sheet " + box + ": " + name + " (" + kind + ")");
}

// Paste on a box: a copied file (Finder / file manager), a pasted path, or text: a text tab
// or a chord sheet, saved as sheets/<box>/pasted <tab|chords> N.txt.
void App::pasteSheet(Project& p, const std::string& box) {
    // A copied file: macOS gives public.file-url, Linux file managers text/uri-list.
    for (const char* mime : {"public.file-url", "text/uri-list"}) {
        if (!SDL_HasClipboardData(mime)) continue;
        size_t len = 0;
        void* data = SDL_GetClipboardData(mime, &len);
        std::string urls = data ? std::string(static_cast<const char*>(data), len) : "";
        SDL_free(data);
        std::istringstream lines(urls);
        std::string line;
        int added = 0;
        while (std::getline(lines, line)) {
            const fs::path file = pastedPath(line);
            if (!line.empty() && line[0] != '#' && fs::is_regular_file(file)) {
                addSheet(p, box, file);
                ++added;
            }
        }
        if (added) return;
    }
    // Text (or nothing): the Paste editor, with the clipboard already in it.
    openPasteEditor(p, box);
}

void App::openPasteEditor(Project& p, const std::string& box, const std::string& editFile) {
    std::string textIn;
    if (!editFile.empty()) {
        textIn = readAll(p.dir / "sheets" / box / editFile);
    } else if (char* clip = SDL_GetClipboardText()) {
        textIn = clip;
        SDL_free(clip);
        // A single line naming a file: add the file itself.
        if (trim(textIn).find('\n') == std::string::npos && fs::is_regular_file(pastedPath(textIn))) {
            addSheet(p, box, pastedPath(textIn));
            return;
        }
    }
    pasteEd_.set(textIn);
    pasteEd_.caret = pasteEd_.text.size();
    pasteEd_.dirty = !editFile.empty() ? false : !textIn.empty();
    pasteBox_ = box;
    pasteFile_ = editFile;
    overlay_ = Overlay::Paste;
    focusEditor(pasteEd_);
}

// The Paste editor's text becomes (or replaces) a sheet of the box.
void App::savePasted() {
    Project* p = currentProject();
    if (!p) return;
    const std::string& textIn = pasteEd_.text;
    if (trim(textIn).empty()) return;
    const bool tab = isAsciiTab(textIn);
    const fs::path dir = p->dir / "sheets" / pasteBox_;
    std::error_code ec;
    fs::create_directories(dir, ec);
    fs::path file = dir / pasteFile_;
    if (pasteFile_.empty()) {
        const std::string base = tab ? "pasted tab" : "pasted chords";
        for (int n = 1; fs::exists(file = dir / (base + " " + std::to_string(n) + ".txt")); ++n) {
        }
    }
    {
        std::ofstream out(file, std::ios::binary);
        out << textIn << (textIn.back() == '\n' ? "" : "\n");
    }
    const bool edited = !pasteFile_.empty();
    unfocus();
    overlay_ = Overlay::None;
    if (edited) status_ = file.filename().string() + " saved (" + kindLabel(sheetKind(file, pasteBox_)) + ").";
    else addSheet(*p, pasteBox_, file);
}

void App::removeSheet(Project& p, const std::string& box, const std::string& name) {
    std::error_code ec;
    fs::remove(p.dir / "sheets" / box / name, ec);
    if (box == "lyrics") {
        if (p.doc.get("sheets", "lyrics_use") == name) setValue(p, "sheets", "lyrics_use", "");
    } else {
        std::vector<std::string> keep;
        for (auto& f : sheetFiles(p, box))
            if (f.used) keep.push_back(f.name);
        if (!p.doc.get("sheets", box + "_use").empty()) setValue(p, "sheets", box + "_use", keep.empty() ? "none" : joinNames(keep));
    }
    status_ = name + " removed.";
}

void App::toggleSheet(Project& p, const std::string& box, const std::string& name) {
    auto files = sheetFiles(p, box);
    std::vector<std::string> used;
    size_t usable = 0;
    for (auto& f : files) {
        if (f.kind.empty()) continue;
        ++usable;
        const bool on = f.name == name ? !f.used : f.used;
        if (on) used.push_back(f.name);
    }
    setValue(p, "sheets", box + "_use", used.empty() ? "none" : used.size() == usable ? "" : joinNames(used));
}

bool App::hasTypedLyrics(const Project& p) const {
    if (p.id == lyricsFor_) return lyricsEd_.text.find_first_not_of(" \t\n") != std::string::npos;
    std::error_code ec;
    return fs::exists(p.dir / "lyrics.txt") && fs::file_size(p.dir / "lyrics.txt", ec) > 2;
}

// What the forge will sing: "typed", a file name, or "none" (python/forge.py: Plan._lyrics).
std::string App::lyricsChoice(const Project& p) const {
    const std::string use = p.doc.get("sheets", "lyrics_use");
    auto files = sheetFiles(p, "lyrics");
    if (use == "none") return "none";
    if (use == "typed") return hasTypedLyrics(p) ? "typed" : "none";
    if (!use.empty())
        for (auto& f : files)
            if (f.name == use) return use;
    if (hasTypedLyrics(p)) return "typed";
    for (auto& f : files)
        if (!f.kind.empty()) return f.name;
    return "none";
}

std::string App::boxSummary(const Project& p, const std::string& box) const {
    if (box == "lyrics") {
        const std::string c = lyricsChoice(p);
        if (c == "none") return "No vocals.";
        return "Vocals sing " + std::string(c == "typed" ? "the typed lyrics" : c) + ".";
    }
    if (partAbsent(p, box)) return "Not charted: this song has no " + box + ".";
    if (box != "drums" && !hasSheet(p, box)) return "Needs a sheet: a tab, MIDI, chart or chord sheet.";
    std::string chart, guide;
    bool chords = false;
    for (auto& f : sheetFiles(p, box)) {
        if (!f.used) continue;
        if (f.kind == "chart" && chart.empty()) chart = f.name;
        else if ((f.kind == "tab" || f.kind == "score" || f.kind == "texttab") && guide.empty()) guide = f.name;
        else if (f.kind == "chords") chords = true;
    }
    if (!chart.empty()) return "Forge uses the chart " + chart + " as-is.";
    if (box == "drums" && !guide.empty() && sheetKind(p.dir / "sheets" / box / guide, box) == "texttab")
        return "Drum text tabs alone can't be lined up yet.";
    std::string s = guide.empty() ? (chords ? "Charted from the stem, using only the chords" : "Forge charts the " + box + " stem")
                                  : "Notes from " + guide;
    if (chords && guide.empty()) return s + ".";
    if (chords) s += " + the chords";
    return s + ".";
}

bool App::partAbsent(const Project& p, const std::string& box) const { return p.doc.get("sheets", box + "_absent") == "yes"; }

// Guitar and bass need a sheet the forge can use (R20): a tab, MIDI, chart or chord sheet.
bool App::hasSheet(const Project& p, const std::string& box) const {
    for (auto& f : sheetFiles(p, box))
        if (f.used && !f.kind.empty()) return true;
    return false;
}

// Why FORGE can't run yet ("" when it can).
std::string App::forgeBlocker(const Project& p) const {
    std::vector<std::string> missing;
    for (const char* box : {"guitar", "bass"})
        if (!partAbsent(p, box) && !hasSheet(p, box)) missing.push_back(box);
    if (missing.empty()) return "";
    std::string parts = missing.size() == 2 ? "guitar and bass need" : missing[0] + " needs";
    parts[0] = char(std::toupper(parts[0]));
    return parts + " a sheet (or tick \"This song has no ...\").";
}

int App::boxAt(float x, float y) const {
    for (int i = 0; i < 4; ++i)
        if (boxRects_[size_t(i)].w > 0 && inside(boxRects_[size_t(i)], x, y)) return i;
    return -1;
}

void App::forgeSong() {
    Project* p = currentProject();
    if (!p) return;
    if (jobFor(*p, JobKind::Forge)) {
        setScreen(Screen::Forge);
        return;
    }
    if (const std::string why = forgeBlocker(*p); !why.empty()) {
        status_ = why;
        setScreen(Screen::Home);
        return;
    }
    saveLyrics();
    saveProject(*p);
    setScreen(Screen::Forge);
    startForge();
}

// ---- drawing ----------------------------------------------------------------------------------

void App::drawHome(Project& p, const SDL_FRect& r) {
    // The song: cover, what was read from the file, details.
    const float coverS = 116 * u_;
    SDL_FRect cover{r.x, r.y, coverS, coverS};
    if (coverTex_) SDL_RenderTexture(renderer_, coverTex_, nullptr, &cover);
    else {
        fill(cover, kPanel);
        drawDisc(cover.x + coverS / 2, cover.y + coverS / 2, coverS * 0.42f, false);
    }
    const float x = r.x + coverS + 24 * u_, w = r.w - coverS - 24 * u_;
    auto get = [&](const char* s, const char* k) { return p.doc.get(s, k); };
    text(x, r.y + 4 * u_, p.title(), kCream, 3 * u_, w * 0.62f);
    text(x, r.y + 40 * u_, p.artist().empty() ? "Unknown artist" : p.artist(), kSand, 2 * u_, w * 0.62f);
    std::string line;
    const double dur = std::atof(get("audio", "duration_s").c_str());
    for (const std::string& part : {get("song", "album"), get("song", "year"), get("song", "genre"),
                                   get("analysis", "bpm").empty() ? std::string() : get("analysis", "bpm") + " BPM",
                                   get("analysis", "key"), dur > 0 ? clockText(dur) : std::string()})
        if (!part.empty()) line += (line.empty() ? "" : "  -  ") + part;
    text(x, r.y + 72 * u_, line.empty() ? "No details yet" : line, kDim, 1.5f * u_, w * 0.62f);
    float bx = x;
    const float by = r.y + 92 * u_;
    bx += button(bx, by, "Edit details", [this] { overlay_ = Overlay::Details; }, true, false, 26 * u_) + 8 * u_;
    const fs::path dir = p.dir;
    button(bx, by, "Show folder", [this, dir] { reveal(dir); }, true, false, 26 * u_);
    // How to read the boxes, on the right.
    const float hx = r.x + r.w - std::min(w * 0.36f, 380 * u_), hw = r.x + r.w - hx;
    text(hx, r.y + 6 * u_, "MUSIC SHEETS", kSand, 1.5f * u_);
    float hy = r.y + 28 * u_;
    for (auto& l : wrap("Guitar and bass need a sheet: a tab, MIDI or chart gives the notes; a chord sheet "
                        "only limits the chords the forge hears. Drums come from the drums stem (a sheet is "
                        "optional). Use Paste for text tabs.",
                        size_t(hw / (8 * 1.25f * u_)))) {
        text(hx, hy, l, kDim, 1.25f * u_);
        hy += 16 * u_;
    }

    // The four boxes.
    const float top = r.y + coverS + 22 * u_, footH = 78 * u_, gap = 16 * u_;
    const float bw = (r.w - gap) / 2, bh = (r.y + r.h - footH - top - gap) / 2;
    for (int i = 0; i < 4; ++i) {
        SDL_FRect box{r.x + float(i % 2) * (bw + gap), top + float(i / 2) * (bh + gap), bw, bh};
        boxRects_[size_t(i)] = box;
        if (std::string(kBoxes[i].id) == "lyrics") drawLyricsBox(p, box);
        else drawBox(p, kBoxes[i], box);
    }

    // FORGE SONG.
    const float fy = r.y + r.h - footH + 16 * u_, fw = std::min(380 * u_, r.w * 0.4f), fh = 56 * u_;
    const bool forging = jobFor(p, JobKind::Forge);
    const bool tuned = p.doc.get("forge", "tuned") == "yes";
    const std::string label = forging ? "FORGING..." : p.forged() ? "FORGE AGAIN" : "FORGE SONG";
    const std::string blocker = forgeBlocker(p);
    bigButton({r.x + (r.w - fw) / 2, fy, fw, fh}, label, [this] { forgeSong(); }, (!busy(p) || forging) && (blocker.empty() || forging));
    // Keys (left) and what happens (right).
    SDL_FRect kb{r.x, fy + fh / 2 - 9 * u_, 18 * u_, 18 * u_};
    const bool keys = p.doc.get("forge", "keys") == "on";
    checkbox(kb, keys);
    text(kb.x + 28 * u_, kb.y + 3 * u_, "Chart keys too (Full band)", keys ? kCream : kDim, 1.5f * u_);
    hit({kb.x, kb.y - 6 * u_, 28 * u_ + textW("Chart keys too (Full band)", 1.5f * u_), 30 * u_},
        [this, keys](float, float) {
            if (Project* q = currentProject()) setValue(*q, "forge", "keys", keys ? "off" : "on");
        });
    std::string note = forging ? "Running: see Song forge." : !blocker.empty() ? blocker : !p.forged() ? "Stems, then charts: a few minutes."
                       : tuned ? "Replaces your fine tuning." : "Forged " + p.doc.get("forge", "forged_at").substr(0, 10) + ".";
    const float nx = r.x + (r.w + fw) / 2 + 20 * u_;
    const float nw = r.x + r.w - nx;
    float ny = fy + fh / 2 - 6 * u_;
    const auto lines = wrap(note, size_t(nw / (8 * 1.25f * u_)));
    if (lines.size() > 1) ny -= 8 * u_ * float(lines.size() - 1);
    for (auto& l : lines) {
        text(nx, ny, l, !blocker.empty() && !forging ? kOxblood : tuned && !forging ? kSand : kDim, 1.25f * u_);
        ny += 16 * u_;
    }
}

void App::drawBox(Project& p, const BoxDef& def, const SDL_FRect& r) {
    const std::string box = def.id;
    fill(r, kPanel);
    const SDL_Color c = stemColor(box);
    fill({r.x, r.y, r.w, 4 * u_}, c);
    text(r.x + 16 * u_, r.y + 18 * u_, def.label, kCream, 2.25f * u_);
    text(r.x + 16 * u_ + textW(def.label, 2.25f * u_) + 14 * u_, r.y + 23 * u_, def.hint, kMuted, 1.25f * u_,
         r.w - textW(def.label, 2.25f * u_) - buttonW("+ Add") - buttonW("Paste") - 60 * u_);
    const float addW = buttonW("+ Add"), pasteW = buttonW("Paste");
    button(r.x + r.w - addW - 12 * u_, r.y + 14 * u_, "+ Add", [this, box] { showDialog(DialogKind::Sheet, box); }, true, false, 28 * u_);
    button(r.x + r.w - addW - pasteW - 20 * u_, r.y + 14 * u_, "Paste", [this, box] {
        if (Project* q = currentProject()) pasteSheet(*q, box);
    }, true, false, 28 * u_);

    auto files = sheetFiles(p, box);
    // Guitar / bass have a two-line footer: the "no guitar" checkbox, then the summary.
    const float listTop = r.y + 54 * u_, rowH = 28 * u_, footY = r.y + r.h - (box == "drums" ? 28 : 50) * u_;
    if (files.empty()) {
        SDL_FRect zone{r.x + 16 * u_, listTop, r.w - 32 * u_, footY - listTop - 8 * u_};
        for (float dx = 0; dx < zone.w; dx += 12 * u_) {  // dashed outline
            fill({zone.x + dx, zone.y, 6 * u_, u_}, kMuted);
            fill({zone.x + dx, zone.y + zone.h - u_, 6 * u_, u_}, kMuted);
        }
        for (float dy = 0; dy < zone.h; dy += 12 * u_) {
            fill({zone.x, zone.y + dy, u_, 6 * u_}, kMuted);
            fill({zone.x + zone.w - u_, zone.y + dy, u_, 6 * u_}, kMuted);
        }
        const std::string a = "Drop a sheet here";
        text(zone.x + (zone.w - textW(a, 1.75f * u_)) / 2, zone.y + zone.h / 2 - 10 * u_, a, kDim, 1.75f * u_);
        const std::string b = box == "drums" ? "optional: the drums stem alone works well"
                                             : "needed: a tab, MIDI, chart or chord sheet";
        text(zone.x + (zone.w - std::min(textW(b, 1.25f * u_), zone.w)) / 2, zone.y + zone.h / 2 + 12 * u_, b, kMuted, 1.25f * u_, zone.w);
    }
    float y = listTop;
    for (auto& f : files) {
        if (y + rowH > footY - 4 * u_) {
            text(r.x + 16 * u_, y + 6 * u_, "...", kDim, 1.5f * u_);
            break;
        }
        SDL_FRect row{r.x + 8 * u_, y, r.w - 16 * u_, rowH - 2 * u_};
        if (hovered(row)) fill(row, kHover);
        SDL_FRect cb{row.x + 8 * u_, row.y + (row.h - 16 * u_) / 2, 16 * u_, 16 * u_};
        checkbox(cb, f.used);
        const std::string name = f.name;
        if (!f.kind.empty()) hit({row.x, row.y, row.w - 34 * u_, row.h}, [this, box, name](float, float) {
                if (Project* q = currentProject()) toggleSheet(*q, box, name);
            });
        const std::string kl = kindLabel(f.kind);
        const float kw = textW(kl, 1.25f * u_);
        text(cb.x + 26 * u_, row.y + (row.h - 12 * u_) / 2, f.name, f.used ? kCream : kMuted, 1.5f * u_, row.w - kw - 150 * u_);
        text(row.x + row.w - kw - 40 * u_, row.y + (row.h - 10 * u_) / 2, kl, f.kind.empty() ? kOxblood : f.kind == "chart" ? kSand : kDim, 1.25f * u_);
        SDL_FRect x{row.x + row.w - 26 * u_, row.y + (row.h - 20 * u_) / 2, 20 * u_, 20 * u_};
        if (hovered(x)) fill(x, kOxblood);
        text(x.x + 6 * u_, x.y + 6 * u_, "x", kCream, 1.25f * u_);
        hit(x, [this, box, name](float, float) {
            if (Project* q = currentProject()) removeSheet(*q, box, name);
        });
        if (lower(fs::path(name).extension().string()) == ".txt") {  // a text sheet: edit it
            const float ew = textW("edit", 1.25f * u_) + 12 * u_;
            SDL_FRect e{row.x + row.w - kw - 40 * u_ - ew - 10 * u_, row.y + (row.h - 20 * u_) / 2, ew, 20 * u_};
            fill(e, hovered(e) ? kHover : kBar);
            text(e.x + 6 * u_, e.y + 6 * u_, "edit", kSand, 1.25f * u_);
            hit(e, [this, box, name](float, float) {
                if (Project* q = currentProject()) openPasteEditor(*q, box, name);
            });
        }
        y += rowH;
    }
    fill({r.x + 16 * u_, footY - 4 * u_, r.w - 32 * u_, u_}, kBar);
    const bool absent = partAbsent(p, box), missing = box != "drums" && !absent && !hasSheet(p, box);
    const float sy = box == "drums" ? footY + 6 * u_ : footY + 28 * u_;
    text(r.x + 16 * u_, sy, boxSummary(p, box), missing ? kOxblood : absent ? kMuted : kDim, 1.25f * u_, r.w - 32 * u_);
    if (box != "drums") {  // R20: the user says the song has no guitar / bass
        const std::string l = "This song has no " + box;
        const float lw = textW(l, 1.25f * u_) + 28 * u_;
        SDL_FRect cb{r.x + 16 * u_, footY + 4 * u_, 16 * u_, 16 * u_};
        checkbox(cb, absent);
        text(cb.x + 24 * u_, footY + 7 * u_, l, absent ? kCream : kDim, 1.25f * u_);
        hit({cb.x - 4 * u_, cb.y - 4 * u_, lw + 8 * u_, 24 * u_}, [this, box, absent](float, float) {
            if (Project* q = currentProject()) setValue(*q, "sheets", box + "_absent", absent ? "" : "yes");
        });
    }
}

void App::drawLyricsBox(Project& p, const SDL_FRect& r) {
    if (lyricsFor_ != p.id) loadLyrics(p);
    fill(r, kPanel);
    fill({r.x, r.y, r.w, 4 * u_}, stemColor("vocals"));
    text(r.x + 16 * u_, r.y + 18 * u_, "Lyrics", kCream, 2.25f * u_);
    text(r.x + 16 * u_ + textW("Lyrics", 2.25f * u_) + 14 * u_, r.y + 23 * u_, kBoxes[3].hint, kMuted, 1.25f * u_,
         r.w - textW("Lyrics", 2.25f * u_) - 230 * u_);
    float bx = r.x + r.w - 12 * u_ - buttonW("+ Add");
    button(bx, r.y + 14 * u_, "+ Add", [this] { showDialog(DialogKind::Sheet, "lyrics"); }, true, false, 28 * u_);
    bx -= buttonW("Type / paste") + 8 * u_;
    button(bx, r.y + 14 * u_, "Type / paste", [this] {
        overlay_ = Overlay::Lyrics;
        focusEditor(lyricsEd_);
    }, true, false, 28 * u_);

    const std::string choice = lyricsChoice(p);
    const float rowH = 28 * u_, footY = r.y + r.h - 28 * u_;
    float y = r.y + 54 * u_;
    auto row = [&](const std::string& label, const std::string& note, const std::string& value, bool on, bool usable,
                   const std::string& removable) {
        if (y + rowH > footY - 4 * u_) return;
        SDL_FRect rr{r.x + 8 * u_, y, r.w - 16 * u_, rowH - 2 * u_};
        if (usable && hovered(rr)) fill(rr, kHover);
        checkbox({rr.x + 8 * u_, rr.y + (rr.h - 16 * u_) / 2, 16 * u_, 16 * u_}, on, true);
        const float nw = textW(note, 1.25f * u_);
        text(rr.x + 34 * u_, rr.y + (rr.h - 12 * u_) / 2, label, on ? kCream : usable ? kSand : kMuted, 1.5f * u_, rr.w - nw - 90 * u_);
        text(rr.x + rr.w - nw - 40 * u_, rr.y + (rr.h - 10 * u_) / 2, note, kDim, 1.25f * u_);
        if (usable) hit({rr.x, rr.y, rr.w - 34 * u_, rr.h}, [this, value](float, float) {
                if (Project* q = currentProject()) setValue(*q, "sheets", "lyrics_use", value);
            });
        if (!removable.empty()) {
            SDL_FRect x{rr.x + rr.w - 26 * u_, rr.y + (rr.h - 20 * u_) / 2, 20 * u_, 20 * u_};
            if (hovered(x)) fill(x, kOxblood);
            text(x.x + 6 * u_, x.y + 6 * u_, "x", kCream, 1.25f * u_);
            hit(x, [this, removable](float, float) {
                if (Project* q = currentProject()) removeSheet(*q, "lyrics", removable);
            });
        }
        y += rowH;
    };
    const int lines = lyricsLineCount();
    row("Typed lyrics", lines ? std::to_string(lines) + " lines" : "empty: click Type / paste", "typed", choice == "typed", lines > 0, "");
    for (auto& f : sheetFiles(p, "lyrics")) row(f.name, kindLabel(f.kind), f.name, choice == f.name, !f.kind.empty(), f.name);
    row("No vocals", "", "none", choice == "none", true, "");
    // A peek at what will be sung.
    if (choice == "typed" && y + 24 * u_ < footY) {
        std::istringstream ss(lyricsEd_.text);
        std::string l;
        y += 6 * u_;
        while (std::getline(ss, l) && y + 18 * u_ < footY - 6 * u_) {
            if (l.find_first_not_of(" \t") == std::string::npos) continue;
            text(r.x + 50 * u_, y, l, kMuted, 1.25f * u_, r.w - 70 * u_);
            y += 16 * u_;
        }
    }
    fill({r.x + 16 * u_, footY - 4 * u_, r.w - 32 * u_, u_}, kBar);
    text(r.x + 16 * u_, footY + 6 * u_, boxSummary(p, "lyrics"), kDim, 1.25f * u_, r.w - 32 * u_);
}

// ---- Details (project.yaml) ----------------------------------------------------------------

void App::drawField(Project& p, int index, float x, float y, float labelW, float valueW) {
    const FieldSpec& f = fieldAt(index);
    const float ts = 1.5f * u_, h = 26 * u_;
    text(x, y + (h - 8 * ts) / 2, f.label, kDim, ts, labelW - 10 * u_);
    std::string v = p.doc.get(f.section, f.key);
    SDL_FRect box{x + labelW, y, valueW, h};
    const bool locked = jobFor(p, JobKind::Metadata);
    if (!f.editable) {
        text(box.x + 8 * u_, y + (h - 8 * ts) / 2, v.empty() ? "-" : v, v.empty() ? kMuted : kSand, ts, valueW - 8 * u_);
        return;
    }
    const bool focused = focus_ == Focus::Field && field_ == index;
    fill(box, kInk);
    frame(box, focused ? kOxblood : hovered(box) && !locked ? kDim : kMuted);
    std::string shown = text::ascii(v);
    const size_t fit = size_t(std::max(1.f, (valueW - 20 * u_) / (8 * ts)));
    if (focused && shown.size() >= fit) shown = shown.substr(shown.size() - fit + 1);
    text(box.x + 8 * u_, y + (h - 8 * ts) / 2, shown, locked ? kMuted : kCream, ts, focused ? 0 : valueW - 12 * u_);
    if (focused && (SDL_GetTicks() / 500) % 2 == 0) fill({box.x + 8 * u_ + textW(shown, ts) + u_, y + 5 * u_, 2 * u_, h - 10 * u_}, kCream);
    if (!locked) hit(box, [this, index](float, float) { focusField(index); });
}

void App::drawDetails(float W, float H) {
    Project* p = currentProject();
    if (!p) {
        overlay_ = Overlay::None;
        return;
    }
    const float pw = std::min(W - 80 * u_, 1100 * u_), ph = std::min(H - 80 * u_, 560 * u_);
    SDL_FRect r = drawPanel(W, H, pw, ph, "Song details");
    auto close = [this] {
        if (Project* q = currentProject()) saveProject(*q);
        unfocus();
        overlay_ = Overlay::None;
    };
    const float cw = buttonW("Done");
    button(r.x + r.w - cw, r.y - 46 * u_, "Done", close, true, true);
    const float gap = 40 * u_, colW = (r.w - gap) / 2, labelW = 170 * u_, rowH = 32 * u_;
    text(r.x, r.y, "SONG", kSand, 1.5f * u_);
    text(r.x + colW + gap, r.y, "READ FROM THE FILE", kSand, 1.5f * u_);
    for (size_t i = 0; i < kLeftFields.size(); ++i) drawField(*p, int(i), r.x, r.y + 24 * u_ + float(i) * rowH, labelW, colW - labelW);
    for (size_t i = 0; i < kRightFields.size(); ++i)
        drawField(*p, int(kLeftFields.size() + i), r.x + colW + gap, r.y + 24 * u_ + float(i) * rowH, labelW, colW - labelW);
    const float by = r.y + r.h - 30 * u_;
    float bx = r.x;
    bx += button(bx, by, "Read the song again", [this] {
              if (Project* q = currentProject()) {
                  saveProject(*q);
                  unfocus();
                  jobs_.push_back({q->dir, JobKind::Metadata});
                  overlay_ = Overlay::None;
              }
          }, !busy(*p)) + 8 * u_;
    const fs::path yaml = p->dir / "project.yaml";
    button(bx, by, "Open project.yaml", [yaml] { SDL_OpenURL(("file://" + yaml.string()).c_str()); });
    text(r.x + r.w - textW("Tab / Enter: next field   Esc: close", 1.25f * u_), by + 10 * u_, "Tab / Enter: next field   Esc: close", kMuted, 1.25f * u_);
}

// ---- Lyrics editor (lyrics.txt) -------------------------------------------------------------

void App::drawLyricsEditor(float W, float H) {
    Project* p = currentProject();
    if (!p) {
        overlay_ = Overlay::None;
        return;
    }
    if (lyricsFor_ != p->id) loadLyrics(*p);
    const float pw = std::min(W - 80 * u_, 900 * u_), ph = std::min(H - 80 * u_, 680 * u_);
    SDL_FRect r = drawPanel(W, H, pw, ph, "Lyrics");
    const float cw = buttonW("Done");
    button(r.x + r.w - cw, r.y - 46 * u_, "Done", [this] {
        saveLyrics();
        unfocus();
        overlay_ = Overlay::None;
        if (Project* q = currentProject())
            if (hasTypedLyrics(*q)) setValue(*q, "sheets", "lyrics_use", "typed");  // just typed: sing these
    }, true, true);
    text(r.x, r.y, "One sung phrase per line. Paste with " + kCmd + "V.", kDim, 1.5f * u_);
    drawEditor(lyricsEd_, {r.x, r.y + 24 * u_, r.w, r.h - 24 * u_}, "Click here and type or paste the lyrics.");
}

// A text editor box (lyrics, Paste): click to place the caret, wheel to scroll.
void App::drawEditor(TextEdit& ed, const SDL_FRect& box, const std::string& placeholder) {
    lyricsRect_ = box;
    const bool focused = focus_ == Focus::Editor && editor_ == &ed;
    fill(box, kInk);
    frame(box, focused ? kOxblood : kMuted);
    const float ts = 1.5f * u_, lineH = 20 * u_, gutter = 52 * u_;
    hit(box, [this, &ed, box, ts, lineH, gutter](float mx, float my) {
        focusEditor(ed);
        const int line = ed.scroll + int((my - box.y - 10 * u_) / lineH);
        const int col = int(std::lround((mx - box.x - gutter) / (8 * ts)));
        ed.moveTo(line, col);
    });
    const int visible = std::max(1, int((box.h - 20 * u_) / lineH));
    auto [cl, cc] = ed.pos();
    if (focused) {
        if (cl < ed.scroll) ed.scroll = cl;
        if (cl >= ed.scroll + visible) ed.scroll = cl - visible + 1;
    }
    ed.scroll = std::clamp(ed.scroll, 0, std::max(0, ed.lineTotal() - visible));
    if (ed.text.empty() && !focused) {
        text(box.x + gutter, box.y + 10 * u_, placeholder, kMuted, ts);
        return;
    }
    int lineNo = 0;
    size_t start = 0;
    while (true) {
        size_t end = ed.text.find('\n', start);
        std::string l = ed.text.substr(start, (end == std::string::npos ? ed.text.size() : end) - start);
        const int row = lineNo - ed.scroll;
        if (row >= 0 && row < visible) {
            const float ly = box.y + 10 * u_ + float(row) * lineH;
            text(box.x + 8 * u_, ly, std::to_string(lineNo + 1), kMuted, 1.1f * u_);
            text(box.x + gutter, ly, l, kCream, ts, box.w - gutter - 10 * u_);
            if (focused && lineNo == cl && (SDL_GetTicks() / 500) % 2 == 0)
                fill({box.x + gutter + float(cc) * 8 * ts, ly - 3 * u_, 2 * u_, 8 * ts + 6 * u_}, kCream);
        }
        if (end == std::string::npos || row >= visible) break;
        start = end + 1;
        ++lineNo;
    }
    if (ed.lineTotal() > visible) {  // a scroll bar
        const float th = std::max(20 * u_, box.h * float(visible) / float(ed.lineTotal()));
        const float ty = box.y + (box.h - th) * float(ed.scroll) / float(std::max(1, ed.lineTotal() - visible));
        fill({box.x + box.w - 5 * u_, ty, 3 * u_, th}, kMuted);
    }
}

// Paste: a text editor for a tab or chord sheet, already holding the clipboard.
void App::drawPasteEditor(float W, float H) {
    Project* p = currentProject();
    if (!p) {
        overlay_ = Overlay::None;
        return;
    }
    std::string label = pasteBox_;
    if (!label.empty()) label[0] = char(std::toupper(label[0]));
    const float pw = std::min(W - 60 * u_, 1200 * u_), ph = std::min(H - 60 * u_, 820 * u_);
    SDL_FRect r = drawPanel(W, H, pw, ph, pasteFile_.empty() ? "Paste a " + pasteBox_ + " sheet" : "Edit " + pasteFile_);
    const bool empty = trim(pasteEd_.text).empty();
    const bool tab = !empty && isAsciiTab(pasteEd_.text);
    const bool ok = !empty && (tab || pasteBox_ != "drums");
    const std::string action = pasteFile_.empty() ? "Add to " + label : "Save";
    const float aw = buttonW(action), cw = buttonW("Cancel");
    button(r.x + r.w - aw, r.y - 46 * u_, action, [this] { savePasted(); }, ok, true);
    button(r.x + r.w - aw - cw - 8 * u_, r.y - 46 * u_, "Cancel", [this] {
        unfocus();
        overlay_ = Overlay::None;
    });
    text(r.x, r.y, "Paste with " + kCmd + "V, then edit. A text tab (lines like e|--0--3--| or HH|x-x-x-|) or a chord sheet.",
         kDim, 1.25f * u_, r.w);
    // What it looks like, live.
    std::string what;
    SDL_Color wc = kDim;
    if (empty) what = "Nothing yet.";
    else if (tab) {
        what = "Text tab: the " + pasteBox_ + " will follow its notes (lined up with the recording).";
        wc = kGood;
    } else if (pasteBox_ == "drums") {
        what = "Not a drum tab: drums need lines like HH|x-x-x-x-| (or add a Guitar Pro / MIDI / chart file).";
        wc = kOxblood;
    } else {
        what = "Chord sheet: the forge listens to the " + pasteBox_ + " and uses only these chords.";
        wc = kSand;
    }
    text(r.x, r.y + 20 * u_, what, wc, 1.25f * u_, r.w);
    drawEditor(pasteEd_, {r.x, r.y + 42 * u_, r.w, r.h - 42 * u_}, "Paste here (" + kCmd + "V)...");
}
