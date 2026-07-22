#include "tmc/audio/remote_audio_stream.h"

#include <algorithm>
#include <cmath>

namespace tmc {

namespace {

constexpr int MinBufferFrames = 2;
constexpr int MaxBufferFrames = 8;
constexpr int MaxPackets = 64;
constexpr int ResetAfterConcealedFrames = 12;
constexpr qint64 StatsIntervalNs = 2'000'000'000LL;

} // namespace

RemoteAudioStream::RemoteAudioStream(QString peerId, int volumePercent)
    : peerId_(std::move(peerId)) {
    int error = OPUS_OK;
    decoder_ = opus_decoder_create(SampleRate, 1, &error);
    if (error != OPUS_OK) {
        decoder_ = nullptr;
    }
    setVolume(volumePercent);
}

RemoteAudioStream::~RemoteAudioStream() {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
    }
}

bool RemoteAudioStream::valid() const {
    return decoder_ != nullptr;
}

qint64 RemoteAudioStream::unwrapTimestamp(quint32 timestamp) {
    if (!hasTimestamp_) {
        hasTimestamp_ = true;
        latestRawTimestamp_ = timestamp;
        latestExtendedTimestamp_ = timestamp;
        return latestExtendedTimestamp_;
    }
    const auto delta = static_cast<qint32>(timestamp - latestRawTimestamp_);
    const auto extended = latestExtendedTimestamp_ + delta;
    if (delta > 0) {
        latestRawTimestamp_ = timestamp;
        latestExtendedTimestamp_ = extended;
    }
    return extended;
}

void RemoteAudioStream::enqueue(quint32 rtpTimestamp, QByteArray opusPayload,
                                qint64 receivedAtNs) {
    if (!decoder_ || opusPayload.isEmpty()) {
        return;
    }
    const auto extendedTimestamp = unwrapTimestamp(rtpTimestamp);
    const auto frame = extendedTimestamp / FrameSamples;
    if (started_ && frame < expectedFrame_) {
        ++latePackets_;
        return;
    }
    if (!packets_.emplace(frame, Packet{std::move(opusPayload), receivedAtNs}).second) {
        return;
    }
    ++receivedPackets_;

    const auto arrivalTicks = receivedAtNs * SampleRate / 1'000'000'000LL;
    const auto transit = arrivalTicks - extendedTimestamp;
    if (hasTransit_ && extendedTimestamp == latestExtendedTimestamp_) {
        const auto delta = std::abs(transit - lastTransitTicks_);
        jitterTicks_ += (static_cast<double>(delta) - jitterTicks_) / 16.0;
    }
    if (extendedTimestamp == latestExtendedTimestamp_) {
        lastTransitTicks_ = transit;
        hasTransit_ = true;
    }

    const auto jitterFrames = static_cast<int>(
        std::ceil((jitterTicks_ * 1000.0 / SampleRate) / 20.0));
    targetFrames_ = std::max(targetFrames_,
                             std::clamp(MinBufferFrames + jitterFrames,
                                        MinBufferFrames, MaxBufferFrames));
    while (packets_.size() > MaxPackets) {
        packets_.erase(packets_.begin());
        ++latePackets_;
    }
}

int RemoteAudioStream::decode(const QByteArray* payload, bool fec, PcmFrame& output) {
    const auto* bytes = payload
                            ? reinterpret_cast<const unsigned char*>(payload->constData())
                            : nullptr;
    const auto size = payload ? payload->size() : 0;
    const auto count = opus_decode(decoder_, bytes, size, output.data(), output.size(), fec ? 1 : 0);
    if (count < 0) {
        output.fill(0);
        return 0;
    }
    std::fill(output.begin() + count, output.end(), opus_int16{0});
    return count;
}

void RemoteAudioStream::discardObsoletePackets() {
    while (!packets_.empty() && packets_.begin()->first < expectedFrame_) {
        packets_.erase(packets_.begin());
        ++latePackets_;
    }
}

bool RemoteAudioStream::render(PcmFrame& output) {
    output.fill(0);
    if (!decoder_) {
        return false;
    }
    if (!started_) {
        if (static_cast<int>(packets_.size()) < targetFrames_) {
            return false;
        }
        expectedFrame_ = packets_.begin()->first;
        started_ = true;
        consecutiveConcealments_ = 0;
        opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
    }

    discardObsoletePackets();
    ++windowExpectedFrames_;
    const auto exact = packets_.find(expectedFrame_);
    bool concealed = exact == packets_.end();
    int decoded = 0;
    if (!concealed) {
        decoded = decode(&exact->second.payload, false, output);
        packets_.erase(exact);
        consecutiveConcealments_ = 0;
    } else {
        ++windowLostFrames_;
        ++concealedFrames_;
        ++consecutiveConcealments_;
        const auto next = packets_.find(expectedFrame_ + 1);
        if (next != packets_.end()) {
            decoded = decode(&next->second.payload, true, output);
            if (decoded > 0) {
                ++fecRecoveredFrames_;
            }
        }
        if (decoded <= 0) {
            decoded = decode(nullptr, false, output);
        }
    }
    ++expectedFrame_;
    adaptBuffer(concealed);

    if (consecutiveConcealments_ >= ResetAfterConcealedFrames) {
        started_ = false;
        consecutiveConcealments_ = 0;
    }
    return decoded > 0;
}

void RemoteAudioStream::adaptBuffer(bool concealed) {
    if (concealed) {
        targetFrames_ = std::min(MaxBufferFrames, targetFrames_ + 1);
        stableFrames_ = 0;
        return;
    }
    ++stableFrames_;
    if (stableFrames_ >= 500 && targetFrames_ > MinBufferFrames) {
        --targetFrames_;
        stableFrames_ = 0;
    }
}

void RemoteAudioStream::setVolume(int percent) {
    volume_ = static_cast<float>(std::clamp(percent, 0, 200)) / 100.0F;
}

float RemoteAudioStream::volume() const {
    return volume_;
}

std::optional<AudioNetworkStats> RemoteAudioStream::takeStats(qint64 nowNs) {
    if (lastStatsAtNs_ == 0) {
        lastStatsAtNs_ = nowNs;
        return std::nullopt;
    }
    if (nowNs - lastStatsAtNs_ < StatsIntervalNs) {
        return std::nullopt;
    }
    const auto sample = windowExpectedFrames_ == 0
                            ? 0.0
                            : 100.0 * static_cast<double>(windowLostFrames_) /
                                  static_cast<double>(windowExpectedFrames_);
    smoothedLossPercent_ = hasLossSample_ ? smoothedLossPercent_ * 0.7 + sample * 0.3 : sample;
    hasLossSample_ = true;
    windowExpectedFrames_ = 0;
    windowLostFrames_ = 0;
    lastStatsAtNs_ = nowNs;
    return AudioNetworkStats{smoothedLossPercent_,
                             static_cast<int>(std::lround(jitterTicks_ * 1000.0 / SampleRate)),
                             targetFrames_ * 20,
                             receivedPackets_,
                             concealedFrames_,
                             fecRecoveredFrames_,
                             latePackets_};
}

} // namespace tmc
