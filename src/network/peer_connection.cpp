#include "tmc/network/peer_connection.h"

#include "tmc/core/logger.h"

#include <QPointer>
#include <QtEndian>

#include <atomic>
#include <chrono>
#include <cstring>
#include <rtc/rtc.hpp>

namespace tmc {

struct PeerConnection::State {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::DataChannel> voiceDc;
    std::atomic<quint64> droppedVoiceFrames{0};
};

namespace {

constexpr char VoiceMagic[] = {'T', 'M', 'V', '1'};
constexpr qsizetype VoiceHeaderSize = 8;
constexpr qsizetype MaxVoicePayload = 4000;

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
    state_->pc = std::make_shared<rtc::PeerConnection>(cfg);
    QPointer<PeerConnection> self(this);
    std::weak_ptr<State> weak = state_;
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
                    if (dc->label() == "tiny-mesh-voice") {
                        self->configureVoiceChannel(dc);
                    } else if (dc->label() == "tiny-mesh-chat") {
                        self->configureChannel(dc);
                    }
                },
                Qt::QueuedConnection);
        }
    });
}

PeerConnection::~PeerConnection() {
    auto s = std::move(state_);
    if (s) {
        if (s->voiceDc) {
            s->voiceDc->close();
        }
        if (s->dc) {
            s->dc->close();
        }
        if (s->pc) {
            s->pc->close();
        }
    }
}

void PeerConnection::configureChannel(const std::shared_ptr<rtc::DataChannel>& dc) {
    state_->dc = dc;
    QPointer<PeerConnection> self(this);
    dc->onOpen([self] {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self] {
                    if (self) {
                        emit self->channelOpened();
                    }
                },
                Qt::QueuedConnection);
        }
    });
    dc->onClosed([self] {
        if (self) {
            QMetaObject::invokeMethod(
                self,
                [self] {
                    if (self) {
                        emit self->channelClosed();
                    }
                },
                Qt::QueuedConnection);
        }
    });
    dc->onError([self](const std::string& error) {
        if (!self) {
            return;
        }
        const auto message = QString::fromStdString(error);
        QMetaObject::invokeMethod(
            self,
            [self, message] {
                if (self) {
                    emit self->errorOccurred(message);
                }
            },
            Qt::QueuedConnection);
    });
    dc->onMessage([self](std::variant<rtc::binary, rtc::string> m) {
        if (!self || !std::holds_alternative<rtc::string>(m)) {
            return;
        }
        auto text = QString::fromUtf8(std::get<rtc::string>(m));
        QMetaObject::invokeMethod(
            self,
            [self, text] {
                if (self) {
                    emit self->textReceived(text);
                }
            },
            Qt::QueuedConnection);
    });
}

void PeerConnection::configureVoiceChannel(const std::shared_ptr<rtc::DataChannel>& dc) {
    state_->voiceDc = dc;
    QPointer<PeerConnection> self(this);
    dc->onError([self](const std::string& error) {
        if (!self) {
            return;
        }
        const auto message = QString::fromStdString(error);
        QMetaObject::invokeMethod(
            self,
            [self, message] {
                if (self) {
                    emit self->errorOccurred("Voice channel: " + message);
                }
            },
            Qt::QueuedConnection);
    });
    dc->onMessage([self](std::variant<rtc::binary, rtc::string> message) {
        if (!self || !std::holds_alternative<rtc::binary>(message)) {
            return;
        }
        const auto& bytes = std::get<rtc::binary>(message);
        const auto receivedAtNs = monotonicNs();
        if (bytes.size() <= VoiceHeaderSize || bytes.size() > VoiceHeaderSize + MaxVoicePayload) {
            return;
        }
        const auto* data = reinterpret_cast<const char*>(bytes.data());
        if (std::memcmp(data, VoiceMagic, sizeof(VoiceMagic)) != 0) {
            return;
        }
        quint32 encodedSequence{};
        std::memcpy(&encodedSequence, data + sizeof(VoiceMagic), sizeof(encodedSequence));
        const auto sequence = qFromBigEndian(encodedSequence);
        const QByteArray payload(data + VoiceHeaderSize,
                                 static_cast<qsizetype>(bytes.size()) - VoiceHeaderSize);
        QMetaObject::invokeMethod(
            self,
            [self, sequence, payload, receivedAtNs] {
                if (self) {
                    emit self->voiceFrameReceived(sequence, payload, receivedAtNs);
                }
            },
            Qt::QueuedConnection);
    });
}

void PeerConnection::createOffer() {
    emit stateChanged(ConnectionState::Gathering);
    configureChannel(state_->pc->createDataChannel("tiny-mesh-chat"));
    rtc::DataChannelInit voiceInit;
    voiceInit.reliability.unordered = true;
    voiceInit.reliability.maxRetransmits = 0;
    configureVoiceChannel(state_->pc->createDataChannel("tiny-mesh-voice", voiceInit));
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

bool PeerConnection::sendText(const QString& s) {
    return state_->dc && state_->dc->isOpen() && state_->dc->send(s.toUtf8().toStdString());
}

bool PeerConnection::sendVoiceFrame(quint32 sequence, const QByteArray& opusPayload,
                                    const VoiceFrameTiming& timing) {
    if (!state_->voiceDc || !state_->voiceDc->isOpen() || opusPayload.isEmpty() ||
        opusPayload.size() > MaxVoicePayload) {
        return false;
    }
    constexpr size_t MaxBufferedVoiceBytes = 64 * 1024;
    const auto bufferedBytes = state_->voiceDc->bufferedAmount();
    if (bufferedBytes > MaxBufferedVoiceBytes) {
        state_->droppedVoiceFrames.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    QByteArray packet(VoiceHeaderSize + opusPayload.size(), Qt::Uninitialized);
    std::memcpy(packet.data(), VoiceMagic, sizeof(VoiceMagic));
    const auto encodedSequence = qToBigEndian(sequence);
    std::memcpy(packet.data() + sizeof(VoiceMagic), &encodedSequence, sizeof(encodedSequence));
    std::memcpy(packet.data() + VoiceHeaderSize, opusPayload.constData(), opusPayload.size());
    Logger::instance().trace(
        "voice_tx",
        QString("seq=%1 capture_queue_ms=%2 dsp_ms=%3 encode_ms=%4 gui_queue_ms=%5 "
                "dc_buffer_bytes=%6 payload_bytes=%7")
            .arg(sequence)
            .arg(timing.captureQueueMs, 0, 'f', 1)
            .arg(timing.dspMs, 0, 'f', 3)
            .arg(timing.encodeMs, 0, 'f', 3)
            .arg(static_cast<double>(monotonicNs() - timing.encodedAtNs) / 1'000'000.0, 0, 'f', 3)
            .arg(bufferedBytes)
            .arg(opusPayload.size()));
    return state_->voiceDc->send(reinterpret_cast<const rtc::byte*>(packet.constData()),
                                 static_cast<size_t>(packet.size()));
}

quint64 PeerConnection::droppedVoiceFrames() const {
    return state_->droppedVoiceFrames.load(std::memory_order_relaxed);
}

} // namespace tmc
