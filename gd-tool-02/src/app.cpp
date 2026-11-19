// gd-tool-02: the window. Each song is a project: a folder in the user's
// projects folder (chosen on the first start, changed in Settings) holding
// project.yaml (the song's metadata), the song, cover.png, lyrics.txt and
// stems/. A new song opens on its metadata (read from the file by
// python/extract_metadata.py) while python/separate_stems.py splits it.
//
// Layout: a Windows-style menu bar, the projects sidebar (collapsible), the
// project (Info / Metadata / Stems / Lyrics / Export) and a status bar.
// C++17 + SDL3 like gd-engine-launcher, drawn with SDL_Renderer and SDL's
// debug font (ASCII; see text_util.h).
#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "forge.h"
#include "stems.h"
#include "text_util.h"
#include "worker.h"
#include "yaml_doc.h"

namespace fs = std::filesystem;

#ifndef GDT02_ROOT
#define GDT02_ROOT "."
#endif

namespace {

// The launcher's palette (gd-engine-launcher/src/palette.h) plus panel shades.
constexpr SDL_Color kInk{0x0b, 0x0a, 0x0d, 255};
constexpr SDL_Color kPanel{0x17, 0x15, 0x19, 255};
constexpr SDL_Color kBar{0x1f, 0x1c, 0x21, 255};
constexpr SDL_Color kHover{0x2c, 0x28, 0x2e, 255};
constexpr SDL_Color kCream{0xf1, 0xe7, 0xd3, 255};
constexpr SDL_Color kSand{0xc9, 0xb8, 0x92, 255};
constexpr SDL_Color kDim{0x91, 0x87, 0x7a, 255};
constexpr SDL_Color kMuted{0x5f, 0x58, 0x4f, 255};
constexpr SDL_Color kOxblood{0xa3, 0x24, 0x2b, 255};

#ifdef __APPLE__
const std::string kCmd = "Cmd+";
#else
const std::string kCmd = "Ctrl+";
#endif

bool inside(const SDL_FRect& r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

std::string nowIso() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
    return buf;
}

std::string clockText(double s) {
    char buf[32];
    SDL_snprintf(buf, sizeof buf, "%d:%02d", int(s) / 60, int(s) % 60);
    return buf;
}

std::string clockTenths(double s) {
    char buf[32];
    SDL_snprintf(buf, sizeof buf, "%d:%02d.%d", int(s) / 60, int(s) % 60, int(s * 10) % 10);
    return buf;
}

// A folder name from a song name: no path separators or characters Windows refuses.
std::string folderName(std::string s) {
    for (char& c : s)
        if (std::string("/\\:*?\"<>|").find(c) != std::string::npos || static_cast<unsigned char>(c) < 0x20) c = '-';
    while (!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
    return s.empty() ? "Untitled" : s;
}

std::vector<std::string> wrap(const std::string& s, size_t width) {
    std::vector<std::string> lines;
    std::string a = text::ascii(s);
    while (a.size() > width) {
        size_t cut = a.rfind(' ', width);
        if (cut == std::string::npos || cut == 0) cut = width;
        lines.push_back(a.substr(0, cut));
        a = a.substr(cut == width ? cut : cut + 1);
    }
    lines.push_back(a);
    return lines;
}

// ---- Projects -----------------------------------------------------------------

struct Project {
    std::string id;  // folder name
    fs::path dir;
    YamlDoc doc;
    bool dirty = false;  // unsaved metadata edits
    int stemCount = 0;

    std::string title() const {
        std::string t = doc.get("song", "title");
        if (t.empty()) t = doc.get("project", "name");
        return t.empty() ? id : t;
    }
    std::string artist() const { return doc.get("song", "artist"); }
    fs::path stemsDir() const { return fs::exists(dir / "stems") ? dir / "stems" : dir; }  // old projects: no stems/
    fs::path audioFile() const {
        std::string a = doc.get("project", "audio_file");
        return a.empty() ? fs::path() : dir / a;
    }
};

int countStems(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".wav" && e.path().stem() != "mix") ++n;
    return n;
}

// The layout of a new project.yaml (python/extract_metadata.py has the same SCHEMA).
const std::vector<std::pair<std::string, std::vector<std::string>>> kSchema = {
    {"project", {"name", "created", "source_file", "audio_file", "charter", "notes"}},
    {"song", {"title", "artist", "album", "album_artist", "year", "genre", "track", "disc", "composer", "language",
              "comment"}},
    {"analysis", {"bpm", "key", "key_confidence", "start_silence_s", "peak_dbfs", "rms_dbfs"}},
    {"audio", {"format", "codec", "duration_s", "sample_rate", "channels", "bitrate_kbps", "file_size_mb", "sha256"}},
    {"stems", {"mode", "model", "status", "error", "separated_at"}},
};

struct FieldSpec {
    const char* section;
    const char* key;
    const char* label;
    bool editable;
};

// Metadata tab: the song (left column) and what was measured (right column).
const std::vector<FieldSpec> kLeftFields = {
    {"project", "name", "Project name", true}, {"song", "title", "Song title", true},
    {"song", "artist", "Artist / band", true},  {"song", "album", "Album", true},
    {"song", "album_artist", "Album artist", true}, {"song", "year", "Year", true},
    {"song", "genre", "Genre", true},            {"song", "track", "Track", true},
    {"song", "disc", "Disc", true},              {"song", "composer", "Composer", true},
    {"song", "language", "Language", true},      {"song", "comment", "Comment", true},
    {"project", "charter", "Charter", true},     {"project", "notes", "Notes", true},
};
const std::vector<FieldSpec> kRightFields = {
    {"analysis", "bpm", "BPM", true},
    {"analysis", "key", "Key", true},
    {"analysis", "key_confidence", "Key confidence", false},
    {"analysis", "start_silence_s", "Start silence (s)", false},
    {"analysis", "peak_dbfs", "Peak (dBFS)", false},
    {"analysis", "rms_dbfs", "Loudness (dBFS)", false},
    {"audio", "format", "Format", false},
    {"audio", "codec", "Codec", false},
    {"audio", "duration_s", "Duration (s)", false},
    {"audio", "sample_rate", "Sample rate (Hz)", false},
    {"audio", "channels", "Channels", false},
    {"audio", "bitrate_kbps", "Bitrate (kbps)", false},
    {"audio", "file_size_mb", "File size (MB)", false},
    {"project", "source_file", "Imported from", false},
    {"project", "created", "Created", false},
    {"stems", "status", "Stems", false},
};
const FieldSpec& fieldAt(int i) {
    return i < int(kLeftFields.size()) ? kLeftFields[size_t(i)] : kRightFields[size_t(i) - kLeftFields.size()];
}
int fieldCount() { return int(kLeftFields.size() + kRightFields.size()); }

// ---- Menus -----------------------------------------------------------------------

struct MenuItem {
    std::string label, shortcut;
    std::function<void()> action;
    bool enabled = true, checked = false, separator = false;
};
struct Menu {
    std::string title;
    std::vector<MenuItem> items;
    SDL_FRect titleRect{};
};
MenuItem item(std::string label, std::string shortcut, std::function<void()> action, bool enabled = true,
              bool checked = false) {
    MenuItem m;
    m.label = std::move(label);
    m.shortcut = std::move(shortcut);
    m.action = std::move(action);
    m.enabled = enabled;
    m.checked = checked;
    return m;
}
MenuItem separator() {
    MenuItem m;
    m.separator = true;
    return m;
}

enum class Tab { Info, Metadata, Stems, Lyrics, Forge };
enum class JobKind { Metadata, Separate, Forge };
enum class DialogKind { Song, StemsFolder, ProjectsDir };

struct Job {
    fs::path dir;
    JobKind kind;
    std::string mode;
    std::vector<std::string> args = {};  // Forge: extra arguments for python/forge.py
    std::string signature = {};          // Forge: the settings it was started with
};

// Forge: the parts a chart can have, and the stems a part can be made from
// (dream-charter-engine: only guitar, bass and keys can take another stem).
struct PartDef {
    const char* id;
    const char* label;
    bool swappable;
};
const PartDef kParts[] = {{"guitar", "Guitar", true}, {"bass", "Bass", true}, {"drums", "Drums", false},
                          {"keys", "Keys", true},     {"vocals", "Vocals", false}};
const char* const kSources[] = {"guitar", "bass", "keys", "vocals", "other"};

struct Hit {  // something clickable drawn this frame
    SDL_FRect rect;
    std::function<void(float, float)> fn;
};

// ---- The app ------------------------------------------------------------------------

class App {
public:
    bool init(int argc, char** argv) {
        std::vector<std::string> paths;
        std::string settingsOverride;
        for (int i = 1; i < argc; ++i) {  // options first, then songs/folders
            std::string a = argv[i];
            if (a == "--simple") modeOverride_ = "simple";
            else if (a == "--screenshot" && i + 1 < argc) shotPath_ = argv[++i];   // debug: save a frame, quit
            else if (a == "--menu" && i + 1 < argc) openMenu_ = std::atoi(argv[++i]);  // debug: a menu open
            else if (a == "--tab" && i + 1 < argc) startTab_ = std::atoi(argv[++i]);    // debug: start on a tab
            else if (a == "--hover" && i + 2 < argc) {                                  // debug: mouse position
                mouseX_ = float(std::atof(argv[++i]));
                mouseY_ = float(std::atof(argv[++i]));
                hoverPinned_ = true;
            } else if (a == "--settings" && i + 1 < argc) settingsOverride = argv[++i];  // debug: other settings
            else if (a == "--wait-jobs") waitJobs_ = true;  // debug: screenshot once the jobs are done
            else if (a == "--seek" && i + 1 < argc) seekTo_ = std::atof(argv[++i]);  // debug: start there
            else if (a == "--part" && i + 1 < argc) forgePart_ = argv[++i];        // debug: Forge part
            else if (a == "--forge") forgeAtStart_ = true;                          // debug: forge at start
            else paths.push_back(a);
        }
        if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) return fail("SDL_Init");
        if (!SDL_CreateWindowAndRenderer("gd-tool-02", 1440, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                         &window_, &renderer_))
            return fail("SDL_CreateWindowAndRenderer");
        SDL_SetRenderVSync(renderer_, 1);
        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        player_.open();

        loadSettings(settingsOverride);
        refreshProjects();
        status_ = projectsDir_.empty() ? "Welcome. Choose where your projects are saved." : "Ready.";
        for (auto& p : paths) openPath(p);
        if (current_.empty() && !projects_.empty() && !shotPath_.empty()) selectProject(projects_[0].id);
        if (startTab_ >= 0) setTab(Tab(startTab_));
        if (seekTo_ > 0) player_.seek(seekTo_);
        if (forgeAtStart_) startForge();
        return true;
    }

    void run() {
        while (!quit_) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) handle(e);
            takeDialogResult();
            pumpWorker();
            startNextJob();
            player_.update();
            draw();
        }
        saveAll();
    }

    void shutdown() {
        worker_.stop();
        player_.close();
        if (coverTex_) SDL_DestroyTexture(coverTex_);
        if (renderer_) SDL_DestroyRenderer(renderer_);
        if (window_) SDL_DestroyWindow(window_);
        SDL_Quit();
    }

private:
    // gd-tool-02.log next to settings.yaml: what the app did and when (jobs, stems, tabs).
    void logEvent(const std::string& msg) {
        static std::ofstream out;
        if (!out.is_open()) {
            fs::path dir = settingsPath_.empty() ? fs::path(".") : settingsPath_.parent_path();
            out.open(dir / "gd-tool-02.log", std::ios::app);
        }
        out << nowIso() << "  " << msg << std::endl;
    }

    bool fail(const char* what) {
        SDL_Log("%s failed: %s", what, SDL_GetError());
        return false;
    }

    static fs::path toolRoot() { return fs::path(GDT02_ROOT); }

    // ---- Settings (settings.yaml in the user's app-data folder) -------------------
    void loadSettings(const std::string& overridePath) {
        if (!overridePath.empty()) {
            settingsPath_ = overridePath;
        } else {
            char* pref = SDL_GetPrefPath("mbracali", "gd-tool-02");
            settingsPath_ = fs::path(pref ? pref : ".") / "settings.yaml";
            SDL_free(pref);
        }
        settings_.load(settingsPath_);
        if (settings_.header.empty()) settings_.header = {"# gd-tool-02 settings (Settings menu)."};
        std::string dir = settings_.get("settings", "projects_dir");
        projectsDir_ = dir.empty() ? fs::path() : fs::path(dir);
        if (!projectsDir_.empty() && !fs::is_directory(projectsDir_)) projectsDir_.clear();  // gone: ask again
        mode_ = settings_.get("settings", "separation_mode") == "simple" ? "simple" : "demucs";
        if (!modeOverride_.empty()) mode_ = modeOverride_;
        sidebarCollapsed_ = settings_.get("settings", "sidebar_collapsed") == "true";
    }

    void saveSettings() {
        settings_.set("settings", "projects_dir", projectsDir_.string());
        settings_.set("settings", "separation_mode", mode_);
        settings_.set("settings", "sidebar_collapsed", sidebarCollapsed_ ? "true" : "false");
        if (!settings_.save(settingsPath_)) status_ = "Could not save the settings to " + settingsPath_.string();
    }

    void setProjectsDir(const fs::path& dir) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        if (!fs::is_directory(dir)) {
            status_ = "Cannot use " + dir.string() + ": " + ec.message();
            return;
        }
        saveAll();
        closeProject();
        projectsDir_ = dir;
        saveSettings();
        refreshProjects();
        status_ = "Projects folder: " + dir.string();
    }

    fs::path defaultProjectsDir() const {
        const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
        return fs::path(docs ? docs : ".") / "GD Tool 02 Projects";
    }

    void setMode(const std::string& m) {
        mode_ = m;
        saveSettings();
    }

    void toggleSidebar() {
        sidebarCollapsed_ = !sidebarCollapsed_;
        saveSettings();
    }

    // ---- Projects --------------------------------------------------------------------
    Project* project(const std::string& id) {
        for (auto& p : projects_)
            if (p.id == id) return &p;
        return nullptr;
    }
    Project* currentProject() { return project(current_); }

    Project* projectAt(const fs::path& dir) {
        for (auto& p : projects_)
            if (fs::equivalent(p.dir, dir, ec_)) return &p;
        return nullptr;
    }

    void refreshProjects() {
        saveAll();
        projects_.clear();
        if (projectsDir_.empty()) return;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(projectsDir_, ec)) {
            if (!e.is_directory()) continue;
            Project p;
            p.id = e.path().filename().string();
            p.dir = e.path();
            const bool hasYaml = p.doc.load(p.dir / "project.yaml");
            p.stemCount = countStems(p.stemsDir());
            if (!hasYaml && p.stemCount == 0 && !fs::exists(p.dir / "mix.wav")) continue;  // not a project
            projects_.push_back(std::move(p));
        }
        std::sort(projects_.begin(), projects_.end(), [](const Project& a, const Project& b) {
            return a.doc.get("project", "created") > b.doc.get("project", "created");
        });
        if (!current_.empty() && !project(current_)) closeProject();
    }

    void saveProject(Project& p) {
        if (!p.dirty) return;
        if (p.doc.save(p.dir / "project.yaml")) {
            p.dirty = false;
            status_ = p.title() + ": metadata saved.";
        } else {
            status_ = "Could not save " + (p.dir / "project.yaml").string();
        }
    }

    void saveAll() {
        for (auto& p : projects_) saveProject(p);
        saveLyrics();
    }

    bool jobFor(const Project& p, JobKind kind) const {
        if (active_ && active_->kind == kind && fs::equivalent(active_->dir, p.dir, ec_)) return true;
        return std::any_of(jobs_.begin(), jobs_.end(),
                           [&](const Job& j) { return j.kind == kind && fs::equivalent(j.dir, p.dir, ec_); });
    }
    bool readingMetadata(const Project& p) const { return jobFor(p, JobKind::Metadata); }
    bool separating(const Project& p) const { return jobFor(p, JobKind::Separate); }

    std::string projectState(const Project& p) const {
        if (active_ && fs::equivalent(active_->dir, p.dir, ec_))
            return std::string(active_->kind == JobKind::Metadata ? "reading the file " : active_->kind == JobKind::Forge ? "forging " : "separating ") +
                   std::to_string((SDL_GetTicks() - busySince_) / 1000) + "s";
        if (readingMetadata(p) || separating(p) || jobFor(p, JobKind::Forge)) return "queued";
        std::string st = p.doc.get("stems", "status");
        if (st == "failed") return "stems failed";
        if (p.stemCount > 0) return std::to_string(p.stemCount) + " stems";
        return "no stems yet";
    }

    // A song becomes a new project: a folder with a copy of the song and a
    // project.yaml; then its metadata is read and its stems separated.
    void newProject(const fs::path& song) {
        if (projectsDir_.empty()) {
            status_ = "Choose a projects folder first.";
            return;
        }
        const fs::path source = fs::absolute(song);
        for (auto& p : projects_)
            if (p.doc.get("project", "source_file") == source.string()) {  // imported before
                selectProject(p.id);
                status_ = p.title() + " is already a project.";
                return;
            }
        std::string base = folderName(source.stem().string()), id = base;
        for (int n = 2; fs::exists(projectsDir_ / id); ++n) id = base + " (" + std::to_string(n) + ")";
        const fs::path dir = projectsDir_ / id;
        std::error_code ec;
        fs::create_directories(dir / "stems", ec);
        std::string ext = source.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        fs::copy_file(source, dir / ("song" + ext), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            status_ = "Could not copy the song into the project: " + ec.message();
            return;
        }
        YamlDoc doc;
        doc.header = {"# gd-tool-02 project file. Edit it in the app (Metadata) or by hand,",
                      "# keeping the layout: a section, then indented `key: value` lines."};
        for (auto& [section, keys] : kSchema)
            for (auto& k : keys) doc.ensure(section, k);
        doc.set("project", "created", nowIso());
        doc.set("project", "source_file", source.string());
        doc.set("project", "audio_file", "song" + ext);
        doc.set("stems", "mode", mode_);
        doc.save(dir / "project.yaml");

        refreshProjects();
        jobs_.push_back({dir, JobKind::Metadata, mode_});
        jobs_.push_back({dir, JobKind::Separate, mode_});
        selectProject(id);
        setTab(Tab::Metadata);  // the first thing a new project shows
        newBanner_ = id;
        status_ = "New project " + id + ": reading the file, then separating the stems.";
    }

    void openPath(const std::string& input) {
        fs::path p(input);
        if (fs::is_directory(p)) {
            p = fs::absolute(p).lexically_normal();
            if (!p.has_filename()) p = p.parent_path();
            if (Project* existing = projectAt(p)) selectProject(existing->id);
            else viewLooseFolder(p);
            return;
        }
        static const char* exts[] = {".mp3", ".flac", ".wav", ".ogg"};
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (std::none_of(std::begin(exts), std::end(exts), [&](const char* e) { return ext == e; })) {
            status_ = "Not an audio file: " + p.filename().string();
            return;
        }
        newProject(p);
    }

    void separateAgain() {
        Project* p = currentProject();
        if (!p || separating(*p) || p->audioFile().empty()) return;
        logEvent("separate again requested: " + p->id);
        p->doc.set("stems", "mode", mode_);
        p->doc.set("stems", "status", "");
        p->dirty = true;
        saveProject(*p);
        jobs_.push_back({p->dir, JobKind::Separate, mode_});
        status_ = p->title() + ": queued for separation (" + mode_ + ").";
    }

    void readMetadataAgain() {
        Project* p = currentProject();
        if (!p || readingMetadata(*p) || p->audioFile().empty()) return;
        saveProject(*p);
        unfocus();
        jobs_.push_back({p->dir, JobKind::Metadata, mode_});
        status_ = p->title() + ": reading the file again (filled fields are kept).";
    }

    // ---- Jobs (one Python helper at a time) ------------------------------------------
    void startNextJob() {
        if (worker_.running() || active_ || jobs_.empty()) return;
        Job job = jobs_.front();
        jobs_.pop_front();
        Project* p = projectAt(job.dir);
        if (!p || p->audioFile().empty()) return;
        const fs::path python = toolRoot() / ".venv" / "bin" / "python";
        std::vector<std::string> argv;
        if (job.kind == JobKind::Metadata) {
            argv = {python.string(), "-u", (toolRoot() / "python" / "extract_metadata.py").string(),
                    p->audioFile().string(), p->dir.string()};
        } else if (job.kind == JobKind::Forge) {
            argv = {python.string(), "-u", (toolRoot() / "python" / "forge.py").string(), p->dir.string()};
            argv.insert(argv.end(), job.args.begin(), job.args.end());
            forgeStatus_ = "Starting dream-charter-engine...";
        } else {
            fs::create_directories(p->dir / "stems", ec_);
            argv = {python.string(), "-u", (toolRoot() / "python" / "separate_stems.py").string(),
                    p->audioFile().string(), (p->dir / "stems").string(), "--mode", job.mode};
            if (p->id == current_ && stemsFor_ == p->id) player_.clear();  // new stems appear as they come
        }
        std::string error;
        if (!worker_.start(argv, error)) {
            status_ = "Could not start Python: " + error;
            return;
        }
        active_ = job;
        logEvent(std::string("job start ") + (job.kind == JobKind::Metadata ? "metadata" : job.kind == JobKind::Forge ? "forge" : "separate") +
                 ": " + p->id + " (current=" + current_ + ", stemsFor=" + stemsFor_ + ", stems loaded=" + std::to_string(player_.stems().size()) + ")");
        jobError_.clear();
        busySince_ = SDL_GetTicks();
    }

    void pumpWorker() {
        std::string line;
        while (worker_.popLine(line)) {
            if (!active_) continue;
            Project* p = projectAt(active_->dir);
            const std::string name = p ? p->title() : active_->dir.filename().string();
            if (line.rfind("STATUS ", 0) == 0) {
                status_ = name + ": " + line.substr(7);
                if (active_->kind == JobKind::Forge) forgeStatus_ = line.substr(7);
            } else if (line.rfind("STEM ", 0) == 0) {
                auto sp = line.find(' ', 5);
                std::string error;
                if (sp != std::string::npos && p && p->id == current_ && stemsFor_ == p->id)
                    logEvent("stem " + line.substr(5, sp - 5) + (player_.add(line.substr(5, sp - 5), line.substr(sp + 1), error) ? " shown" : " FAILED: " + error));
                else if (sp != std::string::npos)
                    logEvent("stem " + line.substr(5, sp - 5) + " not shown (current=" + current_ + ", stemsFor=" + stemsFor_ + ")");
            } else if (line.rfind("ERROR ", 0) == 0) {
                jobError_ = line.substr(6);
            } else if (line.rfind("EXIT ", 0) == 0) {
                const bool ok = line == "EXIT 0" && jobError_.empty();
                if (!ok && jobError_.empty()) jobError_ = "Python exited with code " + line.substr(5);
                finishJob(*active_, ok);
                active_.reset();
            }
        }
    }

    void finishJob(const Job& job, bool ok) {
        Project* p = projectAt(job.dir);
        logEvent(std::string("job end ") + (ok ? "ok" : "FAILED " + jobError_) + ": " + job.dir.filename().string());
        if (!p) return;
        const std::string secs = std::to_string((SDL_GetTicks() - busySince_) / 1000) + "s";
        if (job.kind == JobKind::Metadata) {
            p->doc.load(p->dir / "project.yaml");  // written by Python; editing was off meanwhile
            p->dirty = false;
            if (p->id == current_) {
                loadCover(*p);
                if (!lyricsDirty_) loadLyrics(*p);
            }
            status_ = ok ? p->title() + ": metadata read in " + secs + "." : p->title() + ": reading failed - " + jobError_;
        } else if (job.kind == JobKind::Forge) {
            p->doc.set("forge", "status", ok ? "done" : "failed");
            p->doc.set("forge", "error", ok ? "" : jobError_);
            if (ok) {
                p->doc.set("forge", "forged_at", nowIso());
                p->doc.set("forge", "forged_with", job.signature);
            }
            p->dirty = true;
            saveProject(*p);
            if (p->id == previewFor_) loadPreview(*p);
            status_ = ok ? p->title() + ": charts forged in " + secs + "." : p->title() + ": forging failed - " + jobError_;
        } else {
            p->doc.set("stems", "status", ok ? "done" : "failed");
            p->doc.set("stems", "error", ok ? "" : jobError_);
            p->doc.set("stems", "model", job.mode == "demucs" ? "htdemucs_6s" : "filters");
            p->doc.set("stems", "separated_at", ok ? nowIso() : "");
            p->dirty = true;
            saveProject(*p);
            p->stemCount = countStems(p->stemsDir());
            status_ = ok ? p->title() + ": " + std::to_string(p->stemCount) + " stems in " + secs + "."
                         : p->title() + ": separation failed - " + jobError_;
        }
    }

    // ---- The open project ----------------------------------------------------------------
    void closeProject() {
        saveAll();
        unfocus();
        player_.clear();
        stemsFor_.clear();
        current_.clear();
        looseDir_.clear();
        lyricsFor_.clear();
        previewFor_.clear();
        preview_.clear();
        dropOpen_ = false;
        if (coverTex_) SDL_DestroyTexture(coverTex_);
        coverTex_ = nullptr;
    }

    void selectProject(const std::string& id) {
        if (id == current_ && looseDir_.empty()) return;
        Project* p = project(id);
        if (!p) return;
        closeProject();
        current_ = id;
        tab_ = Tab::Info;
        loadCover(*p);
        loadLyrics(*p);
        if (p->doc.get("stems", "status") == "failed") status_ = p->title() + ": stems failed - " + p->doc.get("stems", "error");
    }

    void viewLooseFolder(const fs::path& dir) {
        closeProject();
        looseDir_ = dir;
        tab_ = Tab::Stems;
        loadStemsFrom(dir, "loose");
        status_ = player_.empty() ? "No .wav stems in " + dir.string() : "Opened the stems in " + dir.string();
    }

    void loadStemsFrom(const fs::path& dir, const std::string& owner) {
        player_.clear();
        stemsFor_ = owner;
        logEvent("load stems from " + dir.string());
        std::error_code ec;
        for (auto& e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".wav") continue;
            std::string error;
            if (!player_.add(e.path().stem().string(), e.path().string(), error)) {
                status_ = "Could not load " + e.path().string() + ": " + error;
                logEvent("  " + e.path().filename().string() + " FAILED: " + error);
            } else {
                logEvent("  " + e.path().filename().string() + " ok");
            }
        }
    }

    void setTab(Tab t) {
        Project* p = currentProject();
        if (!p) return;
        logEvent("tab " + std::to_string(int(t)) + ": " + p->id);
        if (tab_ == Tab::Metadata && t != Tab::Metadata) {
            saveProject(*p);
            if (newBanner_ == p->id) newBanner_.clear();
        }
        if (tab_ == Tab::Lyrics && t != Tab::Lyrics) saveLyrics();
        unfocus();
        tab_ = t;
        dropOpen_ = false;
        if ((t == Tab::Stems || t == Tab::Forge) && stemsFor_ != p->id) loadStemsFrom(p->stemsDir(), p->id);
        if (t == Tab::Forge) {
            loadPreview(*p);
            applyListen();
        } else if (listenPart_) {
            player_.setAllMuted(false, true);  // "only this part" is a Forge thing
        }
    }

    void loadCover(const Project& p) {
        if (coverTex_) SDL_DestroyTexture(coverTex_);
        coverTex_ = nullptr;
        if (!fs::exists(p.dir / "cover.png")) return;
        if (SDL_Surface* s = SDL_LoadPNG((p.dir / "cover.png").string().c_str())) {
            coverTex_ = SDL_CreateTextureFromSurface(renderer_, s);
            SDL_DestroySurface(s);
        }
    }

    // ---- Lyrics (lyrics.txt) ------------------------------------------------------------
    void loadLyrics(const Project& p) {
        lyricsFor_ = p.id;
        lyrics_.clear();
        std::ifstream in(p.dir / "lyrics.txt", std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        for (char c : ss.str())
            if (c != '\r') lyrics_ += c;
        caret_ = 0;
        lyricsScroll_ = 0;
        lyricsDirty_ = false;
    }

    void saveLyrics() {
        if (!lyricsDirty_ || lyricsFor_.empty()) return;
        Project* p = project(lyricsFor_);
        if (!p) return;
        std::ofstream out(p->dir / "lyrics.txt", std::ios::binary);
        out << lyrics_;
        lyricsDirty_ = !out;
        status_ = out ? p->title() + ": lyrics saved." : "Could not save the lyrics.";
    }

    int lyricsLineCount() const {
        return lyrics_.empty() ? 0 : int(std::count(lyrics_.begin(), lyrics_.end(), '\n')) + (lyrics_.back() == '\n' ? 0 : 1);
    }

    // Caret as (line, column in characters).
    std::pair<int, int> caretPos() const {
        int line = 0;
        size_t start = 0;
        for (size_t i = 0; i < caret_ && i < lyrics_.size(); ++i)
            if (lyrics_[i] == '\n') {
                ++line;
                start = i + 1;
            }
        return {line, int(text::count(lyrics_.substr(start, caret_ - start)))};
    }
    size_t lineStart(int line) const {
        size_t i = 0;
        for (int l = 0; l < line; ++l) {
            size_t nl = lyrics_.find('\n', i);
            if (nl == std::string::npos) return lyrics_.size();
            i = nl + 1;
        }
        return i;
    }
    int lineTotal() const { return int(std::count(lyrics_.begin(), lyrics_.end(), '\n')) + 1; }
    void caretTo(int line, int col) {
        line = std::clamp(line, 0, lineTotal() - 1);
        size_t start = lineStart(line);
        size_t end = lyrics_.find('\n', start);
        std::string l = lyrics_.substr(start, (end == std::string::npos ? lyrics_.size() : end) - start);
        caret_ = start + text::byteOf(l, size_t(std::max(0, col)));
    }
    void insertLyrics(const std::string& s) {
        std::string clean;
        for (char c : s)
            if (c != '\r') clean += c;
        lyrics_.insert(caret_, clean);
        caret_ += clean.size();
        lyricsDirty_ = true;
    }

    // ---- Text focus ------------------------------------------------------------------------
    void focusField(int i) {
        Project* p = currentProject();
        if (!p || readingMetadata(*p)) return;
        focus_ = i;
        lyricsFocus_ = false;
        SDL_StartTextInput(window_);
    }
    void focusLyrics() {
        focus_ = -1;
        lyricsFocus_ = true;
        SDL_StartTextInput(window_);
    }
    void unfocus() {
        if (focus_ >= 0 || lyricsFocus_) SDL_StopTextInput(window_);
        focus_ = -1;
        lyricsFocus_ = false;
    }
    void focusNext(int dir) {
        for (int k = 1; k <= fieldCount(); ++k) {
            int i = (focus_ + dir * k + fieldCount() * 2) % fieldCount();
            if (fieldAt(i).editable) {
                focus_ = i;
                return;
            }
        }
    }
    void editField(const std::function<void(std::string&)>& change) {
        Project* p = currentProject();
        if (!p || focus_ < 0) return;
        const FieldSpec& f = fieldAt(focus_);
        std::string v = p->doc.get(f.section, f.key);
        change(v);
        p->doc.set(f.section, f.key, v);
        p->dirty = true;
    }

    // ---- Dialogs (callbacks may come from another thread) --------------------------
    static void SDLCALL onDialog(void* user, const char* const* files, int) {
        auto* self = static_cast<App*>(user);
        if (files && files[0]) {
            std::lock_guard<std::mutex> lock(self->dialogMutex_);
            self->chosen_ = files[0];
        }
    }
    void showDialog(DialogKind kind) {
        dialogKind_ = kind;
        if (kind == DialogKind::Song) {
            static const SDL_DialogFileFilter filters[] = {{"Audio", "mp3;flac;wav;ogg"}};
            SDL_ShowOpenFileDialog(onDialog, this, window_, filters, 1, nullptr, false);
        } else {
            std::string start = kind == DialogKind::ProjectsDir && !projectsDir_.empty() ? projectsDir_.string() : "";
            SDL_ShowOpenFolderDialog(onDialog, this, window_, start.empty() ? nullptr : start.c_str(), false);
        }
    }
    void takeDialogResult() {
        std::string file;
        {
            std::lock_guard<std::mutex> lock(dialogMutex_);
            file.swap(chosen_);
        }
        if (file.empty()) return;
        if (dialogKind_ == DialogKind::ProjectsDir) setProjectsDir(file);
        else openPath(file);
    }

    void reveal(const fs::path& dir) { SDL_OpenURL(("file://" + dir.string()).c_str()); }

    // ---- Menus --------------------------------------------------------------------------------
    void buildMenus() {
        Project* p = currentProject();
        const bool hasDir = !projectsDir_.empty();
        const std::string sidebarLabel = sidebarCollapsed_ ? "Expand Sidebar" : "Collapse Sidebar";
        menus_.clear();
        menus_.push_back({"Settings",
                          {item("Projects Folder...", "", [this] { showDialog(DialogKind::ProjectsDir); }),
                           item("Show Projects Folder in Finder", "", [this] { reveal(projectsDir_); }, hasDir),
                           separator(),
                           item("Open Song...", kCmd + "O", [this] { showDialog(DialogKind::Song); }, hasDir),
                           item("Open Stems Folder...", kCmd + "Shift+O", [this] { showDialog(DialogKind::StemsFolder); }),
                           item("Show Project in Finder", "", [this] { if (auto* q = currentProject()) reveal(q->dir); }, p != nullptr),
                           item("Refresh Projects", "F5", [this] { refreshProjects(); }, hasDir),
                           separator(),
                           item("New songs: Demucs 6 stems (AI)", "", [this] { setMode("demucs"); }, true, mode_ == "demucs"),
                           item("New songs: simple filters (no AI)", "", [this] { setMode("simple"); }, true, mode_ == "simple"),
                           separator(),
                           item(sidebarLabel, kCmd + "B", [this] { toggleSidebar(); }),
                           separator(),
                           item("Quit", kCmd + "Q", [this] { quit_ = true; })},
                          {}});
        menus_.push_back({"Help",
                          {item("Keyboard Shortcuts", "F1", [this] { help_ = true; }),
                           item("About gd-tool-02", "", [this] { about_ = true; })},
                          {}});
    }

    // ---- Input -----------------------------------------------------------------------------------
    void handle(SDL_Event& e) {
        SDL_ConvertEventToRenderCoordinates(renderer_, &e);  // window points -> pixels
        switch (e.type) {
            case SDL_EVENT_QUIT: quit_ = true; break;
            case SDL_EVENT_DROP_FILE: openPath(e.drop.data); break;
            case SDL_EVENT_KEY_DOWN: onKey(e.key.key, e.key.mod); break;
            case SDL_EVENT_TEXT_INPUT:
                if (focus_ >= 0) editField([&](std::string& v) { v += e.text.text; });
                else if (lyricsFocus_) insertLyrics(e.text.text);
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (hoverPinned_) break;
                mouseX_ = e.motion.x;
                mouseY_ = e.motion.y;
                if (openMenu_ >= 0)  // slide between open menus, like Windows
                    for (size_t i = 0; i < menus_.size(); ++i)
                        if (inside(menus_[i].titleRect, mouseX_, mouseY_)) openMenu_ = int(i);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (inside(sidebarRect_, mouseX_, mouseY_)) sidebarScroll_ = std::max(0.f, sidebarScroll_ - e.wheel.y * 40);
                else if (inside(lyricsRect_, mouseX_, mouseY_)) lyricsScroll_ = std::max(0, lyricsScroll_ - int(e.wheel.y * 3));
                else if (tab_ == Tab::Forge && inside(boardRect_, mouseX_, mouseY_))  // scrub the board
                    player_.seek(player_.playhead() + double(e.wheel.y) * 0.25);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (e.button.button == SDL_BUTTON_LEFT) onClick(e.button.x, e.button.y);
                break;
            default: break;
        }
    }

    void onKey(SDL_Keycode key, SDL_Keymod mod) {
        const bool cmd = mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL), shift = mod & SDL_KMOD_SHIFT;
        if (help_ || about_) {
            help_ = about_ = false;
            return;
        }
        // Shortcuts that work everywhere.
        if (cmd) {
            if (key == SDLK_Q) quit_ = true;
            else if (key == SDLK_O && !projectsDir_.empty()) showDialog(shift ? DialogKind::StemsFolder : DialogKind::Song);
            else if (key == SDLK_B) toggleSidebar();
            else if (key == SDLK_S) {
                if (Project* p = currentProject()) saveProject(*p);
                saveLyrics();
            } else if (key >= SDLK_1 && key <= SDLK_5 && currentProject()) setTab(Tab(key - SDLK_1));
            else if (key == SDLK_V && focus_ >= 0) {
                if (char* clip = SDL_GetClipboardText()) {
                    std::string s = clip;
                    SDL_free(clip);
                    std::replace(s.begin(), s.end(), '\n', ' ');
                    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
                    editField([&](std::string& v) { v += s; });
                }
            } else if (key == SDLK_V && lyricsFocus_) {
                if (char* clip = SDL_GetClipboardText()) {
                    insertLyrics(clip);
                    SDL_free(clip);
                }
            }
            return;
        }
        if (focus_ >= 0) {  // a metadata field
            if (key == SDLK_BACKSPACE) editField([](std::string& v) { v.erase(text::prevStart(v, v.size())); });
            else if (key == SDLK_TAB) focusNext(shift ? -1 : 1);
            else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) focusNext(1);
            else if (key == SDLK_ESCAPE) unfocus();
            return;
        }
        if (lyricsFocus_) {
            auto [line, col] = caretPos();
            if (key == SDLK_BACKSPACE && caret_ > 0) {
                size_t from = text::prevStart(lyrics_, caret_);
                lyrics_.erase(from, caret_ - from);
                caret_ = from;
                lyricsDirty_ = true;
            } else if (key == SDLK_DELETE && caret_ < lyrics_.size()) {
                size_t to = caret_;
                text::next(lyrics_, to);
                lyrics_.erase(caret_, to - caret_);
                lyricsDirty_ = true;
            } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) insertLyrics("\n");
            else if (key == SDLK_LEFT && caret_ > 0) caret_ = text::prevStart(lyrics_, caret_);
            else if (key == SDLK_RIGHT && caret_ < lyrics_.size()) text::next(lyrics_, caret_);
            else if (key == SDLK_UP) caretTo(line - 1, col);
            else if (key == SDLK_DOWN) caretTo(line + 1, col);
            else if (key == SDLK_HOME) caretTo(line, 0);
            else if (key == SDLK_END) caretTo(line, 1 << 20);
            else if (key == SDLK_ESCAPE) unfocus();
            return;
        }
        const bool inForge = currentProject() && tab_ == Tab::Forge;
        if (key == SDLK_ESCAPE) {
            openMenu_ = -1;
            dropOpen_ = false;
        } else if (inForge && key >= SDLK_1 && key <= SDLK_4) forgeDiff_ = int(key - SDLK_1);  // Easy..Expert
        else if (inForge && key == SDLK_F) startForge();
        else if (key == SDLK_F1) help_ = true;
        else if (key == SDLK_F5) refreshProjects();
        else if (key == SDLK_SPACE) player_.togglePlay();
        else if (key == SDLK_HOME) player_.seek(0);
        else if (key == SDLK_LEFT) player_.seek(player_.playhead() - 5);
        else if (key == SDLK_RIGHT) player_.seek(player_.playhead() + 5);
        else if (key == SDLK_A) player_.setAllMuted(false, true);
        else if (key == SDLK_M) player_.setAllMuted(true, false);
        else if (key >= SDLK_1 && key <= SDLK_9) player_.toggleMute(size_t(key - SDLK_1));
        else if ((key == SDLK_UP || key == SDLK_DOWN) && !projects_.empty()) {  // walk the sidebar
            int i = key == SDLK_DOWN ? 0 : int(projects_.size()) - 1;
            for (size_t k = 0; k < projects_.size(); ++k)
                if (projects_[k].id == current_) i = int(k) + (key == SDLK_DOWN ? 1 : -1);
            selectProject(projects_[size_t(std::clamp(i, 0, int(projects_.size()) - 1))].id);
        }
    }

    void onClick(float x, float y) {
        if (help_ || about_) {
            help_ = about_ = false;
            return;
        }
        if (openMenu_ >= 0) {  // a click with a menu open: an item, another title, or closes it
            for (auto& [rect, idx] : itemRects_)
                if (inside(rect, x, y)) {
                    MenuItem& it = menus_[size_t(openMenu_)].items[idx];
                    openMenu_ = -1;
                    if (it.enabled && it.action) it.action();
                    return;
                }
            for (size_t i = 0; i < menus_.size(); ++i)
                if (inside(menus_[i].titleRect, x, y)) {
                    openMenu_ = openMenu_ == int(i) ? -1 : int(i);
                    return;
                }
            openMenu_ = -1;
            return;
        }
        for (size_t i = 0; i < menus_.size(); ++i)
            if (inside(menus_[i].titleRect, x, y)) {
                openMenu_ = int(i);
                return;
            }
        unfocus();  // a click elsewhere leaves a field; a field's own hit focuses it again
        if (dropOpen_ && !inside(dropRect_, x, y)) {  // a click outside a pop-up list closes it
            dropOpen_ = false;
            return;
        }
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
            if (inside(it->rect, x, y)) {
                auto fn = it->fn;  // may rebuild hits_
                fn(x, y);
                return;
            }
    }

    // ---- Drawing helpers ----------------------------------------------------------------------
    void color(SDL_Color c, Uint8 a = 255) { SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, Uint8(c.a * a / 255)); }
    void fill(const SDL_FRect& r, SDL_Color c, Uint8 a = 255) {
        color(c, a);
        SDL_RenderFillRect(renderer_, &r);
    }
    void frame(const SDL_FRect& r, SDL_Color c) {
        color(c);
        SDL_RenderRect(renderer_, &r);
    }
    // SDL's 8x8 debug font (ASCII; see text_util.h). maxW clips with "...".
    void text(float x, float y, const std::string& utf8, SDL_Color c, float scale, float maxW = 0) {
        std::string s = text::ascii(utf8);
        if (maxW > 0) {
            size_t fit = size_t(std::max(0.f, maxW / (8 * scale)));
            if (s.size() > fit) s = fit > 3 ? s.substr(0, fit - 3) + "..." : s.substr(0, fit);
        }
        color(c);
        SDL_SetRenderScale(renderer_, scale, scale);
        SDL_RenderDebugText(renderer_, x / scale, y / scale, s.c_str());
        SDL_SetRenderScale(renderer_, 1.f, 1.f);
    }
    float textW(const std::string& s, float scale) const { return float(text::count(s)) * 8 * scale; }
    void hit(const SDL_FRect& r, std::function<void(float, float)> fn) { hits_.push_back({r, std::move(fn)}); }
    bool hovered(const SDL_FRect& r) const { return openMenu_ < 0 && inside(r, mouseX_, mouseY_); }

    // A button; returns its width. Disabled buttons draw dim and take no clicks.
    float button(float x, float y, const std::string& label, std::function<void()> fn, bool enabled = true,
                 bool active = false, float h = 0) {
        const float ts = 1.5f * u_, padX = 14 * u_;
        if (h <= 0) h = 30 * u_;
        SDL_FRect r{x, y, textW(label, ts) + 2 * padX, h};
        if (active) fill(r, kOxblood);
        else if (enabled && hovered(r)) fill(r, kHover);
        else fill(r, kBar);
        if (!active) frame(r, enabled ? kMuted : kBar);
        text(x + padX, y + (h - 8 * ts) / 2, label, enabled ? kCream : kMuted, ts);
        if (enabled && fn) hit(r, [fn](float, float) { fn(); });
        return r.w;
    }

    // ---- Drawing --------------------------------------------------------------------------------
    void draw() {
        buildMenus();
        hits_.clear();
        int w, h;
        SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
        u_ = SDL_GetWindowDisplayScale(window_);
        const float W = float(w), H = float(h), menuH = 30 * u_, statusH = 26 * u_;
        const float sideW = sidebarCollapsed_ ? 44 * u_ : 300 * u_;
        color(kInk);
        SDL_RenderClear(renderer_);
        lyricsRect_ = {};

        drawSidebar({0, menuH, sideW, H - menuH - statusH});
        drawMain({sideW, menuH, W - sideW, H - menuH - statusH});
        drawStatusBar({0, H - statusH, W, statusH});
        drawTooltip(W, H);
        drawMenuBar({0, 0, W, menuH});
        if (help_) drawHelp(W, H);
        if (about_) drawAbout(W, H);
        if (!shotPath_.empty() && ++frames_ >= 30 && SDL_GetTicks() > 1500 && !(waitJobs_ && (active_ || !jobs_.empty()))) {
            if (SDL_Surface* shot = SDL_RenderReadPixels(renderer_, nullptr)) {
                SDL_SaveBMP(shot, shotPath_.c_str());
                SDL_DestroySurface(shot);
            }
            quit_ = true;
        }
        SDL_RenderPresent(renderer_);
    }

    void drawMenuBar(const SDL_FRect& bar) {
        fill(bar, kBar);
        fill({bar.x, bar.y + bar.h - u_, bar.w, u_}, kInk);
        const float ts = 1.5f * u_, padX = 12 * u_;
        float x = 6 * u_;
        for (size_t i = 0; i < menus_.size(); ++i) {
            Menu& m = menus_[i];
            m.titleRect = {x, bar.y, textW(m.title, ts) + 2 * padX, bar.h - u_};
            const bool open = openMenu_ == int(i), hover = inside(m.titleRect, mouseX_, mouseY_);
            if (open || hover) fill(m.titleRect, open ? kOxblood : kHover);
            text(x + padX, bar.y + (bar.h - 8 * ts) / 2, m.title, kCream, ts);
            x += m.titleRect.w;
        }
        const std::string brand = "GD-TOOL-02";
        text(bar.w - textW(brand, ts) - padX, bar.y + (bar.h - 8 * ts) / 2, brand, kOxblood, ts);

        itemRects_.clear();
        if (openMenu_ < 0 || openMenu_ >= int(menus_.size())) return;
        Menu& m = menus_[size_t(openMenu_)];
        float labelW = 0, keyW = 0;
        for (auto& it : m.items) {
            labelW = std::max(labelW, textW(it.label, ts));
            keyW = std::max(keyW, textW(it.shortcut, ts));
        }
        const float itemH = 28 * u_, sepH = 10 * u_, checkW = 24 * u_;
        float boxH = 8 * u_;
        for (auto& it : m.items) boxH += it.separator ? sepH : itemH;
        SDL_FRect box{m.titleRect.x, bar.y + bar.h, checkW + labelW + 40 * u_ + keyW + padX, boxH};
        fill({box.x + 4 * u_, box.y + 4 * u_, box.w, box.h}, kInk, 160);  // shadow
        fill(box, kPanel);
        fill({box.x, box.y, box.w, 2 * u_}, kOxblood);
        float y = box.y + 4 * u_;
        for (size_t k = 0; k < m.items.size(); ++k) {
            const MenuItem& it = m.items[k];
            if (it.separator) {
                fill({box.x + 8 * u_, y + sepH / 2, box.w - 16 * u_, u_}, kMuted);
                y += sepH;
                continue;
            }
            SDL_FRect r{box.x + 2 * u_, y, box.w - 4 * u_, itemH};
            itemRects_.push_back({r, k});
            if (it.enabled && inside(r, mouseX_, mouseY_)) fill(r, kHover);
            const float ty = y + (itemH - 8 * ts) / 2;
            if (it.checked) fill({box.x + 10 * u_, y + itemH / 2 - 4 * u_, 8 * u_, 8 * u_}, kOxblood);
            text(box.x + checkW, ty, it.label, it.enabled ? kCream : kMuted, ts);
            if (!it.shortcut.empty()) text(box.x + box.w - padX - textW(it.shortcut, ts), ty, it.shortcut, kDim, ts);
            y += itemH;
        }
    }

    void drawSidebar(const SDL_FRect& r) {
        sidebarRect_ = r;
        fill(r, kPanel);
        fill({r.x + r.w - u_, r.y, u_, r.h}, kInk);
        const float pad = 14 * u_, ts = 1.5f * u_;
        // Collapse / expand.
        SDL_FRect toggle{r.x + r.w - 36 * u_, r.y + 10 * u_, 28 * u_, 24 * u_};
        if (sidebarCollapsed_) toggle.x = r.x + (r.w - toggle.w) / 2;
        fill(toggle, hovered(toggle) ? kHover : kBar);
        text(toggle.x + (toggle.w - textW(">", ts)) / 2, toggle.y + (toggle.h - 8 * ts) / 2,
             sidebarCollapsed_ ? ">" : "<", kCream, ts);
        hit(toggle, [this](float, float) { toggleSidebar(); });
        hoverProject_.clear();

        SDL_FRect plus{toggle.x, toggle.y + 32 * u_, 28 * u_, 24 * u_};
        if (sidebarCollapsed_) {
            if (!projectsDir_.empty()) {
                fill(plus, hovered(plus) ? kOxblood : kBar);
                text(plus.x + (plus.w - textW("+", ts)) / 2, plus.y + (plus.h - 8 * ts) / 2, "+", kCream, ts);
                hit(plus, [this](float, float) { showDialog(DialogKind::Song); });
            }
            return;
        }
        text(r.x + pad, r.y + 18 * u_, "PROJECTS", kSand, ts);
        if (!projectsDir_.empty()) {
            SDL_FRect add{toggle.x - 72 * u_, toggle.y, 64 * u_, 24 * u_};
            fill(add, hovered(add) ? kOxblood : kBar);
            text(add.x + (add.w - textW("+ New", ts)) / 2, add.y + (add.h - 8 * ts) / 2, "+ New", kCream, ts);
            hit(add, [this](float, float) { showDialog(DialogKind::Song); });
        }

        const float listTop = r.y + 46 * u_, itemH = 54 * u_, maxW = r.w - 2 * pad;
        if (projectsDir_.empty()) {
            text(r.x + pad, listTop + 8 * u_, "No projects folder yet.", kDim, 1.25f * u_);
            return;
        }
        if (projects_.empty()) {
            text(r.x + pad, listTop + 8 * u_, "No projects yet.", kDim, 1.25f * u_);
            text(r.x + pad, listTop + 26 * u_, "Drop a song on the window.", kMuted, 1.25f * u_);
            return;
        }
        const float maxScroll = std::max(0.f, float(projects_.size()) * itemH - (r.y + r.h - listTop));
        sidebarScroll_ = std::min(sidebarScroll_, maxScroll);
        float y = listTop - sidebarScroll_;
        for (auto& p : projects_) {
            SDL_FRect row{r.x, y, r.w - u_, itemH};
            // Only whole rows (SDL's clip rect would scale with the text scale).
            if (y >= listTop - 1 && y + itemH <= r.y + r.h + 1) {
                const bool sel = p.id == current_, over = hovered(row);
                if (sel) fill(row, kHover);
                else if (over) fill(row, kHover, 140);
                if (sel) fill({row.x, row.y, 4 * u_, row.h}, kOxblood);
                if (over) hoverProject_ = p.id;
                text(r.x + pad, y + 9 * u_, p.title(), sel ? kCream : kSand, 1.25f * u_, maxW);
                std::string artist = p.artist();
                text(r.x + pad, y + 23 * u_, artist.empty() ? "Unknown artist" : artist, artist.empty() ? kMuted : kDim,
                     1.1f * u_, maxW);
                const bool busy = active_ && fs::equivalent(active_->dir, p.dir, ec_);
                text(r.x + pad, y + 37 * u_, projectState(p), busy ? kCream : p.doc.get("stems", "status") == "failed" ? kOxblood : kMuted,
                     u_, maxW);
                const std::string id = p.id;
                hit(row, [this, id](float, float) { selectProject(id); });
            }
            y += itemH;
        }
        if (hoverProject_ != lastHover_) {
            lastHover_ = hoverProject_;
            hoverSince_ = SDL_GetTicks();
        }
    }

    // The whole song name and artist of the project under the mouse.
    void drawTooltip(float W, float H) {
        if (hoverProject_.empty() || openMenu_ >= 0 || SDL_GetTicks() - hoverSince_ < 350) return;
        Project* p = project(hoverProject_);
        if (!p) return;
        const float ts = 1.5f * u_, lineH = 18 * u_, pad = 10 * u_;
        const size_t width = 44;
        std::vector<std::pair<std::string, SDL_Color>> lines;
        for (auto& l : wrap(p->title(), width)) lines.push_back({l, kCream});
        std::string artist = p->artist().empty() ? "Unknown artist" : p->artist();
        for (auto& l : wrap(artist, width)) lines.push_back({l, kSand});
        std::string album = p->doc.get("song", "album"), year = p->doc.get("song", "year");
        if (!album.empty() || !year.empty())
            for (auto& l : wrap(album + (year.empty() ? "" : (album.empty() ? "" : " - ") + year), width))
                lines.push_back({l, kDim});
        lines.push_back({projectState(*p), kMuted});
        float bw = 0;
        for (auto& [l, c] : lines) bw = std::max(bw, textW(l, ts));
        bw += 2 * pad;
        const float bh = float(lines.size()) * lineH + 2 * pad - 4 * u_;
        float x = std::min(mouseX_ + 18 * u_, W - bw - 8 * u_), y = std::min(mouseY_ + 14 * u_, H - bh - 8 * u_);
        fill({x + 3 * u_, y + 3 * u_, bw, bh}, kInk, 160);
        fill({x, y, bw, bh}, kBar);
        fill({x, y, 3 * u_, bh}, kOxblood);
        for (auto& [l, c] : lines) {
            text(x + pad, y + pad, l, c, ts);
            y += lineH;
        }
    }

    void drawBigTitle(float x, float y, const std::string& title, const std::string& subtitle, float maxW) {
        text(x, y, title, kCream, 3 * u_, maxW);
        fill({x, y + 32 * u_, 120 * u_, 3 * u_}, kOxblood);
        if (!subtitle.empty()) text(x, y + 46 * u_, subtitle, kSand, 2 * u_, maxW);
    }

    void drawMain(const SDL_FRect& r) {
        const float pad = 28 * u_, x0 = r.x + pad, maxW = r.w - 2 * pad;
        if (projectsDir_.empty()) return drawWelcome(r);
        Project* p = currentProject();
        if (!p && looseDir_.empty()) {
            drawBigTitle(x0, r.y + pad, "GD-TOOL-02  STEM VIEWER", "No project open", maxW);
            drawDropZone({x0, r.y + pad + 90 * u_, maxW, r.h - 2 * pad - 90 * u_});
            return;
        }
        if (!p) {  // a folder of stems, not a project
            drawBigTitle(x0, r.y + pad, looseDir_.filename().string(), "Stems folder (not a project)", maxW);
            drawStems({x0, r.y + pad + 80 * u_, maxW, r.h - 2 * pad - 80 * u_});
            return;
        }
        std::string artist = p->artist();
        drawBigTitle(x0, r.y + pad, p->title(), artist.empty() ? "Unknown artist" : artist, maxW);
        // The project's sections: Info, Metadata, Stems, Lyrics and Export (not yet).
        float bx = x0, by = r.y + pad + 82 * u_;
        const std::pair<const char*, Tab> tabs[] = {
            {"Info", Tab::Info}, {"Metadata", Tab::Metadata}, {"Stems", Tab::Stems}, {"Lyrics", Tab::Lyrics}, {"Forge", Tab::Forge}};
        for (auto& [label, t] : tabs) {
            Tab tt = t;
            bx += button(bx, by, label, [this, tt] { setTab(tt); }, true, tab_ == t) + 8 * u_;
        }
        SDL_FRect exportRect{bx, by, textW("Export", 1.5f * u_) + 28 * u_, 30 * u_};
        button(bx, by, "Export", nullptr, false);
        if (hovered(exportRect)) text(bx + exportRect.w + 10 * u_, by + 9 * u_, "coming soon", kMuted, 1.25f * u_);
        fill({x0, by + 38 * u_, maxW, u_}, kBar);

        SDL_FRect body{x0, by + 54 * u_, maxW, r.y + r.h - (by + 54 * u_) - pad / 2};
        switch (tab_) {
            case Tab::Info: drawInfo(*p, body); break;
            case Tab::Metadata: drawMetadata(*p, body); break;
            case Tab::Stems: drawStems(body); break;
            case Tab::Lyrics: drawLyrics(*p, body); break;
            case Tab::Forge: drawForge(*p, body); break;
        }
    }

    void drawWelcome(const SDL_FRect& r) {
        const float pad = 28 * u_, x0 = r.x + pad;
        drawBigTitle(x0, r.y + pad, "GD-TOOL-02  STEM VIEWER", "Welcome", r.w - 2 * pad);
        float y = r.y + pad + 110 * u_;
        const float ts = 1.75f * u_;
        text(x0, y, "First, choose where your projects are saved.", kCream, 2 * u_);
        y += 40 * u_;
        text(x0, y, "Each song becomes a project: a folder with its metadata (project.yaml),", kDim, ts);
        y += 22 * u_;
        text(x0, y, "a copy of the song, its stems, cover and lyrics. You can change it later", kDim, ts);
        y += 22 * u_;
        text(x0, y, "in Settings > Projects Folder.", kDim, ts);
        y += 44 * u_;
        float bx = x0;
        bx += button(bx, y, "Choose Folder...", [this] { showDialog(DialogKind::ProjectsDir); }, true, true, 36 * u_) +
              12 * u_;
        const fs::path def = defaultProjectsDir();
        button(bx, y, "Use " + def.string(), [this, def] { setProjectsDir(def); }, true, false, 36 * u_);
    }

    void drawDropZone(const SDL_FRect& drop) {
        frame(drop, kMuted);
        const float ts = 2 * u_;
        const std::string a = "Drop an MP3 / FLAC / WAV here to start a project";
        const std::string b = "or Settings > Open Song (" + kCmd + "O)";
        text(drop.x + (drop.w - textW(a, ts)) / 2, drop.y + drop.h / 2 - 20 * u_, a, kSand, ts);
        text(drop.x + (drop.w - textW(b, 1.5f * u_)) / 2, drop.y + drop.h / 2 + 10 * u_, b, kDim, 1.5f * u_);
    }

    void drawBanner(float x, float& y, float w, const std::string& msg, SDL_Color c) {
        SDL_FRect b{x, y, w, 30 * u_};
        fill(b, kBar);
        fill({b.x, b.y, 4 * u_, b.h}, c);
        text(b.x + 14 * u_, b.y + (b.h - 8 * 1.5f * u_) / 2, msg, kCream, 1.5f * u_, w - 24 * u_);
        y += 40 * u_;
    }

    void drawInfo(Project& p, const SDL_FRect& r) {
        const float side = std::min(260 * u_, r.h);
        SDL_FRect cover{r.x, r.y, side, side};
        if (coverTex_) {
            SDL_RenderTexture(renderer_, coverTex_, nullptr, &cover);
        } else {
            fill(cover, kPanel);
            frame(cover, kMuted);
            const std::string t = readingMetadata(p) ? "reading..." : "no cover art";
            text(cover.x + (side - textW(t, 1.5f * u_)) / 2, cover.y + side / 2 - 6 * u_, t, kMuted, 1.5f * u_);
        }
        const float x = r.x + side + 32 * u_, w = r.w - side - 32 * u_, ts = 1.75f * u_, row = 30 * u_;
        float y = r.y;
        if (readingMetadata(p)) drawBanner(x, y, w, "Reading the song file... details appear when it finishes.", kSand);
        auto get = [&](const char* s, const char* k) { return p.doc.get(s, k); };
        std::string album = get("song", "album"), year = get("song", "year"), genre = get("song", "genre");
        std::string line;
        for (auto& part : {album, year, genre})
            if (!part.empty()) line += (line.empty() ? "" : "  -  ") + part;
        text(x, y, line.empty() ? "No album, year or genre yet" : line, line.empty() ? kMuted : kSand, 2 * u_, w);
        y += 44 * u_;
        double dur = std::atof(get("audio", "duration_s").c_str());
        std::string key = get("analysis", "key"), bpm = get("analysis", "bpm");
        const int lyricLines = p.id == lyricsFor_ ? lyricsLineCount() : 0;
        std::vector<std::pair<std::string, std::string>> facts = {
            {"Duration", dur > 0 ? clockText(dur) : "-"},
            {"Tempo", bpm.empty() ? "-" : bpm + " BPM"},
            {"Key", key.empty() ? "-" : key},
            {"Stems", projectState(p)},
            {"Lyrics", lyricLines ? std::to_string(lyricLines) + " lines" : "none yet"},
            {"Charter", get("project", "charter").empty() ? "-" : get("project", "charter")},
            {"Created", get("project", "created").empty() ? "-" : get("project", "created").substr(0, 10)},
            {"Folder", p.dir.string()},
        };
        for (auto& [k, v] : facts) {
            text(x, y, k, kDim, ts);
            text(x + 150 * u_, y, v, kCream, ts, w - 150 * u_);
            y += row;
        }
        y += 14 * u_;
        float bx = x;
        bx += button(bx, y, "Edit metadata", [this] { setTab(Tab::Metadata); }) + 8 * u_;
        bx += button(bx, y, "Stems", [this] { setTab(Tab::Stems); }) + 8 * u_;
        bx += button(bx, y, "Lyrics", [this] { setTab(Tab::Lyrics); }) + 8 * u_;
        bx += button(bx, y, "Forge", [this] { setTab(Tab::Forge); }) + 8 * u_;
        const fs::path dir = p.dir;
        button(bx, y, "Show in Finder", [this, dir] { reveal(dir); });
    }

    void drawField(Project& p, int index, float x, float y, float labelW, float valueW) {
        const FieldSpec& f = fieldAt(index);
        const float ts = 1.5f * u_, h = 26 * u_;
        text(x, y + (h - 8 * ts) / 2, f.label, kDim, ts, labelW - 10 * u_);
        std::string v = p.doc.get(f.section, f.key);
        SDL_FRect box{x + labelW, y, valueW, h};
        const bool locked = readingMetadata(p);
        if (!f.editable) {
            text(box.x + 8 * u_, y + (h - 8 * ts) / 2, v.empty() ? "-" : v, v.empty() ? kMuted : kSand, ts, valueW - 8 * u_);
            return;
        }
        const bool focused = focus_ == index;
        fill(box, kInk);
        frame(box, focused ? kOxblood : hovered(box) && !locked ? kDim : kMuted);
        std::string shown = text::ascii(v);
        const size_t fit = size_t(std::max(1.f, (valueW - 20 * u_) / (8 * ts)));
        if (focused && shown.size() >= fit) shown = shown.substr(shown.size() - fit + 1);  // keep the end in view
        text(box.x + 8 * u_, y + (h - 8 * ts) / 2, shown, locked ? kMuted : kCream, ts, focused ? 0 : valueW - 12 * u_);
        if (focused && (SDL_GetTicks() / 500) % 2 == 0)
            fill({box.x + 8 * u_ + textW(shown, ts) + u_, y + 5 * u_, 2 * u_, h - 10 * u_}, kCream);
        if (!locked) hit(box, [this, index](float, float) { focusField(index); });
    }

    void drawMetadata(Project& p, const SDL_FRect& r) {
        float y = r.y;
        const bool locked = readingMetadata(p);
        if (locked) drawBanner(r.x, y, r.w, "Reading the song file... the fields fill in when it finishes.", kSand);
        else if (newBanner_ == p.id)
            drawBanner(r.x, y, r.w, "New project: check what was read from the file, then Save. Stems separate meanwhile.", kOxblood);
        // Actions.
        float bx = r.x;
        bx += button(bx, y, p.dirty ? "Save *" : "Save", [this] {
                  if (Project* q = currentProject()) {
                      q->dirty = true;
                      saveProject(*q);
                      newBanner_.clear();
                  }
              }, !locked, p.dirty) + 8 * u_;
        bx += button(bx, y, "Revert", [this] {
                  if (Project* q = currentProject()) {
                      q->doc.load(q->dir / "project.yaml");
                      q->dirty = false;
                      status_ = q->title() + ": changes discarded.";
                  }
              }, !locked && p.dirty) + 8 * u_;
        bx += button(bx, y, "Read the file again", [this] { readMetadataAgain(); }, !locked && !p.audioFile().empty()) + 8 * u_;
        const fs::path yaml = p.dir / "project.yaml";
        button(bx, y, "Open project.yaml", [yaml] { SDL_OpenURL(("file://" + yaml.string()).c_str()); });
        y += 46 * u_;

        const float gap = 40 * u_, colW = (r.w - gap) / 2, labelW = 220 * u_, rowH = 32 * u_;
        const float yl = y, yr = y;
        text(r.x, yl, "SONG", kSand, 1.5f * u_);
        text(r.x + colW + gap, yr, "ANALYSIS AND FILE", kSand, 1.5f * u_);
        for (size_t i = 0; i < kLeftFields.size(); ++i)
            drawField(p, int(i), r.x, yl + 24 * u_ + float(i) * rowH, labelW, colW - labelW);
        for (size_t i = 0; i < kRightFields.size(); ++i)
            drawField(p, int(kLeftFields.size() + i), r.x + colW + gap, yr + 24 * u_ + float(i) * rowH, labelW,
                      colW - labelW);
    }

    void drawStems(const SDL_FRect& r) {
        Project* p = currentProject();
        float top = r.y;
        if (p && separating(*p))
            drawBanner(r.x, top, r.w,
                       active_ && active_->kind == JobKind::Separate && fs::equivalent(active_->dir, p->dir, ec_)
                           ? "Separating... the stems appear here as they are ready."
                           : "Queued: the stems are separated after the song before it.",
                       kSand);
        // Toolbar: transport | all on / mute all | separation (right).
        {
            const float tw = transport(r.x, top);
            float rx = r.x + r.w;
            if (p) {
                const bool canSep = !separating(*p) && !p->audioFile().empty();
                const std::string again = p->stemCount > 0 ? "Separate again" : "Separate";
                rx -= textW(again, 1.5f * u_) + 28 * u_;
                button(rx, top, again, [this] { separateAgain(); }, canSep, false, 34 * u_);
                const float segW = textW("Demucs", 1.5f * u_) + textW("Simple", 1.5f * u_) + 48 * u_;
                rx -= segW + 8 * u_;
                segmented(rx, top, {"Demucs", "Simple"}, mode_ == "demucs" ? 0 : 1,
                          [this](int i) { setMode(i == 0 ? "demucs" : "simple"); });
            }
            const bool has = !player_.empty();
            const float mx = r.x + tw + 20 * u_;
            const float need = textW("All on", 1.5f * u_) + textW("Mute all", 1.5f * u_) + 64 * u_;
            if (mx + need < rx - 12 * u_) {  // only when they fit (A and M work anyway)
                float bx2 = mx + button(mx, top, "All on", [this] { player_.setAllMuted(false, true); }, has, false, 34 * u_) + 8 * u_;
                button(bx2, top, "Mute all", [this] { player_.setAllMuted(true, false); }, has, false, 34 * u_);
            }
            top += 50 * u_;
        }
        if (player_.empty()) {
            if (!p || !separating(*p)) {
                const std::string a = "No stems yet.";
                text(r.x, top + 10 * u_, a, kDim, 2 * u_);
                if (p && !p->audioFile().empty())
                    button(r.x, top + 44 * u_, "Separate now (" + mode_ + ")", [this] { separateAgain(); });
            }
            return;
        }
        const float bottom = r.y + r.h - 30 * u_, labelW = 240 * u_;
        const auto& stems = player_.stems();
        const size_t n = stems.size();
        const float rowH = std::min(100 * u_, (bottom - top) / float(n));
        const double dur = player_.duration(), now = player_.playhead();
        for (size_t i = 0; i < n; ++i) {
            const Stem& s = *stems[i];
            SDL_FRect row{r.x + labelW, top + float(i) * rowH, r.w - labelW, rowH - 8 * u_};
            fill({r.x, row.y, r.w, row.h}, kPanel);
            const SDL_Color c = stemColor(s.name);
            fill({r.x, row.y, 6 * u_, row.h}, s.muted ? kMuted : c);
            text(r.x + 16 * u_, row.y + row.h / 2 - 14 * u_, std::to_string(i + 1) + " " + s.name,
                 s.muted ? kMuted : kCream, 2 * u_);
            text(r.x + 16 * u_, row.y + row.h / 2 + 6 * u_, s.muted ? "muted" : "on", s.muted ? kMuted : kDim, 1.5f * u_);
            hit({r.x, row.y, labelW, row.h}, [this, i](float, float) {
                (SDL_GetModState() & SDL_KMOD_SHIFT) ? player_.solo(i) : player_.toggleMute(i);
            });
            // M(ute) and S(olo).
            bool soloed = !s.muted;
            for (size_t k = 0; k < n; ++k)
                if (k != i && !stems[k]->muted) soloed = false;
            const float bs = 22 * u_, by = row.y + row.h / 2 - bs / 2;
            SDL_FRect mb{r.x + labelW - 2 * bs - 18 * u_, by, bs, bs}, sb{r.x + labelW - bs - 12 * u_, by, bs, bs};
            fill(mb, s.muted ? kOxblood : (hovered(mb) ? kHover : kBar));
            fill(sb, soloed ? SDL_Color{0xc9, 0xb8, 0x92, 255} : (hovered(sb) ? kHover : kBar));
            text(mb.x + (bs - 12 * u_) / 2, by + (bs - 12 * u_) / 2, "M", kCream, 1.5f * u_);
            text(sb.x + (bs - 12 * u_) / 2, by + (bs - 12 * u_) / 2, "S", soloed ? kInk : kCream, 1.5f * u_);
            hit(mb, [this, i](float, float) { player_.toggleMute(i); });
            hit(sb, [this, i, soloed](float, float) { soloed ? player_.setAllMuted(false, true) : player_.solo(i); });
            hit(row, [this, row](float x, float) { player_.seek(double((x - row.x) / row.w) * player_.duration()); });

            color(s.muted ? kMuted : c, s.muted ? 140 : 255);
            const float mid = row.y + row.h / 2, amp = row.h / 2 - 4 * u_;
            const int cols = int(row.w);
            for (int x = 0; x < cols; ++x) {
                size_t b0 = size_t(x) * s.peaks.size() / size_t(cols);
                size_t b1 = std::max(b0 + 1, size_t(x + 1) * s.peaks.size() / size_t(cols));
                float pk = 0;
                for (size_t b = b0; b < b1 && b < s.peaks.size(); ++b) pk = std::max(pk, s.peaks[b]);
                const float hh = std::max(1.f, pk * amp);
                SDL_RenderLine(renderer_, row.x + float(x), mid - hh, row.x + float(x), mid + hh);
            }
            if (dur > 0) {
                const float px = row.x + float(now / dur) * row.w;
                color(kCream);
                SDL_RenderLine(renderer_, px, row.y, px, row.y + row.h);
            }
        }
        const std::string hint = "Space play  -  1-9: mute a stem  -  S or Shift+click: solo  -  click a wave: seek";
        text(r.x + r.w - textW(hint, 1.25f * u_), r.y + r.h - 20 * u_, hint, kMuted, 1.25f * u_);
    }

    void drawLyrics(Project& p, const SDL_FRect& r) {
        if (lyricsFor_ != p.id) loadLyrics(p);
        float y = r.y;
        float bx = r.x;
        bx += button(bx, y, lyricsDirty_ ? "Save *" : "Save", [this] {
                  lyricsDirty_ = true;
                  saveLyrics();
              }, true, lyricsDirty_) + 8 * u_;
        bx += button(bx, y, "Revert", [this] {
                  if (Project* q = currentProject()) {
                      loadLyrics(*q);
                      status_ = q->title() + ": lyrics changes discarded.";
                  }
              }, lyricsDirty_) + 8 * u_;
        const fs::path file = p.dir / "lyrics.txt";
        button(bx, y, "Open lyrics.txt", [this, file] {
            saveLyrics();
            if (!fs::exists(file)) std::ofstream(file).close();
            SDL_OpenURL(("file://" + file.string()).c_str());
        });
        text(r.x + r.w - textW("One sung phrase per line.", 1.25f * u_), y + 10 * u_, "One sung phrase per line.", kMuted,
             1.25f * u_);
        y += 44 * u_;

        SDL_FRect box{r.x, y, r.w, r.y + r.h - y};
        lyricsRect_ = box;
        fill(box, kInk);
        frame(box, lyricsFocus_ ? kOxblood : kMuted);
        hit(box, [this, box](float mx, float my) {
            focusLyrics();
            const float ts = 1.75f * u_, lineH = 24 * u_, gutter = 52 * u_;
            int line = lyricsScroll_ + int((my - box.y - 10 * u_) / lineH);
            int col = int(std::lround((mx - box.x - gutter) / (8 * ts)));
            caretTo(line, col);
        });
        const float ts = 1.75f * u_, lineH = 24 * u_, gutter = 52 * u_;
        const int visible = std::max(1, int((box.h - 20 * u_) / lineH));
        auto [cl, cc] = caretPos();
        if (lyricsFocus_) {  // keep the caret in view
            if (cl < lyricsScroll_) lyricsScroll_ = cl;
            if (cl >= lyricsScroll_ + visible) lyricsScroll_ = cl - visible + 1;
        }
        lyricsScroll_ = std::clamp(lyricsScroll_, 0, std::max(0, lineTotal() - visible));
        if (lyrics_.empty() && !lyricsFocus_) {
            text(box.x + gutter, box.y + 10 * u_, "No lyrics yet. Click here and type or paste them,", kMuted, ts);
            text(box.x + gutter, box.y + 10 * u_ + lineH, "one sung phrase per line.", kMuted, ts);
            return;
        }
        int lineNo = 0;
        size_t start = 0;
        while (true) {
            size_t end = lyrics_.find('\n', start);
            std::string l = lyrics_.substr(start, (end == std::string::npos ? lyrics_.size() : end) - start);
            int row = lineNo - lyricsScroll_;
            if (row >= 0 && row < visible) {
                float ly = box.y + 10 * u_ + float(row) * lineH;
                text(box.x + 8 * u_, ly, std::to_string(lineNo + 1), kMuted, 1.25f * u_);
                text(box.x + gutter, ly, l, kCream, ts, box.w - gutter - 10 * u_);
                if (lyricsFocus_ && lineNo == cl && (SDL_GetTicks() / 500) % 2 == 0)
                    fill({box.x + gutter + float(cc) * 8 * ts, ly - 3 * u_, 2 * u_, 8 * ts + 6 * u_}, kCream);
            }
            if (end == std::string::npos) break;
            start = end + 1;
            ++lineNo;
        }
    }

    void drawStatusBar(const SDL_FRect& r) {
        fill(r, kBar);
        fill({r.x, r.y, r.w, u_}, kInk);
        const float ts = 1.25f * u_, pad = 12 * u_, ty = r.y + (r.h - 8 * ts) / 2;
        std::string right = "Separation: " + std::string(mode_ == "demucs" ? "Demucs 6 stems" : "simple filters");
        if (!jobs_.empty()) right = std::to_string(jobs_.size()) + " queued   " + right;
        const float rw = textW(right, ts);
        if (active_) fill({r.x + pad, r.y + r.h / 2 - 3 * u_, 6 * u_, 6 * u_}, kOxblood);
        text(r.x + pad + (active_ ? 14 * u_ : 0), ty, status_, active_ ? kCream : kDim, ts, r.w - rw - 4 * pad);
        text(r.x + r.w - pad - rw, ty, right, kDim, ts);
    }

    void drawDialog(float W, float H, const std::string& title,
                    const std::vector<std::pair<std::string, std::string>>& lines) {
        fill({0, 0, W, H}, kInk, 180);
        const float ts = 1.5f * u_, lineH = 22 * u_, bw = 640 * u_, bh = 90 * u_ + float(lines.size()) * lineH;
        SDL_FRect box{(W - bw) / 2, (H - bh) / 2, bw, bh};
        fill(box, kPanel);
        fill({box.x, box.y, box.w, 3 * u_}, kOxblood);
        text(box.x + 24 * u_, box.y + 22 * u_, title, kCream, 2 * u_);
        float y = box.y + 56 * u_;
        for (auto& [a, b] : lines) {
            text(box.x + 24 * u_, y, a, kSand, ts);
            text(box.x + 230 * u_, y, b, kDim, ts, box.w - 254 * u_);
            y += lineH;
        }
        text(box.x + 24 * u_, box.y + box.h - 24 * u_, "Click or press any key to close.", kMuted, 1.25f * u_);
    }

    void drawHelp(float W, float H) {
        drawDialog(W, H, "Keyboard shortcuts",
                   {{kCmd + "O", "Open a song (new project)"},
                    {kCmd + "Shift+O", "Open a folder of stems"},
                    {kCmd + "1 ... 5", "Info / Metadata / Stems / Lyrics / Forge"},
                    {"Forge: 1-4, F", "Easy ... Expert, forge the charts"},
                    {"Forge: mouse wheel", "Scrub the board"},
                    {kCmd + "S", "Save metadata and lyrics"},
                    {kCmd + "B", "Collapse / expand the sidebar"},
                    {"Up / Down", "Previous / next project"},
                    {"Tab, Enter / Esc", "Next field / leave the field"},
                    {"Space", "Play / pause the stems"},
                    {"Home, Left, Right", "Start, back 5 s, forward 5 s"},
                    {"1-9, A, M", "Mute a stem, all stems on, mute all"},
                    {"F5", "Refresh projects"},
                    {kCmd + "Q", "Quit"}});
    }

    void drawAbout(float W, float H) {
        drawDialog(W, H, "gd-tool-02 Stem Viewer",
                   {{"Separation", "Demucs htdemucs_6s (Meta, MIT), offline"},
                    {"Metadata", "mutagen (tags), librosa (tempo, key)"},
                    {"Projects", projectsDir_.empty() ? "not chosen yet" : projectsDir_.string()},
                    {"Settings", settingsPath_.string()},
                    {"Built with", "C++17, SDL " + std::to_string(SDL_MAJOR_VERSION) + "." + std::to_string(SDL_MINOR_VERSION)},
                    {"Part of", "dream-charter-engine (spike)"}});
    }

    // ---- Forge: what each part is made from, the charts, and the board ----------------
    // project.yaml [forge]: parts (list, empty = default), <part>_source, genre, status,
    // error, forged_at, forged_with (the settings the current charts were made with).
    bool hasLyrics(const Project& p) const {
        if (p.id == lyricsFor_) return !lyrics_.empty();
        std::error_code ec;
        return fs::exists(p.dir / "lyrics.txt") && fs::file_size(p.dir / "lyrics.txt", ec) > 2;
    }
    bool partOn(const Project& p, const std::string& part) const {
        const std::string parts = p.doc.get("forge", "parts");
        if (parts.empty()) return part != "vocals" || hasLyrics(p);
        return ("," + parts + ",").find("," + part + ",") != std::string::npos;
    }
    std::string sourceOf(const Project& p, const std::string& part) const {
        const std::string s = p.doc.get("forge", part + "_source");
        return s.empty() ? part : s;
    }
    void setPartOn(Project& p, const std::string& part, bool on) {
        std::string list;
        for (auto& def : kParts) {
            bool v = def.id == part ? on : partOn(p, def.id);
            if (v) list += (list.empty() ? "" : ",") + std::string(def.id);
        }
        p.doc.set("forge", "parts", list.empty() ? "none" : list);
        p.dirty = true;
        saveProject(p);
    }
    void setSource(Project& p, const std::string& part, const std::string& source) {
        p.doc.set("forge", part + "_source", source);
        p.dirty = true;
        saveProject(p);
        applyListen();
    }
    // The settings, as the charts would be made with them now.
    std::string forgeSignature(const Project& p) const {
        std::string sig;
        for (auto& def : kParts)
            if (partOn(p, def.id)) sig += std::string(def.id) + "=" + sourceOf(p, def.id) + ";";
        return sig + "genre=" + p.doc.get("forge", "genre") + ";drums=" + drumStyle(p) + ";riffs=" + riffStyle(p);
    }
    // How steady the drum grooves are: the engine's repeat_similarity (bars at least this
    // alike get the same notes). "heard" is the engine's own 0.9.
    std::string drumStyle(const Project& p) const {
        const std::string s = p.doc.get("forge", "drums_style");
        return s == "heard" || s == "locked" ? s : "steady";
    }
    static const char* drumSimilarity(const std::string& style) {
        return style == "heard" ? "0.9" : style == "locked" ? "0.7" : "0.8";
    }
    // The same for guitar, bass and keys: "steady" groups repeating riffs (0.8, measured best:
    // lower merged real notes away), "heard" leaves each bar as detected (the engine's default).
    std::string riffStyle(const Project& p) const { return p.doc.get("forge", "riffs_style") == "heard" ? "heard" : "steady"; }
    bool forging(const Project& p) const { return jobFor(p, JobKind::Forge); }
    bool canForge(const Project& p) const {
        return p.stemCount > 0 && !separating(p) && !forging(p) && !p.audioFile().empty() && !forgeSignature(p).empty();
    }

    void startForge() {
        Project* p = currentProject();
        if (!p || !canForge(*p)) return;
        Job job{p->dir, JobKind::Forge, mode_};
        std::string parts, sources;
        for (auto& def : kParts) {
            if (!partOn(*p, def.id)) continue;
            parts += (parts.empty() ? "" : ",") + std::string(def.id);
            const std::string src = sourceOf(*p, def.id);
            if (def.swappable && src != def.id) sources += (sources.empty() ? "" : ",") + std::string(def.id) + "=" + src;
        }
        if (parts.empty()) {
            status_ = "Turn on at least one part to forge.";
            return;
        }
        job.args = {"--instruments", parts, "--sources", sources, "--genre", p->doc.get("forge", "genre"),
                    "--drum-similarity", drumSimilarity(drumStyle(*p)),
                    "--riff-similarity", riffStyle(*p) == "heard" ? "0" : "0.8"};
        job.signature = forgeSignature(*p);
        jobs_.push_back(job);
        forgeStatus_ = "Waiting for the job before it...";
        status_ = p->title() + ": forging " + parts + ".";
    }

    void loadPreview(const Project& p) {
        previewFor_ = p.id;
        preview_.load(p.dir / "forge" / "preview.txt");
    }

    // "Only this part": solo the stem the selected part is made from.
    void applyListen() {
        Project* p = currentProject();
        if (!p || player_.empty()) return;
        if (!listenPart_) {
            player_.setAllMuted(false, true);
            return;
        }
        const std::string stem = forgePart_ == "drums" || forgePart_ == "vocals" ? forgePart_ : sourceOf(*p, forgePart_);
        for (size_t i = 0; i < player_.stems().size(); ++i)
            if (player_.stems()[i]->name == stem) player_.solo(i);
    }

    void selectPart(const std::string& part) {
        forgePart_ = part;
        applyListen();
    }

    // Play / pause / start / -5 s / +5 s and the time; returns its width.
    float transport(float x, float y) {
        const float x0 = x, gap = 6 * u_;
        const bool has = !player_.empty();
        x += iconButton(x, y, player_.playing() ? Icon::Pause : Icon::Play, [this] { player_.togglePlay(); }, has,
                        player_.playing()) + gap;
        x += iconButton(x, y, Icon::Start, [this] { player_.seek(0); }, has) + gap;
        x += iconButton(x, y, Icon::Back, [this] { player_.seek(player_.playhead() - 5); }, has) + gap;
        x += iconButton(x, y, Icon::Forward, [this] { player_.seek(player_.playhead() + 5); }, has) + 14 * u_;
        const std::string t = clockTenths(player_.playhead()) + " / " + clockTenths(player_.duration());
        text(x, y + (34 * u_ - 16 * u_) / 2, t, has ? kCream : kMuted, 2 * u_);
        return x + textW(t, 2 * u_) - x0;
    }

    enum class Icon { Play, Pause, Start, Back, Forward };
    float iconButton(float x, float y, Icon icon, std::function<void()> fn, bool enabled = true, bool active = false) {
        const float s = 34 * u_;
        SDL_FRect r{x, y, s, s};
        fill(r, active ? kOxblood : (enabled && hovered(r) ? kHover : kBar));
        if (!active) frame(r, enabled ? kMuted : kBar);
        const SDL_FColor c = shape::fcolor(enabled ? kCream : kMuted);
        const float cx = x + s / 2, cy = y + s / 2, k = 6 * u_;
        auto tri = [&](float ax, float ay, float bx, float by, float qx, float qy) {
            shape::triangle(renderer_, {ax, ay}, {bx, by}, {qx, qy}, c);
        };
        color(enabled ? kCream : kMuted);
        switch (icon) {
            case Icon::Play: tri(cx - k * 0.7f, cy - k, cx + k, cy, cx - k * 0.7f, cy + k); break;
            case Icon::Pause: {
                SDL_FRect a{cx - k * 0.8f, cy - k, k * 0.55f, 2 * k}, b{cx + k * 0.25f, cy - k, k * 0.55f, 2 * k};
                SDL_RenderFillRect(renderer_, &a);
                SDL_RenderFillRect(renderer_, &b);
                break;
            }
            case Icon::Start: {
                SDL_FRect bar{cx - k, cy - k, k * 0.35f, 2 * k};
                SDL_RenderFillRect(renderer_, &bar);
                tri(cx + k * 0.8f, cy - k, cx - k * 0.5f, cy, cx + k * 0.8f, cy + k);
                break;
            }
            case Icon::Back:
                tri(cx, cy - k * 0.8f, cx - k, cy, cx, cy + k * 0.8f);
                tri(cx + k, cy - k * 0.8f, cx, cy, cx + k, cy + k * 0.8f);
                break;
            case Icon::Forward:
                tri(cx, cy - k * 0.8f, cx + k, cy, cx, cy + k * 0.8f);
                tri(cx - k, cy - k * 0.8f, cx, cy, cx - k, cy + k * 0.8f);
                break;
        }
        if (enabled && fn) hit(r, [fn](float, float) { fn(); });
        return s;
    }

    // A row of choices, one lit; returns its width.
    float segmented(float x, float y, const std::vector<std::string>& labels, int selected,
                    const std::function<void(int)>& pick, float h = 0) {
        const float ts = 1.5f * u_, padX = 12 * u_;
        if (h <= 0) h = 34 * u_;
        float w = 0;
        for (auto& l : labels) w += textW(l, ts) + 2 * padX;
        SDL_FRect all{x, y, w, h};
        fill(all, kBar);
        float cx = x;
        for (size_t i = 0; i < labels.size(); ++i) {
            SDL_FRect r{cx, y, textW(labels[i], ts) + 2 * padX, h};
            const bool on = int(i) == selected;
            if (on) fill(r, kOxblood);
            else if (hovered(r)) fill(r, kHover);
            text(r.x + padX, y + (h - 8 * ts) / 2, labels[i], on ? kCream : kDim, ts);
            const int idx = int(i);
            hit(r, [pick, idx](float, float) { pick(idx); });
            cx += r.w;
        }
        frame(all, kMuted);
        return w;
    }

    // A pop-up list under an anchor (stem source, genre).
    struct DropItem {
        std::string label, note;
        SDL_Color dot;
        bool enabled, checked;
        std::function<void()> pick;
    };
    void openDropdown(const SDL_FRect& anchor, std::vector<DropItem> items) {
        dropAnchor_ = anchor;
        dropItems_ = std::move(items);
        dropOpen_ = true;
    }
    void drawDropdown() {
        if (!dropOpen_) return;
        const float ts = 1.5f * u_, rowH = 28 * u_, pad = 10 * u_;
        float w = dropAnchor_.w;
        for (auto& it : dropItems_) w = std::max(w, textW(it.label, ts) + textW(it.note, 1.25f * u_) + 70 * u_);
        SDL_FRect box{dropAnchor_.x, dropAnchor_.y + dropAnchor_.h + 4 * u_, w, float(dropItems_.size()) * rowH + 8 * u_};
        int ww, wh;
        SDL_GetCurrentRenderOutputSize(renderer_, &ww, &wh);
        if (box.y + box.h > float(wh) - 30 * u_) box.y = dropAnchor_.y - box.h - 4 * u_;  // open upwards
        dropRect_ = box;
        fill({box.x + 4 * u_, box.y + 4 * u_, box.w, box.h}, kInk, 170);
        fill(box, kPanel);
        frame(box, kMuted);
        fill({box.x, box.y, box.w, 2 * u_}, kOxblood);
        float y = box.y + 4 * u_;
        for (auto& it : dropItems_) {
            SDL_FRect r{box.x + 2 * u_, y, box.w - 4 * u_, rowH};
            if (it.enabled && inside(r, mouseX_, mouseY_)) fill(r, kHover);
            if (it.dot.a) shape::ellipse(renderer_, r.x + pad + 5 * u_, y + rowH / 2, 5 * u_, 5 * u_, shape::fcolor(it.dot, it.enabled ? 1 : 0.35f));
            text(r.x + pad + 18 * u_, y + (rowH - 8 * ts) / 2, it.label, it.enabled ? kCream : kMuted, ts);
            if (!it.note.empty())
                text(r.x + r.w - pad - textW(it.note, 1.25f * u_) - (it.checked ? 18 * u_ : 0), y + (rowH - 10 * u_) / 2,
                     it.note, kMuted, 1.25f * u_);
            if (it.checked) fill({r.x + r.w - pad - 8 * u_, y + rowH / 2 - 4 * u_, 8 * u_, 8 * u_}, kOxblood);
            if (it.enabled) {
                auto pick = it.pick;
                hit(r, [this, pick](float, float) {
                    dropOpen_ = false;
                    pick();
                });
            }
            y += rowH;
        }
    }

    // The genre presets of dream-charter-engine (ai_charter/genres/*.toml).
    const std::vector<std::pair<std::string, std::string>>& genres() {
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
                found.push_back({e.path().stem().string(), name});
            }
            std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.second < b.second; });
            genres_.insert(genres_.end(), found.begin(), found.end());
        }
        return genres_;
    }

    void drawForge(Project& p, const SDL_FRect& r) {
        if (previewFor_ != p.id) loadPreview(p);
        const bool busy = forging(p), hasStems = p.stemCount > 0 && !separating(p);
        // Toolbar: transport | difficulty | Forge.
        float y = r.y;
        transport(r.x, y);
        const std::string forgeLabel = preview_.loaded ? "Forge again" : "Forge charts";
        const float forgeW = textW(forgeLabel, 1.5f * u_) + 28 * u_;
        button(r.x + r.w - forgeW, y, forgeLabel, [this] { startForge(); }, canForge(p), canForge(p), 34 * u_);
        std::vector<std::string> diffs = {"Easy", "Medium", "Hard", "Expert"};
        float segW = 0;
        for (auto& d : diffs) segW += textW(d, 1.5f * u_) + 24 * u_;
        if (forgePart_ != "vocals")
            segmented(r.x + r.w - forgeW - 16 * u_ - segW, y, diffs, forgeDiff_, [this](int i) { forgeDiff_ = i; });
        y += 50 * u_;

        const float panelW = 330 * u_;
        drawParts(p, {r.x, y, panelW, r.y + r.h - y});
        SDL_FRect board{r.x + panelW + 20 * u_, y, r.w - panelW - 20 * u_, r.y + r.h - y};
        drawBoard(p, board, busy, hasStems);
        drawDropdown();
    }

    void drawParts(Project& p, const SDL_FRect& r) {
        const float cardH = 66 * u_, gap = 6 * u_;
        text(r.x, r.y, "PARTS", kSand, 1.5f * u_);
        float y = r.y + 24 * u_;
        for (auto& def : kParts) {
            const std::string id = def.id;
            const bool on = partOn(p, id), sel = forgePart_ == id;
            SDL_FRect card{r.x, y, r.w, cardH};
            fill(card, sel ? kHover : kPanel);
            if (sel) frame(card, kOxblood);
            else if (hovered(card)) frame(card, kMuted);
            hit(card, [this, id](float, float) { selectPart(id); });
            const SDL_Color pc = stemColor(id);
            fill({card.x, card.y, 5 * u_, card.h}, on ? pc : kMuted);
            // On / off.
            SDL_FRect box{card.x + 16 * u_, card.y + 12 * u_, 18 * u_, 18 * u_};
            fill(box, on ? kOxblood : kInk);
            frame(box, on ? kOxblood : kMuted);
            if (on) {
                color(kCream);
                SDL_RenderLine(renderer_, box.x + 4 * u_, box.y + 9 * u_, box.x + 8 * u_, box.y + 13 * u_);
                SDL_RenderLine(renderer_, box.x + 8 * u_, box.y + 13 * u_, box.x + 14 * u_, box.y + 5 * u_);
                SDL_RenderLine(renderer_, box.x + 4 * u_, box.y + 10 * u_, box.x + 8 * u_, box.y + 14 * u_);
                SDL_RenderLine(renderer_, box.x + 8 * u_, box.y + 14 * u_, box.x + 14 * u_, box.y + 6 * u_);
            }
            SDL_FRect boxHit{box.x - 6 * u_, box.y - 6 * u_, box.w + 12 * u_, box.h + 12 * u_};
            hit(boxHit, [this, id, on](float, float) {
                if (Project* q = currentProject()) setPartOn(*q, id, !on);
            });
            text(card.x + 46 * u_, card.y + 13 * u_, def.label, on ? kCream : kMuted, 2 * u_);
            // Notes in the current charts.
            if (preview_.loaded) {
                int n = preview_.count(id, forgeDiff_);
                if (id == "vocals" && n == 0) n = -1;
                // On but not in the charts: the engine skips a stem that is nearly silent.
                const bool skipped = n < 0 && on && p.doc.get("forge", "forged_with").find(id + "=") != std::string::npos;
                std::string c = skipped ? "skipped: too quiet" : n < 0 ? "not charted" : std::to_string(n) + (id == "vocals" ? " syllables" : " notes");
                text(card.x + card.w - textW(c, 1.25f * u_) - 12 * u_, card.y + 15 * u_, c, n < 0 ? kMuted : kDim, 1.25f * u_);
            }
            // Made from.
            const float ry = card.y + 36 * u_;
            text(card.x + 46 * u_, ry + 6 * u_, "from", kMuted, 1.25f * u_);
            const std::string src = sourceOf(p, id);
            SDL_FRect chip{card.x + 90 * u_, ry, 126 * u_, 22 * u_};
            const bool canPick = def.swappable && on;
            fill(chip, kInk);
            frame(chip, canPick && hovered(chip) ? kDim : kMuted);
            shape::ellipse(renderer_, chip.x + 11 * u_, chip.y + chip.h / 2, 4 * u_, 4 * u_, shape::fcolor(stemColor(src), on ? 1 : 0.4f));
            text(chip.x + 22 * u_, chip.y + (chip.h - 12 * u_) / 2, src, on ? kCream : kMuted, 1.5f * u_, chip.w - 44 * u_);
            if (def.swappable) {
                const float ax = chip.x + chip.w - 13 * u_, ay = chip.y + chip.h / 2;
                shape::triangle(renderer_, {ax - 4 * u_, ay - 2 * u_}, {ax + 4 * u_, ay - 2 * u_}, {ax, ay + 3 * u_},
                                shape::fcolor(on ? kDim : kMuted));
            }
            if (canPick) {
                hit(chip, [this, id, chip](float, float) {
                    Project* q = currentProject();
                    if (!q) return;
                    selectPart(id);
                    std::vector<DropItem> items;
                    for (auto* s : kSources) {
                        std::string usedBy;
                        for (auto& other : kParts)
                            if (other.id != id && partOn(*q, other.id) && sourceOf(*q, other.id) == s) usedBy = other.label;
                        const bool exists = fs::exists(q->stemsDir() / (std::string(s) + ".wav"));
                        const std::string src2 = s;
                        items.push_back({std::string(s) + " stem", usedBy.empty() ? (exists ? "" : "missing") : "used by " + usedBy,
                                         stemColor(s), usedBy.empty() && exists, sourceOf(*q, id) == s,
                                         [this, id, src2] {
                                             if (Project* qq = currentProject()) setSource(*qq, id, src2);
                                         }});
                    }
                    openDropdown(chip, items);
                });
            } else if (id == "drums" || id == "vocals") {
                const std::string note = id == "drums" ? "fixed"
                                                       : (hasLyrics(p) ? std::to_string(p.id == lyricsFor_ ? lyricsLineCount() : 0) + " lines"
                                                                       : "add lyrics");
                const float nx = chip.x + chip.w + 10 * u_;
                text(nx, chip.y + (chip.h - 10 * u_) / 2, note, id == "vocals" && !hasLyrics(p) ? kSand : kMuted, 1.25f * u_,
                     card.x + card.w - nx - 8 * u_);
                if (id == "vocals" && !hasLyrics(p))
                    hit({nx, chip.y, card.x + card.w - nx, chip.h}, [this](float, float) { setTab(Tab::Lyrics); });
            }
            y += cardH + gap;
        }
        // Charting rules and what to hear.
        y += 6 * u_;
        text(r.x, y + 9 * u_, "Rules", kDim, 1.5f * u_);
        const std::string gid = p.doc.get("forge", "genre");
        std::string gname = "Auto";
        for (auto& [g, n] : genres())
            if (g == gid) gname = n;
        SDL_FRect gchip{r.x + 80 * u_, y, r.w - 80 * u_, 30 * u_};
        fill(gchip, hovered(gchip) ? kHover : kBar);
        frame(gchip, kMuted);
        text(gchip.x + 12 * u_, gchip.y + (gchip.h - 12 * u_) / 2, gname, kCream, 1.5f * u_, gchip.w - 36 * u_);
        shape::triangle(renderer_, {gchip.x + gchip.w - 18 * u_, gchip.y + 13 * u_}, {gchip.x + gchip.w - 10 * u_, gchip.y + 13 * u_},
                        {gchip.x + gchip.w - 14 * u_, gchip.y + 18 * u_}, shape::fcolor(kDim));
        hit(gchip, [this, gchip](float, float) {
            Project* q = currentProject();
            if (!q) return;
            std::vector<DropItem> items;
            for (auto& [g, n] : genres()) {
                const std::string gg = g;
                items.push_back({n, "", SDL_Color{0, 0, 0, 0}, true, q->doc.get("forge", "genre") == g, [this, gg] {
                                     if (Project* qq = currentProject()) {
                                         qq->doc.set("forge", "genre", gg);
                                         qq->dirty = true;
                                         saveProject(*qq);
                                     }
                                 }});
            }
            openDropdown(gchip, items);
        });
        y += 42 * u_;
        text(r.x, y + 9 * u_, "Listen", kDim, 1.5f * u_);
        segmented(r.x + 80 * u_, y, {"Full mix", "This part"}, listenPart_ ? 1 : 0, [this](int i) {
            listenPart_ = i == 1;
            applyListen();
        }, 30 * u_);
        y += 42 * u_;
        text(r.x, y + 9 * u_, "Drums", kDim, 1.5f * u_);
        const std::vector<std::string> styles = {"heard", "steady", "locked"};
        const int si = int(std::find(styles.begin(), styles.end(), drumStyle(p)) - styles.begin());
        segmented(r.x + 80 * u_, y, {"Heard", "Steady", "Locked"}, si, [this, styles](int i) {
            if (Project* q = currentProject()) {
                q->doc.set("forge", "drums_style", styles[size_t(i)]);
                q->dirty = true;
                saveProject(*q);
            }
        }, 30 * u_);
        y += 42 * u_;
        text(r.x, y + 9 * u_, "Riffs", kDim, 1.5f * u_);
        segmented(r.x + 80 * u_, y, {"Heard", "Steady"}, riffStyle(p) == "heard" ? 0 : 1, [this](int i) {
            if (Project* q = currentProject()) {
                q->doc.set("forge", "riffs_style", i == 0 ? "heard" : "steady");
                q->dirty = true;
                saveProject(*q);
            }
        }, 30 * u_);
        y += 46 * u_;
        // Where the charts stand.
        std::string st;
        SDL_Color sc = kDim;
        if (forging(p)) {
            st = "Forging: " + forgeStatus_;
            sc = kCream;
        } else if (p.doc.get("forge", "status") == "failed") {
            st = "Forging failed: " + p.doc.get("forge", "error");
            sc = kOxblood;
        } else if (preview_.loaded) {
            const std::string at = p.doc.get("forge", "forged_at");
            st = at.empty() ? "Charts ready" : "Forged " + at.substr(0, 10) + " " + at.substr(std::min<size_t>(11, at.size()), 5);
            if (p.doc.get("forge", "forged_with") != forgeSignature(p)) {
                st += "  -  settings changed, forge again";
                sc = kSand;
            }
        }
        for (auto& l : wrap(st, size_t(r.w / (8 * 1.25f * u_)))) {
            text(r.x, y, l, sc, 1.25f * u_);
            y += 16 * u_;
        }
    }

    void drawBoard(Project& p, const SDL_FRect& b, bool busy, bool hasStems) {
        fill(b, kPanel);
        const bool vocals = forgePart_ == "vocals";
        const std::string title = std::string(vocals ? "VOCALS" : "") +
                                  (vocals ? "" : text::ascii(forgePart_) + "  -  " + std::vector<std::string>{"EASY", "MEDIUM", "HARD", "EXPERT"}[size_t(forgeDiff_)]);
        std::string upper = title;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        fill({b.x, b.y, 4 * u_, 28 * u_}, stemColor(forgePart_));
        text(b.x + 14 * u_, b.y + 8 * u_, upper, kCream, 1.75f * u_);
        const float now = float(player_.playhead());
        const float overviewH = 34 * u_;
        SDL_FRect area{b.x, b.y + 36 * u_, b.w, b.h - 36 * u_ - overviewH - 14 * u_};
        boardRect_ = area;
        const std::vector<ChartNote>* notes = nullptr;
        if (preview_.loaded && !vocals) {
            auto it = preview_.parts.find(forgePart_);
            if (it != preview_.parts.end()) notes = &it->second[size_t(forgeDiff_)];
        }
        if (vocals) drawVocalBoard(area, now);
        else drawHighway(area, notes, forgePart_ == "drums", now);
        drawOverview({b.x + 12 * u_, b.y + b.h - overviewH - 8 * u_, b.w - 24 * u_, overviewH}, notes, vocals);

        // Empty and busy states, over the board.
        std::string a, c;
        std::function<void()> action;
        std::string actionLabel;
        if (busy) {
            a = "Forging the charts...";
            c = forgeStatus_;
        } else if (!hasStems) {
            a = separating(p) ? "Separating the stems..." : "No stems yet";
            c = "Forge charts the stems: separate the song first.";
            if (!separating(p)) {
                actionLabel = "Go to Stems";
                action = [this] { setTab(Tab::Stems); };
            }
        } else if (!preview_.loaded) {
            a = "No charts yet";
            c = "Choose what each part is made from, then forge.";
            actionLabel = "Forge charts";
            action = [this] { startForge(); };
        } else if (!vocals && !notes) {
            a = "This part was not charted";
            c = "Turn it on and forge again.";
        } else if (vocals && preview_.vocals.empty()) {
            a = "No vocal chart";
            c = hasLyrics(p) ? "Turn Vocals on and forge again." : "Vocals need the lyrics: add them in Lyrics.";
        }
        if (a.empty()) return;
        const float cw = std::min(area.w - 40 * u_, 520 * u_), ch = action ? 120 * u_ : 84 * u_;
        SDL_FRect card{area.x + (area.w - cw) / 2, area.y + area.h * 0.38f - ch / 2, cw, ch};
        fill({card.x + 4 * u_, card.y + 4 * u_, card.w, card.h}, kInk, 150);
        fill(card, kBar);
        fill({card.x, card.y, card.w, 3 * u_}, busy ? kSand : kOxblood);
        text(card.x + (cw - textW(a, 2 * u_)) / 2, card.y + 20 * u_, a, kCream, 2 * u_);
        if (busy) {  // a moving bar
            const float t = float(SDL_GetTicks() % 1400) / 1400.f;
            fill({card.x + 20 * u_, card.y + 46 * u_, cw - 40 * u_, 4 * u_}, kInk);
            fill({card.x + 20 * u_ + (cw - 40 * u_ - 80 * u_) * t, card.y + 46 * u_, 80 * u_, 4 * u_}, kSand);
            text(card.x + 20 * u_, card.y + 60 * u_, c, kDim, 1.25f * u_, cw - 40 * u_);
            return;
        }
        text(card.x + (cw - std::min(textW(c, 1.5f * u_), cw - 40 * u_)) / 2, card.y + 50 * u_, c, kDim, 1.5f * u_, cw - 40 * u_);
        if (action) {
            const float bw = textW(actionLabel, 1.5f * u_) + 28 * u_;
            button(card.x + (cw - bw) / 2, card.y + 76 * u_, actionLabel, action, true, true);
        }
    }

    static SDL_Color laneColor(int lane, bool drums) {
        static const SDL_Color frets[5] = {{0x3c, 0xb3, 0x4a, 255}, {0xd9, 0x3b, 0x3b, 255}, {0xe8, 0xc5, 0x2c, 255},
                                           {0x3b, 0x7d, 0xd9, 255}, {0xe8, 0x84, 0x2c, 255}};
        if (!drums) return frets[std::clamp(lane, 0, 4)];
        static const SDL_Color pads[5] = {{0xe8, 0x84, 0x2c, 255}, {0xd9, 0x3b, 0x3b, 255}, {0xe8, 0xc5, 0x2c, 255},
                                          {0x3b, 0x7d, 0xd9, 255}, {0x3c, 0xb3, 0x4a, 255}};
        return pads[std::clamp(lane, 0, 4)];
    }

    // The Guitar Hero-style highway, in perspective, scrolling with the playback.
    void drawHighway(const SDL_FRect& a, const std::vector<ChartNote>* notes, bool drums, float now) {
        const float L = 1.8f, D = 2.4f, back = 0.07f;  // seconds ahead, depth, seconds below the strike line
        const float bottomW = std::min(a.w * 0.8f, 600 * u_), cx = a.x + a.w / 2;
        const float strikeY = a.y + a.h - 46 * u_, horizonY = a.y + 6 * u_;
        const float span = strikeY - horizonY, farScale = 1.f / (1.f + D);
        auto at = [&](float dt, float& scale, float& y) {
            const float d = dt / L * D;
            scale = 1.f / (1.f + d);
            y = strikeY - span * (1.f - scale) / (1.f - farScale);
        };
        const int lanes = drums ? 4 : 5;
        const float laneW = bottomW / float(lanes);
        auto laneX = [&](int idx, float scale) { return cx + (float(idx) - float(lanes - 1) / 2.f) * laneW * scale; };

        // The neck.
        float s0, y0, s1, y1;
        at(-back, s0, y0);
        at(L, s1, y1);
        {
            SDL_FPoint q[4] = {{cx - bottomW / 2 * s1, y1}, {cx + bottomW / 2 * s1, y1}, {cx + bottomW / 2 * s0, y0}, {cx - bottomW / 2 * s0, y0}};
            SDL_FColor far = shape::fcolor({0x10, 0x0e, 0x12, 255}), near = shape::fcolor({0x26, 0x22, 0x29, 255});
            SDL_FColor cs[4] = {far, far, near, near};
            shape::quad(renderer_, q, cs);
        }
        // Lane separators and rails.
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
        // Beats and bars; sections at their bar.
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
        if (!preview_.beats.empty()) {
            for (auto& [t, bar] : preview_.beats) beatLine(t, bar);
            for (auto& [t, name] : preview_.sections) {
                const float dt = t - now;
                if (dt < 0 || dt > L * 0.85f) continue;
                float s, y;
                at(dt, s, y);
                const std::string label = name;
                const float tw = textW(label, 1.25f * u_);
                text(cx - bottomW / 2 * s - tw - 14 * u_, y - 5 * u_, label, kSand, 1.25f * u_);
            }
        } else if (Project* p = currentProject()) {  // no chart yet: beats from the detected tempo
            const float bpm = float(std::atof(p->doc.get("analysis", "bpm").c_str()));
            if (bpm > 30) {
                const float step = 60.f / bpm;
                for (float t = std::floor((now - back) / step) * step; t < now + L; t += step)
                    beatLine(t, int(std::lround(t / step)) % 4 == 0);
            }
        }

        // Notes: tails first, then gems far to near.
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
                const SDL_Color c = n.star ? SDL_Color{0x7f, 0xe8, 0xff, 255} : laneColor(n.lane, false);
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
                const SDL_Color c = n.star ? SDL_Color{0x7f, 0xe8, 0xff, 255} : laneColor(n.lane, drums);
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
        // The strike line: one button per lane, lit while its note passes.
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

    void drawGem(float x, float y, float rx, SDL_Color c, float alpha, bool hopo, bool cymbal) {
        const float ry = rx * 0.45f;
        auto darker = [](SDL_Color k) { return SDL_Color{Uint8(k.r * 0.55f), Uint8(k.g * 0.55f), Uint8(k.b * 0.55f), 255}; };
        shape::ellipse(renderer_, x, y + ry * 0.55f, rx * 1.04f, ry, shape::fcolor(kInk, 0.55f * alpha));
        shape::ellipse(renderer_, x, y + ry * 0.22f, rx, ry, shape::fcolor(darker(c), alpha));
        if (cymbal) {  // a flat disc with a bright rim
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
    void drawVocalBoard(const SDL_FRect& a, float now) {
        SDL_FRect lane{a.x + 10 * u_, a.y + a.h * 0.1f, a.w - 20 * u_, a.h * 0.55f};
        fill(lane, {0x12, 0x10, 0x14, 255});
        frame(lane, kMuted);
        const float past = 1.2f, future = 4.8f, nowX = lane.x + lane.w * past / (past + future);
        const float pps = lane.w / (past + future);
        int lo = 48, hi = 72;
        if (!preview_.vocals.empty()) {
            lo = 127;
            hi = 0;
            for (auto& v : preview_.vocals) {
                lo = std::min(lo, v.pitch);
                hi = std::max(hi, v.pitch);
            }
            lo -= 2;
            hi += 2;
        }
        auto pitchY = [&](int p) { return lane.y + lane.h * (1.f - float(p - lo) / float(std::max(1, hi - lo))); };
        for (auto& [t, bar] : preview_.beats) {
            const float x = nowX + (t - now) * pps;
            if (x < lane.x || x > lane.x + lane.w) continue;
            fill({x, lane.y, u_, lane.h}, bar ? kMuted : kBar);
        }
        for (auto& [t0, t1] : preview_.phrases) {
            const float x = nowX + (t1 - now) * pps;
            if (x >= lane.x && x <= lane.x + lane.w) fill({x, lane.y, 2 * u_, lane.h}, kDim, 120);
        }
        float lastLyricEnd = -1e9f;
        int activePitch = -1;
        for (auto& v : preview_.vocals) {
            const float x0 = nowX + (v.t - now) * pps, x1 = nowX + (v.t + v.dur - now) * pps;
            if (x1 < lane.x || x0 > lane.x + lane.w) continue;
            const bool active = now >= v.t && now <= v.t + v.dur;
            if (active) activePitch = v.pitch;
            const float y = pitchY(v.pitch), h = 10 * u_;
            const float cx0 = std::max(x0, lane.x), cx1 = std::min(x1, lane.x + lane.w);
            const SDL_Color c = active ? kCream : stemColor("vocals");
            fill({cx0, y - h / 2, std::max(2 * u_, cx1 - cx0), h}, c, x1 < nowX ? 110 : 255);
            const std::string lyric = v.lyric;
            const float tw = textW(lyric, 1.5f * u_);
            if (x0 >= lastLyricEnd + 6 * u_ && x0 >= lane.x && x0 + tw <= lane.x + lane.w) {
                text(x0, lane.y + lane.h + 14 * u_, lyric, active ? kCream : (x1 < nowX ? kMuted : kSand), 1.5f * u_);
                lastLyricEnd = x0 + tw;
            }
        }
        fill({nowX - u_, lane.y - 6 * u_, 2 * u_, lane.h + 12 * u_}, kCream);
        if (activePitch >= 0) shape::ellipse(renderer_, nowX, pitchY(activePitch), 8 * u_, 8 * u_, shape::fcolor(kCream));
    }

    // The whole song: where the notes are (density), the sections, the playhead. Click to seek.
    void drawOverview(const SDL_FRect& r, const std::vector<ChartNote>* notes, bool vocals) {
        fill(r, kInk);
        const float dur = preview_.duration > 0 ? preview_.duration : float(player_.duration());
        if (dur <= 0) return;
        const int cols = std::max(1, int(r.w / (3 * u_)));
        std::vector<int> bins(size_t(cols), 0);
        auto add = [&](float t) {
            int i = int(t / dur * float(cols));
            if (i >= 0 && i < cols) ++bins[size_t(i)];
        };
        if (vocals) for (auto& v : preview_.vocals) add(v.t);
        else if (notes) for (auto& n : *notes) add(n.t);
        const int peak = std::max(1, *std::max_element(bins.begin(), bins.end()));
        const SDL_Color c = stemColor(forgePart_);
        for (int i = 0; i < cols; ++i) {
            if (!bins[size_t(i)]) continue;
            const float h = (r.h - 6 * u_) * float(bins[size_t(i)]) / float(peak);
            fill({r.x + float(i) * 3 * u_, r.y + r.h - 3 * u_ - h, 2 * u_, h}, c, 170);
        }
        for (auto& [t, name] : preview_.sections) fill({r.x + t / dur * r.w, r.y, u_, 6 * u_}, kOxblood);
        const float px = r.x + float(player_.playhead()) / dur * r.w;
        fill({px - u_, r.y - 3 * u_, 2 * u_, r.h + 6 * u_}, kCream);
        frame(r, kBar);
        hit(r, [this, r, dur](float x, float) { player_.seek(double((x - r.x) / r.w * dur)); });
    }

    // ---- State ------------------------------------------------------------------------------------
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    float u_ = 1;  // display scale
    bool quit_ = false, help_ = false, about_ = false;
    mutable std::error_code ec_;

    YamlDoc settings_;
    fs::path settingsPath_, projectsDir_;
    std::string mode_ = "demucs", modeOverride_;
    bool sidebarCollapsed_ = false;

    std::vector<Project> projects_;
    std::string current_, newBanner_, status_;
    fs::path looseDir_;
    Tab tab_ = Tab::Info;
    int startTab_ = -1;

    Worker worker_;
    std::deque<Job> jobs_;
    std::optional<Job> active_;
    std::string jobError_;
    Uint64 busySince_ = 0;

    StemPlayer player_;
    std::string stemsFor_;  // project id (or "loose") whose stems are loaded
    SDL_Texture* coverTex_ = nullptr;

    int focus_ = -1;  // metadata field being edited

    ForgePreview preview_;
    std::string previewFor_, forgePart_ = "guitar", forgeStatus_;
    int forgeDiff_ = 3;  // Expert
    bool listenPart_ = false;
    SDL_FRect boardRect_{};
    bool dropOpen_ = false;
    SDL_FRect dropAnchor_{}, dropRect_{};
    std::vector<DropItem> dropItems_;
    std::vector<std::pair<std::string, std::string>> genres_;
    std::string lyrics_, lyricsFor_;
    bool lyricsDirty_ = false, lyricsFocus_ = false;
    size_t caret_ = 0;
    int lyricsScroll_ = 0;
    SDL_FRect lyricsRect_{};

    std::vector<Menu> menus_;
    int openMenu_ = -1;
    std::vector<std::pair<SDL_FRect, size_t>> itemRects_;
    std::vector<Hit> hits_;
    SDL_FRect sidebarRect_{};
    float sidebarScroll_ = 0, mouseX_ = -1, mouseY_ = -1;
    std::string hoverProject_, lastHover_;
    Uint64 hoverSince_ = 0;

    std::mutex dialogMutex_;
    std::string chosen_;
    DialogKind dialogKind_ = DialogKind::Song;
    std::string shotPath_;
    int frames_ = 0;
    bool waitJobs_ = false, hoverPinned_ = false;
    double seekTo_ = 0;
    bool forgeAtStart_ = false;
};

}  // namespace

int runApp(int argc, char** argv) {
    App app;
    if (!app.init(argc, argv)) return 1;
    app.run();
    app.shutdown();
    return 0;
}
