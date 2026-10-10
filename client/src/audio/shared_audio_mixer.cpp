#include "tmc/audio/shared_audio_mixer.h"

#include <algorithm>
#include <cmath>

namespace tmc {

void SharedAudioMixer::setVoiceEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    voiceEnabled_ = enabled;
    voice_.clear();
    reference_.clear();
}

void SharedAudioMixer::selectScreen(const QString& stream) {
    std::lock_guard lock(mutex_);
    selected_ = stream;
    screen_.clear();
}

bool SharedAudioMixer::wanted() const {
    std::lock_guard lock(mutex_);
    return voiceEnabled_ || !selected_.isEmpty();
}

void SharedAudioMixer::pushVoice(const int16_t* mono, size_t frames) {
    if (!mono || frames > 4800) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (!voiceEnabled_ || deafened_) {
        return;
    }
    voice_.insert(voice_.end(), mono, mono + frames);
    while (voice_.size() > 1920) {
        voice_.pop_front();
    }
}

void SharedAudioMixer::pushScreen(const QString& stream, const int16_t* pcm, size_t frames,
                                  int channels, int rate) {
    if (!pcm || !frames || frames > 4800 || rate != 48000 || (channels != 1 && channels != 2)) {
        return;
    }
    std::lock_guard lock(mutex_);
    if (stream.isEmpty() || stream != selected_ || deafened_) {
        return;
    }
    for (size_t i = 0; i < frames; ++i) {
        screen_.push_back(pcm[i * channels]);
        screen_.push_back(pcm[i * channels + channels - 1]);
    }
    while (screen_.size() > 9600) {
        screen_.pop_front();
        screen_.pop_front();
    }
}

void SharedAudioMixer::render(int16_t* stereo, size_t frames) {
    std::fill_n(stereo, frames * 2, 0);
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock) {
        return;
    }
    const float master = volume_.load() / 100.0f, screenGain = screenVolume_.load() / 100.0f;
    const bool muted = deafened_.load();
    for (size_t i = 0; i < frames; ++i) {
        float voice = 0;
        if (!voice_.empty()) {
            voice = voice_.front();
            voice_.pop_front();
        }
        int16_t played[2]{};
        for (int channel = 0; channel < 2; ++channel) {
            float screen = 0;
            if (!screen_.empty()) {
                screen = screen_.front();
                screen_.pop_front();
            }
            float sum = muted ? 0 : (voice + screen * screenGain) * master;
            if (std::abs(sum) > 30000) {
                sum = std::copysign(
                    30000.0f + 2767.0f * std::tanh((std::abs(sum) - 30000.0f) / 2767.0f), sum);
            }
            played[channel] = static_cast<int16_t>(std::clamp(sum, -32768.0f, 32767.0f));
            stereo[2 * i + channel] = played[channel];
        }
        if (voiceEnabled_) {
            reference_.push_back(static_cast<int16_t>((int(played[0]) + int(played[1])) / 2));
        }
    }
    while (reference_.size() > 4800) {
        reference_.pop_front();
    }
}

size_t SharedAudioMixer::takeRenderReference(int16_t* mono, size_t frames) {
    std::lock_guard lock(mutex_);
    const auto count = std::min(frames, reference_.size());
    for (size_t i = 0; i < count; ++i) {
        mono[i] = reference_.front();
        reference_.pop_front();
    }
    return count;
}

size_t SharedAudioMixer::queuedVoiceFrames() const {
    std::lock_guard lock(mutex_);
    return voice_.size();
}

int SharedAudioMixer::queuedDelayMs() const {
    std::lock_guard lock(mutex_);
    return std::max(voice_.size(), screen_.size() / 2) / 48;
}

void SharedAudioMixer::flush() {
    std::lock_guard lock(mutex_);
    voice_.clear();
    screen_.clear();
    reference_.clear();
}

void SharedAudioMixer::setDeafened(bool value) {
    std::lock_guard lock(mutex_);
    deafened_ = value;
    voice_.clear();
    screen_.clear();
}

void SharedAudioMixer::setVolume(int value) {
    volume_ = std::clamp(value, 0, 200);
}

void SharedAudioMixer::setScreenVolume(int value) {
    screenVolume_ = std::clamp(value, 0, 100);
}

} // namespace tmc
