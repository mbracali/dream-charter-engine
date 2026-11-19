#include "worker.h"

bool Worker::start(const std::vector<std::string>& argv, std::string& error) {
    stop();
    std::vector<const char*> args;
    for (auto& a : argv) args.push_back(a.c_str());
    args.push_back(nullptr);
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, args.data());
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, SDL_PROCESS_STDIO_APP);
    SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_INHERITED);
    proc_ = SDL_CreateProcessWithProperties(props);
    SDL_DestroyProperties(props);
    if (!proc_) {
        error = SDL_GetError();
        return false;
    }
    running_ = true;
    reader_ = std::thread([this] { readLoop(); });
    return true;
}

void Worker::stop() {
    if (proc_ && running_) SDL_KillProcess(proc_, true);
    if (reader_.joinable()) reader_.join();
    if (proc_) SDL_DestroyProcess(proc_);
    proc_ = nullptr;
    running_ = false;
}

bool Worker::popLine(std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (lines_.empty()) return false;
    line = std::move(lines_.front());
    lines_.pop_front();
    return true;
}

void Worker::readLoop() {
    SDL_IOStream* out = SDL_GetProcessOutput(proc_);
    std::string pending;
    char buf[512];
    while (out) {
        const size_t n = SDL_ReadIO(out, buf, sizeof buf);
        if (n == 0) {
            // The stream is non-blocking: 0 with NOT_READY only means "nothing yet"
            // (the child is busy, e.g. Demucs separating). Stop at the real end.
            if (SDL_GetIOStatus(out) == SDL_IO_STATUS_NOT_READY) {
                SDL_Delay(15);
                continue;
            }
            break;
        }
        pending.append(buf, n);
        size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            push(pending.substr(0, nl));
            pending.erase(0, nl + 1);
        }
    }
    if (!pending.empty()) push(pending);
    int code = 0;
    SDL_WaitProcess(proc_, true, &code);
    push("EXIT " + std::to_string(code));
    running_ = false;
}

void Worker::push(std::string line) {
    std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(std::move(line));
}
