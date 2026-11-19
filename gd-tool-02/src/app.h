// gd-tool-02 (Dream Charter Engine): the window. Each song is a project, a folder in
// the user's projects folder:
//
//   project.yaml   metadata + settings      song.<ext>   the song      cover.png
//   lyrics.txt     typed lyrics             sheets/<guitar|bass|drums|lyrics>/  sheets
//   stems/         Demucs stems (.wav)      forge/       the engine's notes.mid,
//                                                        chart.txt (edited), forged.txt
//
// A project goes through four screens, shown as a step bar (docs/product_spec.md):
// Song home (sheets) > Song forge (stems + charts) > Fine tuning (note editor) >
// Export song (Clone Hero / YARG). Python helpers (python/*.py) do the heavy work,
// one at a time, as child processes (Worker).
//
// The class is split over several files: app.cpp (core, input, shell, widgets),
// board.cpp (highway and vocal boards), screen_home.cpp, screen_forge.cpp,
// screen_tune.cpp and screen_export.cpp.
#pragma once

#include <SDL3/SDL.h>

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "chart.h"
#include "stems.h"
#include "worker.h"
#include "yaml_doc.h"

namespace fs = std::filesystem;

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
constexpr SDL_Color kGood{0x5f, 0xa8, 0x5a, 255};
constexpr SDL_Color kStar{0x7f, 0xe8, 0xff, 255};

#ifdef __APPLE__
inline const std::string kCmd = "Cmd+";
#else
inline const std::string kCmd = "Ctrl+";
#endif

bool inside(const SDL_FRect& r, float x, float y);
std::string nowIso();
std::string clockText(double s);
std::string clockTenths(double s);
std::string folderName(std::string s);
std::vector<std::string> wrap(const std::string& s, size_t width);
std::string lower(std::string s);

struct Project {
    std::string id;  // folder name
    fs::path dir;
    YamlDoc doc;
    bool dirty = false;  // unsaved metadata edits
    int stemCount = 0;

    std::string title() const;
    std::string artist() const { return doc.get("song", "artist"); }
    fs::path stemsDir() const { return dir / "stems"; }
    fs::path audioFile() const;
    fs::path chartFile() const { return dir / "forge" / "chart.txt"; }
    bool forged() const { return fs::exists(chartFile()); }
};

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

enum class Screen { Home, Forge, Tune, Export };
enum class JobKind { Metadata, Forge, Export };
enum class DialogKind { Song, ProjectsDir, CloneHeroDir, YargDir, Sheet };
enum class Overlay { None, Details, Lyrics, Help, About };
enum class Focus { None, Field, Lyrics, VocalLyric };
enum class Icon { Play, Pause, Start, Back, Forward };

struct Job {
    fs::path dir;
    JobKind kind;
    std::vector<std::string> args = {};
    std::string label = {};
};

struct Hit {  // something clickable drawn this frame
    SDL_FRect rect;
    std::function<void(float, float)> fn;
};

struct DropItem {  // a row of a pop-up list
    std::string label, note;
    SDL_Color dot;
    bool enabled, checked;
    std::function<void()> pick;
};

// The music-sheet boxes of Song home.
struct BoxDef {
    const char* id;
    const char* label;
    const char* hint;
};
inline const BoxDef kBoxes[] = {{"guitar", "Guitar", "tab, MIDI, chords or chart"},
                                {"bass", "Bass", "tab, MIDI, chords or chart"},
                                {"drums", "Drums", "tab, MIDI or chart"},
                                {"lyrics", "Lyrics", ".txt / .lrc"}};

struct SheetFile {
    std::string name, kind;  // kind: tab, score, chart, chords, lyrics, "" (not usable)
    bool used = true;
};

class App {
public:
    bool init(int argc, char** argv);
    void run();
    void shutdown();

    // ---- core (app.cpp) ---------------------------------------------------------------
    void logEvent(const std::string& msg);
    static fs::path toolRoot();
    void loadSettings(const std::string& overridePath);
    void saveSettings();
    void setProjectsDir(const fs::path& dir);
    fs::path defaultProjectsDir() const;
    fs::path exportDir(const std::string& format) const;  // clonehero / yarg
    void toggleSidebar();

    Project* project(const std::string& id);
    Project* currentProject() { return project(current_); }
    Project* projectAt(const fs::path& dir);
    void refreshProjects();
    void saveProject(Project& p);
    void saveAll();
    void setValue(Project& p, const std::string& section, const std::string& key, const std::string& value);

    bool jobFor(const Project& p, JobKind kind) const;
    bool activeFor(const Project& p, JobKind kind) const;
    bool busy(const Project& p) const;
    std::string projectState(const Project& p) const;
    void newProject(const fs::path& song);
    void openPath(const std::string& input, float dropX = -1, float dropY = -1);
    void startNextJob();
    void pumpWorker();
    void finishJob(const Job& job, bool ok);

    void closeProject();
    void selectProject(const std::string& id);
    void setScreen(Screen s);
    bool screenOpen(const Project& p, Screen s) const;
    void loadStems(const Project& p);
    void loadCover(const Project& p);
    void loadChart(const Project& p);
    void saveChart();

    void loadLyrics(const Project& p);
    void saveLyrics();
    int lyricsLineCount() const;
    std::pair<int, int> caretPos() const;
    size_t lineStart(int line) const;
    int lineTotal() const;
    void caretTo(int line, int col);
    void insertLyrics(const std::string& s);

    void focusField(int i);
    void focusLyrics();
    void focusVocalLyric();
    void unfocus();
    void focusNext(int dir);
    void editField(const std::function<void(std::string&)>& change);

    static void SDLCALL onDialog(void* user, const char* const* files, int);
    void showDialog(DialogKind kind, const std::string& box = "");
    void takeDialogResult();
    void reveal(const fs::path& dir);

    void buildMenus();
    void handle(SDL_Event& e);
    void onKey(SDL_Keycode key, SDL_Keymod mod);
    void onClick(float x, float y);

    // Drawing helpers and widgets.
    void color(SDL_Color c, Uint8 a = 255);
    void fill(const SDL_FRect& r, SDL_Color c, Uint8 a = 255);
    void frame(const SDL_FRect& r, SDL_Color c);
    void text(float x, float y, const std::string& utf8, SDL_Color c, float scale, float maxW = 0);
    float textW(const std::string& s, float scale) const;
    void hit(const SDL_FRect& r, std::function<void(float, float)> fn);
    bool hovered(const SDL_FRect& r) const;
    float button(float x, float y, const std::string& label, std::function<void()> fn, bool enabled = true,
                 bool active = false, float h = 0);
    float buttonW(const std::string& label) const;
    void bigButton(const SDL_FRect& r, const std::string& label, std::function<void()> fn, bool enabled = true);
    float iconButton(float x, float y, Icon icon, std::function<void()> fn, bool enabled = true, bool active = false);
    float segmented(float x, float y, const std::vector<std::string>& labels, int selected,
                    const std::function<void(int)>& pick, float h = 0);
    float segmentedW(const std::vector<std::string>& labels) const;
    void checkbox(const SDL_FRect& box, bool on, bool radio = false);
    void openDropdown(const SDL_FRect& anchor, std::vector<DropItem> items);
    void drawDropdown();
    void progressBar(const SDL_FRect& r, float fraction, bool indeterminate);
    void drawDisc(float cx, float cy, float radius, bool spinning);
    float transport(float x, float y);
    void banner(float x, float& y, float w, const std::string& msg, SDL_Color c);

    // Shell.
    void draw();
    void drawMenuBar(const SDL_FRect& bar);
    void drawSidebar(const SDL_FRect& r);
    void drawTooltip(float W, float H);
    void drawMain(const SDL_FRect& r);
    void drawWelcome(const SDL_FRect& r);
    void drawEmpty(const SDL_FRect& r);
    void drawLoading(Project& p, const SDL_FRect& r);
    void drawStepBar(Project& p, const SDL_FRect& r);
    void drawStatusBar(const SDL_FRect& r);
    void drawDialog(float W, float H, const std::string& title,
                    const std::vector<std::pair<std::string, std::string>>& lines);
    void drawHelp(float W, float H);
    void drawAbout(float W, float H);
    SDL_FRect drawPanel(float W, float H, float w, float h, const std::string& title);

    // ---- boards (board.cpp) -------------------------------------------------------------
    static SDL_Color laneColor(int lane, bool drums);
    void drawHighway(const SDL_FRect& a, const std::vector<ChartNote>* notes, bool drums, float now);
    void drawGem(float x, float y, float rx, SDL_Color c, float alpha, bool hopo, bool cymbal);
    void drawVocalBoard(const SDL_FRect& a, float now);
    void drawOverview(const SDL_FRect& r, const std::vector<ChartNote>* notes, bool vocals, const std::string& part);
    const std::vector<ChartNote>* partNotes(const std::string& part, int diff) const;

    // ---- Song home (screen_home.cpp) --------------------------------------------------------
    void drawHome(Project& p, const SDL_FRect& r);
    void drawBox(Project& p, const BoxDef& box, const SDL_FRect& r);
    void drawLyricsBox(Project& p, const SDL_FRect& r);
    void drawDetails(float W, float H);
    void drawField(Project& p, int index, float x, float y, float labelW, float valueW);
    void drawLyricsEditor(float W, float H);
    std::vector<SheetFile> sheetFiles(const Project& p, const std::string& box) const;
    static std::string sheetKind(const fs::path& file, const std::string& box);
    static std::string kindLabel(const std::string& kind);
    void addSheet(Project& p, const std::string& box, const fs::path& file);
    void removeSheet(Project& p, const std::string& box, const std::string& name);
    void toggleSheet(Project& p, const std::string& box, const std::string& name);
    std::string lyricsChoice(const Project& p) const;
    bool hasTypedLyrics(const Project& p) const;
    std::string boxSummary(const Project& p, const std::string& box) const;
    int boxAt(float x, float y) const;
    void forgeSong();

    // ---- Song forge (screen_forge.cpp) ----------------------------------------------------
    void drawForgeScreen(Project& p, const SDL_FRect& r);
    void drawForgeProgress(Project& p, const SDL_FRect& r);
    void drawMixer(const SDL_FRect& r);
    void drawForgeSettings(Project& p, const SDL_FRect& r);
    void drawPreview(Project& p, const SDL_FRect& r);
    void drawBandView(const SDL_FRect& r);
    float partTabs(float x, float y, const std::vector<std::string>& parts, std::string& selected);
    std::vector<std::string> chartParts() const;
    const std::vector<std::pair<std::string, std::string>>& genres();
    std::string drumStyle(const Project& p) const;
    std::string riffStyle(const Project& p) const;
    std::string sourceOf(const Project& p, const std::string& part) const;
    std::string forgeSignature(const Project& p) const;
    void startForge();

    // ---- Fine tuning (screen_tune.cpp) -------------------------------------------------------
    void drawTune(Project& p, const SDL_FRect& r);
    void drawRoll(const SDL_FRect& r);
    void drawVocalRoll(const SDL_FRect& r);
    std::vector<ChartNote>* editNotes();
    bool tuneVocals() const { return tunePart_ == "vocals"; }
    float rollX(float t) const;
    float rollT(float x) const;
    void pushUndo();
    void undo();
    void redo();
    void chartChanged();
    void revertChart();
    void tuneMouseDown(float x, float y, Uint8 button, bool shift);
    void tuneMouseMove(float x, float y);
    void tuneMouseUp();
    bool tuneKey(SDL_Keycode key, SDL_Keymod mod);
    void tuneWheel(float dx, float dy, bool zoom);
    void deleteSelection();
    void toggleFlag(char flag);
    void selectNote(int index);
    int noteAt(float x, float y) const;
    int vocalAt(float x, float y) const;
    int laneAtY(float y) const;
    int pitchAtY(float y) const;
    float pitchY(int pitch) const;
    void followPlayhead();

    // ---- --selftest (selftest.cpp): drives the editor through its own handlers -------------
    int selfTest();

    // ---- Export song (screen_export.cpp) ---------------------------------------------------
    void drawExport(Project& p, const SDL_FRect& r);
    bool targetOn(const Project& p, const std::string& target) const;
    void startExport(const std::string& format);

    // ---- state ---------------------------------------------------------------------------------
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    float u_ = 1;  // display scale
    bool quit_ = false;
    mutable std::error_code ec_;

    YamlDoc settings_;
    fs::path settingsPath_, projectsDir_, cloneHeroDir_, yargDir_;
    std::string mode_ = "demucs", modeOverride_;
    bool sidebarCollapsed_ = false;

    std::vector<Project> projects_;
    std::string current_, status_;
    Screen screen_ = Screen::Home;
    int startScreen_ = -1;

    Worker worker_;
    std::deque<Job> jobs_;
    std::optional<Job> active_;
    std::string jobError_, jobStatus_;
    float jobProgress_ = -1;  // -1: unknown
    Uint64 busySince_ = 0;
    std::vector<std::string> exported_;  // folders of the last export

    StemPlayer player_;
    std::string stemsFor_;
    SDL_Texture* coverTex_ = nullptr;
    SDL_Texture* discTex_ = nullptr;

    Overlay overlay_ = Overlay::None;
    Focus focus_ = Focus::None;
    int field_ = -1;  // metadata field being edited

    Chart chart_;
    std::string chartFor_;
    std::vector<Chart> undo_, redo_;
    bool chartDirty_ = false;
    std::string previewPart_ = "guitar";
    int previewDiff_ = 3;  // Expert
    bool listenPart_ = false;
    bool bandView_ = true;     // Song forge: every part at once (else one big highway)
    bool showPanels_ = false;  // Song forge, band view: the mixer and settings beside it

    std::string tunePart_ = "guitar";
    int tuneDiff_ = 3;
    int snap_ = 4;          // grid subdivisions per beat
    float pps_ = 180;       // roll pixels per second (scaled by u_ when drawn)
    float viewStart_ = 0;   // roll: seconds at the left edge
    int sel_ = -1;          // selected note (index in editNotes()) or vocal
    enum class Drag { None, Move, Sustain, Resize, Scrub } drag_ = Drag::None;
    float dragX_ = 0, dragY_ = 0, dragT0_ = 0, dragV0_ = 0;
    int dragLane0_ = 0;
    bool dragPushed_ = false, follow_ = true;
    SDL_FRect rollRect_{}, rulerRect_{};
    int pitchLo_ = 48, pitchHi_ = 72;

    std::string lyrics_, lyricsFor_;
    bool lyricsDirty_ = false;
    size_t caret_ = 0;
    int lyricsScroll_ = 0;
    SDL_FRect lyricsRect_{};

    std::array<SDL_FRect, 4> boxRects_{};
    SDL_FRect boardRect_{};
    bool dropOpen_ = false;
    SDL_FRect dropAnchor_{}, dropRect_{};
    std::vector<DropItem> dropItems_;
    std::vector<std::pair<std::string, std::string>> genres_;

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
    std::string dialogBox_;
    std::string shotPath_;
    int frames_ = 0;
    bool waitJobs_ = false, hoverPinned_ = false, forgeAtStart_ = false, selfTest_ = false;
    int selfTestResult_ = -1;
    double seekTo_ = 0;
    std::string overlayAtStart_;
};

int runApp(int argc, char** argv);
