#include "tmc/network/peer_connection.h"

#include "tmc/core/logger.h"

#include <QPointer>
#include <QQueue>

#include <atomic>
#include <chrono>
#include <rtc/rtc.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtpdepacketizer.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtppacketizer.hpp>

namespace tmc {

struct PeerConnection::State {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> controlDc;
    std::shared_ptr<rtc::DataChannel> chatDc;
    std::shared_ptr<rtc::Track> audioTrack;
    QQueue<QByteArray> controlQueue;
    QQueue<QByteArray> chatQueue;
    quint64 queuedControlBytes{0};
    quint64 queuedChatBytes{0};
    std::atomic_bool audioNegotiating{false};
    std::atomic<quint64> audioFramesAttempted{0};
    std::atomic<quint64> audioFramesSent{0};
    std::atomic<quint64> audioFramesReceived{0};
    quint32 audioSsrc{0x544d4301};
};

namespace {

constexpr size_t TextBufferedHighWater = 256 * 1024;
constexpr size_t TextBufferedLowWater = 64 * 1024;
constexpr quint64 MaxQueuedTextBytes = 512 * 1024;
constexpr quint8 OpusPayloadType = 111;
constexpr auto OpusProfile =
    "minptime=10;maxaveragebitrate=48000;stereo=0;sprop-stereo=0;useinbandfec=1;usedtx=1";

qint64 monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

static ConnectionState mapState(rtc::PeerConnection::State s) {
    switch (s) {
    case rtc::PeerConnection::State::Connected:
        return ConnectionState::Connected;
    case rtc::PeerConnection::State::Connecting:
        return ConnectionState::Connecting;
    case rtc::PeerConnection::State::Failed:
        return ConnectionState::Failed;
    default:
        return ConnectionState::Disconnected;
    }
}

static QString iceStateName(rtc::PeerConnection::IceState state) {
    switch (state) {
    case rtc::PeerConnection::IceState::New:
        return "new";
    case rtc::PeerConnection::IceState::Checking:
        return "checking";
    case rtc::PeerConnection::IceState::Connected:
        return "connected";
    case rtc::PeerConnection::IceState::Completed:
        return "completed";
    case rtc::PeerConnection::IceState::Failed:
        return "failed";
    case rtc::PeerConnection::IceState::Disconnected:
        return "disconnected";
    case rtc::PeerConnection::IceState::Closed:
        return "closed";
    }
    return "unknown";
}

static QString gatheringStateName(rtc::PeerConnection::GatheringState state) {
    switch (state) {
    case rtc::PeerConnection::GatheringState::New:
        return "new";
    case rtc::PeerConnection::GatheringState::InProgress:
        return "in-progress";
    case rtc::PeerConnection::GatheringState::Complete:
        return "complete";
    }
    return "unknown";
}

static QString candidateTypeName(rtc::Candidate::Type type) {
    switch (type) {
    case rtc::Candidate::Type::Host:
        return "host";
    case rtc::Candidate::Type::ServerReflexive:
        return "srflx";
    case rtc::Candidate::Type::PeerReflexive:
        return "prflx";
    case rtc::Candidate::Type::Relayed:
        return "relay";
    case rtc::Candidate::Type::Unknown:
        return "unknown";
    }
    return "unknown";
}

static QString candidateTransportName(rtc::Candidate::TransportType type) {
    switch (type) {
    case rtc::Candidate::TransportType::Udp:
        return "udp";
    case rtc::Candidate::TransportType::TcpActive:
        return "tcp-active";
    case rtc::Candidate::TransportType::TcpPassive:
        return "tcp-passive";
    case rtc::Candidate::TransportType::TcpSo:
        return "tcp-so";
    case rtc::Candidate::TransportType::TcpUnknown:
        return "tcp";
    case rtc::Candidate::TransportType::Unknown:
        return "unknown";
    }
    return "unknown";
}

PeerConnection::PeerConnection(const QStringList& stunServers, QObject* p)
    : QObject(p), state_(std::make_shared<State>()) {
    rtc::Configuration cfg;
    for (const auto& s : stunServers) {
        cfg.iceServers.emplace_back(s.toStdString());
    }
    cfg.disableAutoNegotiation = true;
    cfg.forceMediaTransport = true;
    state_->pc = std::make_shared<rtc::PeerConnection>(cfg);
    QPointer<PeerConnection> self(this);
    std::weak_ptr<State> weak = state_;
    state_->pc->onLocalDescription([self, weak](const rtc::Description& description) {
        const auto state = weak.lock();
        if (!self || !state || !state->audioNegotiating.load(std::memory_order_acquire)) {
            return;
        }
        const auto type = QString::fromStdString(description.typeString());
        const auto sdp = QString::fromStdString(std::string(description));
        QMetaObject::invokeMethod(
            self,
            [self, type, sdp] {
                if (self) {
                    emit self->audioDescriptionReady(type, sdp);
                }
            },
            Qt::QueuedConnection);
    });
    state_->pc->onTrack([self](const std::shared_ptr<rtc::Track>& track) {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, track] {
                    if (self) {
                        self->configureAudioTrack(track);
                    }
                },
                Qt::QueuedConnection);
        }
    });
    state_->pc->onStateChange([self](auto s) {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, s] {
                    if (self) {
                        emit self->stateChanged(mapState(s));
                    }
                },
                Qt::QueuedConnection);
        }
    });
    state_->pc->onIceStateChange([self, weak](auto state) {
        if (!self) {
            return;
        }
        const auto stateName = iceStateName(state);
        QString localType;
        QString remoteType;
        if (state == rtc::PeerConnection::IceState::Connected ||
            state == rtc::PeerConnection::IceState::Completed) {
            if (const auto shared = weak.lock()) {
                rtc::Candidate local;
                rtc::Candidate remote;
                if (shared->pc->getSelectedCandidatePair(&local, &remote)) {
                    localType = candidateTypeName(local.type());
                    remoteType = candidateTypeName(remote.type());
                }
            }
        }
        QMetaObject::invokeMethod(
            self,
            [self, state, stateName, localType, remoteType] {
                if (self) {
                    emit self->iceStateChanged(stateName);
                    if (!localType.isEmpty()) {
                        emit self->selectedCandidatePairChanged(localType, remoteType);
                    }
                    if (state == rtc::PeerConnection::IceState::Failed) {
                        emit self->errorOccurred(
                            "ICE checks failed: direct P2P connection could not be established.");
                    }
                }
            },
            Qt::QueuedConnection);
    });
    state_->pc->onLocalCandidate([self](const rtc::Candidate& candidate) {
        if (!self) {
            return;
        }
        const auto type = candidateTypeName(candidate.type());
        const auto transport = candidateTransportName(candidate.transportType());
        QMetaObject::invokeMethod(
            self,
            [self, type, transport] {
                if (self) {
                    emit self->candidateDiscovered(type, transport);
                }
            },
            Qt::QueuedConnection);
    });
    state_->pc->onGatheringStateChange([self, weak](auto s) {
        if (!self) {
            return;
        }
        const auto stateName = gatheringStateName(s);
        QMetaObject::invokeMethod(
            self,
            [self, stateName] {
                if (self) {
                    emit self->gatheringStateChanged(stateName);
                }
            },
            Qt::QueuedConnection);
        if (s != rtc::PeerConnection::GatheringState::Complete) {
            return;
        }
        auto state = weak.lock();
        if (!state) {
            return;
        }
        auto local = state->pc->localDescription();
        if (!local) {
            return;
        }
        QString type = QString::fromStdString(local->typeString()),
                sdp = QString::fromStdString(std::string(*local));
        QMetaObject::invokeMethod(
            self,
            [self, type, sdp] {
                if (self) {
                    emit self->localDescriptionReady(type, sdp);
                    emit self->stateChanged(type == "offer" ? ConnectionState::WaitingForAnswer
                                                            : ConnectionState::Connecting);
                }
            },
            Qt::QueuedConnection);
    });
    state_->pc->onDataChannel([self](auto dc) {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, dc] {
                    if (!self) {
                        return;
                    }
                    if (dc->label() == "tiny-mesh-control") {
                        self->configureControlChannel(dc);
                    } else if (dc->label() == "tiny-mesh-chat") {
                        self->configureChatChannel(dc);
                    } else {
                        emit self->errorOccurred(
                            "Unknown DataChannel received: " +
                            QString::fromStdString(dc->label()));
                        dc->close();
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

PeerConnection::~PeerConnection() {
    auto s = std::move(state_);
    if (s) {
        if (s->audioTrack) {
            s->audioTrack->close();
        }
        if (s->chatDc) {
            s->chatDc->close();
        }
        if (s->controlDc) {
            s->controlDc->close();
        }
        if (s->pc) {
            s->pc->close();
        }
    }
}

void PeerConnection::configureControlChannel(const std::shared_ptr<rtc::DataChannel>& dc) {
    state_->controlDc = dc;
    configureTextChannel(dc, true);
}

void PeerConnection::configureChatChannel(const std::shared_ptr<rtc::DataChannel>& dc) {
    state_->chatDc = dc;
    configureTextChannel(dc, false);
}

void PeerConnection::configureTextChannel(const std::shared_ptr<rtc::DataChannel>& dc,
                                          bool control) {
    QPointer<PeerConnection> self(this);
    dc->setBufferedAmountLowThreshold(TextBufferedLowWater);
    dc->onOpen([self, control] {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, control] {
                    if (self) {
                        if (control) {
                            emit self->controlChannelOpened();
                        } else {
                            emit self->chatChannelOpened();
                        }
                        self->flushTextQueue(control);
                    }
                },
                Qt::QueuedConnection);
        }
    });
    dc->onClosed([self, control] {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, control] {
                    if (self) {
                        if (control) {
                            emit self->controlChannelClosed();
                        } else {
                            emit self->chatChannelClosed();
                        }
                    }
                },
                Qt::QueuedConnection);
        }
    });
    dc->onError([self, control](const std::string& error) {
        if (!self) {
            return;
        }
        const auto message = QString::fromStdString(error);
        QMetaObject::invokeMethod(
            self,
            [self, control, message] {
                if (self) {
                    emit self->errorOccurred(
                        QString(control ? "Control channel: " : "Chat channel: ") + message);
                }
            },
            Qt::QueuedConnection);
    });
    dc->onMessage([self, control](std::variant<rtc::binary, rtc::string> m) {
        if (!self || !std::holds_alternative<rtc::string>(m)) {
            return;
        }
        auto text = QString::fromUtf8(std::get<rtc::string>(m));
        QMetaObject::invokeMethod(
            self,
            [self, control, text] {
                if (self) {
                    if (control) {
                        emit self->controlTextReceived(text);
                    } else {
                        emit self->chatTextReceived(text);
                    }
                }
            },
            Qt::QueuedConnection);
    });
    dc->onBufferedAmountLow([self, control] {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, control] {
                    if (self) {
                        self->flushTextQueue(control);
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

void PeerConnection::configureAudioTrack(const std::shared_ptr<rtc::Track>& track) {
    if (!track) {
        return;
    }
    state_->audioTrack = track;

    const auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        state_->audioSsrc,
        "tiny-mesh-audio",
        OpusPayloadType,
        rtc::OpusRtpPacketizer::DefaultClockRate);
    const auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
    packetizer->addToChain(std::make_shared<rtc::RtpDepacketizer>(
        rtc::OpusRtpPacketizer::DefaultClockRate));
    packetizer->addToChain(std::make_shared<rtc::RtcpReceivingSession>());
    track->setMediaHandler(packetizer);

    QPointer<PeerConnection> self(this);
    track->onError([self](const std::string& error) {
        if (!self) {
            return;
        }
        const auto message = QString::fromStdString(error);
        QMetaObject::invokeMethod(
            self,
            [self, message] {
                if (self) {
                    emit self->errorOccurred("Audio RTP track: " + message);
                }
            },
            Qt::QueuedConnection);
    });
    track->onFrame([self](rtc::binary bytes, rtc::FrameInfo info) {
        if (!self || bytes.empty()) {
            return;
        }
        const auto receivedAtNs = monotonicNs();
        self->state_->audioFramesReceived.fetch_add(1, std::memory_order_relaxed);
        const QByteArray payload(reinterpret_cast<const char*>(bytes.data()),
                                 static_cast<qsizetype>(bytes.size()));
        QMetaObject::invokeMethod(
            self,
            [self, timestamp = info.timestamp, payload, receivedAtNs] {
                if (self) {
                    emit self->audioFrameReceived(timestamp, payload, receivedAtNs);
                }
            },
            Qt::QueuedConnection);
    });
}

void PeerConnection::createOffer() {
    emit stateChanged(ConnectionState::Gathering);
    rtc::DataChannelInit controlInit;
    controlInit.protocol = "application/tinymesh-control+json;v=0";
    configureControlChannel(state_->pc->createDataChannel("tiny-mesh-control", controlInit));
    rtc::DataChannelInit chatInit;
    chatInit.protocol = "application/tinymesh-chat+json;v=0";
    configureChatChannel(state_->pc->createDataChannel("tiny-mesh-chat", chatInit));
    state_->pc->setLocalDescription(rtc::Description::Type::Offer);
}

void PeerConnection::acceptOffer(const QString& s) {
    emit stateChanged(ConnectionState::Gathering);
    state_->pc->setRemoteDescription(rtc::Description(s.toStdString(), "offer"));
    state_->pc->setLocalDescription(rtc::Description::Type::Answer);
}

void PeerConnection::acceptAnswer(const QString& s) {
    state_->pc->setRemoteDescription(rtc::Description(s.toStdString(), "answer"));
    emit stateChanged(ConnectionState::Connecting);
}

void PeerConnection::createAudioOffer() {
    if (!state_->audioTrack) {
        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
        audio.addOpusCodec(OpusPayloadType, OpusProfile);
        audio.addSSRC(state_->audioSsrc,
                      "tiny-mesh-audio",
                      "tiny-mesh-audio",
                      "opus");
        configureAudioTrack(state_->pc->addTrack(audio));
    }
    state_->audioNegotiating.store(true, std::memory_order_release);
    state_->pc->setLocalDescription(rtc::Description::Type::Offer);
}

void PeerConnection::acceptAudioOffer(const QString& sdp) {
    state_->audioNegotiating.store(true, std::memory_order_release);
    state_->pc->setRemoteDescription(rtc::Description(sdp.toStdString(), "offer"));
    state_->pc->setLocalDescription(rtc::Description::Type::Answer);
}

void PeerConnection::acceptAudioAnswer(const QString& sdp) {
    state_->pc->setRemoteDescription(rtc::Description(sdp.toStdString(), "answer"));
    state_->audioNegotiating.store(false, std::memory_order_release);
}

bool PeerConnection::sendControl(const QString& text) {
    return sendText(state_->controlDc, text, true);
}

bool PeerConnection::sendChat(const QString& text) {
    return sendText(state_->chatDc, text, false);
}

bool PeerConnection::sendText(const std::shared_ptr<rtc::DataChannel>& channel,
                              const QString& text, bool control) {
    if (!channel || !channel->isOpen()) {
        return false;
    }
    const auto bytes = text.toUtf8();
    if (bytes.isEmpty() || static_cast<size_t>(bytes.size()) > channel->maxMessageSize()) {
        return false;
    }

    auto& queue = control ? state_->controlQueue : state_->chatQueue;
    auto& queuedBytes = control ? state_->queuedControlBytes : state_->queuedChatBytes;
    if (!queue.isEmpty() || channel->bufferedAmount() > TextBufferedHighWater) {
        if (queuedBytes + static_cast<quint64>(bytes.size()) > MaxQueuedTextBytes) {
            return false;
        }
        queue.enqueue(bytes);
        queuedBytes += static_cast<quint64>(bytes.size());
        return true;
    }

    channel->send(bytes.toStdString());
    return true;
}

void PeerConnection::flushTextQueue(bool control) {
    const auto channel = control ? state_->controlDc : state_->chatDc;
    if (!channel || !channel->isOpen()) {
        return;
    }
    auto& queue = control ? state_->controlQueue : state_->chatQueue;
    auto& queuedBytes = control ? state_->queuedControlBytes : state_->queuedChatBytes;
    while (!queue.isEmpty() && channel->bufferedAmount() <= TextBufferedHighWater) {
        const auto bytes = queue.dequeue();
        queuedBytes -= static_cast<quint64>(bytes.size());
        channel->send(bytes.toStdString());
    }
}

PeerConnectionSnapshot PeerConnection::snapshot() const {
    PeerConnectionSnapshot result;
    result.controlOpen = state_->controlDc && state_->controlDc->isOpen();
    result.chatOpen = state_->chatDc && state_->chatDc->isOpen();
    result.audioTrackOpen = state_->audioTrack && state_->audioTrack->isOpen();
    result.audioFramesAttempted =
        state_->audioFramesAttempted.load(std::memory_order_relaxed);
    result.audioFramesSent = state_->audioFramesSent.load(std::memory_order_relaxed);
    result.audioFramesReceived =
        state_->audioFramesReceived.load(std::memory_order_relaxed);
    result.controlBufferedBytes =
        state_->controlDc ? static_cast<quint64>(state_->controlDc->bufferedAmount()) : 0;
    result.chatBufferedBytes =
        state_->chatDc ? static_cast<quint64>(state_->chatDc->bufferedAmount()) : 0;
    result.queuedControlBytes = state_->queuedControlBytes;
    result.queuedChatBytes = state_->queuedChatBytes;
    return result;
}

bool PeerConnection::sendAudioFrame(quint32 sequence, const QByteArray& opusPayload) {
    state_->audioFramesAttempted.fetch_add(1, std::memory_order_relaxed);
    if (!state_->audioTrack || !state_->audioTrack->isOpen() || opusPayload.isEmpty()) {
        return false;
    }
    rtc::binary frame(reinterpret_cast<const rtc::byte*>(opusPayload.constData()),
                      reinterpret_cast<const rtc::byte*>(opusPayload.constData()) +
                          opusPayload.size());
    try {
        state_->audioTrack->sendFrame(std::move(frame), rtc::FrameInfo(sequence * 960));
        state_->audioFramesSent.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace tmc
