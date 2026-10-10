#include "screen_media_peer.h"

#include "tmc/core/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>

#include <algorithm>
#include <api/jsep.h>
#include <api/make_ref_counted.h>
#include <api/rtp_receiver_interface.h>
#include <api/rtp_sender_interface.h>
#include <api/rtp_transceiver_interface.h>
#include <api/set_local_description_observer_interface.h>
#include <api/set_remote_description_observer_interface.h>
#include <api/stats/rtc_stats_collector_callback.h>
#include <api/stats/rtc_stats_report.h>
#include <api/stats/rtcstats_objects.h>
#include <api/video/video_frame.h>
#include <atomic>
#include <mutex>
#include <optional>
#include <utility>

#include "peer_screen_source.h"
#include "screen_codec_factory.h"
#include "screen_media_engine.h"
#include "screen_video_bridge.h"

namespace tmc {
namespace {

class CreatedDescription : public webrtc::CreateSessionDescriptionObserver {
public:
    using Success = std::function<void(std::unique_ptr<webrtc::SessionDescriptionInterface>)>;

    CreatedDescription(Success success, std::function<void(QString)> fail)
        : success_(std::move(success)), fail_(std::move(fail)) {
    }

    void OnSuccess(webrtc::SessionDescriptionInterface* description) override {
        success_(std::unique_ptr<webrtc::SessionDescriptionInterface>(description));
    }

    void OnFailure(webrtc::RTCError error) override {
        fail_(QString::fromUtf8(error.message()));
    }

private:
    Success success_;
    std::function<void(QString)> fail_;
};

class LocalDescription : public webrtc::SetLocalDescriptionObserverInterface {
public:
    explicit LocalDescription(std::function<void(webrtc::RTCError)> callback)
        : callback_(std::move(callback)) {
    }

    void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
        callback_(std::move(error));
    }

private:
    std::function<void(webrtc::RTCError)> callback_;
};

class RemoteDescription : public webrtc::SetRemoteDescriptionObserverInterface {
public:
    explicit RemoteDescription(std::function<void(webrtc::RTCError)> callback)
        : callback_(std::move(callback)) {
    }

    void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
        callback_(std::move(error));
    }

private:
    std::function<void(webrtc::RTCError)> callback_;
};

class StatsCallback : public webrtc::RTCStatsCollectorCallback {
public:
    explicit StatsCallback(std::function<void(QJsonObject)> callback)
        : callback_(std::move(callback)) {
    }

    void
    OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
        const auto text = report->ToJson();
        callback_({{"reports", QJsonDocument::fromJson(QByteArray::fromStdString(text)).array()}});
    }

private:
    std::function<void(QJsonObject)> callback_;
};

} // namespace

struct ScreenMediaPeer::State : webrtc::PeerConnectionObserver,
                                webrtc::VideoSinkInterface<webrtc::VideoFrame>,
                                webrtc::AudioTrackSinkInterface,
                                std::enable_shared_from_this<State> {
    ScreenMediaEngine& engine;
    QPointer<ScreenMediaPeer> owner;
    QString local, remote, media;
    quint64 generation{}, negotiation{};
    QStringList stun;
    QList<RelayServer> turn;
    std::shared_ptr<SharedAudioOutput> output;
    webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
    webrtc::scoped_refptr<PeerScreenSource> source;
    webrtc::scoped_refptr<webrtc::VideoTrackInterface> localVideo, remoteVideo;
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> localAudio, remoteAudio;
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> videoTransceiver, audioTransceiver;
    bool busy{false}, remoteReady{false}, pendingRestart{false}, software{false}, sendVideo{false},
        sendAudio{false};
    ScreenQuality quality;
    QString sendStream;
    std::atomic<bool> closed{false}, receivedFirst{false};
    std::mutex frameMutex;
    std::optional<webrtc::VideoFrame> pendingFrame;
    bool frameQueued{false};
    std::mutex receiveMutex;
    QString receiveStream;
    bool receiveVideo{false}, receiveAudio{false};
    ScreenVideoBridge* bridge{};
    QList<QJsonObject> candidates, localCandidates;
    bool localReady{false};
    bool sentFirst{false};
    quint64 sourceFrames{}, sourceDropped{};

    State(ScreenMediaEngine& engine) : engine(engine) {
    }

    template <class F> void notify(F callback) {
        const auto target = owner;
        if (closed || !target) {
            return;
        }
        QMetaObject::invokeMethod(
            target,
            [target, callback = std::move(callback)] {
                if (target) {
                    callback(*target);
                }
            },
            Qt::QueuedConnection);
    }

    void fail(QString error) {
        log("error=" + error, QtWarningMsg);
        notify([error = std::move(error)](auto& o) { emit o.errorOccurred(error); });
    }

    void log(const QString& message, QtMsgType level = QtInfoMsg) const {
        Logger::instance().log(level, "screen_media",
                               QString("peer=%1 %2").arg(remote.left(8), message));
    }

    QJsonObject envelope(const QString& kind) const {
        return {{"v", 2},
                {"kind", kind},
                {"media", media},
                {"generation", static_cast<qint64>(generation)},
                {"negotiation", static_cast<qint64>(negotiation)}};
    }

    void send(QJsonObject message) {
        notify([message = std::move(message)](auto& o) { emit o.outgoing(message); });
    }

    void create() {
        if (closed || pc) {
            return;
        }
        log("create_peer_connection");
        auto result = engine.factory()->CreatePeerConnectionOrError(
            engine.configuration(stun, turn), webrtc::PeerConnectionDependencies(this));
        if (!result.ok()) {
            fail(QString::fromUtf8(result.error().message()));
            return;
        }
        pc = result.MoveValue();
        {
            std::lock_guard lock(frameMutex);
            source = webrtc::make_ref_counted<PeerScreenSource>(quality);
        }
        localVideo = engine.factory()->CreateVideoTrack(source, "screen-video");
        localAudio =
            engine.factory()->CreateAudioTrack("screen-system-audio", engine.audioSource().get());
        webrtc::RtpTransceiverInit init;
        init.direction = webrtc::RtpTransceiverDirection::kSendRecv;
        init.stream_ids = {"screen"};
        if (local < remote) {
            auto video = pc->AddTransceiver(localVideo, init);
            auto audio = pc->AddTransceiver(localAudio, init);
            if (!video.ok() || !audio.ok()) {
                fail("Не удалось создать медиатреки.");
                return;
            }
            videoTransceiver = video.MoveValue();
            audioTransceiver = audio.MoveValue();
        } else {
            // JSEP only reuses AddTrack-created transceivers for an incoming offer.
            // Precreating with AddTransceiver here would leave our senders unassociated.
            const auto video = pc->AddTrack(localVideo, {"screen"});
            const auto audio = pc->AddTrack(localAudio, {"screen"});
            if (!video.ok() || !audio.ok()) {
                fail("Не удалось создать медиатреки ответа.");
                return;
            }
            for (const auto& transceiver : pc->GetTransceivers()) {
                if (transceiver->media_type() == webrtc::MediaType::VIDEO) {
                    videoTransceiver = transceiver;
                } else if (transceiver->media_type() == webrtc::MediaType::AUDIO) {
                    audioTransceiver = transceiver;
                }
            }
            if (!videoTransceiver || !audioTransceiver) {
                fail("Не найдены медиатреки ответа.");
                return;
            }
        }
        videoTransceiver->sender()->SetEncoderSelector(createScreenEncoderSelector());
        auto codecs = engine.factory()->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs;
        if (software) {
            std::erase_if(codecs, [](const auto& codec) { return codec.name == "H264"; });
        }
        const auto error = videoTransceiver->SetCodecPreferences(codecs);
        if (!error.ok()) {
            fail(QString::fromUtf8(error.message()));
            return;
        }
        applySend();
        log("peer_connection_ready");
    }

    void applySend() {
        if (!pc || !videoTransceiver || !audioTransceiver) {
            return;
        }
        source->setQuality(quality);
        const bool videoSet =
            videoTransceiver->sender()->SetTrack(sendVideo ? localVideo.get() : nullptr);
        const bool audioSet =
            audioTransceiver->sender()->SetTrack(sendAudio ? localAudio.get() : nullptr);
        if (!videoSet || !audioSet) {
            fail("Не удалось подключить исходящие медиатреки.");
            return;
        }
        auto parameters = videoTransceiver->sender()->GetParameters();
        if (!parameters.encodings.empty()) {
            parameters.encodings[0].active = sendVideo;
            parameters.encodings[0].max_bitrate_bps = quality.maxBitrate();
            parameters.encodings[0].max_framerate = quality.fps;
            parameters.degradation_preference = webrtc::DegradationPreference::BALANCED;
            const auto error = videoTransceiver->sender()->SetParameters(parameters);
            if (!error.ok()) {
                fail(QString::fromUtf8(error.message()));
            }
        }
        logVideoState("apply_send");
        if (!sendStream.isEmpty() && videoTransceiver->mid() && negotiation > 0) {
            auto binding = envelope("binding");
            binding["stream"] = sendStream;
            binding["revision"] = static_cast<qint64>(negotiation);
            binding["videoMid"] = QString::fromStdString(*videoTransceiver->mid());
            send(std::move(binding));
        }
    }

    void logVideoState(const QString& phase) const {
        if (!videoTransceiver) {
            return;
        }
        const auto sender = videoTransceiver->sender();
        const auto parameters = sender->GetParameters();
        const auto direction = videoTransceiver->current_direction();
        log(QString("%1 video=%2 track=%3 enabled=%4 active=%5 direction=%6 negotiated=%7 "
                    "encodings=%8")
                .arg(phase)
                .arg(sendVideo)
                .arg(bool(sender->track()))
                .arg(localVideo && localVideo->enabled())
                .arg(!parameters.encodings.empty() && parameters.encodings[0].active)
                .arg(static_cast<int>(videoTransceiver->direction()))
                .arg(direction ? static_cast<int>(*direction) : -1)
                .arg(parameters.encodings.size()));
    }

    void describe(bool offer, bool restart = false) {
        if (!pc || closed) {
            return;
        }
        if (busy) {
            pendingRestart |= restart;
            return;
        }
        log(QString("create_%1 restart=%2").arg(offer ? "offer" : "answer").arg(restart));
        busy = true;
        if (offer) {
            ++negotiation;
            remoteReady = false;
        }
        localReady = false;
        localCandidates.clear();
        const auto weak = weak_from_this();
        auto observer = webrtc::make_ref_counted<CreatedDescription>(
            [weak, offer](auto description) {
                if (auto self = weak.lock(); self && !self->closed && self->pc) {
                    std::string sdp;
                    description->ToString(&sdp);
                    self->pc->SetLocalDescription(
                        std::move(description),
                        webrtc::make_ref_counted<LocalDescription>(
                            [weak, offer, sdp = std::move(sdp)](auto error) {
                                if (auto self = weak.lock(); self && !self->closed) {
                                    if (!error.ok()) {
                                        self->busy = false;
                                        self->fail(QString::fromUtf8(error.message()));
                                        return;
                                    }
                                    auto message = self->envelope(offer ? "offer" : "answer");
                                    self->log(offer ? "local_offer_ready" : "local_answer_ready");
                                    message["sdp"] = QString::fromStdString(sdp);
                                    self->send(std::move(message));
                                    self->localReady = true;
                                    for (const auto& candidate : self->localCandidates) {
                                        self->send(candidate);
                                    }
                                    self->localCandidates.clear();
                                    if (!offer) {
                                        self->busy = false;
                                        self->applySend();
                                    }
                                }
                            }));
                }
            },
            [weak](QString error) {
                if (auto self = weak.lock()) {
                    self->busy = false;
                    self->fail(error);
                }
            });
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        options.ice_restart = restart;
        if (offer) {
            pc->CreateOffer(observer.get(), options);
        } else {
            pc->CreateAnswer(observer.get(), options);
        }
    }

    void addCandidate(const QJsonObject& m) {
        webrtc::SdpParseError parse;
        const std::unique_ptr<webrtc::IceCandidate> candidate(webrtc::CreateIceCandidate(
            m.value("mid").toString().toStdString(), m.value("mline").toInt(),
            m.value("candidate").toString().toStdString(), &parse));
        if (candidate && pc) {
            if (!pc->AddIceCandidate(candidate.get())) {
                log("ice_candidate_rejected", QtWarningMsg);
            }
        } else if (!candidate) {
            fail(QString::fromStdString(parse.description));
        }
    }

    void receive(const QJsonObject& m) {
        if (closed) {
            return;
        }
        const auto kind = m.value("kind").toString();
        if (kind == "offer") {
            if (local < remote || busy) {
                return;
            }
            if (!media.isEmpty() && (m.value("media").toString() != media ||
                                     quint64(m.value("generation").toInteger()) != generation)) {
                return;
            }
            media = m.value("media").toString();
            generation = m.value("generation").toInteger();
            const auto revision = quint64(m.value("negotiation").toInteger());
            if (revision <= negotiation) {
                return;
            }
            negotiation = revision;
            create();
        } else if (m.value("media").toString() != media ||
                   quint64(m.value("generation").toInteger()) != generation ||
                   quint64(m.value("negotiation").toInteger()) != negotiation) {
            return;
        }
        if (kind == "candidate") {
            if (!remoteReady) {
                if (candidates.size() < 128) {
                    candidates.append(m);
                }
            } else {
                addCandidate(m);
            }
            return;
        }
        if (kind != "offer" && kind != "answer") {
            return;
        }
        if (!pc || (kind == "answer" && (!busy || !(local < remote)))) {
            return;
        }
        webrtc::SdpParseError parse;
        auto description = webrtc::CreateSessionDescription(
            kind == "offer" ? webrtc::SdpType::kOffer : webrtc::SdpType::kAnswer,
            m.value("sdp").toString().toStdString(), &parse);
        if (!description) {
            fail(QString::fromStdString(parse.description));
            return;
        }
        remoteReady = false;
        const auto weak = weak_from_this();
        pc->SetRemoteDescription(
            std::move(description),
            webrtc::make_ref_counted<RemoteDescription>([weak, kind](auto error) {
                if (auto self = weak.lock(); self && !self->closed) {
                    if (!error.ok()) {
                        self->busy = false;
                        self->fail(QString::fromUtf8(error.message()));
                        return;
                    }
                    self->remoteReady = true;
                    self->log("remote_" + kind + "_applied");
                    for (const auto& candidate : self->candidates) {
                        self->addCandidate(candidate);
                    }
                    self->candidates.clear();
                    if (kind == "offer") {
                        self->describe(false);
                    } else {
                        self->busy = false;
                        self->applySend();
                        if (std::exchange(self->pendingRestart, false)) {
                            self->describe(true, true);
                        }
                    }
                }
            }));
    }

    void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {
    }

    void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
        channel->Close();
    }

    void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override {
        log(QString("ice_gathering=%1").arg(static_cast<int>(state)));
    }

    void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) override {
        log(QString("ice_connection=%1").arg(static_cast<int>(state)));
    }

    void OnIceCandidate(const webrtc::IceCandidate* candidate) override {
        std::string text;
        if (!candidate->ToString(&text)) {
            return;
        }
        auto m = envelope("candidate");
        m["candidate"] = QString::fromStdString(text);
        m["mid"] = QString::fromStdString(candidate->sdp_mid());
        m["mline"] = candidate->sdp_mline_index();
        if (localReady) {
            send(std::move(m));
        } else if (localCandidates.size() < 128) {
            localCandidates.append(std::move(m));
        }
    }

    void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override {
        QString name = "connecting";
        if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected) {
            name = "connected";
        } else if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected) {
            name = "reconnecting";
        } else if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kFailed) {
            name = "failed";
        } else if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kClosed) {
            name = "closed";
        }
        log("connection=" + name);
        if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected) {
            logVideoState("connected_video");
        }
        notify([name](auto& owner) { emit owner.stateChanged(name); });
    }

    void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        const auto track = transceiver->receiver()->track();
        log("remote_track=" + QString::fromStdString(track->kind()));
        if (track->kind() == webrtc::MediaStreamTrackInterface::kVideoKind) {
            if (remoteVideo) {
                remoteVideo->RemoveSink(this);
            }
            remoteVideo = static_cast<webrtc::VideoTrackInterface*>(track.get());
            remoteVideo->AddOrUpdateSink(this, webrtc::VideoSinkWants{});
        } else if (track->kind() == webrtc::MediaStreamTrackInterface::kAudioKind) {
            if (remoteAudio) {
                remoteAudio->RemoveSink(this);
            }
            remoteAudio = static_cast<webrtc::AudioTrackInterface*>(track.get());
            remoteAudio->AddSink(this);
        }
    }

    void OnFrame(const webrtc::VideoFrame& frame) override {
        std::lock_guard lock(receiveMutex);
        if (closed || !receiveVideo || !bridge) {
            return;
        }
        bridge->OnFrame(frame);
        if (!receivedFirst.exchange(true)) {
            log("first_decoded_frame");
            notify([](auto& o) { emit o.firstFrame(); });
        }
    }

    void OnData(const void* data, int bits, int rate, size_t channels, size_t frames) override {
        std::lock_guard lock(receiveMutex);
        if (!closed && receiveAudio && bits == 16) {
            output->pushScreen(receiveStream, static_cast<const int16_t*>(data), frames, channels,
                               rate);
        }
    }

    void close() {
        log("close_peer_connection");
        closed = true;
        {
            std::lock_guard lock(receiveMutex);
            bridge = nullptr;
            receiveVideo = false;
            receiveAudio = false;
        }
        if (remoteVideo) {
            remoteVideo->RemoveSink(this);
        }
        if (remoteAudio) {
            remoteAudio->RemoveSink(this);
        }
        if (pc) {
            pc->Close();
        }
        remoteVideo = nullptr;
        remoteAudio = nullptr;
        localVideo = nullptr;
        localAudio = nullptr;
        videoTransceiver = nullptr;
        audioTransceiver = nullptr;
        pc = nullptr;
        std::lock_guard lock(frameMutex);
        pendingFrame.reset();
        source = nullptr;
        log("peer_connection_closed");
    }
};

ScreenMediaPeer::ScreenMediaPeer(ScreenMediaEngine& engine, QString local, QString remote,
                                 QStringList stun, QList<RelayServer> turn,
                                 std::shared_ptr<SharedAudioOutput> output, QObject* parent)
    : QObject(parent), state_(std::make_shared<State>(engine)) {
    state_->owner = this;
    state_->local = std::move(local);
    state_->remote = std::move(remote);
    state_->stun = std::move(stun);
    state_->turn = std::move(turn);
    state_->output = std::move(output);
}

ScreenMediaPeer::~ScreenMediaPeer() {
    close();
}

void ScreenMediaPeer::begin(QString id, quint64 generation) {
    const auto s = state_;
    s->engine.signaling()->PostTask([s, id = std::move(id), generation] {
        if (s->closed) {
            return;
        }
        s->media = id;
        s->generation = generation;
        s->create();
        s->describe(true);
    });
}

void ScreenMediaPeer::receive(const QJsonObject& message) {
    const auto s = state_;
    s->engine.signaling()->PostTask([s, message] { s->receive(message); });
}

void ScreenMediaPeer::setSending(QString stream, bool video, bool audio, ScreenQuality quality) {
    const auto s = state_;
    s->engine.signaling()->PostTask([s, stream = std::move(stream), video, audio, quality] {
        if (s->closed) {
            return;
        }
        s->sendStream = stream;
        s->sendVideo = video;
        s->sendAudio = audio;
        s->quality = quality;
        s->applySend();
    });
}

void ScreenMediaPeer::setReceiving(QString stream, bool video, bool audio,
                                   ScreenVideoBridge* bridge) {
    std::lock_guard lock(state_->receiveMutex);
    if (state_->receiveStream != stream) {
        state_->receivedFirst = false;
    }
    state_->receiveStream = std::move(stream);
    state_->receiveVideo = video;
    state_->receiveAudio = audio;
    state_->bridge = bridge;
    if (!video) {
        state_->receivedFirst = false;
    }
}

void ScreenMediaPeer::pushFrame(const webrtc::VideoFrame& frame) {
    const auto s = state_;
    {
        std::lock_guard lock(s->frameMutex);
        if (s->closed || !s->source) {
            return;
        }
        s->pendingFrame = frame;
        if (s->frameQueued) {
            return;
        }
        s->frameQueued = true;
    }
    // The capture thread must not wait for WebRTC while the GUI waits for capture to stop.
    // Keep only the latest frame for each peer and deliver on WebRTC's worker thread.
    s->engine.worker()->PostTask([s] {
        webrtc::scoped_refptr<PeerScreenSource> source;
        std::optional<webrtc::VideoFrame> frame;
        {
            std::lock_guard lock(s->frameMutex);
            s->frameQueued = false;
            frame = std::exchange(s->pendingFrame, std::nullopt);
            if (s->closed || !frame) {
                return;
            }
            source = s->source;
        }
        if (!source) {
            return;
        }
        if (++s->sourceFrames == 1) {
            s->log("deliver_first_source_frame");
        }
        if (!source->push(*frame)) {
            if (++s->sourceDropped == 1 || s->sourceDropped % 300 == 0) {
                s->log(QString("source_frames=%1 dropped=%2")
                           .arg(s->sourceFrames)
                           .arg(s->sourceDropped));
            }
            return;
        }
        if (!s->sentFirst) {
            s->sentFirst = true;
            Logger::instance().log(QtInfoMsg, "screen_media",
                                   QString("peer=%1 first_source_frame").arg(s->remote.left(8)));
        }
    });
}

void ScreenMediaPeer::updateIce(QStringList stun, QList<RelayServer> turn) {
    const auto s = state_;
    s->engine.signaling()->PostTask([s, stun = std::move(stun), turn = std::move(turn)] {
        s->stun = stun;
        s->turn = turn;
        if (s->pc && !s->closed) {
            s->pc->SetConfiguration(s->engine.configuration(stun, turn));
        }
    });
}

void ScreenMediaPeer::restartIce() {
    const auto s = state_;
    s->engine.signaling()->PostTask([s] {
        if (s->local < s->remote) {
            s->describe(true, true);
        }
    });
}

void ScreenMediaPeer::preferSoftware() {
    const auto s = state_;
    s->engine.signaling()->PostTask([s] {
        s->software = true;
        if (!s->videoTransceiver || s->closed) {
            return;
        }
        auto codecs =
            s->engine.factory()->GetRtpSenderCapabilities(webrtc::MediaType::VIDEO).codecs;
        std::erase_if(codecs, [](const auto& codec) { return codec.name == "H264"; });
        s->videoTransceiver->SetCodecPreferences(codecs);
        if (s->local < s->remote) {
            s->describe(true);
        }
    });
}

void ScreenMediaPeer::requestStats(quint64 sendEpoch) {
    const auto s = state_;
    s->engine.signaling()->PostTask([s, sendEpoch] {
        if (!s->pc || s->closed) {
            return;
        }
        const auto weak = s->weak_from_this();
        s->pc->GetStats(
            webrtc::make_ref_counted<StatsCallback>([weak, sendEpoch](QJsonObject stats) {
                if (const auto s = weak.lock()) {
                    s->notify([stats = std::move(stats), sendEpoch](auto& o) {
                        emit o.statsChanged(stats, sendEpoch);
                    });
                }
            }).get());
    });
}

void ScreenMediaPeer::close() {
    if (state_ && !state_->closed) {
        const auto s = state_;
        s->engine.signaling()->BlockingCall([s] { s->close(); });
    }
}

} // namespace tmc
