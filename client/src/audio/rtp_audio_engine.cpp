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
#include <functional>
#include <mutex>
#include <opus/opus.h>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace tmc {

namespace {

constexpr int SampleRate = 48000;
constexpr int Channels = 1;
constexpr int ApmFrameSamples = 480;
constexpr int OpusFrameSamples = 960;
constexpr int MaxOpusPacketBytes = 4000;
constexpr int MaxQueuedPlaybackFrames = 2;
constexpr int ExpectedPacketLossPercent = 3;
constexpr qint64 RemoteTalkingTimeoutNs = 300'000'000LL;
constexpr auto WorkerWait = std::chrono::milliseconds(5);

constexpr float LimiterThreshold = 30000.0F;
constexpr float PcmPeak = 32767.0F;

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

class AudioEngineIngress final : public IncomingRtpAudioSink {
public:
    explicit AudioEngineIngress(std::function<void(IncomingRtpAudioFrame)> handler)
        : handler_(std::move(handler)) {
    }

    void enqueue(IncomingRtpAudioFrame frame) override {
        std::lock_guard lock(mutex_);
        if (handler_) {
            handler_(std::move(frame));
        }
    }

    void detach() {
        std::lock_guard lock(mutex_);
        handler_ = {};
    }

private:
    std::mutex mutex_;
    std::function<void(IncomingRtpAudioFrame)> handler_;
};

struct DeviceChoice {
    QString name;
    ma_device_id id{};
};

} // namespace

struct RtpAudioEngine::State {
    explicit State(RtpAudioEngine* owner, AudioPreferences initialPreferences)
        : owner(owner), preferences(std::move(initialPreferences)),
          ingress(std::make_shared<AudioEngineIngress>(
              [this](IncomingRtpAudioFrame frame) { enqueueIncoming(std::move(frame)); })) {
    }

    ~State() {
        ingress->detach();
        stopWorker();
        if (contextInitialized) {
            ma_context_uninit(&context);
        }
    }

    RtpAudioEngine* owner{};
    AudioPreferences preferences;

    // miniaudio context and devices
    ma_context context{};
    ma_device captureDevice{};
    ma_device playbackDevice{};
    bool contextInitialized{false};
    bool captureDeviceInitialized{false};
    bool playbackDeviceInitialized{false};

    // flags and state variables
    std::atomic_bool running{false};
    std::atomic_bool muted{false};
    std::atomic_bool deafened{false};
    std::atomic_bool micTest{false};
    std::atomic_bool resetMonitor{false};
    std::atomic<int> outputVolume{100};
    std::atomic<float> micLevel{0.0f};

    // ring buffers for audio data
    SpscRing<opus_int16, RingCapacity> captureRing;
    SpscRing<opus_int16, RingCapacity> playbackRing;
    SpscRing<opus_int16, RingCapacity> renderRing;

    // cache device lists
    std::vector<DeviceChoice> captureDevices;
    std::vector<DeviceChoice> playbackDevices;

    // queue network commands
    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::deque<IncomingRtpAudioFrame> incoming;
    QStringList removals;
    QHash<QString, int> pendingVolumes;
    QHash<QString, int> pendingPacketLoss;

    std::shared_ptr<AudioEngineIngress> ingress;
    std::mutex outgoingSinkMutex;
    std::weak_ptr<OutgoingOpusAudioSink> outgoingSink;

    // audio processing and encoding
    std::jthread worker;
    OpusEncoder* encoder{};
    webrtc::scoped_refptr<webrtc::AudioProcessing> apm;
    std::unordered_map<std::string, std::unique_ptr<RemoteAudioStream>> remotes;
    std::unordered_map<std::string, int> peerVolumes;
    std::unordered_map<std::string, int> peerPacketLoss;
    std::unordered_map<std::string, qint64> remoteMediaAtNs;
    std::unordered_map<std::string, bool> remoteTalking;
    std::deque<opus_int16> monitorSamples;
    double meterLevel{0.0};
    quint32 nextRtpTimestamp{0};
    int configuredPacketLoss{ExpectedPacketLossPercent};
    int streamDelayMs{0};
    bool apmErrorReported{false};
    std::atomic_bool isTalking{false};
    std::atomic_bool pttPressed{false};
    int hangoverFramesCounter{0};

    static void captureCallback(ma_device* device, void* output, const void* input,
                                ma_uint32 frameCount);
    static void playbackCallback(ma_device* device, void* output, const void* input,
                                 ma_uint32 frameCount);
    void run(std::stop_token stopToken);
    void processIncoming();
    void mixPlayback();
    void updateRemoteTalking(qint64 nowNs);
    void setLocalTalking(bool talking);
    void setPeerTalking(const std::string& peerId, bool talking);
    void reportApmError(int error);
    void enqueueIncoming(IncomingRtpAudioFrame frame);
    void sendEncoded(quint32 rtpTimestamp, QByteArray payload);
    void stopWorker();
};

void RtpAudioEngine::State::playbackCallback(ma_device* device, void* output, const void*,
                                             ma_uint32 frameCount) {
    auto* state = static_cast<State*>(device->pUserData);
    auto* outputSamples = static_cast<opus_int16*>(output);
    const auto popped = state->playbackRing.pop(outputSamples, frameCount);
    std::fill(outputSamples + popped, outputSamples + frameCount, opus_int16{0});

    if (state->deafened.load(std::memory_order_relaxed)) {
        std::fill(outputSamples, outputSamples + frameCount, opus_int16{0});
    }
    state->renderRing.push(
        outputSamples, std::min(static_cast<size_t>(frameCount), state->renderRing.freeSpace()));
    state->queueCondition.notify_one();
}

void RtpAudioEngine::State::captureCallback(ma_device* device, void*, const void* input,
                                            ma_uint32 frameCount) {
    auto* state = static_cast<State*>(device->pUserData);
    if (!input) {
        return;
    }
    const auto* inputSamples = static_cast<const opus_int16*>(input);
    state->captureRing.push(
        inputSamples, std::min(static_cast<size_t>(frameCount), state->captureRing.freeSpace()));
    state->queueCondition.notify_one();
}

void RtpAudioEngine::State::run(std::stop_token stopToken) {
    std::array<opus_int16, ApmFrameSamples> capture{};
    std::array<opus_int16, ApmFrameSamples> render{};
    std::array<opus_int16, ApmFrameSamples> processed{};
    std::array<opus_int16, OpusFrameSamples> opusFrame{};
    std::array<unsigned char, MaxOpusPacketBytes> encoded{};
    webrtc::StreamConfig streamConfig(SampleRate, Channels);
    size_t opusOffset = 0;
    bool opusFrameShouldTransmit = false;

    while (!stopToken.stop_requested()) {
        bool didWork = false;
        if (resetMonitor.exchange(false, std::memory_order_acq_rel)) {
            monitorSamples.clear();
        }
        processIncoming();
        while (captureRing.available() >= ApmFrameSamples) {
            didWork = true;
            captureRing.pop(capture.data(), capture.size());

            const auto renderCount = renderRing.pop(render.data(), render.size());
            std::fill(render.begin() + static_cast<ptrdiff_t>(renderCount), render.end(), 0);

            int apmResult =
                apm->ProcessReverseStream(render.data(), streamConfig, streamConfig, render.data());
            if (apmResult == webrtc::AudioProcessing::kNoError && preferences.echoCancellation) {
                apmResult = apm->set_stream_delay_ms(streamDelayMs);
            }
            if (apmResult == webrtc::AudioProcessing::kNoError) {
                apmResult = apm->ProcessStream(capture.data(), streamConfig, streamConfig,
                                               processed.data());
            }
            if (apmResult != webrtc::AudioProcessing::kNoError) {
                processed = capture;
                reportApmError(apmResult);
            }

            const auto rms = [](const auto& samples) {
                double energy = 0.0;
                for (const auto sample : samples) {
                    const auto normalized = static_cast<double>(sample) / 32768.0;
                    energy += normalized * normalized;
                }
                return std::sqrt(energy / samples.size());
            };

            const auto processedLevel = rms(processed);

            const auto levelDb = 20.0 * std::log10((std::max)(processedLevel, 1e-9));
            const auto meterTarget =
                std::clamp((levelDb - MeterFloorDb) / (MeterCeilingDb - MeterFloorDb), 0.0, 1.0);
            const auto smoothing = meterTarget > meterLevel ? MeterAttack : MeterRelease;
            meterLevel += (meterTarget - meterLevel) * smoothing;
            if (meterTarget == 0.0 && meterLevel < 0.005) {
                meterLevel = 0.0;
            }

            const bool isMuted = muted.load(std::memory_order_relaxed);
            const bool isMicTesting = micTest.load(std::memory_order_relaxed);
            micLevel.store(static_cast<float>(isMuted && !isMicTesting ? 0.0 : meterLevel),
                           std::memory_order_relaxed);

            bool voiceDetected = false;
            if (!isMuted && !isMicTesting &&
                preferences.inputMode == AudioInputMode::VoiceActivity) {
                voiceDetected = (meterLevel >= preferences.vadThreshold);
            } else if (!isMuted && !isMicTesting &&
                       preferences.inputMode == AudioInputMode::PushToTalk) {
                voiceDetected = pttPressed.load(std::memory_order_relaxed);
            }

            bool shouldTransmit = false;
            if (preferences.inputMode == AudioInputMode::VoiceActivity && !isMuted &&
                !isMicTesting) {
                if (voiceDetected) {
                    hangoverFramesCounter = (preferences.vadHangoverMs + 9) / 10;
                } else if (hangoverFramesCounter > 0) {
                    --hangoverFramesCounter;
                }
                shouldTransmit = voiceDetected || hangoverFramesCounter > 0;
            } else {
                hangoverFramesCounter = 0;
                shouldTransmit = voiceDetected;
            }

            setLocalTalking(shouldTransmit);

            if (isMicTesting) {
                monitorSamples.insert(monitorSamples.end(), processed.begin(), processed.end());
                while (monitorSamples.size() > static_cast<size_t>(OpusFrameSamples * 2)) {
                    monitorSamples.pop_front();
                }
            }

            auto* opusDestination = opusFrame.data() + static_cast<ptrdiff_t>(opusOffset);
            if (isMuted || isMicTesting ||
                (preferences.inputMode == AudioInputMode::PushToTalk && !shouldTransmit)) {
                std::fill(opusDestination, opusDestination + ApmFrameSamples, opus_int16{0});
            } else {
                std::copy(processed.begin(), processed.end(), opusDestination);
            }
            opusOffset += ApmFrameSamples;
            opusFrameShouldTransmit = opusFrameShouldTransmit || shouldTransmit;

            if (opusOffset == OpusFrameSamples) {
                const auto timestamp = nextRtpTimestamp;
                nextRtpTimestamp += OpusFrameSamples;
                const bool encodeFrame = opusFrameShouldTransmit && !isMuted && !isMicTesting;
                opusOffset = 0;
                opusFrameShouldTransmit = false;

                if (encodeFrame) {
                    const auto encodedSize =
                        opus_encode(encoder, opusFrame.data(), OpusFrameSamples, encoded.data(),
                                    encoded.size());
                    if (encodedSize > 0) {
                        sendEncoded(
                            timestamp,
                            QByteArray(reinterpret_cast<const char*>(encoded.data()), encodedSize));
                    }
                }
            }
        }

        const auto playbackBefore = playbackRing.available();
        mixPlayback();
        didWork = didWork || playbackRing.available() != playbackBefore;

        const auto statsAtNs = monotonicNs();
        updateRemoteTalking(statsAtNs);
        for (auto& [peerId, remote] : remotes) {
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
                        emit target->networkStatsChanged(remotePeerId, stats.packetLossPercent,
                                                         stats.jitterMs, stats.bufferMs);
                    }
                },
                Qt::QueuedConnection);
        }

        if (!didWork) {
            std::unique_lock lock(queueMutex);
            queueCondition.wait_for(lock, WorkerWait, [&] {
                return stopToken.stop_requested() || !incoming.empty() || !removals.isEmpty() ||
                       !pendingVolumes.isEmpty() || !pendingPacketLoss.isEmpty() ||
                       captureRing.available() >= ApmFrameSamples;
            });
        }
    }

    setLocalTalking(false);
}

void RtpAudioEngine::State::processIncoming() {
    std::deque<IncomingRtpAudioFrame> frames;
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
        setPeerTalking(key, false);
        remotes.erase(key);
        peerPacketLoss.erase(key);
        peerVolumes.erase(key);
        remoteMediaAtNs.erase(key);
        remoteTalking.erase(key);
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
        peerVolumes[it.key().toStdString()] = it.value();
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
        if (remote->enqueue(frame.ssrc, frame.sequenceNumber, frame.rtpTimestamp,
                            std::move(frame.opusPayload), frame.receivedAtNs)) {
            remoteMediaAtNs[key] = frame.receivedAtNs;
            setPeerTalking(key, true);
        }
    }
}

void RtpAudioEngine::State::mixPlayback() {
    if (playbackRing.freeSpace() < OpusFrameSamples ||
        playbackRing.available() > OpusFrameSamples * (MaxQueuedPlaybackFrames - 1)) {
        return;
    }
    const bool suppressOutput = deafened.load(std::memory_order_relaxed);
    const bool isTestMode = micTest.load(std::memory_order_relaxed);
    std::array<opus_int16, OpusFrameSamples> mixed{};
    std::vector<std::pair<float, RemoteAudioStream::PcmFrame>> decodedRemotes;
    decodedRemotes.reserve(remotes.size());
    for (auto& [peerId, remote] : remotes) {
        Q_UNUSED(peerId)
        RemoteAudioStream::PcmFrame decoded{};
        if (remote->render(decoded) && !suppressOutput) {
            decodedRemotes.emplace_back(remote->volume(), std::move(decoded));
        }
    }

    for (int index = 0; index < OpusFrameSamples; ++index) {
        float sum = 0.0F;
        for (const auto& [volume, decoded] : decodedRemotes) {
            sum += static_cast<float>(decoded[index]) * volume;
        }
        if (isTestMode && !suppressOutput && !monitorSamples.empty()) {
            sum += static_cast<float>(monitorSamples.front());
            monitorSamples.pop_front();
        }
        mixed[index] = static_cast<opus_int16>(std::clamp(sum, -32768.0F, 32767.0F));
    }

    const auto master = static_cast<float>(outputVolume.load(std::memory_order_relaxed)) / 100.0F;
    for (int index = 0; index < OpusFrameSamples; ++index) {
        float sample = static_cast<float>(mixed[index]) * master;
        const auto magnitude = std::abs(sample);
        if (magnitude > LimiterThreshold) {
            const auto headroom = PcmPeak - LimiterThreshold;
            const auto compressed =
                LimiterThreshold + headroom * std::tanh((magnitude - LimiterThreshold) / headroom);
            sample = std::copysign(compressed, sample);
        }
        mixed[index] = static_cast<opus_int16>(std::clamp(sample, -32768.0F, 32767.0F));
    }

    playbackRing.push(mixed.data(), mixed.size());
}

void RtpAudioEngine::State::updateRemoteTalking(qint64 nowNs) {
    for (const auto& [peerId, receivedAtNs] : remoteMediaAtNs) {
        if (nowNs - receivedAtNs >= RemoteTalkingTimeoutNs) {
            setPeerTalking(peerId, false);
        }
    }
}

void RtpAudioEngine::State::setLocalTalking(bool talking) {
    const bool previous = isTalking.exchange(talking, std::memory_order_relaxed);
    if (previous == talking) {
        return;
    }
    QPointer<RtpAudioEngine> target(owner);
    QMetaObject::invokeMethod(
        owner,
        [target, talking] {
            if (target && target->state_->isTalking.load(std::memory_order_relaxed) == talking) {
                emit target->talkingStateChanged(talking);
            }
        },
        Qt::QueuedConnection);
}

void RtpAudioEngine::State::setPeerTalking(const std::string& peerId, bool talking) {
    auto& current = remoteTalking[peerId];
    if (current == talking) {
        return;
    }
    current = talking;
    QPointer<RtpAudioEngine> target(owner);
    const auto id = QString::fromStdString(peerId);
    QMetaObject::invokeMethod(
        owner,
        [target, id, talking] {
            if (target) {
                emit target->peerTalkingStateChanged(id, talking);
            }
        },
        Qt::QueuedConnection);
}

void RtpAudioEngine::State::reportApmError(int error) {
    if (apmErrorReported) {
        return;
    }
    apmErrorReported = true;
    QPointer<RtpAudioEngine> target(owner);
    QMetaObject::invokeMethod(
        owner,
        [target, error] {
            if (target) {
                emit target->errorOccurred(
                    QString("WebRTC APM вернул ошибку %1; используется необработанный микрофон.")
                        .arg(error));
            }
        },
        Qt::QueuedConnection);
}

void RtpAudioEngine::State::enqueueIncoming(IncomingRtpAudioFrame frame) {
    if (!running.load(std::memory_order_acquire) || frame.peerId.isEmpty() ||
        frame.opusPayload.isEmpty() || frame.opusPayload.size() > MaxOpusPacketBytes) {
        return;
    }
    std::lock_guard lock(queueMutex);
    if (incoming.size() >= 256) {
        incoming.pop_front();
    }
    incoming.push_back(std::move(frame));
    queueCondition.notify_one();
}

void RtpAudioEngine::State::sendEncoded(quint32 rtpTimestamp, QByteArray payload) {
    std::weak_ptr<OutgoingOpusAudioSink> sink;
    {
        std::lock_guard lock(outgoingSinkMutex);
        sink = outgoingSink;
    }
    if (const auto target = sink.lock()) {
        target->enqueue({rtpTimestamp, std::move(payload)});
    }
}

void RtpAudioEngine::State::stopWorker() {
    if (worker.joinable()) {
        worker.request_stop();
        queueCondition.notify_all();
        worker.join();
    }
    for (const auto& [peerId, talking] : remoteTalking) {
        if (talking) {
            setPeerTalking(peerId, false);
        }
    }
    setLocalTalking(false);
    if (encoder) {
        opus_encoder_destroy(encoder);
        encoder = nullptr;
    }
    apm = nullptr;
    remotes.clear();
    remoteMediaAtNs.clear();
    remoteTalking.clear();
    monitorSamples.clear();
    hangoverFramesCounter = 0;
    meterLevel = 0.0;
    micLevel.store(0.0f, std::memory_order_relaxed);
    {
        std::lock_guard lock(queueMutex);
        incoming.clear();
        removals.clear();
        pendingVolumes.clear();
        pendingPacketLoss.clear();
    }
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

    // create and configure Opus encoder
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

    // WebRTC APM consumes exactly 10 ms chunks. Opus packetization remains 20 ms.
    webrtc::AudioProcessing::Config config;
    config.echo_canceller.enabled = state_->preferences.echoCancellation;
    config.echo_canceller.enforce_high_pass_filtering = false;
    config.high_pass_filter.enabled = state_->preferences.highPassFilter;
    config.noise_suppression.enabled = state_->preferences.noiseSuppression;
    switch (state_->preferences.noiseSuppressionLevel) {
    case NoiseSuppressionLevel::Low:
        config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
        break;
    case NoiseSuppressionLevel::Moderate:
        config.noise_suppression.level =
            webrtc::AudioProcessing::Config::NoiseSuppression::kModerate;
        break;
    case NoiseSuppressionLevel::High:
        config.noise_suppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
        break;
    case NoiseSuppressionLevel::VeryHigh:
        config.noise_suppression.level =
            webrtc::AudioProcessing::Config::NoiseSuppression::kVeryHigh;
        break;
    }
    config.gain_controller1.enabled = state_->preferences.automaticGainControl;
    config.gain_controller1.mode =
        webrtc::AudioProcessing::Config::GainController1::kAdaptiveDigital;
    config.gain_controller1.target_level_dbfs = state_->preferences.agcTargetLevelDbfs;
    config.gain_controller1.compression_gain_db = state_->preferences.agcCompressionGainDb;
    config.gain_controller1.enable_limiter = state_->preferences.agcLimiter;
    config.gain_controller1.analog_gain_controller.enabled = false;
    state_->apm = webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
    if (!state_->apm) {
        state_->stopWorker();
        return Result<void>::failure("Не удалось создать WebRTC Audio Processing Module.");
    }
    const auto apmInitialize = state_->apm->Initialize();
    if (apmInitialize != webrtc::AudioProcessing::kNoError) {
        state_->stopWorker();
        return Result<void>::failure(
            QString("Не удалось инициализировать WebRTC APM: %1").arg(apmInitialize));
    }

    // miniaudio device configuration
    auto captureConfig = ma_device_config_init(ma_device_type_capture);
    captureConfig.capture.format = ma_format_s16;
    captureConfig.capture.channels = Channels;
    captureConfig.sampleRate = SampleRate;
    captureConfig.periodSizeInFrames = ApmFrameSamples / 2;
    captureConfig.periods = 2;
    captureConfig.performanceProfile = ma_performance_profile_low_latency;
    captureConfig.dataCallback = State::captureCallback;
    captureConfig.pUserData = state_.get();
    auto playbackConfig = ma_device_config_init(ma_device_type_playback);
    playbackConfig.playback.format = ma_format_s16;
    playbackConfig.playback.channels = Channels;
    playbackConfig.sampleRate = SampleRate;
    playbackConfig.periodSizeInFrames = ApmFrameSamples / 2;
    playbackConfig.periods = 2;
    playbackConfig.performanceProfile = ma_performance_profile_low_latency;
    playbackConfig.dataCallback = State::playbackCallback;
    playbackConfig.pUserData = state_.get();
    const auto capture = std::find_if(
        state_->captureDevices.cbegin(), state_->captureDevices.cend(),
        [this](const auto& device) { return device.name == state_->preferences.captureDevice; });
    const auto playback = std::find_if(
        state_->playbackDevices.cbegin(), state_->playbackDevices.cend(),
        [this](const auto& device) { return device.name == state_->preferences.playbackDevice; });
    if (!state_->preferences.captureDevice.isEmpty() && capture == state_->captureDevices.cend()) {
        state_->stopWorker();
        return Result<void>::failure("Выбранное устройство микрофона недоступно: " +
                                     state_->preferences.captureDevice);
    }
    if (!state_->preferences.playbackDevice.isEmpty() &&
        playback == state_->playbackDevices.cend()) {
        state_->stopWorker();
        return Result<void>::failure("Выбранное устройство воспроизведения недоступно: " +
                                     state_->preferences.playbackDevice);
    }
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
    state_->streamDelayMs = static_cast<int>(
        (static_cast<quint64>(captureConfig.periodSizeInFrames) * captureConfig.periods +
         static_cast<quint64>(playbackConfig.periodSizeInFrames) * playbackConfig.periods) *
        1000 / SampleRate);
    std::array<char, MA_MAX_DEVICE_NAME_LENGTH + 1> captureName{};
    std::array<char, MA_MAX_DEVICE_NAME_LENGTH + 1> playbackName{};
    ma_device_get_name(&state_->captureDevice, ma_device_type_capture, captureName.data(),
                       captureName.size(), nullptr);
    ma_device_get_name(&state_->playbackDevice, ma_device_type_playback, playbackName.data(),
                       playbackName.size(), nullptr);
    Logger::instance().log(QtInfoMsg, "audio",
                           QString("backend=%1 capture='%2' playback='%3' rate=%4 period=%5x%6")
                               .arg(QString::fromUtf8(ma_get_backend_name(state_->context.backend)),
                                    QString::fromUtf8(captureName.data()),
                                    QString::fromUtf8(playbackName.data()))
                               .arg(state_->captureDevice.sampleRate)
                               .arg(captureConfig.periodSizeInFrames)
                               .arg(captureConfig.periods));

    // clear ring buffers and start worker thread
    state_->captureRing.clear();
    state_->playbackRing.clear();
    state_->renderRing.clear();
    state_->monitorSamples.clear();
    state_->apmErrorReported = false;
    state_->hangoverFramesCounter = 0;
    state_->isTalking.store(false, std::memory_order_relaxed);
    state_->micLevel.store(0.0f, std::memory_order_relaxed);
    std::array<opus_int16, ApmFrameSamples> initialRenderDelay{};
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
    state_->monitorSamples.clear();
}

Result<void> RtpAudioEngine::applyPreferences(const AudioPreferences& preferences) {
    if (!preferences.isValid()) {
        return Result<void>::failure("Некорректные настройки аудио.");
    }
    if (!isRunning()) {
        refreshDevices();
        const auto captureAvailable =
            preferences.captureDevice.isEmpty() ||
            std::any_of(
                state_->captureDevices.cbegin(), state_->captureDevices.cend(),
                [&](const auto& device) { return device.name == preferences.captureDevice; });
        const auto playbackAvailable =
            preferences.playbackDevice.isEmpty() ||
            std::any_of(
                state_->playbackDevices.cbegin(), state_->playbackDevices.cend(),
                [&](const auto& device) { return device.name == preferences.playbackDevice; });
        if (!captureAvailable) {
            return Result<void>::failure("Выбранное устройство микрофона недоступно: " +
                                         preferences.captureDevice);
        }
        if (!playbackAvailable) {
            return Result<void>::failure("Выбранное устройство воспроизведения недоступно: " +
                                         preferences.playbackDevice);
        }
    }
    const auto previous = state_->preferences;
    const bool wasRunning = isRunning();
    if (wasRunning) {
        stop();
    }
    state_->preferences = preferences;
    state_->outputVolume.store(preferences.outputVolume, std::memory_order_relaxed);
    if (!wasRunning) {
        return Result<void>::success();
    }

    const auto applied = start();
    if (applied) {
        return Result<void>::success();
    }

    const auto applyError = applied.error();
    stop();
    state_->preferences = previous;
    state_->outputVolume.store(previous.outputVolume, std::memory_order_relaxed);
    const auto restored = start();
    if (!restored) {
        return Result<void>::failure(
            applyError +
            QStringLiteral("\nНе удалось восстановить предыдущую конфигурацию аудио: ") +
            restored.error());
    }
    return Result<void>::failure(applyError);
}

void RtpAudioEngine::setMuted(bool muted) {
    state_->muted.store(muted, std::memory_order_relaxed);
    if (muted) {
        if (!state_->micTest.load(std::memory_order_relaxed)) {
            state_->micLevel.store(0.0f, std::memory_order_relaxed);
        }
        state_->pttPressed.store(false, std::memory_order_relaxed);
        state_->setLocalTalking(false);
    }
    state_->queueCondition.notify_one();
}

void RtpAudioEngine::setDeafened(bool deafened) {
    state_->deafened.store(deafened, std::memory_order_relaxed);
    state_->queueCondition.notify_one();
}

void RtpAudioEngine::setMicrophoneTest(bool enabled) {
    state_->micTest.store(enabled, std::memory_order_release);
    state_->resetMonitor.store(true, std::memory_order_release);
    if (enabled) {
        state_->pttPressed.store(false, std::memory_order_relaxed);
        state_->setLocalTalking(false);
    }
    state_->queueCondition.notify_one();
}

void RtpAudioEngine::setPttPressed(bool pressed) {
    state_->pttPressed.store(pressed, std::memory_order_relaxed);
    if (!pressed && state_->preferences.inputMode == AudioInputMode::PushToTalk) {
        state_->setLocalTalking(false);
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

double RtpAudioEngine::microphoneLevel() const {
    return static_cast<double>(state_->micLevel.load(std::memory_order::relaxed));
}

AudioPreferences RtpAudioEngine::preferences() const {
    return state_->preferences;
}

std::shared_ptr<IncomingRtpAudioSink> RtpAudioEngine::incomingSink() const {
    return state_->ingress;
}

void RtpAudioEngine::setOutgoingSink(std::weak_ptr<OutgoingOpusAudioSink> sink) {
    std::lock_guard lock(state_->outgoingSinkMutex);
    state_->outgoingSink = std::move(sink);
}

void RtpAudioEngine::removePeer(const QString& peerId) {
    std::lock_guard lock(state_->queueMutex);
    state_->removals.append(peerId);
    state_->queueCondition.notify_one();
}

} // namespace tmc
