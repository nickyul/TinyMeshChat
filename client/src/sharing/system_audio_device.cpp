#include "system_audio_device.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <rtc_base/time_utils.h>

namespace tmc {

SystemAudioDevice::SystemAudioDevice(std::shared_ptr<SharedAudioOutput> output)
    : output_(std::move(output)) {
}

SystemAudioDevice::~SystemAudioDevice() {
    Terminate();
}

int32_t SystemAudioDevice::RegisterAudioCallback(webrtc::AudioTransport* callback) {
    std::lock_guard lock(mutex_);
    callback_ = callback;
    return 0;
}

int32_t SystemAudioDevice::Init() {
    if (initialized_.exchange(true)) {
        return 0;
    }
    pump_ = std::jthread([this](std::stop_token token) {
        auto next = std::chrono::steady_clock::now();
        while (!token.stop_requested()) {
            next += std::chrono::milliseconds(10);
            std::array<int16_t, 960> capture{}, render{};
            // Protect callback lifetime through the call; registration waits for in-flight calls.
            {
                std::lock_guard lock(mutex_);
                if (callback_) {
                    if (recording_) {
                        const auto count = std::min(capture.size(), samples_.size());
                        for (size_t i = 0; i < count; ++i) {
                            capture[i] = samples_.front();
                            samples_.pop_front();
                        }
                        uint32_t level = 100;
                        callback_->RecordedDataIsAvailable(capture.data(), 480, 2 * sizeof(int16_t),
                                                           2, 48000, output_->delayMs(), 0, 100,
                                                           false, level, webrtc::TimeNanos());
                    }
                    if (playing_) {
                        size_t count{};
                        int64_t elapsed{}, ntp{};
                        callback_->NeedMorePlayData(480, 2 * sizeof(int16_t), 2, 48000,
                                                    render.data(), count, &elapsed, &ntp);
                    }
                }
            }
            std::this_thread::sleep_until(next);
            if (next < std::chrono::steady_clock::now() - std::chrono::milliseconds(30)) {
                next = std::chrono::steady_clock::now();
            }
        }
    });
    return 0;
}

int32_t SystemAudioDevice::Terminate() {
    playing_ = false;
    recording_ = false;
    if (pump_.joinable()) {
        pump_.request_stop();
        pump_.join();
    }
    std::lock_guard lock(mutex_);
    callback_ = nullptr;
    samples_.clear();
    initialized_ = false;
    playoutReady_ = false;
    recordingReady_ = false;
    return 0;
}

void SystemAudioDevice::push(const float* stereo, size_t frames) {
    if (!stereo || frames > 8192) {
        return;
    }
    std::lock_guard lock(mutex_);
    for (size_t i = 0; i < frames * 2; ++i) {
        samples_.push_back(static_cast<int16_t>(
            (std::isfinite(stereo[i]) ? std::clamp(stereo[i], -1.0f, 1.0f) : 0.0f) * 32767.0f));
    }
    while (samples_.size() > 9600) {
        samples_.pop_front();
        samples_.pop_front();
    }
}

void SystemAudioDevice::clearCapture() {
    std::lock_guard lock(mutex_);
    samples_.clear();
}

int32_t SystemAudioDevice::PlayoutDeviceName(uint16_t, char* name, char* guid) {
    std::strcpy(name, "TinyMesh shared playback");
    std::strcpy(guid, "tinymesh-playback");
    return 0;
}

int32_t SystemAudioDevice::RecordingDeviceName(uint16_t, char* name, char* guid) {
    std::strcpy(name, "TinyMesh system audio");
    std::strcpy(guid, "tinymesh-system-audio");
    return 0;
}

} // namespace tmc
