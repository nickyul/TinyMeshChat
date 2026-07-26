#include "tmc/audio/rtp_audio_engine.h"
#include "tmc/audio/remote_audio_stream.h"
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
constexpr int MaxQueuedPlaybackFrames = 1;
constexpr int ExpectedPacketLossPercent = 3;
constexpr float LimiterThreshold = 30000.0F;
constexpr float PcmPeak = 32767.0F;
constexpr size_t MaxMicrophoneTestSamples = SampleRate * 10;
constexpr size_t RingCapacity = 1 << 16;
constexpr double MeterFloorDb = -60.0;
constexpr double MeterCeilingDb = -6.0;
constexpr double MeterAttack = 0.65;
constexpr double MeterRelease = 0.12;

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
    quint32 rtpTimestamp{0};
    QByteArray payload;
    qint64 transportReceivedAtNs{0};
};

struct DeviceChoice {
    QString name;
    ma_device_id id{};
};

} // namespace

struct RtpAudioEngine::State {
    explicit State(RtpAudioEngine* owner, AudioPreferences initialPreferences)
        : owner(owner), preferences(std::move(initialPreferences)) {
    }

    ~State() {
        stopWorker();
        if (contextInitialized) {
            ma_context_uninit(&context);
        }
    }

    RtpAudioEngine* owner{};
    AudioPreferences preferences;
    ma_context context{};
    ma_device captureDevice{};
    ma_device playbackDevice{};
    bool contextInitialized{false};
    bool captureDeviceInitialized{false};
    bool playbackDeviceInitialized{false};
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
    QHash<QString, int> pendingPacketLoss;
    std::jthread worker;
    OpusEncoder* encoder{};
    SpeexEchoState* echo{};
    SpeexPreprocessState* preprocess{};
    std::unordered_map<std::string, std::unique_ptr<RemoteAudioStream>> remotes;
    std::unordered_map<std::string, int> peerVolumes;
    std::unordered_map<std::string, int> peerPacketLoss;
    std::deque<opus_int16> testSamples;
    bool testPlaybackActive{false};
    double meterLevel{0.0};
    quint32 nextSequence{0};
    int configuredPacketLoss{ExpectedPacketLossPercent};

    static void captureCallback(ma_device* device, void* output, const void* input,
                                ma_uint32 frameCount);
    static void playbackCallback(ma_device* device, void* output, const void* input,
                                 ma_uint32 frameCount);
    void run(std::stop_token stopToken);
    void processIncoming();
    void processCapture();
    void mixPlayback();
    void stopWorker();
};

void RtpAudioEngine::State::playbackCallback(ma_device* device, void* output, const void*,
                                             ma_uint32 frameCount) {
    auto* state = static_cast<State*>(device->pUserData);
    auto* outputSamples = static_cast<opus_int16*>(output);
    const auto popped = state->playbackRing.pop(outputSamples, frameCount);
    std::fill(outputSamples + popped, outputSamples + frameCount, opus_int16{0});
    if (!state->muted.load(std::memory_order_relaxed)) {
        state->renderRing.push(outputSamples,
                               std::min(static_cast<size_t>(frameCount),
                                        state->renderRing.freeSpace()));
    }
}

void RtpAudioEngine::State::captureCallback(ma_device* device, void*, const void* input,
                                            ma_uint32 frameCount) {
    auto* state = static_cast<State*>(device->pUserData);
    if (!input) {
        return;
    }
    const auto* inputSamples = static_cast<const opus_int16*>(input);
    if (!state->muted.load(std::memory_order_relaxed)) {
        state->captureRing.push(inputSamples,
                                std::min(static_cast<size_t>(frameCount),
                                         state->captureRing.freeSpace()));
    }
}

void RtpAudioEngine::State::run(std::stop_token stopToken) {
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
                QPointer<RtpAudioEngine> target(owner);
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
            const auto rms = [](const auto& samples) {
                double energy = 0.0;
                for (const auto sample : samples) {
                    const auto normalized = static_cast<double>(sample) / 32768.0;
                    energy += normalized * normalized;
                }
                return std::sqrt(energy / samples.size());
            };
            const auto rawLevel = rms(capture);
            const auto renderLevel = rms(render);
            if (preferences.echoCancellation && echo) {
                speex_echo_cancellation(echo, capture.data(), render.data(), processed.data());
            } else {
                processed = capture;
            }
            if (preprocess && (preferences.noiseSuppression || preferences.automaticGainControl)) {
                speex_preprocess_run(preprocess, processed.data());
            }
            auto processedLevel = rms(processed);
            if (preferences.echoCancellation && rawLevel > 0.001 && renderLevel < 0.003 &&
                processedLevel < rawLevel * 0.08) {
                processed = capture;
                processedLevel = rawLevel;
            }
            const auto dspFinishedAtNs = monotonicNs();

            const auto levelDb = 20.0 * std::log10((std::max)(processedLevel, 1e-9));
            const auto meterTarget =
                std::clamp((levelDb - MeterFloorDb) / (MeterCeilingDb - MeterFloorDb), 0.0, 1.0);
            const auto smoothing = meterTarget > meterLevel ? MeterAttack : MeterRelease;
            meterLevel += (meterTarget - meterLevel) * smoothing;
            if (meterTarget == 0.0 && meterLevel < 0.005) {
                meterLevel = 0.0;
            }
            const auto level = meterLevel;
            QPointer<RtpAudioEngine> target(owner);
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
        mixPlayback();

        const auto statsAtNs = monotonicNs();
        for (auto& [peerId, remote] : remotes) {
            Q_UNUSED(peerId)
            const auto stats = remote->takeStats(statsAtNs);
            if (!stats) {
                continue;
            }
            QPointer<RtpAudioEngine> target(owner);
            const auto remotePeerId = QString::fromStdString(peerId);
            QMetaObject::invokeMethod(
                owner,
                [target, remotePeerId, stats = *stats] {
                    if (target) {
                        emit target->networkStatsChanged(remotePeerId,
                                                         stats.packetLossPercent,
                                                         stats.jitterMs,
                                                         stats.bufferMs);
                    }
                },
                Qt::QueuedConnection);
        }

        std::unique_lock lock(queueMutex);
        queueCondition.wait_for(lock, std::chrono::milliseconds(5));
    }
}

void RtpAudioEngine::State::processIncoming() {
    std::deque<EncodedFrame> frames;
    QStringList peersToRemove;
    QHash<QString, int> volumes;
    QHash<QString, int> packetLoss;
    {
        std::lock_guard lock(queueMutex);
        frames.swap(incoming);
        peersToRemove.swap(removals);
        volumes.swap(pendingVolumes);
        packetLoss.swap(pendingPacketLoss);
    }
    for (const auto& peerId : peersToRemove) {
        const auto key = peerId.toStdString();
        remotes.erase(key);
        peerPacketLoss.erase(key);
    }
    for (auto it = packetLoss.cbegin(); it != packetLoss.cend(); ++it) {
        peerPacketLoss[it.key().toStdString()] = it.value();
    }
    int worstPacketLoss = ExpectedPacketLossPercent;
    for (const auto& [peerId, loss] : peerPacketLoss) {
        Q_UNUSED(peerId)
        worstPacketLoss = std::max(worstPacketLoss, loss);
    }
    if (encoder && worstPacketLoss != configuredPacketLoss) {
        opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(worstPacketLoss));
        opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(worstPacketLoss >= 2 ? 1 : 0));
        configuredPacketLoss = worstPacketLoss;
    }
    for (auto it = volumes.cbegin(); it != volumes.cend(); ++it) {
        const auto key = it.key().toStdString();
        peerVolumes[key] = it.value();
        const auto remote = remotes.find(key);
        if (remote != remotes.end()) {
            remote->second->setVolume(it.value());
        }
    }
    for (auto& frame : frames) {
        const auto key = frame.peerId.toStdString();
        auto& remote = remotes[key];
        if (!remote) {
            const auto volume = peerVolumes.find(key);
            remote = std::make_unique<RemoteAudioStream>(
                frame.peerId, volume == peerVolumes.end() ? 100 : volume->second);
            if (!remote->valid()) {
                remotes.erase(key);
                continue;
            }
        }
        remote->enqueue(frame.rtpTimestamp, std::move(frame.payload),
                        frame.transportReceivedAtNs);
    }
}

void RtpAudioEngine::State::mixPlayback() {
    if (playbackRing.freeSpace() < FrameSamples ||
        playbackRing.available() >= FrameSamples * MaxQueuedPlaybackFrames) {
        return;
    }
    const bool suppressOutput = deafened.load(std::memory_order_relaxed);
    std::array<opus_int16, FrameSamples> mixed{};
    std::vector<std::pair<float, RemoteAudioStream::PcmFrame>> decodedRemotes;
    decodedRemotes.reserve(remotes.size());
    for (auto& [peerId, remote] : remotes) {
        Q_UNUSED(peerId)
        RemoteAudioStream::PcmFrame decoded{};
        if (remote->render(decoded) && !suppressOutput) {
            decodedRemotes.emplace_back(remote->volume(), std::move(decoded));
        }
    }
    const auto master = static_cast<float>(outputVolume.load(std::memory_order_relaxed)) / 100.0F;
    for (int index = 0; index < FrameSamples; ++index) {
        float sum = 0.0F;
        for (const auto& [volume, decoded] : decodedRemotes) {
            sum += static_cast<float>(decoded[index]) * volume;
        }
        if (!suppressOutput && testPlaybackActive && !testSamples.empty()) {
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
        QPointer<RtpAudioEngine> target(owner);
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

void RtpAudioEngine::State::stopWorker() {
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
    meterLevel = 0.0;
}

RtpAudioEngine::RtpAudioEngine(AudioPreferences preferences, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(this, std::move(preferences))) {
    state_->outputVolume.store(state_->preferences.outputVolume, std::memory_order_relaxed);
}

RtpAudioEngine::~RtpAudioEngine() {
    stop();
}

AudioDeviceLists RtpAudioEngine::refreshDevices() {
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

Result<void> RtpAudioEngine::start() {
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
    if (!state_->echo || !state_->preprocess) {
        state_->stopWorker();
        return Result<void>::failure("Не удалось инициализировать Speex DSP.");
    }

    int sampleRate = SampleRate;
    int denoise = state_->preferences.noiseSuppression ? 1 : 0;
    int agc = state_->preferences.automaticGainControl ? 1 : 0;
    float agcLevel = 12000.0F;
    const auto echoRateResult =
        speex_echo_ctl(state_->echo, SPEEX_ECHO_SET_SAMPLING_RATE, &sampleRate);
    const auto echoStateResult =
        state_->preferences.echoCancellation
            ? speex_preprocess_ctl(state_->preprocess, SPEEX_PREPROCESS_SET_ECHO_STATE, state_->echo)
            : 0;
    const auto denoiseResult =
        speex_preprocess_ctl(state_->preprocess, SPEEX_PREPROCESS_SET_DENOISE, &denoise);
    const auto agcResult =
        speex_preprocess_ctl(state_->preprocess, SPEEX_PREPROCESS_SET_AGC, &agc);
    const auto agcLevelResult =
        speex_preprocess_ctl(state_->preprocess, SPEEX_PREPROCESS_SET_AGC_LEVEL, &agcLevel);
    if (echoRateResult != 0 || echoStateResult != 0 || denoiseResult != 0 || agcResult != 0 ||
        agcLevelResult != 0) {
        state_->stopWorker();
        return Result<void>::failure("Не удалось настроить Speex DSP.");
    }

    auto captureConfig = ma_device_config_init(ma_device_type_capture);
    captureConfig.capture.format = ma_format_s16;
    captureConfig.capture.channels = Channels;
    captureConfig.sampleRate = SampleRate;
    captureConfig.periodSizeInFrames = FrameSamples / 2;
    captureConfig.periods = 3;
    captureConfig.dataCallback = State::captureCallback;
    captureConfig.pUserData = state_.get();
    auto playbackConfig = ma_device_config_init(ma_device_type_playback);
    playbackConfig.playback.format = ma_format_s16;
    playbackConfig.playback.channels = Channels;
    playbackConfig.sampleRate = SampleRate;
    playbackConfig.periodSizeInFrames = FrameSamples / 2;
    playbackConfig.periods = 3;
    playbackConfig.dataCallback = State::playbackCallback;
    playbackConfig.pUserData = state_.get();
    const auto capture = std::find_if(
        state_->captureDevices.cbegin(), state_->captureDevices.cend(),
        [this](const auto& device) { return device.name == state_->preferences.captureDevice; });
    const auto playback = std::find_if(
        state_->playbackDevices.cbegin(), state_->playbackDevices.cend(),
        [this](const auto& device) { return device.name == state_->preferences.playbackDevice; });
    if (capture != state_->captureDevices.cend()) {
        captureConfig.capture.pDeviceID = &capture->id;
    }
    if (playback != state_->playbackDevices.cend()) {
        playbackConfig.playback.pDeviceID = &playback->id;
    }
    const auto captureInit =
        ma_device_init(&state_->context, &captureConfig, &state_->captureDevice);
    if (captureInit != MA_SUCCESS) {
        state_->stopWorker();
        return Result<void>::failure(
            QString("Не удалось открыть микрофон: %1")
                .arg(QString::fromUtf8(ma_result_description(captureInit))));
    }
    state_->captureDeviceInitialized = true;
    const auto playbackInit =
        ma_device_init(&state_->context, &playbackConfig, &state_->playbackDevice);
    if (playbackInit != MA_SUCCESS) {
        ma_device_uninit(&state_->captureDevice);
        state_->captureDeviceInitialized = false;
        state_->stopWorker();
        return Result<void>::failure(
            QString("Не удалось открыть устройство воспроизведения: %1")
                .arg(QString::fromUtf8(ma_result_description(playbackInit))));
    }
    state_->playbackDeviceInitialized = true;
    std::array<char, MA_MAX_DEVICE_NAME_LENGTH + 1> captureName{};
    std::array<char, MA_MAX_DEVICE_NAME_LENGTH + 1> playbackName{};
    ma_device_get_name(&state_->captureDevice, ma_device_type_capture, captureName.data(),
                       captureName.size(), nullptr);
    ma_device_get_name(&state_->playbackDevice, ma_device_type_playback, playbackName.data(),
                       playbackName.size(), nullptr);
    Logger::instance().log(
        QtInfoMsg, "audio",
        QString("backend=%1 capture='%2' playback='%3' rate=%4 period=%5x%6")
            .arg(QString::fromUtf8(ma_get_backend_name(state_->context.backend)),
                 QString::fromUtf8(captureName.data()), QString::fromUtf8(playbackName.data()))
            .arg(state_->captureDevice.sampleRate)
            .arg(captureConfig.periodSizeInFrames)
            .arg(captureConfig.periods));
    state_->captureRing.clear();
    state_->playbackRing.clear();
    state_->renderRing.clear();
    std::array<opus_int16, FrameSamples> initialRenderDelay{};
    state_->renderRing.push(initialRenderDelay.data(), initialRenderDelay.size());
    state_->configuredPacketLoss = ExpectedPacketLossPercent;
    state_->running.store(true, std::memory_order_release);
    state_->worker =
        std::jthread([state = state_.get()](std::stop_token token) { state->run(token); });
    const auto playbackStart = ma_device_start(&state_->playbackDevice);
    if (playbackStart != MA_SUCCESS) {
        stop();
        return Result<void>::failure(
            QString("Не удалось запустить устройство воспроизведения: %1")
                .arg(QString::fromUtf8(ma_result_description(playbackStart))));
    }
    const auto captureStart = ma_device_start(&state_->captureDevice);
    if (captureStart != MA_SUCCESS) {
        stop();
        return Result<void>::failure(
            QString("Не удалось запустить микрофон: %1")
                .arg(QString::fromUtf8(ma_result_description(captureStart))));
    }
    return Result<void>::success();
}

void RtpAudioEngine::stop() {
    if (state_->captureDeviceInitialized) {
        ma_device_uninit(&state_->captureDevice);
        state_->captureDeviceInitialized = false;
    }
    if (state_->playbackDeviceInitialized) {
        ma_device_uninit(&state_->playbackDevice);
        state_->playbackDeviceInitialized = false;
    }
    state_->running.store(false, std::memory_order_release);
    state_->stopWorker();
    state_->captureRing.clear();
    state_->playbackRing.clear();
    state_->renderRing.clear();
}

Result<void> RtpAudioEngine::applyPreferences(const AudioPreferences& preferences) {
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

void RtpAudioEngine::setMuted(bool muted) {
    state_->muted.store(muted, std::memory_order_relaxed);
    if (muted) {
        emit microphoneLevelChanged(0.0);
    }
}

void RtpAudioEngine::setDeafened(bool deafened) {
    state_->deafened.store(deafened, std::memory_order_relaxed);
    if (deafened) {
        state_->playbackRing.clear();
    }
}

void RtpAudioEngine::setMicrophoneTest(bool enabled) {
    state_->micTest.store(enabled, std::memory_order_release);
    if (enabled) {
        state_->resetMicTest.store(true, std::memory_order_release);
    } else {
        state_->startMicTestPlayback.store(true, std::memory_order_release);
    }
    state_->queueCondition.notify_one();
}

void RtpAudioEngine::setPeerVolume(const QString& peerId, int percent) {
    if (peerId.isEmpty()) {
        return;
    }
    std::lock_guard lock(state_->queueMutex);
    state_->pendingVolumes[peerId] = std::clamp(percent, 0, 200);
    state_->queueCondition.notify_one();
}

void RtpAudioEngine::setPeerNetworkLoss(const QString& peerId, double packetLossPercent) {
    if (peerId.isEmpty()) {
        return;
    }
    std::lock_guard lock(state_->queueMutex);
    state_->pendingPacketLoss[peerId] =
        std::clamp(static_cast<int>(std::ceil(packetLossPercent)), 0, 20);
    state_->queueCondition.notify_one();
}

bool RtpAudioEngine::isRunning() const {
    return state_->running.load(std::memory_order_acquire);
}

bool RtpAudioEngine::isDeafened() const {
    return state_->deafened.load(std::memory_order_relaxed);
}

bool RtpAudioEngine::microphoneTest() const {
    return state_->micTest.load(std::memory_order_relaxed);
}

AudioPreferences RtpAudioEngine::preferences() const {
    return state_->preferences;
}

void RtpAudioEngine::receiveFrame(const QString& peerId, quint32 rtpTimestamp,
                                  const QByteArray& opusPayload,
                                  qint64 transportReceivedAtNs) {
    if (!isRunning() || peerId.isEmpty() || opusPayload.isEmpty() ||
        opusPayload.size() > MaxOpusPacketBytes) {
        return;
    }
    std::lock_guard lock(state_->queueMutex);
    if (state_->incoming.size() >= 256) {
        state_->incoming.pop_front();
    }
    state_->incoming.push_back({peerId, rtpTimestamp, opusPayload, transportReceivedAtNs});
    state_->queueCondition.notify_one();
}

void RtpAudioEngine::removePeer(const QString& peerId) {
    std::lock_guard lock(state_->queueMutex);
    state_->removals.append(peerId);
    state_->queueCondition.notify_one();
}

} // namespace tmc
