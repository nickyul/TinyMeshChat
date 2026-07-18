#include "tmc/audio/audio_engine.h"

#include "tmc/core/logger.h"

#include <QHash>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <opus/opus.h>
#include <speex/speex_echo.h>
#include <speex/speex_preprocess.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace tmc {

namespace {

constexpr int SampleRate = 48000;
constexpr int Channels = 1;
constexpr int FrameSamples = 960;
constexpr int MaxOpusPacketBytes = 4000;
constexpr int MinJitterFrames = 3;
constexpr int MaxJitterFrames = 10;
constexpr int MaxQueuedPlaybackFrames = 2;
constexpr int ExpectedPacketLossPercent = 3;
constexpr float LimiterThreshold = 30000.0F;
constexpr float PcmPeak = 32767.0F;
constexpr size_t MaxMicrophoneTestSamples = SampleRate * 10;
constexpr size_t RingCapacity = 1 << 16;

qint64 monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

double elapsedMs(qint64 startNs, qint64 endNs) {
    return static_cast<double>(endNs - startNs) / 1'000'000.0;
}

template <typename T, size_t Capacity> class SpscRing {
public:
    size_t available() const {
        const auto read = read_.load(std::memory_order_acquire);
        const auto write = write_.load(std::memory_order_acquire);
        return write - read;
    }

    size_t freeSpace() const {
        return Capacity - available();
    }

    size_t push(const T* values, size_t count) {
        const auto write = write_.load(std::memory_order_relaxed);
        const auto read = read_.load(std::memory_order_acquire);
        const auto writable = (std::min)(count, Capacity - (write - read));
        for (size_t index = 0; index < writable; ++index) {
            data_[(write + index) & (Capacity - 1)] = values[index];
        }
        write_.store(write + writable, std::memory_order_release);
        return writable;
    }

    size_t pop(T* values, size_t count) {
        const auto read = read_.load(std::memory_order_relaxed);
        const auto write = write_.load(std::memory_order_acquire);
        const auto readable = (std::min)(count, write - read);
        for (size_t index = 0; index < readable; ++index) {
            values[index] = data_[(read + index) & (Capacity - 1)];
        }
        read_.store(read + readable, std::memory_order_release);
        return readable;
    }

    void clear() {
        read_.store(0, std::memory_order_release);
        write_.store(0, std::memory_order_release);
    }

private:
    static_assert((Capacity & (Capacity - 1)) == 0);
    std::array<T, Capacity> data_{};
    std::atomic<size_t> read_{0};
    std::atomic<size_t> write_{0};
};

struct EncodedFrame {
    QString peerId;
    quint32 sequence{0};
    QByteArray payload;
    qint64 transportReceivedAtNs{0};
    qint64 audioQueuedAtNs{0};
};

struct ReceivedPacket {
    QByteArray payload;
    qint64 transportReceivedAtNs{0};
    qint64 audioQueuedAtNs{0};
};

struct DeviceChoice {
    QString name;
    ma_device_id id{};
};

struct RemoteAudio {
    OpusDecoder* decoder{};
    QString peerId;
    std::map<quint32, ReceivedPacket> packets;
    std::deque<opus_int16> samples;
    quint32 expectedSequence{0};
    bool hasSequence{false};
    bool started{false};
    int targetFrames{MinJitterFrames};
    int stableFrames{0};
    float volume{1.0F};

    ~RemoteAudio() {
        if (decoder) {
            opus_decoder_destroy(decoder);
        }
    }
};

} // namespace

struct AudioEngine::State {
    explicit State(AudioEngine* owner, AudioPreferences initialPreferences)
        : owner(owner), preferences(std::move(initialPreferences)) {
    }

    ~State() {
        stopWorker();
        if (contextInitialized) {
            ma_context_uninit(&context);
        }
    }

    AudioEngine* owner{};
    AudioPreferences preferences;
    ma_context context{};
    ma_device device{};
    bool contextInitialized{false};
    bool deviceInitialized{false};
    std::atomic_bool running{false};
    std::atomic_bool muted{false};
    std::atomic_bool deafened{false};
    std::atomic_bool micTest{false};
    std::atomic_bool resetMicTest{false};
    std::atomic_bool startMicTestPlayback{false};
    std::atomic<int> outputVolume{100};
    SpscRing<opus_int16, RingCapacity> captureRing;
    SpscRing<opus_int16, RingCapacity> playbackRing;
    SpscRing<opus_int16, RingCapacity> renderRing;
    std::vector<DeviceChoice> captureDevices;
    std::vector<DeviceChoice> playbackDevices;
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::deque<EncodedFrame> incoming;
    QStringList removals;
    QHash<QString, int> pendingVolumes;
    std::jthread worker;
    OpusEncoder* encoder{};
    SpeexEchoState* echo{};
    SpeexPreprocessState* preprocess{};
    std::unordered_map<std::string, std::unique_ptr<RemoteAudio>> remotes;
    std::unordered_map<std::string, int> peerVolumes;
    std::deque<opus_int16> testSamples;
    bool testPlaybackActive{false};
    quint32 nextSequence{0};

    static void callback(ma_device* device, void* output, const void* input, ma_uint32 frameCount);
    void run(std::stop_token stopToken);
    void processIncoming();
    void processCapture();
    void processRemote(RemoteAudio& remote);
    void mixPlayback();
    void stopWorker();
};

void AudioEngine::State::callback(ma_device* device, void* output, const void* input,
                                  ma_uint32 frameCount) {
    auto* state = static_cast<State*>(device->pUserData);
    auto* outputSamples = static_cast<opus_int16*>(output);
    const auto popped = state->playbackRing.pop(outputSamples, frameCount);
    std::fill(outputSamples + popped, outputSamples + frameCount, opus_int16{0});
    if (input && !state->muted.load(std::memory_order_relaxed)) {
        const auto writable =
            (std::min)({static_cast<size_t>(frameCount), state->captureRing.freeSpace(),
                        state->renderRing.freeSpace()});
        state->renderRing.push(outputSamples, writable);
        state->captureRing.push(static_cast<const opus_int16*>(input), writable);
    }
}

void AudioEngine::State::run(std::stop_token stopToken) {
    std::array<opus_int16, FrameSamples> capture{};
    std::array<opus_int16, FrameSamples> render{};
    std::array<opus_int16, FrameSamples> processed{};
    std::array<unsigned char, MaxOpusPacketBytes> encoded{};

    while (!stopToken.stop_requested()) {
        if (resetMicTest.exchange(false, std::memory_order_acq_rel)) {
            testSamples.clear();
            testPlaybackActive = false;
        }
        if (startMicTestPlayback.exchange(false, std::memory_order_acq_rel)) {
            testPlaybackActive = !testSamples.empty();
            if (!testPlaybackActive) {
                QPointer<AudioEngine> target(owner);
                QMetaObject::invokeMethod(
                    owner,
                    [target] {
                        if (target) {
                            emit target->microphoneTestPlaybackFinished();
                        }
                    },
                    Qt::QueuedConnection);
            }
        }
        processIncoming();
        while (captureRing.available() >= FrameSamples) {
            const auto captureAvailable = captureRing.available();
            captureRing.pop(capture.data(), capture.size());
            const auto renderCount = renderRing.pop(render.data(), render.size());
            std::fill(render.begin() + static_cast<ptrdiff_t>(renderCount), render.end(), 0);

            const auto dspStartedAtNs = monotonicNs();
            if (preferences.echoCancellation && echo) {
                speex_echo_cancellation(echo, capture.data(), render.data(), processed.data());
            } else {
                processed = capture;
            }
            if (preprocess && (preferences.noiseSuppression || preferences.automaticGainControl)) {
                speex_preprocess_run(preprocess, processed.data());
            }
            const auto dspFinishedAtNs = monotonicNs();

            double energy = 0.0;
            for (const auto sample : processed) {
                const auto normalized = static_cast<double>(sample) / 32768.0;
                energy += normalized * normalized;
            }
            const auto level = std::clamp(std::sqrt(energy / FrameSamples), 0.0, 1.0);
            QPointer<AudioEngine> target(owner);
            QMetaObject::invokeMethod(
                owner,
                [target, level] {
                    if (target) {
                        emit target->microphoneLevelChanged(level);
                    }
                },
                Qt::QueuedConnection);

            if (micTest.load(std::memory_order_relaxed)) {
                const auto available = MaxMicrophoneTestSamples -
                                       (std::min)(MaxMicrophoneTestSamples, testSamples.size());
                const auto count = (std::min)(available, processed.size());
                testSamples.insert(testSamples.end(), processed.begin(), processed.begin() + count);
            }
            const auto encodeStartedAtNs = monotonicNs();
            const auto encodedSize = opus_encode(encoder, processed.data(), FrameSamples,
                                                 encoded.data(), encoded.size());
            const auto encodedAtNs = monotonicNs();
            if (encodedSize > 0) {
                const QByteArray packet(reinterpret_cast<const char*>(encoded.data()), encodedSize);
                const auto sequence = nextSequence++;
                const VoiceFrameTiming timing{encodedAtNs,
                                              static_cast<double>(captureAvailable - FrameSamples) *
                                                  1000.0 / SampleRate,
                                              elapsedMs(dspStartedAtNs, dspFinishedAtNs),
                                              elapsedMs(encodeStartedAtNs, encodedAtNs)};
                QMetaObject::invokeMethod(
                    owner,
                    [target, sequence, packet, timing] {
                        if (target) {
                            emit target->encodedFrameReady(sequence, packet, timing);
                        }
                    },
                    Qt::QueuedConnection);
            }
        }
        for (auto& [peerId, remote] : remotes) {
            Q_UNUSED(peerId)
            processRemote(*remote);
        }
        mixPlayback();

        std::unique_lock lock(queueMutex);
        queueCondition.wait_for(lock, std::chrono::milliseconds(5));
    }
}

void AudioEngine::State::processIncoming() {
    std::deque<EncodedFrame> frames;
    QStringList peersToRemove;
    QHash<QString, int> volumes;
    {
        std::lock_guard lock(queueMutex);
        frames.swap(incoming);
        peersToRemove.swap(removals);
        volumes.swap(pendingVolumes);
    }
    for (const auto& peerId : peersToRemove) {
        remotes.erase(peerId.toStdString());
    }
    for (auto it = volumes.cbegin(); it != volumes.cend(); ++it) {
        const auto key = it.key().toStdString();
        peerVolumes[key] = it.value();
        const auto remote = remotes.find(key);
        if (remote != remotes.end()) {
            remote->second->volume = static_cast<float>(it.value()) / 100.0F;
        }
    }
    for (auto& frame : frames) {
        auto& remote = remotes[frame.peerId.toStdString()];
        if (!remote) {
            remote = std::make_unique<RemoteAudio>();
            remote->peerId = frame.peerId;
            int error = OPUS_OK;
            remote->decoder = opus_decoder_create(SampleRate, Channels, &error);
            if (!remote->decoder || error != OPUS_OK) {
                remotes.erase(frame.peerId.toStdString());
                continue;
            }
            if (const auto volume = peerVolumes.find(frame.peerId.toStdString());
                volume != peerVolumes.end()) {
                remote->volume = static_cast<float>(volume->second) / 100.0F;
            }
        }
        remote->packets.emplace(frame.sequence,
                                ReceivedPacket{std::move(frame.payload),
                                               frame.transportReceivedAtNs, frame.audioQueuedAtNs});
        while (remote->packets.size() > 32) {
            remote->packets.erase(remote->packets.begin());
        }
    }
}

void AudioEngine::State::processRemote(RemoteAudio& remote) {
    if (!remote.started) {
        if (static_cast<int>(remote.packets.size()) < remote.targetFrames) {
            return;
        }
        remote.expectedSequence = remote.packets.begin()->first;
        remote.hasSequence = true;
        remote.started = true;
    }
    while (remote.samples.size() < static_cast<size_t>(FrameSamples * remote.targetFrames) &&
           !remote.packets.empty()) {
        const auto exact = remote.packets.find(remote.expectedSequence);
        std::array<opus_int16, FrameSamples> decoded{};
        if (exact != remote.packets.end()) {
            const auto packet = std::move(exact->second);
            const auto decodeStartedAtNs = monotonicNs();
            const auto count = opus_decode(
                remote.decoder, reinterpret_cast<const unsigned char*>(packet.payload.constData()),
                packet.payload.size(), decoded.data(), decoded.size(), 0);
            const auto decodedAtNs = monotonicNs();
            remote.packets.erase(exact);
            if (count > 0) {
                const auto pcmQueuedMs =
                    static_cast<double>(remote.samples.size() + count) * 1000.0 / SampleRate;
                const auto playbackQueuedMs =
                    static_cast<double>(playbackRing.available()) * 1000.0 / SampleRate;
                Logger::instance().trace(
                    "voice_rx",
                    QString("peer=%1 seq=%2 transport_to_audio_ms=%3 worker_queue_ms=%4 "
                            "decode_ms=%5 pcm_queue_ms=%6 playback_queue_ms=%7 target_ms=%8")
                        .arg(remote.peerId.left(8))
                        .arg(remote.expectedSequence)
                        .arg(elapsedMs(packet.transportReceivedAtNs, packet.audioQueuedAtNs), 0,
                             'f', 3)
                        .arg(elapsedMs(packet.audioQueuedAtNs, decodeStartedAtNs), 0, 'f', 3)
                        .arg(elapsedMs(decodeStartedAtNs, decodedAtNs), 0, 'f', 3)
                        .arg(pcmQueuedMs, 0, 'f', 1)
                        .arg(playbackQueuedMs, 0, 'f', 1)
                        .arg(remote.targetFrames * 20));
                remote.samples.insert(remote.samples.end(), decoded.begin(),
                                      decoded.begin() + count);
            }
            ++remote.expectedSequence;
            if (++remote.stableFrames >= 250 && remote.targetFrames > MinJitterFrames) {
                --remote.targetFrames;
                remote.stableFrames = 0;
            }
            continue;
        }

        const auto next = remote.packets.begin();
        if (static_cast<qint32>(next->first - remote.expectedSequence) <= 0) {
            remote.packets.erase(next);
            continue;
        }
        if (remote.samples.size() >= FrameSamples) {
            break;
        }
        int count = 0;
        if (next->first == remote.expectedSequence + 1) {
            count = opus_decode(
                remote.decoder,
                reinterpret_cast<const unsigned char*>(next->second.payload.constData()),
                next->second.payload.size(), decoded.data(), decoded.size(), 1);
        }
        if (count <= 0) {
            count = opus_decode(remote.decoder, nullptr, 0, decoded.data(), decoded.size(), 0);
        }
        if (count > 0) {
            remote.samples.insert(remote.samples.end(), decoded.begin(), decoded.begin() + count);
        }
        ++remote.expectedSequence;
        remote.targetFrames = (std::min)(MaxJitterFrames, remote.targetFrames + 1);
        remote.stableFrames = 0;
    }
}

void AudioEngine::State::mixPlayback() {
    if (playbackRing.freeSpace() < FrameSamples ||
        playbackRing.available() >= FrameSamples * MaxQueuedPlaybackFrames) {
        return;
    }
    bool hasAudio = testPlaybackActive && !testSamples.empty();
    for (const auto& [peerId, remote] : remotes) {
        Q_UNUSED(peerId)
        hasAudio = hasAudio || !remote->samples.empty();
    }
    if (deafened.load(std::memory_order_relaxed)) {
        const auto testWasPlaying = testPlaybackActive;
        testSamples.clear();
        testPlaybackActive = false;
        for (auto& [peerId, remote] : remotes) {
            Q_UNUSED(peerId)
            remote->samples.clear();
            remote->packets.clear();
            remote->started = false;
        }
        if (testWasPlaying) {
            QPointer<AudioEngine> target(owner);
            QMetaObject::invokeMethod(
                owner,
                [target] {
                    if (target) {
                        emit target->microphoneTestPlaybackFinished();
                    }
                },
                Qt::QueuedConnection);
        }
        return;
    }
    if (!hasAudio) {
        return;
    }
    std::array<opus_int16, FrameSamples> mixed{};
    const auto master = static_cast<float>(outputVolume.load(std::memory_order_relaxed)) / 100.0F;
    for (int index = 0; index < FrameSamples; ++index) {
        float sum = 0.0F;
        for (auto& [peerId, remote] : remotes) {
            Q_UNUSED(peerId)
            if (!remote->samples.empty()) {
                sum += static_cast<float>(remote->samples.front()) * remote->volume;
                remote->samples.pop_front();
            }
        }
        if (testPlaybackActive && !testSamples.empty()) {
            sum += static_cast<float>(testSamples.front());
            testSamples.pop_front();
        }
        sum *= master;
        auto limited = sum;
        const auto magnitude = std::abs(sum);
        if (magnitude > LimiterThreshold) {
            const auto headroom = PcmPeak - LimiterThreshold;
            const auto compressed =
                LimiterThreshold + headroom * std::tanh((magnitude - LimiterThreshold) / headroom);
            limited = std::copysign(compressed, sum);
        }
        mixed[index] = static_cast<opus_int16>(std::clamp(limited, -32768.0F, 32767.0F));
    }
    playbackRing.push(mixed.data(), mixed.size());
    if (testPlaybackActive && testSamples.empty()) {
        testPlaybackActive = false;
        QPointer<AudioEngine> target(owner);
        QMetaObject::invokeMethod(
            owner,
            [target] {
                if (target) {
                    emit target->microphoneTestPlaybackFinished();
                }
            },
            Qt::QueuedConnection);
    }
}

void AudioEngine::State::stopWorker() {
    if (worker.joinable()) {
        worker.request_stop();
        queueCondition.notify_all();
        worker.join();
    }
    if (preprocess) {
        speex_preprocess_state_destroy(preprocess);
        preprocess = nullptr;
    }
    if (echo) {
        speex_echo_state_destroy(echo);
        echo = nullptr;
    }
    if (encoder) {
        opus_encoder_destroy(encoder);
        encoder = nullptr;
    }
    remotes.clear();
    testSamples.clear();
    testPlaybackActive = false;
}

AudioEngine::AudioEngine(AudioPreferences preferences, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(preferences))) {
    state_->outputVolume.store(state_->preferences.outputVolume, std::memory_order_relaxed);
}

AudioEngine::~AudioEngine() {
    stop();
}

AudioDeviceLists AudioEngine::refreshDevices() {
    AudioDeviceLists result;
    if (!state_->contextInitialized) {
        if (ma_context_init(nullptr, 0, nullptr, &state_->context) != MA_SUCCESS) {
            emit errorOccurred("Не удалось инициализировать аудиосистему.");
            return result;
        }
        state_->contextInitialized = true;
    }
    ma_device_info* playbackInfos{};
    ma_device_info* captureInfos{};
    ma_uint32 playbackCount{};
    ma_uint32 captureCount{};
    if (ma_context_get_devices(&state_->context, &playbackInfos, &playbackCount, &captureInfos,
                               &captureCount) != MA_SUCCESS) {
        emit errorOccurred("Не удалось получить список аудиоустройств.");
        return result;
    }
    state_->captureDevices.clear();
    state_->playbackDevices.clear();
    result.capture.append("Системное устройство по умолчанию");
    result.playback.append("Системное устройство по умолчанию");
    for (ma_uint32 index = 0; index < captureCount; ++index) {
        const auto name = QString::fromUtf8(captureInfos[index].name);
        state_->captureDevices.push_back({name, captureInfos[index].id});
        result.capture.append(name);
    }
    for (ma_uint32 index = 0; index < playbackCount; ++index) {
        const auto name = QString::fromUtf8(playbackInfos[index].name);
        state_->playbackDevices.push_back({name, playbackInfos[index].id});
        result.playback.append(name);
    }
    return result;
}

Result<void> AudioEngine::start() {
    if (state_->running.load(std::memory_order_acquire)) {
        return Result<void>::success();
    }
    if (!state_->preferences.isValid()) {
        return Result<void>::failure("Некорректные настройки аудио.");
    }
    refreshDevices();

    int opusError = OPUS_OK;
    state_->encoder = opus_encoder_create(SampleRate, Channels, OPUS_APPLICATION_VOIP, &opusError);
    if (!state_->encoder || opusError != OPUS_OK) {
        return Result<void>::failure(
            QString("Не удалось запустить Opus encoder: %1").arg(opus_strerror(opusError)));
    }
    opus_encoder_ctl(state_->encoder, OPUS_SET_BITRATE(state_->preferences.qualityKbps * 1000));
    opus_encoder_ctl(state_->encoder, OPUS_SET_COMPLEXITY(10));
    opus_encoder_ctl(state_->encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(state_->encoder, OPUS_SET_VBR(1));
    opus_encoder_ctl(state_->encoder, OPUS_SET_DTX(1));
    opus_encoder_ctl(state_->encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(state_->encoder, OPUS_SET_PACKET_LOSS_PERC(ExpectedPacketLossPercent));

    state_->echo = speex_echo_state_init(FrameSamples, FrameSamples * 10);
    state_->preprocess = speex_preprocess_state_init(FrameSamples, SampleRate);
    int sampleRate = SampleRate;
    speex_echo_ctl(state_->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &sampleRate);
    if (state_->preferences.echoCancellation) {
        speex_preprocess_ctl(state_->preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, state_->echo);
    }
    int denoise = state_->preferences.noiseSuppression ? 1 : 0;
    int agc = state_->preferences.automaticGainControl ? 1 : 0;
    int agcLevel = 12000;
    speex_preprocess_ctl(state_->preprocess, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
    speex_preprocess_ctl(state_->preprocess, SPEEX_PREPROCESS_SET_AGC, &agc);
    speex_preprocess_ctl(state_->preprocess, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agcLevel);

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
    const auto capture = std::find_if(
        state_->captureDevices.cbegin(), state_->captureDevices.cend(),
        [this](const auto& device) { return device.name == state_->preferences.captureDevice; });
    const auto playback = std::find_if(
        state_->playbackDevices.cbegin(), state_->playbackDevices.cend(),
        [this](const auto& device) { return device.name == state_->preferences.playbackDevice; });
    if (capture != state_->captureDevices.cend()) {
        config.capture.pDeviceID = &capture->id;
    }
    if (playback != state_->playbackDevices.cend()) {
        config.playback.pDeviceID = &playback->id;
    }
    const auto initResult = ma_device_init(&state_->context, &config, &state_->device);
    if (initResult != MA_SUCCESS) {
        state_->stopWorker();
        return Result<void>::failure(
            QString("Не удалось открыть микрофон или динамики: %1")
                .arg(QString::fromUtf8(ma_result_description(initResult))));
    }
    state_->deviceInitialized = true;
    state_->captureRing.clear();
    state_->playbackRing.clear();
    state_->renderRing.clear();
    std::array<opus_int16, FrameSamples> initialRenderDelay{};
    state_->renderRing.push(initialRenderDelay.data(), initialRenderDelay.size());
    state_->nextSequence = 0;
    state_->running.store(true, std::memory_order_release);
    state_->worker =
        std::jthread([state = state_.get()](std::stop_token token) { state->run(token); });
    const auto startResult = ma_device_start(&state_->device);
    if (startResult != MA_SUCCESS) {
        stop();
        return Result<void>::failure(
            QString("Не удалось запустить аудиоустройство: %1")
                .arg(QString::fromUtf8(ma_result_description(startResult))));
    }
    return Result<void>::success();
}

void AudioEngine::stop() {
    if (state_->deviceInitialized) {
        ma_device_uninit(&state_->device);
        state_->deviceInitialized = false;
    }
    state_->running.store(false, std::memory_order_release);
    state_->stopWorker();
    state_->captureRing.clear();
    state_->playbackRing.clear();
    state_->renderRing.clear();
}

Result<void> AudioEngine::applyPreferences(const AudioPreferences& preferences) {
    if (!preferences.isValid()) {
        return Result<void>::failure("Некорректные настройки аудио.");
    }
    const bool restart = isRunning();
    if (restart) {
        stop();
    }
    state_->preferences = preferences;
    state_->outputVolume.store(preferences.outputVolume, std::memory_order_relaxed);
    return restart ? start() : Result<void>::success();
}

void AudioEngine::setMuted(bool muted) {
    state_->muted.store(muted, std::memory_order_relaxed);
}

void AudioEngine::setDeafened(bool deafened) {
    state_->deafened.store(deafened, std::memory_order_relaxed);
    if (deafened) {
        state_->playbackRing.clear();
    }
}

void AudioEngine::setMicrophoneTest(bool enabled) {
    state_->micTest.store(enabled, std::memory_order_release);
    if (enabled) {
        state_->resetMicTest.store(true, std::memory_order_release);
    } else {
        state_->startMicTestPlayback.store(true, std::memory_order_release);
    }
    state_->queueCondition.notify_one();
}

void AudioEngine::setPeerVolume(const QString& peerId, int percent) {
    if (peerId.isEmpty()) {
        return;
    }
    std::lock_guard lock(state_->queueMutex);
    state_->pendingVolumes[peerId] = std::clamp(percent, 0, 200);
    state_->queueCondition.notify_one();
}

bool AudioEngine::isRunning() const {
    return state_->running.load(std::memory_order_acquire);
}

bool AudioEngine::isDeafened() const {
    return state_->deafened.load(std::memory_order_relaxed);
}

bool AudioEngine::microphoneTest() const {
    return state_->micTest.load(std::memory_order_relaxed);
}

AudioPreferences AudioEngine::preferences() const {
    return state_->preferences;
}

void AudioEngine::receiveFrame(const QString& peerId, quint32 sequence,
                               const QByteArray& opusPayload, qint64 transportReceivedAtNs) {
    if (!isRunning() || peerId.isEmpty() || opusPayload.isEmpty() ||
        opusPayload.size() > MaxOpusPacketBytes) {
        return;
    }
    std::lock_guard lock(state_->queueMutex);
    if (state_->incoming.size() >= 256) {
        state_->incoming.pop_front();
    }
    state_->incoming.push_back(
        {peerId, sequence, opusPayload, transportReceivedAtNs, monotonicNs()});
    state_->queueCondition.notify_one();
}

void AudioEngine::removePeer(const QString& peerId) {
    std::lock_guard lock(state_->queueMutex);
    state_->removals.append(peerId);
    state_->queueCondition.notify_one();
}

} // namespace tmc
