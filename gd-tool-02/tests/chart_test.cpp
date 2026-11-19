// chart.txt in C++ (Fine tuning saves it, python/chart_text.py reads it): a save/load
// round trip keeps every note, flag and syllable; the beat grid snaps as expected.
#include <cmath>
#include <cstdio>
#include <filesystem>

#include "../src/chart.h"

static int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                 \
        }                                                               \
    } while (0)

int main() {
    Chart c;
    c.duration = 12.5f;
    for (int i = 0; i < 8; ++i) c.beats.push_back({0.5f * float(i), i % 4 == 0});
    c.sections.push_back({0.f, "Intro riff"});
    c.parts["guitar"][3].push_back({1.0f, 0.75f, 2, true, true, false});
    c.parts["guitar"][3].push_back({0.5f, 0.f, 0, false, false, false});
    c.parts["drums"][3].push_back({0.5f, 0.f, 3, false, false, true});
    c.vocals.push_back({1.f, 0.4f, 62, "can-"});
    c.vocals.push_back({1.5f, 0.4f, 64, ""});
    c.phrases.push_back({1.f, 1.9f});
    c.sort();

    const auto file = std::filesystem::temp_directory_path() / "gdt02_chart_test.txt";
    CHECK(c.save(file));
    Chart b;
    CHECK(b.load(file));
    CHECK(b.loaded && std::abs(b.duration - 12.5f) < 1e-3f);
    CHECK(b.beats.size() == 8 && b.beats[4].second && !b.beats[5].second);
    CHECK(b.sections.size() == 1 && b.sections[0].second == "Intro riff");
    const auto& g = b.parts["guitar"][3];
    CHECK(g.size() == 2 && g[0].t == 0.5f && g[1].lane == 2 && g[1].hopo && g[1].star && std::abs(g[1].sustain - 0.75f) < 1e-4f);
    CHECK(b.parts["drums"][3].size() == 1 && b.parts["drums"][3][0].cymbal);
    CHECK(b.vocals.size() == 2 && b.vocals[0].lyric == "can-" && b.vocals[1].lyric.empty() && b.vocals[1].pitch == 64);
    CHECK(b.phrases.size() == 1);
    CHECK(b.count("guitar", 3) == 2 && b.count("keys", 3) == -1 && b.count("vocals", 0) == 2);
    CHECK(b.hasPart("drums") && !b.hasPart("bass"));

    // Grid: beats every 0.5 s.
    CHECK(std::abs(b.snap(0.61f, 4) - 0.625f) < 1e-4f);    // 16ths: 0.125 s
    CHECK(std::abs(b.snap(0.61f, 1) - 0.5f) < 1e-4f);      // quarters
    CHECK(std::abs(b.snap(0.61f, 3) - 2.f / 3.f) < 1e-4f); // triplets
    CHECK(std::abs(b.stepFrom(0.5f, 2, 1) - 0.75f) < 1e-4f);
    CHECK(std::abs(b.stepFrom(0.5f, 2, -2) - 0.f) < 1e-4f);
    CHECK(std::abs(b.snap(5.1f, 1) - 5.f) < 1e-4f);        // past the last beat: same beat length
    std::filesystem::remove(file);
    std::printf(failures ? "%d failure(s)\n" : "chart_test: all passed\n", failures);
    return failures ? 1 : 0;
}
