#include "tmc/sharing/system_audio_capture.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <miniaudio.h>

namespace tmc {
struct SystemAudioCapture::Native {
    ma_device device{};
    bool initialized{false};
    Sink sink;
};
SystemAudioCapture::SystemAudioCapture(QObject* parent) : QObject(parent), native_(std::make_unique<Native>()) {}
SystemAudioCapture::~SystemAudioCapture() { stop(); }
bool SystemAudioCapture::supported() { return true; }
Result<void> SystemAudioCapture::start() {
    stop();
    native_->sink = makeSink();
    auto config = ma_device_config_init(ma_device_type_loopback);
    config.sampleRate = 48000;
    config.capture.format = ma_format_f32;
    config.capture.channels = 2;
    config.periodSizeInFrames = 960;
    config.pUserData = native_.get();
    config.dataCallback = [](ma_device* device, void*, const void* input, ma_uint32 count) {
        const auto* state = static_cast<Native*>(device->pUserData);
        state->sink(static_cast<const float*>(input), static_cast<int>(count));
    };
    if (ma_device_init(nullptr, &config, &native_->device) != MA_SUCCESS)
        return Result<void>::failure("Не удалось включить захват системного звука.");
    native_->initialized = true;
    if (ma_device_start(&native_->device) != MA_SUCCESS) {
        stop();
        return Result<void>::failure("Не удалось запустить захват системного звука.");
    }
    return Result<void>::success();
}
void SystemAudioCapture::stop() {
    invalidateSink();
    if (native_->initialized) ma_device_uninit(&native_->device);
    native_->initialized = false;
    native_->sink = {};
}
} // namespace tmc
