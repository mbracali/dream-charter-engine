// Worker must deliver every line a child prints, even after pauses (SDL's process
// output stream is non-blocking: a read can return 0 before the child is done).
#include <SDL3/SDL.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/worker.h"

int main() {
    if (!SDL_Init(0)) return 2;
    Worker w;
    std::string error;
    // Prints a line, pauses 0.3 s, prints two more: like Demucs between STATUS and STEM.
    if (!w.start({"/bin/sh", "-c", "echo STATUS one; sleep 0.3; echo STEM drums /x/drums.wav; echo DONE"}, error)) {
        std::printf("FAIL: could not start: %s\n", error.c_str());
        return 1;
    }
    std::vector<std::string> lines;
    const Uint64 until = SDL_GetTicks() + 5000;
    while (SDL_GetTicks() < until) {
        std::string line;
        while (w.popLine(line)) lines.push_back(line);
        if (!lines.empty() && lines.back().rfind("EXIT ", 0) == 0) break;
        SDL_Delay(10);
    }
    const std::vector<std::string> want = {"STATUS one", "STEM drums /x/drums.wav", "DONE", "EXIT 0"};
    if (lines != want) {
        std::printf("FAIL: got %zu lines:\n", lines.size());
        for (auto& l : lines) std::printf("  %s\n", l.c_str());
        return 1;
    }
    std::printf("ok: all %zu lines\n", lines.size());
    SDL_Quit();
    return 0;
}
