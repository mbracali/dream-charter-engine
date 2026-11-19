// gd-tool-02: runs a Python helper (python/*.py) as a child process. Its stdout
// lines (STATUS / STEM / DONE / ERROR, then our own "EXIT <code>") are queued
// by a reader thread and taken on the main thread with popLine().
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Worker {
public:
    ~Worker() { stop(); }

    bool start(const std::vector<std::string>& argv, std::string& error);
    void stop();  // kills a running process
    bool running() const { return running_; }
    bool popLine(std::string& line);

private:
    void readLoop();
    void push(std::string line);

    SDL_Process* proc_ = nullptr;
    std::thread reader_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::deque<std::string> lines_;
};
