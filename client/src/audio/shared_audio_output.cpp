#include "tmc/audio/shared_audio_output.h"

#include "tmc/audio/shared_audio_mixer.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define MINIAUDIO_IMPLEMENTATION
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <miniaudio.h>
#include <mutex>

namespace tmc {

struct SharedAudioOutput::State {
    ma_context context{};
    ma_device device{};
    bool contextReady{false}, deviceReady{false};
    QString deviceName;
    SharedAudioMixer mixer;

    static void render(ma_device* device, void* output, const void*, ma_uint32 frames) {
        static_cast<State*>(device->pUserData)->mixer.render(static_cast<int16_t*>(output), frames);
    }
};

SharedAudioOutput::SharedAudioOutput() : state_(std::make_unique<State>()) {
}

SharedAudioOutput::~SharedAudioOutput() {
    if (state_->deviceReady) {
        ma_device_uninit(&state_->device);
    }
    if (state_->contextReady) {
        ma_context_uninit(&state_->context);
    }
}

Result<void> SharedAudioOutput::updateDevice() {
    const bool wanted = state_->mixer.wanted();
    if (!wanted) {
        if (state_->deviceReady) {
            ma_device_uninit(&state_->device);
            state_->deviceReady = false;
        }
        return Result<void>::success();
    }
    if (state_->deviceReady) {
        return Result<void>::success();
    }
    if (!state_->contextReady) {
        if (ma_context_init(nullptr, 0, nullptr, &state_->context) != MA_SUCCESS) {
            return Result<void>::failure("Не удалось открыть аудиовыход.");
        }
        state_->contextReady = true;
    }
    ma_device_info* devices{};
    ma_uint32 count{};
    ma_device_id id{};
    bool found = state_->deviceName.isEmpty();
    if (!found && ma_context_get_devices(&state_->context, &devices, &count, nullptr, nullptr) ==
                      MA_SUCCESS) {
        for (ma_uint32 i = 0; i < count; ++i) {
            if (QString::fromUtf8(devices[i].name) == state_->deviceName) {
                id = devices[i].id;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        return Result<void>::failure("Выбранный аудиовыход недоступен: " + state_->deviceName);
    }
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = 2;
    config.playback.pDeviceID = state_->deviceName.isEmpty() ? nullptr : &id;
    config.sampleRate = 48000;
    config.periodSizeInFrames = 480;
    config.periods = 2;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback = State::render;
    config.pUserData = state_.get();
    const auto initialized = ma_device_init(&state_->context, &config, &state_->device);
    if (initialized != MA_SUCCESS) {
        return Result<void>::failure("Не удалось подготовить аудиовыход: " +
                                     QString::fromUtf8(ma_result_description(initialized)));
    }
    state_->deviceReady = true;
    const auto started = ma_device_start(&state_->device);
    if (started != MA_SUCCESS) {
        ma_device_uninit(&state_->device);
        state_->deviceReady = false;
        return Result<void>::failure("Не удалось запустить аудиовыход: " +
                                     QString::fromUtf8(ma_result_description(started)));
    }
    return Result<void>::success();
}

Result<void> SharedAudioOutput::configure(const QString& device, int volume) {
    state_->mixer.setVolume(volume);
    if (device != state_->deviceName) {
        if (state_->deviceReady) {
            ma_device_uninit(&state_->device);
            state_->deviceReady = false;
        }
        state_->deviceName = device;
        state_->mixer.flush();
    }
    return updateDevice();
}

Result<void> SharedAudioOutput::setVoiceEnabled(bool enabled) {
    state_->mixer.setVoiceEnabled(enabled);
    return updateDevice();
}

Result<void> SharedAudioOutput::selectScreen(const QString& stream) {
    state_->mixer.selectScreen(stream);
    return updateDevice();
}

void SharedAudioOutput::pushVoice(const int16_t* mono, size_t frames) {
    state_->mixer.pushVoice(mono, frames);
}

void SharedAudioOutput::pushScreen(const QString& stream, const int16_t* pcm, size_t frames,
                                   int channels, int rate) {
    state_->mixer.pushScreen(stream, pcm, frames, channels, rate);
}

size_t SharedAudioOutput::takeRenderReference(int16_t* mono, size_t frames) {
    return state_->mixer.takeRenderReference(mono, frames);
}

size_t SharedAudioOutput::queuedVoiceFrames() const {
    return state_->mixer.queuedVoiceFrames();
}

int SharedAudioOutput::delayMs() const {
    return 20 + state_->mixer.queuedDelayMs();
}

void SharedAudioOutput::setDeafened(bool deafened) {
    state_->mixer.setDeafened(deafened);
}

void SharedAudioOutput::setScreenVolume(int percent) {
    state_->mixer.setScreenVolume(percent);
}

} // namespace tmc
