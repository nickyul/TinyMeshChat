#include "tmc/network/peer_connection.h"

#include "tmc/core/logger.h"
#include "tmc/network/audio_transport_worker.h"

#include <QPointer>
#include <QQueue>
#include <QRandomGenerator>
#include <QThread>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <rtc/mediahandler.hpp>
#include <rtc/rtc.hpp>
#include <rtc/rtcpreceivingsession.hpp>
#include <rtc/rtcpsrreporter.hpp>
#include <rtc/rtppacketizationconfig.hpp>
#include <rtc/rtppacketizer.hpp>
#include <stdexcept>

namespace tmc {

namespace {

enum class AudioNegotiationState {
    Idle,
    CreatingOffer,
    CreatingAnswer,
    AwaitingAnswer,
};

} // namespace

struct PeerConnection::State {
    QString connectionId;
    std::weak_ptr<AudioTransportWorker> audioTransport;

    // WebRTC transports.
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> controlDc;
    std::shared_ptr<rtc::DataChannel> chatDc;
    std::shared_ptr<rtc::Track> audioTrack;
    std::shared_ptr<AudioTransportEndpoint> audioEndpoint{
        std::make_shared<AudioTransportEndpoint>()};
    quint32 audioSsrc{0};

    // Text backpressure.
    QQueue<QByteArray> controlQueue;
    QQueue<QByteArray> chatQueue;
    quint64 queuedControlBytes{0};
    quint64 queuedChatBytes{0};

    // Audio negotiation and diagnostics.
    std::atomic<AudioNegotiationState> audioNegotiation{AudioNegotiationState::Idle};
};

namespace {

constexpr size_t TextBufferedHighWater = 256 * 1024;
constexpr size_t TextBufferedLowWater = 64 * 1024;
constexpr quint64 MaxQueuedTextBytes = 512 * 1024;
constexpr quint8 OpusPayloadType = 111;
constexpr size_t MaxOpusPayloadBytes = 4000;
constexpr auto OpusProfile =
    "minptime=10;maxaveragebitrate=48000;stereo=0;sprop-stereo=0;useinbandfec=1;usedtx=1";

qint64 monotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

class OpusRtpFrameHandler final : public rtc::MediaHandler {
public:
    using Callback = std::function<void(quint32, quint16, quint32, QByteArray, qint64)>;

    explicit OpusRtpFrameHandler(Callback callback) : callback_(std::move(callback)) {
    }

    void incoming(rtc::message_vector& messages, const rtc::message_callback&) override {
        rtc::message_vector remaining;
        for (auto& message : messages) {
            if (message->type == rtc::Message::Control) {
                remaining.push_back(std::move(message));
                continue;
            }
            if (message->size() < 12) {
                continue;
            }
            const auto byteAt = [&](size_t index) {
                return std::to_integer<quint8>((*message)[index]);
            };
            const auto first = byteAt(0);
            if ((first >> 6) != 2 || (byteAt(1) & 0x7f) != OpusPayloadType) {
                continue;
            }
            size_t headerSize = 12 + static_cast<size_t>(first & 0x0f) * 4;
            if (message->size() < headerSize) {
                continue;
            }
            if ((first & 0x10) != 0) {
                if (message->size() < headerSize + 4) {
                    continue;
                }
                const auto extensionWords =
                    static_cast<size_t>((byteAt(headerSize + 2) << 8) | byteAt(headerSize + 3));
                headerSize += 4 + extensionWords * 4;
                if (message->size() < headerSize) {
                    continue;
                }
            }
            auto payloadSize = message->size() - headerSize;
            if ((first & 0x20) != 0) {
                if (payloadSize == 0) {
                    continue;
                }
                const auto padding = std::to_integer<quint8>(message->back());
                if (padding == 0 || padding > payloadSize) {
                    continue;
                }
                payloadSize -= padding;
            }
            if (payloadSize == 0 || payloadSize > MaxOpusPayloadBytes) {
                continue;
            }
            const QByteArray payload(reinterpret_cast<const char*>(message->data() + headerSize),
                                     static_cast<qsizetype>(payloadSize));
            const auto sequenceNumber = static_cast<quint16>((byteAt(2) << 8) | byteAt(3));
            const auto readU32 = [&](size_t offset) {
                return (static_cast<quint32>(byteAt(offset)) << 24) |
                       (static_cast<quint32>(byteAt(offset + 1)) << 16) |
                       (static_cast<quint32>(byteAt(offset + 2)) << 8) |
                       static_cast<quint32>(byteAt(offset + 3));
            };
            callback_(readU32(8), sequenceNumber, readU32(4), payload, monotonicNs());
        }
        messages.swap(remaining);
    }

private:
    Callback callback_;
};

ConnectionState mapState(rtc::PeerConnection::State s) {
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

QString iceStateName(rtc::PeerConnection::IceState state) {
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

QString gatheringStateName(rtc::PeerConnection::GatheringState state) {
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

QString candidateTypeName(rtc::Candidate::Type type) {
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

QString candidateTransportName(rtc::Candidate::TransportType type) {
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

} // namespace

PeerConnection::PeerConnection(const QStringList& stunServers, QString connectionId,
                               std::weak_ptr<AudioTransportWorker> audioTransport, QObject* p)
    : QObject(p), state_(std::make_shared<State>()) {
    state_->connectionId = std::move(connectionId);
    state_->audioTransport = std::move(audioTransport);
    do {
        state_->audioSsrc = QRandomGenerator::global()->generate();
    } while (state_->audioSsrc == 0);
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
        if (!self || !state) {
            return;
        }

        const auto type = QString::fromStdString(description.typeString());
        auto expectedState = AudioNegotiationState::Idle;
        auto nextState = AudioNegotiationState::Idle;
        if (type == "offer") {
            expectedState = AudioNegotiationState::CreatingOffer;
            nextState = AudioNegotiationState::AwaitingAnswer;
        } else if (type == "answer") {
            expectedState = AudioNegotiationState::CreatingAnswer;
        } else {
            return;
        }
        if (!state->audioNegotiation.compare_exchange_strong(expectedState, nextState,
                                                             std::memory_order_acq_rel)) {
            return;
        }

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
                        emit self->errorOccurred("Unknown DataChannel received: " +
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
        s->audioEndpoint->clearTrack();
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
    configureTextChannel(dc, TextChannel::Control);
}

void PeerConnection::configureChatChannel(const std::shared_ptr<rtc::DataChannel>& dc) {
    state_->chatDc = dc;
    configureTextChannel(dc, TextChannel::Chat);
}

void PeerConnection::configureTextChannel(const std::shared_ptr<rtc::DataChannel>& dc,
                                          TextChannel channel) {
    QPointer<PeerConnection> self(this);
    dc->setBufferedAmountLowThreshold(TextBufferedLowWater);
    dc->onOpen([self, channel] {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, channel] {
                    if (self) {
                        if (channel == TextChannel::Control) {
                            emit self->controlChannelOpened();
                        } else {
                            emit self->chatChannelOpened();
                        }
                        self->flushTextQueue(channel);
                    }
                },
                Qt::QueuedConnection);
        }
    });
    dc->onClosed([self, channel] {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, channel] {
                    if (self) {
                        if (channel == TextChannel::Control) {
                            emit self->controlChannelClosed();
                        } else {
                            emit self->chatChannelClosed();
                        }
                    }
                },
                Qt::QueuedConnection);
        }
    });
    dc->onError([self, channel](const std::string& error) {
        if (!self) {
            return;
        }
        const auto message = QString::fromStdString(error);
        QMetaObject::invokeMethod(
            self,
            [self, channel, message] {
                if (self) {
                    emit self->errorOccurred(QString(channel == TextChannel::Control
                                                         ? "Control channel: "
                                                         : "Chat channel: ") +
                                             message);
                }
            },
            Qt::QueuedConnection);
    });
    dc->onMessage([self, channel](std::variant<rtc::binary, rtc::string> message) {
        if (!self) {
            return;
        }
        if (!std::holds_alternative<rtc::string>(message)) {
            QMetaObject::invokeMethod(
                self,
                [self, channel] {
                    if (self) {
                        emit self->errorOccurred(
                            QString(channel == TextChannel::Control ? "Control" : "Chat") +
                            " channel received an unexpected binary message.");
                    }
                },
                Qt::QueuedConnection);
            return;
        }

        const auto text = QString::fromUtf8(std::get<rtc::string>(message));
        QMetaObject::invokeMethod(
            self,
            [self, channel, text] {
                if (self) {
                    if (channel == TextChannel::Control) {
                        emit self->controlTextReceived(text);
                    } else {
                        emit self->chatTextReceived(text);
                    }
                }
            },
            Qt::QueuedConnection);
    });
    dc->onBufferedAmountLow([self, channel] {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self, channel] {
                    if (self) {
                        self->flushTextQueue(channel);
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
    state_->audioEndpoint->setTrack(track);

    const auto rtpConfig = std::make_shared<rtc::RtpPacketizationConfig>(
        state_->audioSsrc, "tiny-mesh-audio", OpusPayloadType,
        rtc::OpusRtpPacketizer::DefaultClockRate);
    const auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtpConfig);
    packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfig));
    const auto endpoint = state_->audioEndpoint;
    packetizer->addToChain(std::make_shared<OpusRtpFrameHandler>(
        [connectionId = state_->connectionId, audioTransport = state_->audioTransport,
         endpoint](quint32 ssrc, quint16 sequenceNumber, quint32 rtpTimestamp, QByteArray payload,
                   qint64 receivedAtNs) {
            endpoint->recordReceived();
            if (const auto worker = audioTransport.lock()) {
                worker->enqueueIncoming(connectionId, ssrc, sequenceNumber, rtpTimestamp,
                                        std::move(payload), receivedAtNs);
            }
        }));
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
    auto expectedState = AudioNegotiationState::Idle;
    if (!state_->audioNegotiation.compare_exchange_strong(
            expectedState, AudioNegotiationState::CreatingOffer, std::memory_order_acq_rel)) {
        throw std::runtime_error("Audio negotiation is already in progress.");
    }

    try {
        if (!state_->audioTrack) {
            rtc::Description::Audio audio("audio", rtc::Description::Direction::SendRecv);
            audio.addOpusCodec(OpusPayloadType, OpusProfile);
            audio.addSSRC(state_->audioSsrc, "tiny-mesh-audio", "tiny-mesh-audio", "opus");
            configureAudioTrack(state_->pc->addTrack(audio));
        }
        state_->pc->setLocalDescription(rtc::Description::Type::Offer);
    } catch (...) {
        state_->audioNegotiation.store(AudioNegotiationState::Idle, std::memory_order_release);
        throw;
    }
}

void PeerConnection::acceptAudioOffer(const QString& sdp) {
    auto expectedState = AudioNegotiationState::Idle;
    if (!state_->audioNegotiation.compare_exchange_strong(
            expectedState, AudioNegotiationState::CreatingAnswer, std::memory_order_acq_rel)) {
        throw std::runtime_error("Audio negotiation is already in progress.");
    }

    try {
        state_->pc->setRemoteDescription(rtc::Description(sdp.toStdString(), "offer"));
        state_->pc->setLocalDescription(rtc::Description::Type::Answer);
    } catch (...) {
        state_->audioNegotiation.store(AudioNegotiationState::Idle, std::memory_order_release);
        throw;
    }
}

void PeerConnection::acceptAudioAnswer(const QString& sdp) {
    if (state_->audioNegotiation.load(std::memory_order_acquire) !=
        AudioNegotiationState::AwaitingAnswer) {
        throw std::runtime_error("No audio offer is awaiting an answer.");
    }

    state_->pc->setRemoteDescription(rtc::Description(sdp.toStdString(), "answer"));
    state_->audioNegotiation.store(AudioNegotiationState::Idle, std::memory_order_release);
}

bool PeerConnection::sendControl(const QString& text) {
    return sendText(state_->controlDc, text, TextChannel::Control);
}

bool PeerConnection::sendChat(const QString& text) {
    return sendText(state_->chatDc, text, TextChannel::Chat);
}

bool PeerConnection::sendText(const std::shared_ptr<rtc::DataChannel>& channel, const QString& text,
                              TextChannel textChannel) {
    Q_ASSERT(QThread::currentThread() == thread());

    if (!channel || !channel->isOpen()) {
        return false;
    }
    const auto bytes = text.toUtf8();
    if (bytes.isEmpty() || static_cast<size_t>(bytes.size()) > channel->maxMessageSize()) {
        return false;
    }

    auto& queue = textChannel == TextChannel::Control ? state_->controlQueue : state_->chatQueue;
    auto& queuedBytes =
        textChannel == TextChannel::Control ? state_->queuedControlBytes : state_->queuedChatBytes;
    if (!queue.isEmpty() || channel->bufferedAmount() > TextBufferedHighWater) {
        if (queuedBytes + static_cast<quint64>(bytes.size()) > MaxQueuedTextBytes) {
            return false;
        }
        queue.enqueue(bytes);
        queuedBytes += static_cast<quint64>(bytes.size());
        return true;
    }

    try {
        channel->send(bytes.toStdString());
        return true;
    } catch (const std::exception& error) {
        emit errorOccurred("DataChannel send: " + QString::fromUtf8(error.what()));
        return false;
    }
}

void PeerConnection::flushTextQueue(TextChannel textChannel) {
    Q_ASSERT(QThread::currentThread() == thread());

    const auto channel = textChannel == TextChannel::Control ? state_->controlDc : state_->chatDc;
    if (!channel || !channel->isOpen()) {
        return;
    }
    auto& queue = textChannel == TextChannel::Control ? state_->controlQueue : state_->chatQueue;
    auto& queuedBytes =
        textChannel == TextChannel::Control ? state_->queuedControlBytes : state_->queuedChatBytes;
    while (!queue.isEmpty() && channel->bufferedAmount() <= TextBufferedHighWater) {
        const auto& bytes = queue.head();
        try {
            channel->send(bytes.toStdString());
        } catch (const std::exception& error) {
            emit errorOccurred("DataChannel queue flush: " + QString::fromUtf8(error.what()));
            return;
        }
        queuedBytes -= static_cast<quint64>(bytes.size());
        queue.dequeue();
    }
}

PeerConnectionSnapshot PeerConnection::snapshot() const {
    Q_ASSERT(QThread::currentThread() == thread());

    PeerConnectionSnapshot result;
    result.controlOpen = state_->controlDc && state_->controlDc->isOpen();
    result.chatOpen = state_->chatDc && state_->chatDc->isOpen();
    result.audioTrackOpen = state_->audioEndpoint->isOpen();
    result.audioFramesAttempted = state_->audioEndpoint->framesAttempted();
    result.audioFramesSent = state_->audioEndpoint->framesSent();
    result.audioFramesReceived = state_->audioEndpoint->framesReceived();
    result.controlBufferedBytes =
        state_->controlDc ? static_cast<quint64>(state_->controlDc->bufferedAmount()) : 0;
    result.chatBufferedBytes =
        state_->chatDc ? static_cast<quint64>(state_->chatDc->bufferedAmount()) : 0;
    result.queuedControlBytes = state_->queuedControlBytes;
    result.queuedChatBytes = state_->queuedChatBytes;
    return result;
}

std::shared_ptr<AudioTransportEndpoint> PeerConnection::audioEndpoint() const {
    return state_->audioEndpoint;
}

} // namespace tmc
