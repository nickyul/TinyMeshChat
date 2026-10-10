#pragma once

#include "tmc/core/result.h"

#include <QString>

#include <cstdint>
#include <memory>

namespace tmc {

class SharedAudioOutput final {
public:
    SharedAudioOutput();
    ~SharedAudioOutput();
    Result<void> configure(const QString& device, int volume);
    Result<void> setVoiceEnabled(bool enabled);
    Result<void> selectScreen(const QString& stream);
    void pushVoice(const int16_t* mono, size_t frames);
    void pushScreen(const QString& stream, const int16_t* pcm, size_t frames, int channels,
                    int sampleRate);
    size_t takeRenderReference(int16_t* mono, size_t frames);
    size_t queuedVoiceFrames() const;
    int delayMs() const;
    void setDeafened(bool deafened);
    void setScreenVolume(int percent);

private:
    struct State;
    std::unique_ptr<State> state_;
    Result<void> updateDevice();
};

} // namespace tmc
