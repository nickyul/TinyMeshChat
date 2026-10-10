#pragma once

#include <QString>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace tmc {

// Bounded stereo mixer for voice and the selected screen audio stream.
class SharedAudioMixer final {
public:
    void setVoiceEnabled(bool enabled);
    void selectScreen(const QString& stream);
    bool wanted() const;
    void pushVoice(const int16_t* mono, size_t frames);
    void pushScreen(const QString& stream, const int16_t* pcm, size_t frames, int channels,
                    int rate);
    void render(int16_t* stereo, size_t frames);
    size_t takeRenderReference(int16_t* mono, size_t frames);
    size_t queuedVoiceFrames() const;
    int queuedDelayMs() const;
    void flush();
    void setDeafened(bool value);
    void setVolume(int value);
    void setScreenVolume(int value);

private:
    mutable std::mutex mutex_;
    QString selected_;
    bool voiceEnabled_{};
    std::deque<int16_t> voice_, screen_, reference_;
    std::atomic<bool> deafened_{};
    std::atomic<int> volume_{100}, screenVolume_{100};
};

} // namespace tmc
