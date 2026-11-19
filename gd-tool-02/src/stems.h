// gd-tool-02: the stems of the open project, played together (one SDL audio
// stream per stem on one device), each with a waveform summary for drawing.
#pragma once

#include <SDL3/SDL.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Stem {
    std::string name;
    SDL_AudioSpec spec{};
    std::vector<int16_t> pcm;  // interleaved S16
    std::vector<float> peaks;  // max |sample| per bucket, 0..1
    SDL_AudioStream* stream = nullptr;
    bool muted = false;
    double seconds() const { return double(pcm.size()) / spec.channels / spec.freq; }
};

SDL_Color stemColor(const std::string& name);

class StemPlayer {
public:
    bool open();  // the default playback device
    void close();

    // Loads a .wav (keeps the order mix, guitar, bass, drums, keys, vocals, other).
    bool add(const std::string& name, const std::string& path, std::string& error);
    void clear();
    bool has(const std::string& name) const;

    const std::vector<std::unique_ptr<Stem>>& stems() const { return stems_; }
    bool empty() const { return stems_.empty(); }
    bool playing() const { return playing_; }

    double duration() const;
    double playhead() const;
    void seek(double seconds);  // keeps playing/paused
    void togglePlay();
    void update();  // stops at the end

    void toggleMute(size_t i);
    void solo(size_t i);
    void setAllMuted(bool muted, bool exceptMix);

private:
    void refill(double seconds);
    void applyGain(Stem& s);

    SDL_AudioDeviceID device_ = 0;
    std::vector<std::unique_ptr<Stem>> stems_;
    bool playing_ = false;
    double position_ = 0;
};
