// gd-tool-02 core: settings, projects, jobs, input, the shell (menu bar, sidebar, step
// bar, status bar, dialogs) and the widgets every screen uses. See app.h.
#include "app.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include "fields.h"
#include "text_util.h"

#ifndef GDT02_ROOT
#define GDT02_ROOT "."
#endif

// ---- small helpers ----------------------------------------------------------------------

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
    s = std::max(0.0, s);
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
    width = std::max<size_t>(width, 4);
    while (a.size() > width) {
        size_t cut = a.rfind(' ', width);
        if (cut == std::string::npos || cut == 0) cut = width;
        lines.push_back(a.substr(0, cut));
        a = a.substr(cut == width ? cut : cut + 1);
    }
    lines.push_back(a);
    return lines;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string Project::title() const {
    std::string t = doc.get("song", "title");
    if (t.empty()) t = doc.get("project", "name");
    return t.empty() ? id : t;
}

fs::path Project::audioFile() const {
    std::string a = doc.get("project", "audio_file");
    return a.empty() ? fs::path() : dir / a;
}

namespace {

int countStems(const fs::path& dir) {
    int n = 0;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(dir, ec))
        if (e.path().extension() == ".wav" && e.path().stem() != "mix") ++n;
    return n;
}

// An image for the window: <name>.bmp next to it if there is one (every SDL3 reads BMP),
// else the PNG itself when this SDL can read PNG (3.4+).
SDL_Surface* loadImage(const fs::path& png) {
    fs::path bmp = png;
    bmp.replace_extension(".bmp");
    std::error_code ec;
    if (fs::exists(bmp, ec)) return SDL_LoadBMP(bmp.string().c_str());
#if SDL_VERSION_ATLEAST(3, 4, 0)
    if (fs::exists(png, ec)) return SDL_LoadPNG(png.string().c_str());
#endif
    return nullptr;
}

bool isAudio(const fs::path& p) {
    const std::string ext = lower(p.extension().string());
    return ext == ".mp3" || ext == ".wav" || ext == ".flac" || ext == ".ogg";
}

}  // namespace

// ---- lifecycle -------------------------------------------------------------------------------

bool App::init(int argc, char** argv) {
    std::vector<std::string> paths;
    std::string settingsOverride;
    for (int i = 1; i < argc; ++i) {  // options first, then songs/folders
        std::string a = argv[i];
        if (a == "--simple") modeOverride_ = "simple";
        else if (a == "--screenshot" && i + 1 < argc) shotPath_ = argv[++i];  // debug: save a frame, quit
        else if (a == "--menu" && i + 1 < argc) openMenu_ = std::atoi(argv[++i]);
        else if (a == "--screen" && i + 1 < argc) startScreen_ = std::atoi(argv[++i]);  // 0 home .. 3 export
        else if (a == "--hover" && i + 2 < argc) {
            mouseX_ = float(std::atof(argv[++i]));
            mouseY_ = float(std::atof(argv[++i]));
            hoverPinned_ = true;
        } else if (a == "--settings" && i + 1 < argc) settingsOverride = argv[++i];
        else if (a == "--wait-jobs") waitJobs_ = true;
        else if (a == "--seek" && i + 1 < argc) seekTo_ = std::atof(argv[++i]);
        else if (a == "--part" && i + 1 < argc) previewPart_ = tunePart_ = argv[++i];
        else if (a == "--forge") forgeAtStart_ = true;
        else if (a == "--selftest") selfTest_ = true;
        else if (a == "--paste" && i + 1 < argc) pasteAtStart_.push_back(argv[++i]);  // debug: paste into a box
        else if (a == "--one-part") bandView_ = false;  // Song forge: the single-part view
        else if (a == "--panels") showPanels_ = true;   // Song forge: band view with the mixer and settings  // test the editor on the first project, then quit
        else if (a == "--overlay" && i + 1 < argc) overlayAtStart_ = argv[++i];  // details / lyrics / help
        else paths.push_back(a);
    }
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    if (!SDL_CreateWindowAndRenderer("Dream Charter Engine", 1440, 900, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &window_, &renderer_)) {
        SDL_Log("SDL_CreateWindowAndRenderer failed: %s", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(window_, 1024, 680);
    SDL_SetRenderVSync(renderer_, 1);
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    player_.open();
    if (SDL_Surface* s = loadImage(toolRoot() / "assets" / "disc.png")) {
        discTex_ = SDL_CreateTextureFromSurface(renderer_, s);
        SDL_SetTextureScaleMode(discTex_, SDL_SCALEMODE_LINEAR);
        SDL_DestroySurface(s);
    }

    loadSettings(settingsOverride);
    refreshProjects();
    status_ = projectsDir_.empty() ? "Welcome. Choose where your projects are saved." : "Ready. Drop a song to start.";
    for (auto& p : paths) openPath(p);
    if (current_.empty() && !projects_.empty() && (!shotPath_.empty() || selfTest_ || !pasteAtStart_.empty())) selectProject(projects_[0].id);
    if (startScreen_ >= 0) setScreen(Screen(std::clamp(startScreen_, 0, 3)));
    if (seekTo_ > 0) {
        player_.seek(seekTo_);
        viewStart_ = float(std::max(0.0, seekTo_ - 1.0));
    }
    for (auto& box : pasteAtStart_)
        if (Project* p = currentProject()) {
            pasteSheet(*p, box);
            std::printf("paste %s: %s\n", box.c_str(), status_.c_str());
        }
    if (forgeAtStart_) forgeSong();
    if (overlayAtStart_ == "details") overlay_ = Overlay::Details;
    else if (overlayAtStart_ == "lyrics") overlay_ = Overlay::Lyrics;
    else if (overlayAtStart_ == "help") overlay_ = Overlay::Help;
    return true;
}

void App::run() {
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
    saveChart();
}

void App::shutdown() {
    worker_.stop();
    player_.close();
    if (coverTex_) SDL_DestroyTexture(coverTex_);
    if (discTex_) SDL_DestroyTexture(discTex_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

// gd-tool-02.log next to settings.yaml: what the app did and when.
void App::logEvent(const std::string& msg) {
    static std::ofstream out;
    if (!out.is_open()) {
        fs::path dir = settingsPath_.empty() ? fs::path(".") : settingsPath_.parent_path();
        out.open(dir / "gd-tool-02.log", std::ios::app);
    }
    out << nowIso() << "  " << msg << std::endl;
}

fs::path App::toolRoot() { return fs::path(GDT02_ROOT); }

// ---- settings (settings.yaml in the user's app-data folder) ------------------------------

void App::loadSettings(const std::string& overridePath) {
    if (!overridePath.empty()) {
        settingsPath_ = overridePath;
    } else {
        char* pref = SDL_GetPrefPath("mbracali", "gd-tool-02");
        settingsPath_ = fs::path(pref ? pref : ".") / "settings.yaml";
        SDL_free(pref);
    }
    settings_.load(settingsPath_);
    if (settings_.header.empty()) settings_.header = {"# gd-tool-02 settings (Settings menu)."};
    auto dirOf = [&](const char* key) {
        std::string d = settings_.get("settings", key);
        fs::path p = d.empty() ? fs::path() : fs::path(d);
        return !p.empty() && fs::is_directory(p) ? p : fs::path();
    };
    projectsDir_ = dirOf("projects_dir");
    cloneHeroDir_ = dirOf("clonehero_dir");
    yargDir_ = dirOf("yarg_dir");
    mode_ = settings_.get("settings", "separation_mode") == "simple" ? "simple" : "demucs";
    if (!modeOverride_.empty()) mode_ = modeOverride_;
    sidebarCollapsed_ = settings_.get("settings", "sidebar_collapsed") == "true";
}

void App::saveSettings() {
    settings_.set("settings", "projects_dir", projectsDir_.string());
    settings_.set("settings", "clonehero_dir", cloneHeroDir_.string());
    settings_.set("settings", "yarg_dir", yargDir_.string());
    settings_.set("settings", "separation_mode", mode_);
    settings_.set("settings", "sidebar_collapsed", sidebarCollapsed_ ? "true" : "false");
    if (!settings_.save(settingsPath_)) status_ = "Could not save the settings to " + settingsPath_.string();
}

void App::setProjectsDir(const fs::path& dir) {
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

fs::path App::defaultProjectsDir() const {
    const char* docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    return fs::path(docs ? docs : ".") / "GD Tool 02 Projects";
}

// Where an export goes: the folder set in Settings, else the project's export/ folder.
fs::path App::exportDir(const std::string& format) const {
    const fs::path& set = format == "yarg" ? yargDir_ : cloneHeroDir_;
    if (!set.empty()) return set;
    for (auto& p : projects_)
        if (p.id == current_) return p.dir / "export" / format;
    return {};
}

void App::toggleSidebar() {
    sidebarCollapsed_ = !sidebarCollapsed_;
    saveSettings();
}

// ---- projects ----------------------------------------------------------------------------------

Project* App::project(const std::string& id) {
    for (auto& p : projects_)
        if (p.id == id) return &p;
    return nullptr;
}

Project* App::projectAt(const fs::path& dir) {
    for (auto& p : projects_)
        if (fs::equivalent(p.dir, dir, ec_)) return &p;
    return nullptr;
}

void App::refreshProjects() {
    saveAll();
    projects_.clear();
    if (projectsDir_.empty()) return;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(projectsDir_, ec)) {
        if (!e.is_directory()) continue;
        Project p;
        p.id = e.path().filename().string();
        p.dir = e.path();
        if (!p.doc.load(p.dir / "project.yaml")) continue;  // not a project
        p.stemCount = countStems(p.stemsDir());
        projects_.push_back(std::move(p));
    }
    std::sort(projects_.begin(), projects_.end(), [](const Project& a, const Project& b) {
        return a.doc.get("project", "created") > b.doc.get("project", "created");
    });
    if (!current_.empty() && !project(current_)) closeProject();
}

void App::saveProject(Project& p) {
    if (!p.dirty) return;
    if (p.doc.save(p.dir / "project.yaml")) p.dirty = false;
    else status_ = "Could not save " + (p.dir / "project.yaml").string();
}

void App::saveAll() {
    for (auto& p : projects_) saveProject(p);
    saveLyrics();
}

void App::setValue(Project& p, const std::string& section, const std::string& key, const std::string& value) {
    if (p.doc.get(section, key) == value) return;
    p.doc.set(section, key, value);
    p.dirty = true;
    saveProject(p);
}

bool App::activeFor(const Project& p, JobKind kind) const {
    return active_ && active_->kind == kind && fs::equivalent(active_->dir, p.dir, ec_);
}

bool App::jobFor(const Project& p, JobKind kind) const {
    if (activeFor(p, kind)) return true;
    return std::any_of(jobs_.begin(), jobs_.end(),
                       [&](const Job& j) { return j.kind == kind && fs::equivalent(j.dir, p.dir, ec_); });
}

bool App::busy(const Project& p) const {
    return jobFor(p, JobKind::Metadata) || jobFor(p, JobKind::Forge) || jobFor(p, JobKind::Export);
}

std::string App::projectState(const Project& p) const {
    if (active_ && fs::equivalent(active_->dir, p.dir, ec_)) {
        const char* what = active_->kind == JobKind::Metadata ? "reading the song" : active_->kind == JobKind::Forge ? "forging" : "exporting";
        return std::string(what) + (jobProgress_ >= 0 ? " " + std::to_string(int(jobProgress_ * 100)) + "%" : "...");
    }
    if (busy(p)) return "queued";
    if (p.doc.get("forge", "status") == "failed") return "forge failed";
    if (!p.doc.get("export", "exported_at").empty()) return "exported";
    if (p.doc.get("forge", "tuned") == "yes") return "fine tuned";
    if (p.forged()) return "forged";
    return "new: add sheets, then forge";
}

// A song becomes a new project: a folder with a copy of the song and a project.yaml;
// its metadata is read while the disc spins.
void App::newProject(const fs::path& song) {
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
    fs::create_directories(dir / "sheets", ec);
    const std::string ext = lower(source.extension().string());
    fs::copy_file(source, dir / ("song" + ext), fs::copy_options::overwrite_existing, ec);
    if (ec) {
        status_ = "Could not copy the song into the project: " + ec.message();
        return;
    }
    YamlDoc doc;
    doc.header = {"# Dream Charter Engine project file. Edit it in the app (Song home > Details) or by hand,",
                  "# keeping the layout: a section, then indented `key: value` lines."};
    for (auto& [section, keys] : kSchema)
        for (auto& k : keys) doc.ensure(section, k);
    doc.set("project", "created", nowIso());
    doc.set("project", "source_file", source.string());
    doc.set("project", "audio_file", "song" + ext);
    doc.save(dir / "project.yaml");

    refreshProjects();
    jobs_.push_back({dir, JobKind::Metadata});
    selectProject(id);
    setScreen(Screen::Home);
    status_ = "New project " + id + ": reading the song...";
    logEvent("new project " + id + " from " + source.string());
}

void App::openPath(const std::string& input, float dropX, float dropY) {
    fs::path p(input);
    Project* cur = currentProject();
    // A sheet dropped on a Song home box.
    if (cur && screen_ == Screen::Home && overlay_ == Overlay::None && dropX >= 0) {
        const int box = boxAt(dropX, dropY);
        if (box >= 0 && !fs::is_directory(p)) {
            addSheet(*cur, kBoxes[box].id, p);
            return;
        }
    }
    if (fs::is_directory(p)) {
        p = fs::absolute(p).lexically_normal();
        if (!p.has_filename()) p = p.parent_path();
        if (Project* existing = projectAt(p)) selectProject(existing->id);
        else status_ = p.string() + " is not a project.";
        return;
    }
    if (isAudio(p)) {
        newProject(p);
        return;
    }
    if (cur) {
        // Not audio and not on a box: the only box that takes it, if there is one.
        const std::string kind = sheetKind(p, "guitar");
        if (!kind.empty() || !sheetKind(p, "lyrics").empty())
            status_ = "Drop " + p.filename().string() + " on the Guitar, Bass, Drums or Lyrics box of Song home.";
        else
            status_ = "Not a song or a sheet: " + p.filename().string();
        if (screen_ != Screen::Home) setScreen(Screen::Home);
        return;
    }
    status_ = "Not an audio file: " + p.filename().string();
}

// ---- jobs (one Python helper at a time) ----------------------------------------------------

void App::startNextJob() {
    if (worker_.running() || active_ || jobs_.empty()) return;
    Job job = jobs_.front();
    jobs_.pop_front();
    Project* p = projectAt(job.dir);
    if (!p || p->audioFile().empty()) return;
#ifdef _WIN32
    const fs::path python = toolRoot() / ".venv" / "Scripts" / "python.exe";
#else
    const fs::path python = toolRoot() / ".venv" / "bin" / "python";
#endif
    std::vector<std::string> argv{python.string(), "-u"};
    if (job.kind == JobKind::Metadata) {
        argv.insert(argv.end(), {(toolRoot() / "python" / "extract_metadata.py").string(), p->audioFile().string(), p->dir.string()});
    } else if (job.kind == JobKind::Forge) {
        if (p->id == chartFor_) saveChart();
        argv.insert(argv.end(), {(toolRoot() / "python" / "forge.py").string(), p->dir.string(), "--mode", mode_});
        if (p->id == stemsFor_ && countStems(p->stemsDir()) == 0) player_.clear();
    } else {
        argv.insert(argv.end(), {(toolRoot() / "python" / "export_song.py").string(), p->dir.string()});
    }
    argv.insert(argv.end(), job.args.begin(), job.args.end());
    std::string error;
    if (!worker_.start(argv, error)) {
        status_ = "Could not start Python: " + error;
        return;
    }
    active_ = job;
    jobError_.clear();
    jobStatus_ = "Starting...";
    jobProgress_ = -1;
    busySince_ = SDL_GetTicks();
    if (job.kind == JobKind::Export) exported_.clear();
    logEvent(std::string("job start ") + (job.kind == JobKind::Metadata ? "metadata" : job.kind == JobKind::Forge ? "forge" : "export") +
             ": " + p->id);
}

void App::pumpWorker() {
    std::string line;
    while (worker_.popLine(line)) {
        if (!active_) continue;
        Project* p = projectAt(active_->dir);
        const std::string name = p ? p->title() : active_->dir.filename().string();
        if (line.rfind("STATUS ", 0) == 0) {
            jobStatus_ = line.substr(7);
            status_ = name + ": " + jobStatus_;
        } else if (line.rfind("PROGRESS ", 0) == 0) {
            jobProgress_ = std::clamp(float(std::atof(line.c_str() + 9)), 0.f, 1.f);
        } else if (line.rfind("STEM ", 0) == 0) {
            auto sp = line.find(' ', 5);
            if (sp != std::string::npos && p && p->id == stemsFor_) {
                const std::string stem = line.substr(5, sp - 5);
                std::string error;
                if (!player_.has(stem) && !player_.add(stem, line.substr(sp + 1), error)) logEvent("stem " + stem + " FAILED: " + error);
            }
        } else if (line.rfind("STEMS_DONE", 0) == 0 && p) {
            p->doc.set("stems", "status", "done");
            p->doc.set("stems", "mode", mode_);
            p->doc.set("stems", "model", mode_ == "demucs" ? "htdemucs_6s" : "filters");
            p->doc.set("stems", "separated_at", nowIso());
            p->dirty = true;
            saveProject(*p);
            p->stemCount = countStems(p->stemsDir());
        } else if (line.rfind("EXPORTED ", 0) == 0) {
            exported_.push_back(line.substr(9));
        } else if (line.rfind("ERROR ", 0) == 0) {
            jobError_ = line.substr(6);
        } else if (line.rfind("EXIT ", 0) == 0) {
            const bool ok = line == "EXIT 0" && jobError_.empty();
            if (!ok && jobError_.empty()) jobError_ = "Python exited with code " + line.substr(5);
            Job job = *active_;
            active_.reset();
            finishJob(job, ok);
        }
    }
}

void App::finishJob(const Job& job, bool ok) {
    Project* p = projectAt(job.dir);
    logEvent(std::string("job end ") + (ok ? "ok" : "FAILED " + jobError_) + ": " + job.dir.filename().string());
    if (!p) return;
    const std::string secs = std::to_string((SDL_GetTicks() - busySince_) / 1000) + "s";
    if (job.kind == JobKind::Metadata) {
        p->doc.load(p->dir / "project.yaml");  // written by Python; editing was off meanwhile
        p->dirty = false;
        if (p->id == current_) {
            loadCover(*p);
            if (!lyricsEd_.dirty) loadLyrics(*p);
        }
        status_ = ok ? p->title() + ": ready. Add the sheets you have, then FORGE SONG." : p->title() + ": reading failed - " + jobError_;
    } else if (job.kind == JobKind::Forge) {
        p->doc.set("forge", "status", ok ? "done" : "failed");
        p->doc.set("forge", "error", ok ? "" : jobError_);
        if (ok) {
            p->doc.set("forge", "forged_at", nowIso());
            p->doc.set("forge", "forged_with", job.label);
            p->doc.set("forge", "tuned", "no");
        }
        p->dirty = true;
        saveProject(*p);
        p->stemCount = countStems(p->stemsDir());
        if (p->id == current_) {
            chartFor_.clear();
            undo_.clear();
            redo_.clear();
            loadChart(*p);
            if (stemsFor_ != p->id || player_.empty()) loadStems(*p);
        }
        status_ = ok ? p->title() + ": forged in " + secs + ". Preview it, then Fine tuning." : p->title() + ": forging failed - " + jobError_;
    } else {
        if (ok) {
            p->doc.set("export", "exported_at", nowIso());
            p->doc.set("export", "last_folder", exported_.empty() ? "" : exported_.back());
            p->dirty = true;
            saveProject(*p);
        }
        status_ = ok ? p->title() + ": exported " + std::to_string(exported_.size()) + " song folder(s) in " + secs + "."
                     : p->title() + ": export failed - " + jobError_;
    }
    jobProgress_ = -1;
}

// ---- the open project ---------------------------------------------------------------------

void App::closeProject() {
    saveAll();
    saveChart();
    unfocus();
    player_.clear();
    stemsFor_.clear();
    current_.clear();
    lyricsFor_.clear();
    chartFor_.clear();
    chart_.clear();
    undo_.clear();
    redo_.clear();
    sel_ = -1;
    dropOpen_ = false;
    overlay_ = Overlay::None;
    exported_.clear();
    if (coverTex_) SDL_DestroyTexture(coverTex_);
    coverTex_ = nullptr;
}

void App::selectProject(const std::string& id) {
    if (id == current_) return;
    Project* p = project(id);
    if (!p) return;
    closeProject();
    current_ = id;
    screen_ = Screen::Home;
    viewStart_ = 0;
    loadCover(*p);
    loadLyrics(*p);
    if (p->forged()) loadChart(*p);
}

bool App::screenOpen(const Project& p, Screen s) const {
    if (s == Screen::Tune || s == Screen::Export) return p.forged();
    return true;
}

void App::setScreen(Screen s) {
    Project* p = currentProject();
    if (!p) return;
    if (!screenOpen(*p, s)) {
        status_ = "Forge the song first: " + std::string(s == Screen::Tune ? "Fine tuning" : "Export song") + " needs its charts.";
        return;
    }
    if (screen_ == Screen::Tune && s != Screen::Tune) saveChart();
    saveLyrics();
    unfocus();
    dropOpen_ = false;
    screen_ = s;
    logEvent("screen " + std::to_string(int(s)) + ": " + p->id);
    if (s == Screen::Forge || s == Screen::Tune) {
        if (stemsFor_ != p->id) loadStems(*p);
        if (p->forged() && chartFor_ != p->id) loadChart(*p);
    }
    if (s == Screen::Tune) {
        sel_ = -1;
        auto parts = chartParts();
        if (std::find(parts.begin(), parts.end(), tunePart_) == parts.end() && !parts.empty()) tunePart_ = parts[0];
    }
    if (s != Screen::Forge && listenPart_) player_.setAllMuted(false, true);
}

void App::loadStems(const Project& p) {
    player_.clear();
    stemsFor_ = p.id;
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(p.stemsDir(), ec))
        if (e.path().extension() == ".wav") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        std::string error;
        if (!player_.add(f.stem().string(), f.string(), error)) logEvent("stem " + f.string() + " FAILED: " + error);
    }
}

void App::loadCover(const Project& p) {
    if (coverTex_) SDL_DestroyTexture(coverTex_);
    coverTex_ = nullptr;
    if (!fs::exists(p.dir / "cover.png")) return;
    if (SDL_Surface* s = loadImage(p.dir / "cover.png")) {
        coverTex_ = SDL_CreateTextureFromSurface(renderer_, s);
        SDL_DestroySurface(s);
    }
}

void App::loadChart(const Project& p) {
    if (chartFor_ == p.id) return;
    chartFor_ = p.id;
    chart_.load(p.chartFile());
    chartDirty_ = false;
    sel_ = -1;
}

void App::saveChart() {
    if (!chartDirty_ || chartFor_.empty()) return;
    Project* p = project(chartFor_);
    if (!p) return;
    if (chart_.save(p->chartFile())) {
        chartDirty_ = false;
        setValue(*p, "forge", "tuned", "yes");
    } else {
        status_ = "Could not save " + p->chartFile().string();
    }
}

// ---- lyrics (lyrics.txt) ------------------------------------------------------------------

void App::loadLyrics(const Project& p) {
    lyricsFor_ = p.id;
    std::ifstream in(p.dir / "lyrics.txt", std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    lyricsEd_.set(ss.str());
}

void App::saveLyrics() {
    if (!lyricsEd_.dirty || lyricsFor_.empty()) return;
    Project* p = project(lyricsFor_);
    if (!p) return;
    std::ofstream out(p->dir / "lyrics.txt", std::ios::binary);
    out << lyricsEd_.text;
    lyricsEd_.dirty = !out;
    status_ = out ? p->title() + ": lyrics saved." : "Could not save the lyrics.";
}

// ---- text focus ------------------------------------------------------------------------------

void App::focusField(int i) {
    Project* p = currentProject();
    if (!p || jobFor(*p, JobKind::Metadata)) return;
    focus_ = Focus::Field;
    field_ = i;
    SDL_StartTextInput(window_);
}

void App::focusEditor(TextEdit& ed) {
    editor_ = &ed;
    focus_ = Focus::Editor;
    SDL_StartTextInput(window_);
}

void App::focusVocalLyric() {
    focus_ = Focus::VocalLyric;
    SDL_StartTextInput(window_);
}

void App::unfocus() {
    if (focus_ != Focus::None) SDL_StopTextInput(window_);
    focus_ = Focus::None;
    field_ = -1;
}

void App::focusNext(int dir) {
    for (int k = 1; k <= fieldCount(); ++k) {
        int i = (field_ + dir * k + fieldCount() * 2) % fieldCount();
        if (fieldAt(i).editable) {
            field_ = i;
            return;
        }
    }
}

void App::editField(const std::function<void(std::string&)>& change) {
    Project* p = currentProject();
    if (!p || field_ < 0) return;
    const FieldSpec& f = fieldAt(field_);
    std::string v = p->doc.get(f.section, f.key);
    change(v);
    p->doc.set(f.section, f.key, v);
    p->dirty = true;
}

// ---- dialogs (callbacks may come from another thread) ---------------------------------------

void SDLCALL App::onDialog(void* user, const char* const* files, int) {
    auto* self = static_cast<App*>(user);
    if (files && files[0]) {
        std::lock_guard<std::mutex> lock(self->dialogMutex_);
        self->chosen_ = files[0];
    }
}

void App::showDialog(DialogKind kind, const std::string& box) {
    dialogKind_ = kind;
    dialogBox_ = box;
    if (kind == DialogKind::Song) {
        static const SDL_DialogFileFilter filters[] = {{"Audio", "mp3;flac;wav;ogg"}};
        SDL_ShowOpenFileDialog(onDialog, this, window_, filters, 1, nullptr, false);
    } else if (kind == DialogKind::Sheet) {
        static const SDL_DialogFileFilter sheets[] = {{"Sheets", "gp3;gp4;gp5;gpx;gp;gp6;gp7;gp8;mid;midi;kar;chart;txt"}};
        static const SDL_DialogFileFilter lyrics[] = {{"Lyrics", "txt;lrc"}};
        SDL_ShowOpenFileDialog(onDialog, this, window_, box == "lyrics" ? lyrics : sheets, 1, nullptr, false);
    } else {
        const fs::path& start = kind == DialogKind::ProjectsDir ? projectsDir_ : kind == DialogKind::CloneHeroDir ? cloneHeroDir_ : yargDir_;
        SDL_ShowOpenFolderDialog(onDialog, this, window_, start.empty() ? nullptr : start.string().c_str(), false);
    }
}

void App::takeDialogResult() {
    std::string file;
    {
        std::lock_guard<std::mutex> lock(dialogMutex_);
        file.swap(chosen_);
    }
    if (file.empty()) return;
    switch (dialogKind_) {
        case DialogKind::ProjectsDir: setProjectsDir(file); break;
        case DialogKind::CloneHeroDir:
            cloneHeroDir_ = file;
            saveSettings();
            status_ = "Clone Hero songs folder: " + file;
            break;
        case DialogKind::YargDir:
            yargDir_ = file;
            saveSettings();
            status_ = "YARG songs folder: " + file;
            break;
        case DialogKind::Sheet:
            if (Project* p = currentProject()) addSheet(*p, dialogBox_, file);
            break;
        case DialogKind::Song: openPath(file); break;
    }
}

void App::reveal(const fs::path& dir) { SDL_OpenURL(("file://" + dir.string()).c_str()); }

// ---- menus ------------------------------------------------------------------------------------

namespace {
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
}  // namespace

void App::buildMenus() {
    Project* p = currentProject();
    const bool hasDir = !projectsDir_.empty();
    menus_.clear();
    menus_.push_back({"Settings",
                      {item("Open Song...", kCmd + "O", [this] { showDialog(DialogKind::Song); }, hasDir),
                       item("Show Project Folder", "", [this] { if (auto* q = currentProject()) reveal(q->dir); }, p != nullptr),
                       item("Refresh Projects", "F5", [this] { refreshProjects(); }, hasDir),
                       separator(),
                       item("Projects Folder...", "", [this] { showDialog(DialogKind::ProjectsDir); }),
                       item("Show Projects Folder", "", [this] { reveal(projectsDir_); }, hasDir),
                       item("Clone Hero Songs Folder...", "", [this] { showDialog(DialogKind::CloneHeroDir); }),
                       item("YARG Songs Folder...", "", [this] { showDialog(DialogKind::YargDir); }),
                       separator(),
                       item("Stems: Demucs 6 stems (AI)", "", [this] { mode_ = "demucs"; saveSettings(); }, true, mode_ == "demucs"),
                       item("Stems: simple filters (no AI, fast)", "", [this] { mode_ = "simple"; saveSettings(); }, true, mode_ == "simple"),
                       separator(),
                       item(sidebarCollapsed_ ? "Expand Sidebar" : "Collapse Sidebar", kCmd + "B", [this] { toggleSidebar(); }),
                       separator(),
                       item("Quit", kCmd + "Q", [this] { quit_ = true; })},
                      {}});
    menus_.push_back({"Help",
                      {item("Keyboard Shortcuts", "F1", [this] { overlay_ = Overlay::Help; }),
                       item("About Dream Charter Engine", "", [this] { overlay_ = Overlay::About; })},
                      {}});
}

// ---- input --------------------------------------------------------------------------------------

void App::handle(SDL_Event& e) {
    SDL_ConvertEventToRenderCoordinates(renderer_, &e);  // window points -> pixels
    switch (e.type) {
        case SDL_EVENT_QUIT: quit_ = true; break;
        case SDL_EVENT_DROP_FILE: openPath(e.drop.data, e.drop.x, e.drop.y); break;
        case SDL_EVENT_KEY_DOWN: onKey(e.key.key, e.key.mod); break;
        case SDL_EVENT_TEXT_INPUT:
            if (focus_ == Focus::Field) editField([&](std::string& v) { v += e.text.text; });
            else if (focus_ == Focus::Editor && editor_) editor_->insert(e.text.text);
            else if (focus_ == Focus::VocalLyric && tuneVocals() && sel_ >= 0 && sel_ < int(chart_.vocals.size())) {
                if (!dragPushed_) pushUndo();
                dragPushed_ = true;  // one undo step per typed word
                chart_.vocals[size_t(sel_)].lyric += e.text.text;
                chartChanged();
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (hoverPinned_) break;
            mouseX_ = e.motion.x;
            mouseY_ = e.motion.y;
            if (openMenu_ >= 0)
                for (size_t i = 0; i < menus_.size(); ++i)
                    if (inside(menus_[i].titleRect, mouseX_, mouseY_)) openMenu_ = int(i);
            if (drag_ != Drag::None) tuneMouseMove(mouseX_, mouseY_);
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            const bool zoom = SDL_GetModState() & (SDL_KMOD_GUI | SDL_KMOD_CTRL);
            if ((overlay_ == Overlay::Lyrics || overlay_ == Overlay::Paste) && inside(lyricsRect_, mouseX_, mouseY_)) {
                TextEdit& ed = overlay_ == Overlay::Paste ? pasteEd_ : lyricsEd_;
                ed.scroll = std::max(0, ed.scroll - int(e.wheel.y * 3));
            }
            else if (overlay_ != Overlay::None) break;
            else if (inside(sidebarRect_, mouseX_, mouseY_)) sidebarScroll_ = std::max(0.f, sidebarScroll_ - e.wheel.y * 40);
            else if (screen_ == Screen::Tune && (inside(rollRect_, mouseX_, mouseY_) || inside(rulerRect_, mouseX_, mouseY_)))
                tuneWheel(e.wheel.x, e.wheel.y, zoom);
            else if ((screen_ == Screen::Forge || screen_ == Screen::Tune) && inside(boardRect_, mouseX_, mouseY_))
                player_.seek(player_.playhead() + double(e.wheel.y) * 0.25);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            const bool shift = SDL_GetModState() & SDL_KMOD_SHIFT;
            const bool free = overlay_ == Overlay::None && openMenu_ < 0 && !dropOpen_;
            if (free && screen_ == Screen::Tune && (inside(rollRect_, e.button.x, e.button.y) || inside(rulerRect_, e.button.x, e.button.y))) {
                tuneMouseDown(e.button.x, e.button.y, e.button.button, shift);
                break;
            }
            if (e.button.button == SDL_BUTTON_LEFT) onClick(e.button.x, e.button.y);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (drag_ != Drag::None) tuneMouseUp();
            break;
        default: break;
    }
}

void App::onKey(SDL_Keycode key, SDL_Keymod mod) {
    const bool cmd = mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL), shift = mod & SDL_KMOD_SHIFT;
    if (overlay_ == Overlay::Help || overlay_ == Overlay::About) {
        overlay_ = Overlay::None;
        return;
    }
    if (cmd) {
        if (key == SDLK_Q) quit_ = true;
        else if (key == SDLK_O && !projectsDir_.empty()) showDialog(DialogKind::Song);
        else if (key == SDLK_B) toggleSidebar();
        else if (key == SDLK_S) {
            if (Project* p = currentProject()) saveProject(*p);
            saveLyrics();
            saveChart();
        } else if (key >= SDLK_1 && key <= SDLK_4 && currentProject() && overlay_ == Overlay::None) setScreen(Screen(key - SDLK_1));
        else if (key == SDLK_V && focus_ == Focus::Field) {
            if (char* clip = SDL_GetClipboardText()) {
                std::string s = clip;
                SDL_free(clip);
                std::replace(s.begin(), s.end(), '\n', ' ');
                s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
                editField([&](std::string& v) { v += s; });
            }
        } else if (key == SDLK_V && focus_ == Focus::None && overlay_ == Overlay::None && screen_ == Screen::Home) {
            // Cmd/Ctrl+V over an instrument box pastes into it (over Lyrics: the lyrics editor).
            const int b = boxAt(mouseX_, mouseY_);
            if (Project* p = currentProject(); p && b >= 0 && b < 3) pasteSheet(*p, kBoxes[b].id);
            else if (b == 3) {
                overlay_ = Overlay::Lyrics;
                focusEditor(lyricsEd_);
                lyricsEd_.caret = lyricsEd_.text.size();
                if (char* clip = SDL_GetClipboardText()) {
                    lyricsEd_.insert(clip);
                    SDL_free(clip);
                }
            }
        } else if (key == SDLK_V && focus_ == Focus::Editor && editor_) {
            if (char* clip = SDL_GetClipboardText()) {
                editor_->insert(clip);
                SDL_free(clip);
            }
        } else if (key == SDLK_A && focus_ == Focus::Editor && editor_) {
            editor_->caret = editor_->text.size();  // select-all is not supported: Cmd+A goes to the end
        } else if (screen_ == Screen::Tune && overlay_ == Overlay::None) {
            tuneKey(key, mod);
        }
        return;
    }
    if (focus_ == Focus::Field) {
        if (key == SDLK_BACKSPACE) editField([](std::string& v) { v.erase(text::prevStart(v, v.size())); });
        else if (key == SDLK_TAB) focusNext(shift ? -1 : 1);
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) focusNext(1);
        else if (key == SDLK_ESCAPE) unfocus();
        return;
    }
    if (focus_ == Focus::Editor && editor_) {
        TextEdit& ed = *editor_;
        auto [line, col] = ed.pos();
        if (key == SDLK_BACKSPACE) ed.backspace();
        else if (key == SDLK_DELETE) ed.del();
        else if (key == SDLK_RETURN || key == SDLK_KP_ENTER) ed.insert("\n");
        else if (key == SDLK_TAB) ed.insert("  ");
        else if (key == SDLK_LEFT) ed.left();
        else if (key == SDLK_RIGHT) ed.right();
        else if (key == SDLK_UP) ed.moveTo(line - 1, col);
        else if (key == SDLK_DOWN) ed.moveTo(line + 1, col);
        else if (key == SDLK_PAGEUP) ed.moveTo(line - 20, col);
        else if (key == SDLK_PAGEDOWN) ed.moveTo(line + 20, col);
        else if (key == SDLK_HOME) ed.moveTo(line, 0);
        else if (key == SDLK_END) ed.moveTo(line, 1 << 20);
        else if (key == SDLK_ESCAPE) unfocus();
        return;
    }
    if (focus_ == Focus::VocalLyric) {
        if (key == SDLK_BACKSPACE && sel_ >= 0 && sel_ < int(chart_.vocals.size())) {
            std::string& l = chart_.vocals[size_t(sel_)].lyric;
            if (!l.empty()) {
                if (!dragPushed_) pushUndo();
                dragPushed_ = true;
                l.erase(text::prevStart(l, l.size()));
                chartChanged();
            }
        } else if (key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_ESCAPE || key == SDLK_TAB) {
            unfocus();
            dragPushed_ = false;
            if (key == SDLK_TAB && sel_ >= 0 && sel_ + 1 < int(chart_.vocals.size())) {  // next syllable
                selectNote(sel_ + 1);
                focusVocalLyric();
            }
        }
        return;
    }
    if (overlay_ == Overlay::Details || overlay_ == Overlay::Lyrics || overlay_ == Overlay::Paste) {
        if (key == SDLK_ESCAPE) {
            if (Project* p = currentProject()) saveProject(*p);
            saveLyrics();
            overlay_ = Overlay::None;
        }
        return;
    }
    if (key == SDLK_ESCAPE) {
        openMenu_ = -1;
        dropOpen_ = false;
        if (screen_ == Screen::Tune) sel_ = -1;
        return;
    }
    if (screen_ == Screen::Tune && tuneKey(key, mod)) return;
    const bool inForge = currentProject() && screen_ == Screen::Forge;
    if (inForge && key >= SDLK_1 && key <= SDLK_4) previewDiff_ = int(key - SDLK_1);
    else if (key == SDLK_F1) overlay_ = Overlay::Help;
    else if (key == SDLK_F5) refreshProjects();
    else if (key == SDLK_SPACE) player_.togglePlay();
    else if (key == SDLK_HOME) player_.seek(0);
    else if (key == SDLK_LEFT) player_.seek(player_.playhead() - 5);
    else if (key == SDLK_RIGHT) player_.seek(player_.playhead() + 5);
    else if ((key == SDLK_UP || key == SDLK_DOWN) && !projects_.empty() && screen_ == Screen::Home) {
        int i = key == SDLK_DOWN ? 0 : int(projects_.size()) - 1;
        for (size_t k = 0; k < projects_.size(); ++k)
            if (projects_[k].id == current_) i = int(k) + (key == SDLK_DOWN ? 1 : -1);
        selectProject(projects_[size_t(std::clamp(i, 0, int(projects_.size()) - 1))].id);
    }
}

void App::onClick(float x, float y) {
    if (overlay_ == Overlay::Help || overlay_ == Overlay::About) {
        overlay_ = Overlay::None;
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
    if (focus_ != Focus::VocalLyric) unfocus();  // a field's own hit focuses it again
    if (dropOpen_ && !inside(dropRect_, x, y)) {
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

// ---- drawing helpers and widgets ---------------------------------------------------------------

void App::color(SDL_Color c, Uint8 a) { SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, Uint8(c.a * a / 255)); }

void App::fill(const SDL_FRect& r, SDL_Color c, Uint8 a) {
    color(c, a);
    SDL_RenderFillRect(renderer_, &r);
}

void App::frame(const SDL_FRect& r, SDL_Color c) {
    color(c);
    SDL_RenderRect(renderer_, &r);
}

// SDL's 8x8 debug font (ASCII; see text_util.h). maxW clips with "...".
void App::text(float x, float y, const std::string& utf8, SDL_Color c, float scale, float maxW) {
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

float App::textW(const std::string& s, float scale) const { return float(text::count(s)) * 8 * scale; }
void App::hit(const SDL_FRect& r, std::function<void(float, float)> fn) { hits_.push_back({r, std::move(fn)}); }
bool App::hovered(const SDL_FRect& r) const { return openMenu_ < 0 && inside(r, mouseX_, mouseY_); }

float App::buttonW(const std::string& label) const { return textW(label, 1.5f * u_) + 28 * u_; }

// A button; returns its width. Disabled buttons draw dim and take no clicks.
float App::button(float x, float y, const std::string& label, std::function<void()> fn, bool enabled, bool active, float h) {
    const float ts = 1.5f * u_, padX = 14 * u_;
    if (h <= 0) h = 30 * u_;
    SDL_FRect r{x, y, textW(label, ts) + 2 * padX, h};
    if (active && enabled) fill(r, hovered(r) ? SDL_Color{0xb8, 0x2e, 0x36, 255} : kOxblood);
    else if (enabled && hovered(r)) fill(r, kHover);
    else fill(r, kBar);
    if (!active || !enabled) frame(r, enabled ? kMuted : kBar);
    text(x + padX, y + (h - 8 * ts) / 2, label, enabled ? kCream : kMuted, ts);
    if (enabled && fn) hit(r, [fn](float, float) { fn(); });
    return r.w;
}

// The big call to action (FORGE SONG, Next).
void App::bigButton(const SDL_FRect& r, const std::string& label, std::function<void()> fn, bool enabled) {
    const bool over = enabled && hovered(r);
    fill({r.x + 4 * u_, r.y + 5 * u_, r.w, r.h}, kInk, 160);
    fill(r, enabled ? (over ? SDL_Color{0xbe, 0x30, 0x38, 255} : kOxblood) : kBar);
    if (enabled) fill({r.x, r.y, r.w, 3 * u_}, SDL_Color{0xd8, 0x55, 0x5c, 255});
    else frame(r, kMuted);
    const float ts = 2.5f * u_;
    text(r.x + (r.w - textW(label, ts)) / 2, r.y + (r.h - 8 * ts) / 2, label, enabled ? kCream : kMuted, ts);
    if (enabled && fn) hit(r, [fn](float, float) { fn(); });
}

float App::iconButton(float x, float y, Icon icon, std::function<void()> fn, bool enabled, bool active) {
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

float App::segmentedW(const std::vector<std::string>& labels) const {
    float w = 0;
    for (auto& l : labels) w += textW(l, 1.5f * u_) + 24 * u_;
    return w;
}

// A row of choices, one lit; returns its width.
float App::segmented(float x, float y, const std::vector<std::string>& labels, int selected,
                     const std::function<void(int)>& pick, float h) {
    const float ts = 1.5f * u_, padX = 12 * u_;
    if (h <= 0) h = 34 * u_;
    const float w = segmentedW(labels);
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

void App::checkbox(const SDL_FRect& box, bool on, bool radio) {
    if (radio) {
        const float cx = box.x + box.w / 2, cy = box.y + box.h / 2;
        shape::ellipse(renderer_, cx, cy, box.w / 2, box.h / 2, shape::fcolor(on ? kOxblood : kMuted));
        shape::ellipse(renderer_, cx, cy, box.w / 2 - 2 * u_, box.h / 2 - 2 * u_, shape::fcolor(on ? kOxblood : kInk));
        if (on) shape::ellipse(renderer_, cx, cy, box.w / 5, box.h / 5, shape::fcolor(kCream));
        return;
    }
    fill(box, on ? kOxblood : kInk);
    frame(box, on ? kOxblood : kMuted);
    if (on) {
        color(kCream);
        const float k = box.w / 18;
        for (float d = 0; d < 2; d += 1) {
            SDL_RenderLine(renderer_, box.x + 4 * k, box.y + (9 + d) * k, box.x + 8 * k, box.y + (13 + d) * k);
            SDL_RenderLine(renderer_, box.x + 8 * k, box.y + (13 + d) * k, box.x + 14 * k, box.y + (5 + d) * k);
        }
    }
}

void App::openDropdown(const SDL_FRect& anchor, std::vector<DropItem> items) {
    dropAnchor_ = anchor;
    dropItems_ = std::move(items);
    dropOpen_ = true;
}

void App::drawDropdown() {
    if (!dropOpen_) return;
    const float ts = 1.5f * u_, rowH = 28 * u_, pad = 10 * u_;
    float w = dropAnchor_.w;
    for (auto& it : dropItems_) w = std::max(w, textW(it.label, ts) + textW(it.note, 1.25f * u_) + 70 * u_);
    SDL_FRect box{dropAnchor_.x, dropAnchor_.y + dropAnchor_.h + 4 * u_, w, float(dropItems_.size()) * rowH + 8 * u_};
    int ww, wh;
    SDL_GetCurrentRenderOutputSize(renderer_, &ww, &wh);
    if (box.y + box.h > float(wh) - 30 * u_) box.y = dropAnchor_.y - box.h - 4 * u_;  // open upwards
    if (box.x + box.w > float(ww) - 8 * u_) box.x = float(ww) - 8 * u_ - box.w;
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

void App::progressBar(const SDL_FRect& r, float fraction, bool indeterminate) {
    fill(r, kInk);
    frame(r, kMuted);
    const SDL_FRect in{r.x + 2 * u_, r.y + 2 * u_, r.w - 4 * u_, r.h - 4 * u_};
    if (indeterminate || fraction < 0) {
        const float t = float(SDL_GetTicks() % 1600) / 1600.f, seg = in.w * 0.22f;
        fill({in.x + (in.w - seg) * t, in.y, seg, in.h}, kSand);
        return;
    }
    fill({in.x, in.y, in.w * std::clamp(fraction, 0.f, 1.f), in.h}, kOxblood);
    fill({in.x, in.y, in.w * std::clamp(fraction, 0.f, 1.f), std::max(u_, in.h * 0.3f)}, SDL_Color{0xd8, 0x55, 0x5c, 255});
}

// The record (res/record-disk...webp, as assets/disc.png), spinning while work runs.
void App::drawDisc(float cx, float cy, float radius, bool spinning) {
    const double angle = spinning ? double(SDL_GetTicks() % 3600) * 0.1 * 3 : 0.0;  // ~33 rpm look
    shape::ellipse(renderer_, cx + radius * 0.04f, cy + radius * 0.06f, radius, radius, shape::fcolor(kInk, 0.6f), 64);
    if (discTex_) {
        SDL_FRect dst{cx - radius, cy - radius, radius * 2, radius * 2};
        SDL_RenderTextureRotated(renderer_, discTex_, nullptr, &dst, angle, nullptr, SDL_FLIP_NONE);
    } else {  // no image: a drawn record
        shape::ellipse(renderer_, cx, cy, radius, radius, shape::fcolor({0x14, 0x12, 0x15, 255}), 64);
        for (float k = 0.9f; k > 0.4f; k -= 0.1f) shape::ring(renderer_, cx, cy, radius * k, radius * k, u_, shape::fcolor(kMuted, 0.4f), 64);
        shape::ellipse(renderer_, cx, cy, radius * 0.32f, radius * 0.32f, shape::fcolor(kOxblood), 48);
        const float a = float(angle) * 3.14159f / 180.f;
        shape::ellipse(renderer_, cx + std::cos(a) * radius * 0.2f, cy + std::sin(a) * radius * 0.2f, radius * 0.04f, radius * 0.04f,
                       shape::fcolor(kCream), 16);
    }
    shape::ellipse(renderer_, cx, cy, radius * 0.035f, radius * 0.035f, shape::fcolor(kInk), 16);
}

// Play / pause / start / -5 s / +5 s and the time; returns its width.
float App::transport(float x, float y) {
    const float x0 = x, gap = 6 * u_;
    const bool has = !player_.empty();
    x += iconButton(x, y, player_.playing() ? Icon::Pause : Icon::Play, [this] { player_.togglePlay(); }, has, player_.playing()) + gap;
    x += iconButton(x, y, Icon::Start, [this] { player_.seek(0); }, has) + gap;
    x += iconButton(x, y, Icon::Back, [this] { player_.seek(player_.playhead() - 5); }, has) + gap;
    x += iconButton(x, y, Icon::Forward, [this] { player_.seek(player_.playhead() + 5); }, has) + 14 * u_;
    const std::string t = clockTenths(player_.playhead()) + " / " + clockTenths(player_.duration());
    text(x, y + (34 * u_ - 16 * u_) / 2, t, has ? kCream : kMuted, 2 * u_);
    return x + textW(t, 2 * u_) - x0;
}

void App::banner(float x, float& y, float w, const std::string& msg, SDL_Color c) {
    SDL_FRect b{x, y, w, 30 * u_};
    fill(b, kBar);
    fill({b.x, b.y, 4 * u_, b.h}, c);
    text(b.x + 14 * u_, b.y + (b.h - 8 * 1.5f * u_) / 2, msg, kCream, 1.5f * u_, w - 24 * u_);
    y += 40 * u_;
}

// ---- the shell --------------------------------------------------------------------------------

void App::draw() {
    buildMenus();
    hits_.clear();
    int w, h;
    SDL_GetCurrentRenderOutputSize(renderer_, &w, &h);
    u_ = SDL_GetWindowDisplayScale(window_);
    const float W = float(w), H = float(h), menuH = 30 * u_, statusH = 26 * u_;
    const float sideW = sidebarCollapsed_ ? 44 * u_ : 280 * u_;
    color(kInk);
    SDL_RenderClear(renderer_);
    lyricsRect_ = {};
    rollRect_ = rulerRect_ = boardRect_ = {};
    boxRects_ = {};

    drawSidebar({0, menuH, sideW, H - menuH - statusH});
    drawMain({sideW, menuH, W - sideW, H - menuH - statusH});
    drawStatusBar({0, H - statusH, W, statusH});
    drawDropdown();
    drawTooltip(W, H);
    if (overlay_ == Overlay::Details) drawDetails(W, H);
    if (overlay_ == Overlay::Lyrics) drawLyricsEditor(W, H);
    if (overlay_ == Overlay::Paste) drawPasteEditor(W, H);
    drawMenuBar({0, 0, W, menuH});
    if (overlay_ == Overlay::Help) drawHelp(W, H);
    if (overlay_ == Overlay::About) drawAbout(W, H);
    if (selfTest_ && ++frames_ == 3) {
        selfTestResult_ = selfTest();
        quit_ = true;
    }
    if (!shotPath_.empty() && ++frames_ >= 30 && SDL_GetTicks() > 1500 && !(waitJobs_ && (active_ || !jobs_.empty()))) {
        if (SDL_Surface* shot = SDL_RenderReadPixels(renderer_, nullptr)) {
            SDL_SaveBMP(shot, shotPath_.c_str());
            SDL_DestroySurface(shot);
        }
        quit_ = true;
    }
    SDL_RenderPresent(renderer_);
}

void App::drawMenuBar(const SDL_FRect& bar) {
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
    const std::string brand = "DREAM CHARTER ENGINE";
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
    fill({box.x + 4 * u_, box.y + 4 * u_, box.w, box.h}, kInk, 160);
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

void App::drawSidebar(const SDL_FRect& r) {
    sidebarRect_ = r;
    fill(r, kPanel);
    fill({r.x + r.w - u_, r.y, u_, r.h}, kInk);
    const float pad = 14 * u_, ts = 1.5f * u_;
    SDL_FRect toggle{r.x + r.w - 36 * u_, r.y + 10 * u_, 28 * u_, 24 * u_};
    if (sidebarCollapsed_) toggle.x = r.x + (r.w - toggle.w) / 2;
    fill(toggle, hovered(toggle) ? kHover : kBar);
    text(toggle.x + (toggle.w - textW(">", ts)) / 2, toggle.y + (toggle.h - 8 * ts) / 2, sidebarCollapsed_ ? ">" : "<", kCream, ts);
    hit(toggle, [this](float, float) { toggleSidebar(); });
    hoverProject_.clear();

    if (sidebarCollapsed_) {
        SDL_FRect plus{toggle.x, toggle.y + 32 * u_, 28 * u_, 24 * u_};
        if (!projectsDir_.empty()) {
            fill(plus, hovered(plus) ? kOxblood : kBar);
            text(plus.x + (plus.w - textW("+", ts)) / 2, plus.y + (plus.h - 8 * ts) / 2, "+", kCream, ts);
            hit(plus, [this](float, float) { showDialog(DialogKind::Song); });
        }
        return;
    }
    text(r.x + pad, r.y + 18 * u_, "SONGS", kSand, ts);
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
        text(r.x + pad, listTop + 8 * u_, "No songs yet.", kDim, 1.25f * u_);
        text(r.x + pad, listTop + 26 * u_, "Drop an MP3 on the window.", kMuted, 1.25f * u_);
        return;
    }
    const float maxScroll = std::max(0.f, float(projects_.size()) * itemH - (r.y + r.h - listTop));
    sidebarScroll_ = std::min(sidebarScroll_, maxScroll);
    float y = listTop - sidebarScroll_;
    for (auto& p : projects_) {
        SDL_FRect row{r.x, y, r.w - u_, itemH};
        if (y >= listTop - 1 && y + itemH <= r.y + r.h + 1) {  // only whole rows
            const bool sel = p.id == current_, over = hovered(row);
            if (sel) fill(row, kHover);
            else if (over) fill(row, kHover, 140);
            if (sel) fill({row.x, row.y, 4 * u_, row.h}, kOxblood);
            if (over) hoverProject_ = p.id;
            text(r.x + pad, y + 9 * u_, p.title(), sel ? kCream : kSand, 1.25f * u_, maxW);
            std::string artist = p.artist();
            text(r.x + pad, y + 23 * u_, artist.empty() ? "Unknown artist" : artist, artist.empty() ? kMuted : kDim, 1.1f * u_, maxW);
            const bool working = active_ && fs::equivalent(active_->dir, p.dir, ec_);
            const std::string st = projectState(p);
            text(r.x + pad, y + 37 * u_, st, working ? kCream : st.find("failed") != std::string::npos ? kOxblood : kMuted, u_, maxW);
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
void App::drawTooltip(float W, float H) {
    if (hoverProject_.empty() || openMenu_ >= 0 || overlay_ != Overlay::None || SDL_GetTicks() - hoverSince_ < 350) return;
    Project* p = project(hoverProject_);
    if (!p) return;
    const float ts = 1.5f * u_, lineH = 18 * u_, pad = 10 * u_;
    const size_t width = 44;
    std::vector<std::pair<std::string, SDL_Color>> lines;
    for (auto& l : wrap(p->title(), width)) lines.push_back({l, kCream});
    for (auto& l : wrap(p->artist().empty() ? "Unknown artist" : p->artist(), width)) lines.push_back({l, kSand});
    std::string album = p->doc.get("song", "album"), year = p->doc.get("song", "year");
    if (!album.empty() || !year.empty())
        for (auto& l : wrap(album + (year.empty() ? "" : (album.empty() ? "" : " - ") + year), width)) lines.push_back({l, kDim});
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

void App::drawMain(const SDL_FRect& r) {
    if (projectsDir_.empty()) return drawWelcome(r);
    Project* p = currentProject();
    if (!p) return drawEmpty(r);
    if (jobFor(*p, JobKind::Metadata)) return drawLoading(*p, r);
    const float stepH = 64 * u_;
    drawStepBar(*p, {r.x, r.y, r.w, stepH});
    const float pad = 24 * u_;
    SDL_FRect body{r.x + pad, r.y + stepH + 16 * u_, r.w - 2 * pad, r.h - stepH - 16 * u_ - pad * 0.6f};
    switch (screen_) {
        case Screen::Home: drawHome(*p, body); break;
        case Screen::Forge: drawForgeScreen(*p, body); break;
        case Screen::Tune: drawTune(*p, body); break;
        case Screen::Export: drawExport(*p, body); break;
    }
}

void App::drawWelcome(const SDL_FRect& r) {
    const float cx = r.x + r.w / 2;
    drawDisc(cx, r.y + r.h * 0.3f, std::min(r.w, r.h) * 0.17f, false);
    float y = r.y + r.h * 0.52f;
    const std::string t = "DREAM CHARTER ENGINE";
    text(cx - textW(t, 3 * u_) / 2, y, t, kCream, 3 * u_);
    y += 44 * u_;
    for (const std::string l : {"Any MP3 or WAV becomes a playable Guitar Hero / Rock Band song.",
                                "First, choose where your song projects are saved (Settings can change it)."}) {
        text(cx - textW(l, 1.75f * u_) / 2, y, l, kDim, 1.75f * u_);
        y += 24 * u_;
    }
    y += 20 * u_;
    const fs::path def = defaultProjectsDir();
    const std::string a = "Choose Folder...", b = "Use " + def.string();
    const float total = buttonW(a) + buttonW(b) + 12 * u_;
    float bx = cx - total / 2;
    bx += button(bx, y, a, [this] { showDialog(DialogKind::ProjectsDir); }, true, true, 36 * u_) + 12 * u_;
    button(bx, y, b, [this, def] { setProjectsDir(def); }, true, false, 36 * u_);
}

// No song open: the record waiting for one.
void App::drawEmpty(const SDL_FRect& r) {
    const float cx = r.x + r.w / 2, radius = std::min(r.w, r.h) * 0.2f;
    drawDisc(cx, r.y + r.h * 0.36f, radius, false);
    float y = r.y + r.h * 0.36f + radius + 40 * u_;
    const std::string a = "Drop an MP3 or WAV here", b = "Each song becomes a project: sheets, forge, fine tuning, export.";
    text(cx - textW(a, 2.5f * u_) / 2, y, a, kCream, 2.5f * u_);
    y += 40 * u_;
    text(cx - textW(b, 1.5f * u_) / 2, y, b, kDim, 1.5f * u_);
    y += 36 * u_;
    const std::string c = "Open Song...";
    button(cx - buttonW(c) / 2, y, c, [this] { showDialog(DialogKind::Song); }, true, true, 34 * u_);
}

// A new song: the disc spins while its metadata is read.
void App::drawLoading(Project& p, const SDL_FRect& r) {
    const float cx = r.x + r.w / 2, radius = std::min(r.w, r.h) * 0.22f;
    drawDisc(cx, r.y + r.h * 0.38f, radius, true);
    float y = r.y + r.h * 0.38f + radius + 36 * u_;
    const std::string name = p.doc.get("project", "source_file").empty() ? p.id : fs::path(p.doc.get("project", "source_file")).filename().string();
    text(cx - std::min(textW(name, 2.25f * u_), r.w - 40 * u_) / 2, y, name, kCream, 2.25f * u_, r.w - 40 * u_);
    y += 40 * u_;
    const float bw = std::min(520 * u_, r.w - 80 * u_);
    const bool running = activeFor(p, JobKind::Metadata);
    progressBar({cx - bw / 2, y, bw, 14 * u_}, running ? jobProgress_ : -1, !running);
    y += 28 * u_;
    const std::string st = running ? jobStatus_ : "Waiting for the job before it...";
    text(cx - std::min(textW(st, 1.5f * u_), bw) / 2, y, st, kDim, 1.5f * u_, bw);
}

// Song home > Song forge > Fine tuning > Export song.
void App::drawStepBar(Project& p, const SDL_FRect& r) {
    fill(r, kPanel);
    fill({r.x, r.y + r.h - u_, r.w, u_}, kInk);
    const char* labels[] = {"Song home", "Song forge", "Fine tuning", "Export song"};
    const bool done[] = {p.forged(), p.forged(), p.doc.get("forge", "tuned") == "yes", !p.doc.get("export", "exported_at").empty()};
    // Steps on the right (smaller when the window is narrow); the song's name on the left if it fits.
    const float pad = 24 * u_;
    float ts = 1.75f * u_, gap = 36 * u_, total = 0;
    for (int pass = 0; pass < 3; ++pass) {
        total = gap * 3;
        for (auto* l : labels) total += 30 * u_ + textW(l, ts) + 16 * u_;
        if (total <= r.w - 2 * pad) break;
        ts -= 0.25f * u_;
        gap -= 10 * u_;
    }
    float x = r.x + r.w - pad - total;
    const float titleW = x - r.x - pad - 30 * u_;
    if (titleW > 120 * u_) {
        text(r.x + pad, r.y + 14 * u_, p.title(), kCream, 2 * u_, titleW);
        text(r.x + pad, r.y + 38 * u_, p.artist().empty() ? "Unknown artist" : p.artist(), kDim, 1.25f * u_, titleW);
    } else {
        x = r.x + (r.w - total) / 2;
    }
    const float cy = r.y + r.h / 2;
    for (int i = 0; i < 4; ++i) {
        const Screen s = Screen(i);
        const bool cur = screen_ == s, open = screenOpen(p, s);
        const float w = 30 * u_ + textW(labels[i], ts) + 16 * u_;
        SDL_FRect chip{x - 6 * u_, cy - 20 * u_, w + 6 * u_, 40 * u_};
        if (cur) fill(chip, kHover);
        else if (open && hovered(chip)) fill(chip, kBar);
        if (cur) fill({chip.x, chip.y + chip.h - 3 * u_, chip.w, 3 * u_}, kOxblood);
        const SDL_Color dot = cur ? kOxblood : done[i] ? kGood : open ? kMuted : kBar;
        shape::ellipse(renderer_, x + 12 * u_, cy, 12 * u_, 12 * u_, shape::fcolor(dot));
        if (done[i] && !cur) {
            color(kInk);
            for (float d = 0; d < 2; d += 1) {
                SDL_RenderLine(renderer_, x + 6 * u_, cy + d * u_, x + 10 * u_, cy + (4 + d) * u_);
                SDL_RenderLine(renderer_, x + 10 * u_, cy + (4 + d) * u_, x + 18 * u_, cy - (4 - d) * u_);
            }
        } else {
            const std::string n = std::to_string(i + 1);
            text(x + 12 * u_ - textW(n, 1.5f * u_) / 2, cy - 6 * u_, n, open || cur ? kCream : kMuted, 1.5f * u_);
        }
        text(x + 30 * u_, cy - 4 * ts, labels[i], cur ? kCream : open ? kSand : kMuted, ts);
        if (open) hit(chip, [this, s](float, float) { setScreen(s); });
        x += w;
        if (i < 3) {
            fill({x + 6 * u_, cy - u_, gap - 12 * u_, 2 * u_}, done[i] ? kGood : kBar);
            x += gap;
        }
    }
}

void App::drawStatusBar(const SDL_FRect& r) {
    fill(r, kBar);
    fill({r.x, r.y, r.w, u_}, kInk);
    const float ts = 1.25f * u_, pad = 12 * u_, ty = r.y + (r.h - 8 * ts) / 2;
    std::string right = "Stems: " + std::string(mode_ == "demucs" ? "Demucs 6 stems" : "simple filters");
    if (!jobs_.empty()) right = std::to_string(jobs_.size()) + " queued   " + right;
    const float rw = textW(right, ts);
    if (active_) fill({r.x + pad, r.y + r.h / 2 - 3 * u_, 6 * u_, 6 * u_}, kOxblood);
    text(r.x + pad + (active_ ? 14 * u_ : 0), ty, status_, active_ ? kCream : kDim, ts, r.w - rw - 4 * pad);
    text(r.x + r.w - pad - rw, ty, right, kDim, ts);
}

// A modal panel over a dimmed window; returns its body. Clicks outside it go nowhere.
SDL_FRect App::drawPanel(float W, float H, float w, float h, const std::string& title) {
    fill({0, 0, W, H}, kInk, 190);
    hit({0, 0, W, H}, [](float, float) {});
    SDL_FRect box{(W - w) / 2, (H - h) / 2, w, h};
    fill({box.x + 5 * u_, box.y + 6 * u_, box.w, box.h}, kInk, 200);
    fill(box, kPanel);
    fill({box.x, box.y, box.w, 3 * u_}, kOxblood);
    text(box.x + 24 * u_, box.y + 22 * u_, title, kCream, 2.25f * u_);
    return {box.x + 24 * u_, box.y + 60 * u_, box.w - 48 * u_, box.h - 84 * u_};
}

void App::drawDialog(float W, float H, const std::string& title, const std::vector<std::pair<std::string, std::string>>& lines) {
    fill({0, 0, W, H}, kInk, 180);
    const float ts = 1.5f * u_, lineH = 22 * u_, bw = 700 * u_, bh = 90 * u_ + float(lines.size()) * lineH;
    SDL_FRect box{(W - bw) / 2, (H - bh) / 2, bw, bh};
    fill(box, kPanel);
    fill({box.x, box.y, box.w, 3 * u_}, kOxblood);
    text(box.x + 24 * u_, box.y + 22 * u_, title, kCream, 2 * u_);
    float y = box.y + 56 * u_;
    for (auto& [a, b] : lines) {
        text(box.x + 24 * u_, y, a, kSand, ts);
        text(box.x + 250 * u_, y, b, kDim, ts, box.w - 274 * u_);
        y += lineH;
    }
    text(box.x + 24 * u_, box.y + box.h - 24 * u_, "Click or press any key to close.", kMuted, 1.25f * u_);
}

void App::drawHelp(float W, float H) {
    drawDialog(W, H, "Keyboard shortcuts",
               {{kCmd + "O", "Open a song (new project)"},
                {kCmd + "1 ... 4", "Song home / Song forge / Fine tuning / Export song"},
                {kCmd + "S", "Save everything"},
                {kCmd + "B", "Collapse / expand the sidebar"},
                {"Space, Home, Left/Right", "Play / pause, start, back / forward 5 s"},
                {"Song forge: 1-4", "Easy ... Expert"},
                {"Song home: Paste / " + kCmd + "V", "Paste a tab, chord sheet or copied file into the box under the mouse"},
                {"Fine tuning: click", "Add a note (empty spot) or select one"},
                {"Fine tuning: drag", "Move a note; drag its tail end (or Alt+drag): sustain"},
                {"Fine tuning: right click", "Delete a note (or Delete / Backspace)"},
                {"Fine tuning: H, S, C", "HOPO, Star Power, cymbal (drums)"},
                {"Fine tuning: Up / Down", "Move the selected note a lane (pitch for vocals)"},
                {"Fine tuning: Enter", "Vocals: edit the syllable (Tab: next one)"},
                {"Fine tuning: " + kCmd + "Z / Shift+Z", "Undo / redo"},
                {"Fine tuning: wheel", "Scroll; " + kCmd + "wheel zooms"},
                {"F5", "Refresh the song list"},
                {kCmd + "Q", "Quit"}});
}

void App::drawAbout(float W, float H) {
    drawDialog(W, H, "Dream Charter Engine (gd-tool-02)",
               {{"Charting", "dream-charter-engine (ai_charter), Beat This!, ADTOF"},
                {"Separation", "Demucs htdemucs_6s (Meta, MIT), offline"},
                {"Metadata", "mutagen (tags), librosa (tempo, key)"},
                {"Projects", projectsDir_.empty() ? "not chosen yet" : projectsDir_.string()},
                {"Settings", settingsPath_.string()},
                {"Built with", "C++17, SDL " + std::to_string(SDL_MAJOR_VERSION) + "." + std::to_string(SDL_MINOR_VERSION)}});
}

int runApp(int argc, char** argv) {
    App app;
    if (!app.init(argc, argv)) return 1;
    app.run();
    app.shutdown();
    return app.selfTestResult_ > 0 ? 1 : 0;
}
