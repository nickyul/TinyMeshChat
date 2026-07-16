#include "audio/audio_engine.h"

#include <QMetaObject>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <opus/opus.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

using namespace tmc;

namespace {
constexpr int SampleRate = 48000;
constexpr int Channels = 1;
constexpr int FrameSamples = 960; // 20 ms at 48 kHz
constexpr int InitialPlaybackSamples = FrameSamples * 3;
constexpr int MaxQueuedSamples = FrameSamples * 10; // cap latency at roughly 200 ms
constexpr int MaxOpusPacketBytes = 4000;

struct RemoteAudio {
    OpusDecoder* decoder{};
    std::deque<opus_int16> samples;
    quint32 lastSequence{};
    bool hasSequence{false};
    bool playbackStarted{false};

    RemoteAudio() = default;
    RemoteAudio(const RemoteAudio&) = delete;
    RemoteAudio& operator=(const RemoteAudio&) = delete;
    RemoteAudio(RemoteAudio&& other) noexcept
        : decoder(std::exchange(other.decoder, nullptr)), samples(std::move(other.samples)),
          lastSequence(other.lastSequence), hasSequence(other.hasSequence),
          playbackStarted(other.playbackStarted) {}
    RemoteAudio& operator=(RemoteAudio&& other) noexcept {
        if (this == &other)
            return *this;
        if (decoder)
            opus_decoder_destroy(decoder);
        decoder = std::exchange(other.decoder, nullptr);
        samples = std::move(other.samples);
        lastSequence = other.lastSequence;
        hasSequence = other.hasSequence;
        playbackStarted = other.playbackStarted;
        return *this;
    }
    ~RemoteAudio() {
        if (decoder)
            opus_decoder_destroy(decoder);
    }
};
} // namespace

struct AudioEngine::State {
    explicit State(AudioEngine* owner) : owner(owner) {}
    ~State() {
        if (encoder)
            opus_encoder_destroy(encoder);
    }

    AudioEngine* owner{};
    ma_device device{};
    bool deviceInitialized{false};
    bool running{false};
    std::atomic_bool muted{false};
    OpusEncoder* encoder{};
    quint32 nextSequence{0};
    std::vector<opus_int16> captureSamples;
    std::mutex playbackMutex;
    std::unordered_map<std::string, RemoteAudio> remotes;

    static void callback(ma_device* device, void* output, const void* input,
                         ma_uint32 frameCount);
};

void AudioEngine::State::callback(ma_device* device, void* output, const void* input,
                                  ma_uint32 frameCount) {
    auto* state = static_cast<State*>(device->pUserData);
    auto* outputSamples = static_cast<opus_int16*>(output);
    std::fill_n(outputSamples, frameCount, opus_int16{0});

    {
        std::lock_guard lock(state->playbackMutex);
        for (auto& [peerId, remote] : state->remotes) {
            Q_UNUSED(peerId)
            if (!remote.playbackStarted) {
                if (remote.samples.size() < InitialPlaybackSamples)
                    continue;
                remote.playbackStarted = true;
            }
            for (ma_uint32 i = 0; i < frameCount; ++i) {
                if (remote.samples.empty()) {
                    remote.playbackStarted = false;
                    break;
                }
                const int mixed = static_cast<int>(outputSamples[i]) + remote.samples.front();
                remote.samples.pop_front();
                outputSamples[i] = static_cast<opus_int16>(std::clamp(
                    mixed, static_cast<int>((std::numeric_limits<opus_int16>::min)()),
                    static_cast<int>((std::numeric_limits<opus_int16>::max)())));
            }
        }
    }

    if (!input || state->muted.load(std::memory_order_relaxed)) {
        state->captureSamples.clear();
        return;
    }

    const auto* inputSamples = static_cast<const opus_int16*>(input);
    state->captureSamples.insert(state->captureSamples.end(), inputSamples,
                                 inputSamples + frameCount);
    while (state->captureSamples.size() >= FrameSamples) {
        std::array<unsigned char, MaxOpusPacketBytes> encoded{};
        const auto encodedSize = opus_encode(state->encoder, state->captureSamples.data(),
                                             FrameSamples, encoded.data(), encoded.size());
        state->captureSamples.erase(state->captureSamples.begin(),
                                    state->captureSamples.begin() + FrameSamples);
        if (encodedSize <= 0)
            continue;
        const QByteArray packet(reinterpret_cast<const char*>(encoded.data()), encodedSize);
        emit state->owner->encodedFrameReady(state->nextSequence++, packet);
    }
}

AudioEngine::AudioEngine(QObject* parent) : QObject(parent), state_(std::make_unique<State>(this)) {}

AudioEngine::~AudioEngine() {
    stop();
}

Result<void> AudioEngine::start() {
    if (state_->running)
        return Result<void>::success();

    int opusError = OPUS_OK;
    if (!state_->encoder) {
        state_->encoder = opus_encoder_create(SampleRate, Channels, OPUS_APPLICATION_VOIP,
                                              &opusError);
        if (!state_->encoder || opusError != OPUS_OK)
            return Result<void>::failure(
                QString("Не удалось запустить Opus encoder: %1").arg(opus_strerror(opusError)));
        opus_encoder_ctl(state_->encoder, OPUS_SET_BITRATE(32000));
        opus_encoder_ctl(state_->encoder, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(state_->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }

    auto config = ma_device_config_init(ma_device_type_duplex);
    config.capture.format = ma_format_s16;
    config.capture.channels = Channels;
    config.playback.format = ma_format_s16;
    config.playback.channels = Channels;
    config.sampleRate = SampleRate;
    config.periodSizeInFrames = FrameSamples;
    config.periods = 3;
    config.dataCallback = State::callback;
    config.pUserData = state_.get();

    const auto initResult = ma_device_init(nullptr, &config, &state_->device);
    if (initResult != MA_SUCCESS)
        return Result<void>::failure(
            QString("Не удалось открыть микрофон или динамики: %1")
                .arg(QString::fromUtf8(ma_result_description(initResult))));
    state_->deviceInitialized = true;

    const auto startResult = ma_device_start(&state_->device);
    if (startResult != MA_SUCCESS) {
        ma_device_uninit(&state_->device);
        state_->deviceInitialized = false;
        return Result<void>::failure(
            QString("Не удалось запустить аудиоустройство: %1")
                .arg(QString::fromUtf8(ma_result_description(startResult))));
    }
    state_->captureSamples.clear();
    state_->nextSequence = 0;
    state_->running = true;
    return Result<void>::success();
}

void AudioEngine::stop() {
    if (state_->deviceInitialized) {
        ma_device_uninit(&state_->device);
        state_->deviceInitialized = false;
    }
    state_->running = false;
    state_->captureSamples.clear();
    std::lock_guard lock(state_->playbackMutex);
    state_->remotes.clear();
}

void AudioEngine::setMuted(bool muted) {
    state_->muted.store(muted, std::memory_order_relaxed);
}

bool AudioEngine::isRunning() const {
    return state_->running;
}

void AudioEngine::receiveFrame(const QString& peerId, quint32 sequence,
                               const QByteArray& opusPayload) {
    if (!state_->running || peerId.isEmpty() || opusPayload.isEmpty() ||
        opusPayload.size() > MaxOpusPacketBytes)
        return;

    std::lock_guard lock(state_->playbackMutex);
    auto& remote = state_->remotes[peerId.toStdString()];
    if (!remote.decoder) {
        int error = OPUS_OK;
        remote.decoder = opus_decoder_create(SampleRate, Channels, &error);
        if (!remote.decoder || error != OPUS_OK) {
            state_->remotes.erase(peerId.toStdString());
            emit errorOccurred(QString("Не удалось запустить Opus decoder: %1")
                                   .arg(opus_strerror(error)));
            return;
        }
    }

    if (remote.hasSequence) {
        const auto delta = static_cast<qint32>(sequence - remote.lastSequence);
        if (delta <= 0)
            return;
        const int missing = (std::min)(delta - 1, 3);
        for (int i = 0; i < missing; ++i) {
            std::array<opus_int16, FrameSamples> concealed{};
            const int decoded = opus_decode(remote.decoder, nullptr, 0, concealed.data(),
                                            FrameSamples, 0);
            if (decoded > 0)
                remote.samples.insert(remote.samples.end(), concealed.begin(),
                                      concealed.begin() + decoded);
        }
    }

    std::array<opus_int16, FrameSamples> decodedSamples{};
    const int decoded = opus_decode(
        remote.decoder, reinterpret_cast<const unsigned char*>(opusPayload.constData()),
        opusPayload.size(), decodedSamples.data(), FrameSamples, 0);
    if (decoded < 0)
        return;
    remote.samples.insert(remote.samples.end(), decodedSamples.begin(),
                          decodedSamples.begin() + decoded);
    while (remote.samples.size() > MaxQueuedSamples)
        remote.samples.pop_front();
    remote.lastSequence = sequence;
    remote.hasSequence = true;
}

void AudioEngine::removePeer(const QString& peerId) {
    std::lock_guard lock(state_->playbackMutex);
    state_->remotes.erase(peerId.toStdString());
}
