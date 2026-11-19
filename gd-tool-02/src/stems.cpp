#include "stems.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr int kPeakBuckets = 4000;

int stemOrder(const std::string& name) {
    static const char* order[] = {"mix", "guitar", "bass", "drums", "keys", "vocals", "other"};
    for (int i = 0; i < 7; ++i)
        if (name == order[i]) return i;
    return 99;
}

}  // namespace

SDL_Color stemColor(const std::string& name) {
    if (name == "guitar") return {0x3c, 0xb3, 0x4a, 255};
    if (name == "bass") return {0xd9, 0x3b, 0x3b, 255};
    if (name == "drums") return {0xe8, 0xc5, 0x2c, 255};
    if (name == "keys") return {0x3b, 0x7d, 0xd9, 255};
    if (name == "vocals") return {0xe8, 0x84, 0x2c, 255};
    if (name == "mix") return {0xc9, 0xb8, 0x92, 255};
    return {0x91, 0x87, 0x7a, 255};
}

bool StemPlayer::open() {
    device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (device_) SDL_PauseAudioDevice(device_);
    return device_ != 0;
}

void StemPlayer::close() {
    clear();
    if (device_) SDL_CloseAudioDevice(device_);
    device_ = 0;
}

bool StemPlayer::has(const std::string& name) const {
    return std::any_of(stems_.begin(), stems_.end(), [&](auto& s) { return s->name == name; });
}

bool StemPlayer::add(const std::string& name, const std::string& path, std::string& error) {
    if (has(name)) return true;
    SDL_AudioSpec spec;
    Uint8* buf = nullptr;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path.c_str(), &spec, &buf, &len)) {
        error = SDL_GetError();
        return false;
    }
    SDL_AudioSpec want{SDL_AUDIO_S16, spec.channels, spec.freq};
    Uint8* conv = nullptr;
    int convLen = 0;
    bool ok = SDL_ConvertAudioSamples(&spec, buf, int(len), &want, &conv, &convLen);
    SDL_free(buf);
    if (!ok) {
        error = SDL_GetError();
        return false;
    }
    auto stem = std::make_unique<Stem>();
    stem->name = name;
    stem->spec = want;
    stem->pcm.assign(reinterpret_cast<int16_t*>(conv), reinterpret_cast<int16_t*>(conv) + convLen / 2);
    SDL_free(conv);
    const size_t ch = size_t(want.channels), frames = stem->pcm.size() / ch;
    stem->peaks.assign(kPeakBuckets, 0.f);
    for (size_t f = 0; f < frames; ++f) {
        float& p = stem->peaks[f * kPeakBuckets / std::max<size_t>(frames, 1)];
        for (size_t c = 0; c < ch; ++c) p = std::max(p, std::fabs(float(stem->pcm[f * ch + c]) / 32768.f));
    }
    if (device_) {
        stem->stream = SDL_CreateAudioStream(&stem->spec, nullptr);
        SDL_BindAudioStream(device_, stem->stream);
    }
    stems_.push_back(std::move(stem));
    std::sort(stems_.begin(), stems_.end(), [](auto& a, auto& b) { return stemOrder(a->name) < stemOrder(b->name); });
    // The stems together are the song: the mix starts muted once there are stems
    // (only then: muting it again later would undo the user's choice).
    for (auto& s : stems_)
        if (s->name == "mix" && stems_.size() > 1 && (name == "mix" || stems_.size() == 2)) s->muted = true;
    refill(playing_ ? playhead() : position_);
    return true;
}

void StemPlayer::clear() {
    playing_ = false;
    if (device_) SDL_PauseAudioDevice(device_);
    for (auto& s : stems_)
        if (s->stream) SDL_DestroyAudioStream(s->stream);
    stems_.clear();
    position_ = 0;
}

double StemPlayer::duration() const {
    double d = 0;
    for (auto& s : stems_) d = std::max(d, s->seconds());
    return d;
}

// Seconds into the song: what was queued minus what is still waiting.
double StemPlayer::playhead() const {
    if (stems_.empty() || !stems_[0]->stream) return position_;
    auto& s = *stems_[0];
    double queued = double(SDL_GetAudioStreamQueued(s.stream)) / 2 / s.spec.channels / s.spec.freq;
    return std::clamp(duration() - queued, 0.0, duration());
}

void StemPlayer::applyGain(Stem& s) {
    if (s.stream) SDL_SetAudioStreamGain(s.stream, s.muted ? 0.f : 1.f);
}

void StemPlayer::refill(double seconds) {
    position_ = std::clamp(seconds, 0.0, duration());
    for (auto& s : stems_) {
        if (!s->stream) continue;
        SDL_ClearAudioStream(s->stream);
        size_t start = std::min(size_t(position_ * s->spec.freq) * size_t(s->spec.channels), s->pcm.size());
        SDL_PutAudioStreamData(s->stream, s->pcm.data() + start, int((s->pcm.size() - start) * 2));
        double pad = duration() - s->seconds();
        if (pad > 0) {  // every stream ends together (the playhead reads stems_[0])
            std::vector<int16_t> zeros(size_t(pad * s->spec.freq) * size_t(s->spec.channels), 0);
            SDL_PutAudioStreamData(s->stream, zeros.data(), int(zeros.size() * 2));
        }
        applyGain(*s);
    }
}

void StemPlayer::seek(double seconds) {
    if (playing_) SDL_PauseAudioDevice(device_);
    refill(seconds);
    if (playing_) SDL_ResumeAudioDevice(device_);
}

void StemPlayer::togglePlay() {
    if (!device_ || stems_.empty()) return;
    if (playing_) {
        position_ = playhead();
        SDL_PauseAudioDevice(device_);
    } else {
        if (playhead() >= duration() - 0.01) refill(0);
        SDL_ResumeAudioDevice(device_);
    }
    playing_ = !playing_;
}

void StemPlayer::update() {
    if (playing_ && duration() > 0 && playhead() >= duration() - 0.01) {
        playing_ = false;
        SDL_PauseAudioDevice(device_);
        position_ = 0;
    }
}

void StemPlayer::toggleMute(size_t i) {
    if (i >= stems_.size()) return;
    stems_[i]->muted = !stems_[i]->muted;
    applyGain(*stems_[i]);
}

void StemPlayer::solo(size_t i) {
    for (size_t k = 0; k < stems_.size(); ++k) {
        stems_[k]->muted = k != i;
        applyGain(*stems_[k]);
    }
}

void StemPlayer::setAllMuted(bool muted, bool exceptMix) {
    for (auto& s : stems_) {
        s->muted = (exceptMix && s->name == "mix" && stems_.size() > 1) ? true : muted;
        applyGain(*s);
    }
}
