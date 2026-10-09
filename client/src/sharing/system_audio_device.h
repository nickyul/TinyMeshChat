#pragma once

#include "tmc/audio/shared_audio_output.h"

#include <api/audio/audio_device.h>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace tmc {

class SystemAudioDevice : public webrtc::AudioDeviceModule {
public:
    explicit SystemAudioDevice(std::shared_ptr<SharedAudioOutput> output);
    ~SystemAudioDevice() override;
    void push(const float* stereo, size_t frames);
    void clearCapture();

    int32_t ActiveAudioLayer(AudioLayer* layer) const override {
        *layer = kDummyAudio;
        return 0;
    }

    int32_t RegisterAudioCallback(webrtc::AudioTransport* callback) override;
    int32_t Init() override;
    int32_t Terminate() override;

    bool Initialized() const override {
        return initialized_;
    }

    int16_t PlayoutDevices() override {
        return 1;
    }

    int16_t RecordingDevices() override {
        return 1;
    }

    int32_t PlayoutDeviceName(uint16_t, char*, char*) override;
    int32_t RecordingDeviceName(uint16_t, char*, char*) override;

    int32_t SetPlayoutDevice(uint16_t) override {
        return 0;
    }

    int32_t SetPlayoutDevice(WindowsDeviceType) override {
        return 0;
    }

    int32_t SetRecordingDevice(uint16_t) override {
        return 0;
    }

    int32_t SetRecordingDevice(WindowsDeviceType) override {
        return 0;
    }

    int32_t PlayoutIsAvailable(bool* value) override {
        *value = true;
        return 0;
    }

    int32_t InitPlayout() override {
        playoutReady_ = true;
        return 0;
    }

    bool PlayoutIsInitialized() const override {
        return playoutReady_;
    }

    int32_t RecordingIsAvailable(bool* value) override {
        *value = true;
        return 0;
    }

    int32_t InitRecording() override {
        recordingReady_ = true;
        return 0;
    }

    bool RecordingIsInitialized() const override {
        return recordingReady_;
    }

    int32_t StartPlayout() override {
        playing_ = true;
        return 0;
    }

    int32_t StopPlayout() override {
        playing_ = false;
        return 0;
    }

    bool Playing() const override {
        return playing_;
    }

    int32_t StartRecording() override {
        recording_ = true;
        return 0;
    }

    int32_t StopRecording() override {
        recording_ = false;
        clearCapture();
        return 0;
    }

    bool Recording() const override {
        return recording_;
    }

    int32_t InitSpeaker() override {
        return 0;
    }

    bool SpeakerIsInitialized() const override {
        return initialized_;
    }

    int32_t InitMicrophone() override {
        return 0;
    }

    bool MicrophoneIsInitialized() const override {
        return initialized_;
    }

    int32_t SpeakerVolumeIsAvailable(bool* value) override {
        *value = false;
        return 0;
    }

    int32_t SetSpeakerVolume(uint32_t) override {
        return 0;
    }

    int32_t SpeakerVolume(uint32_t* value) const override {
        *value = 100;
        return 0;
    }

    int32_t MaxSpeakerVolume(uint32_t* value) const override {
        *value = 100;
        return 0;
    }

    int32_t MinSpeakerVolume(uint32_t* value) const override {
        *value = 0;
        return 0;
    }

    int32_t MicrophoneVolumeIsAvailable(bool* value) override {
        *value = false;
        return 0;
    }

    int32_t SetMicrophoneVolume(uint32_t) override {
        return 0;
    }

    int32_t MicrophoneVolume(uint32_t* value) const override {
        *value = 100;
        return 0;
    }

    int32_t MaxMicrophoneVolume(uint32_t* value) const override {
        *value = 100;
        return 0;
    }

    int32_t MinMicrophoneVolume(uint32_t* value) const override {
        *value = 0;
        return 0;
    }

    int32_t SpeakerMuteIsAvailable(bool* value) override {
        *value = false;
        return 0;
    }

    int32_t SetSpeakerMute(bool) override {
        return 0;
    }

    int32_t SpeakerMute(bool* value) const override {
        *value = false;
        return 0;
    }

    int32_t MicrophoneMuteIsAvailable(bool* value) override {
        *value = false;
        return 0;
    }

    int32_t SetMicrophoneMute(bool) override {
        return 0;
    }

    int32_t MicrophoneMute(bool* value) const override {
        *value = false;
        return 0;
    }

    int32_t StereoPlayoutIsAvailable(bool* value) const override {
        *value = true;
        return 0;
    }

    int32_t SetStereoPlayout(bool enabled) override {
        return enabled ? 0 : -1;
    }

    int32_t StereoPlayout(bool* value) const override {
        *value = true;
        return 0;
    }

    int32_t StereoRecordingIsAvailable(bool* value) const override {
        *value = true;
        return 0;
    }

    int32_t SetStereoRecording(bool enabled) override {
        return enabled ? 0 : -1;
    }

    int32_t StereoRecording(bool* value) const override {
        *value = true;
        return 0;
    }

    int32_t PlayoutDelay(uint16_t* value) const override {
        *value = static_cast<uint16_t>(output_->delayMs());
        return 0;
    }

    bool BuiltInAECIsAvailable() const override {
        return false;
    }

    bool BuiltInAGCIsAvailable() const override {
        return false;
    }

    bool BuiltInNSIsAvailable() const override {
        return false;
    }

    int32_t EnableBuiltInAEC(bool) override {
        return -1;
    }

    int32_t EnableBuiltInAGC(bool) override {
        return -1;
    }

    int32_t EnableBuiltInNS(bool) override {
        return -1;
    }

private:
    std::shared_ptr<SharedAudioOutput> output_;
    std::mutex mutex_;
    webrtc::AudioTransport* callback_{};
    std::deque<int16_t> samples_;
    std::atomic<bool> initialized_{false}, playoutReady_{false}, recordingReady_{false},
        playing_{false}, recording_{false};
    std::jthread pump_;
};

} // namespace tmc
