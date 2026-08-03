#include "tmc/app/connection_manager.h"

#include "tmc/core/logger.h"
#include "tmc/network/peer_connection.h"

#include <QDateTime>
#include <QSet>
#include <QTimer>

#include <utility>

namespace tmc {

namespace {

constexpr qsizetype MaxRecentAttempts = 8;

} // namespace

struct ConnectionManager::Link {
    // Identity and owned transport.
    QString connectionId;
    PeerIdentity remote;
    std::shared_ptr<PeerConnection> transport;
    qint64 createdAtMs{0};
    qint64 lastActivityMs{0};
    bool open{false};

    // Signaling role and progress.
    ConnectionKind kind{ConnectionKind::ManualOffer};
    bool answerApplied{false};
    bool signalingProduced{false};
    quint64 generation{0};

    // Transport and handshake readiness.
    bool transportOpen{false};
    bool controlChannelOpen{false};
    bool chatChannelOpen{false};
    bool helloReceived{false};
    ConnectionState transportState{ConnectionState::Disconnected};
    ConnectionAttemptState attemptState{ConnectionAttemptState::Gathering};

    // ICE and connection diagnostics.
    QString iceState{"new"};
    QString selectedCandidatePair;
    QString lastError;
    int hostCandidates{0};
    int serverReflexiveCandidates{0};
    int relayCandidates{0};
    int roundTripTimeMs{-1};

    // Deadline and delayed-disconnect guards.
    QTimer* deadline{};
    quint64 deadlineGeneration{0};
    quint64 disconnectGeneration{0};
};

ConnectionManager::ConnectionManager(QStringList stunServers, ConnectionPolicy policy,
                                     QObject* parent)
    : QObject(parent), stunServers_(std::move(stunServers)), policy_(policy) {
    Q_ASSERT(policy_.isValid());
}

ConnectionManager::~ConnectionManager() = default;

void ConnectionManager::setStunServers(QStringList stunServers) {
    stunServers_ = std::move(stunServers);
}

int ConnectionManager::recordRoundTripTime(const QString& connectionId, int sampleMs) {
    const auto link = current(connectionId);
    if (!link || sampleMs < 0) {
        return -1;
    }
    link->roundTripTimeMs = link->roundTripTimeMs < 0
                                ? sampleMs
                                : (link->roundTripTimeMs * 3 + sampleMs) / 4;
    return link->roundTripTimeMs;
}

Result<void> ConnectionManager::create(const QString& connectionId, const PeerIdentity& remote,
                                       ConnectionKind kind, quint64 generation) {
    if (connectionId.isEmpty() || links_.contains(connectionId)) {
        return Result<void>::failure("Соединение с таким идентификатором уже существует.");
    }
    auto link = std::make_shared<Link>();
    link->connectionId = connectionId;
    link->remote = remote;
    link->kind = kind;
    link->generation = generation;
    link->createdAtMs = QDateTime::currentMSecsSinceEpoch();
    link->transport = std::make_shared<PeerConnection>(stunServers_);
    links_.insert(connectionId, link);
    configure(link);
    emit attemptChanged(connectionId, link->attemptState);
    return Result<void>::success();
}

Result<void> ConnectionManager::startOffer(const QString& connectionId) {
    const auto link = current(connectionId);
    if (!link) {
        return Result<void>::failure("Соединение не найдено.");
    }
    startDeadline(link, policy_.iceGatheringTimeoutSeconds,
                  "Истёк таймаут сбора ICE-кандидатов. Проверьте STUN и сеть.");
    try {
        link->transport->createOffer();
    } catch (const std::exception& e) {
        setAttemptState(link, ConnectionAttemptState::Failed);
        remove(link);
        return Result<void>::failure(QString::fromUtf8(e.what()));
    }
    return Result<void>::success();
}

Result<void> ConnectionManager::acceptOffer(const QString& connectionId, const QString& sdp) {
    const auto link = current(connectionId);
    if (!link) {
        return Result<void>::failure("Соединение не найдено.");
    }
    startDeadline(link, policy_.iceGatheringTimeoutSeconds,
                  "Истёк таймаут подготовки answer. Проверьте STUN и сеть.");
    try {
        link->transport->acceptOffer(sdp);
    } catch (const std::exception& e) {
        setAttemptState(link, ConnectionAttemptState::Failed);
        remove(link);
        return Result<void>::failure(QString::fromUtf8(e.what()));
    }
    return Result<void>::success();
}

Result<void> ConnectionManager::acceptAnswer(const QString& connectionId, const QString& sdp) {
    const auto link = current(connectionId);
    if (!link) {
        return Result<void>::failure("Соединение не найдено.");
    }
    if (link->answerApplied) {
        return Result<void>::failure("Этот answer уже импортирован.");
    }
    link->answerApplied = true;
    cancelDeadline(link);
    setAttemptState(link, ConnectionAttemptState::Connecting);
    try {
        link->transport->acceptAnswer(sdp);
    } catch (const std::exception& e) {
        link->answerApplied = false;
        if (isMeshManaged(link->kind)) {
            setAttemptState(link, ConnectionAttemptState::Failed);
            remove(link);
        } else {
            setAttemptState(link, ConnectionAttemptState::AwaitingAnswer);
            startDeadline(link, policy_.manualSignalingTimeoutSeconds,
                          "Истёк срок ожидания корректного answer. Создайте новое приглашение.");
        }
        return Result<void>::failure(QString::fromUtf8(e.what()));
    }
    startDeadline(link, policy_.connectionTimeoutSeconds,
                  "Не удалось установить прямое соединение за отведённое время.");
    return Result<void>::success();
}

Result<void> ConnectionManager::startAudioOffer(const QString& connectionId) {
    const auto link = current(connectionId);
    if (!link || !link->open) {
        return Result<void>::failure("Открытое соединение не найдено.");
    }
    try {
        link->transport->createAudioOffer();
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(QString::fromUtf8(error.what()));
    }
}

Result<void> ConnectionManager::acceptAudioOffer(const QString& connectionId,
                                                 const QString& sdp) {
    const auto link = current(connectionId);
    if (!link || !link->open) {
        return Result<void>::failure("Открытое соединение не найдено.");
    }
    try {
        link->transport->acceptAudioOffer(sdp);
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(QString::fromUtf8(error.what()));
    }
}

Result<void> ConnectionManager::acceptAudioAnswer(const QString& connectionId,
                                                  const QString& sdp) {
    const auto link = current(connectionId);
    if (!link || !link->open) {
        return Result<void>::failure("Открытое соединение не найдено.");
    }
    try {
        link->transport->acceptAudioAnswer(sdp);
        return Result<void>::success();
    } catch (const std::exception& error) {
        return Result<void>::failure(QString::fromUtf8(error.what()));
    }
}

void ConnectionManager::markHelloReceived(const QString& connectionId,
                                           const PeerIdentity& remote) {
    const auto link = current(connectionId);
    if (!link) {
        return;
    }
    link->remote = remote;
    link->helloReceived = true;
    link->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    updateHandshakeReadiness(link);
}

void ConnectionManager::discard(const QString& connectionId) {
    const auto link = current(connectionId);
    if (!link) {
        return;
    }
    if (link->attemptState != ConnectionAttemptState::Connected &&
        link->attemptState != ConnectionAttemptState::TimedOut &&
        link->attemptState != ConnectionAttemptState::Failed) {
        setAttemptState(link, ConnectionAttemptState::Cancelled);
    }
    remove(link);
}

void ConnectionManager::markTimedOut(const QString& connectionId, const QString& message) {
    const auto link = current(connectionId);
    if (!link || !link->open) {
        return;
    }
    fail(link, ConnectionAttemptState::TimedOut, message);
}

bool ConnectionManager::contains(const QString& connectionId) const {
    return links_.contains(connectionId);
}

std::optional<ConnectionInfo> ConnectionManager::info(const QString& connectionId) const {
    const auto link = current(connectionId);
    if (!link) {
        return std::nullopt;
    }
    return snapshot(link);
}

std::optional<ConnectionInfo> ConnectionManager::infoForPeer(const QString& peerId) const {
    std::shared_ptr<Link> fallback;
    for (const auto& link : links_) {
        if (link->remote.peerId != peerId) {
            continue;
        }
        if (link->open) {
            return snapshot(link);
        }
        if (!fallback) {
            fallback = link;
        }
    }
    if (fallback) {
        return snapshot(fallback);
    }
    return std::nullopt;
}

QList<ConnectionInfo> ConnectionManager::connections() const {
    QList<ConnectionInfo> result;
    result.reserve(links_.size());
    for (const auto& link : links_) {
        result.append(snapshot(link));
    }
    return result;
}

QList<ConnectionInfo> ConnectionManager::recentAttempts() const {
    return recentAttempts_;
}

QStringList ConnectionManager::openConnectionIds() const {
    QStringList result;
    for (const auto& link : links_) {
        if (link->open) {
            result.append(link->connectionId);
        }
    }
    return result;
}

QStringList ConnectionManager::inactiveConnectionIds(qint64 nowMs, qint64 timeoutMs) const {
    QStringList result;
    for (const auto& link : links_) {
        if (link->open && link->lastActivityMs > 0 && nowMs - link->lastActivityMs > timeoutMs) {
            result.append(link->connectionId);
        }
    }
    return result;
}

int ConnectionManager::connectedPeerCount() const {
    QSet<QString> peers;
    for (const auto& link : links_) {
        if (link->open && !link->remote.peerId.isEmpty()) {
            peers.insert(link->remote.peerId);
        }
    }
    return peers.size();
}

bool ConnectionManager::sendControl(const QString& connectionId, const QString& text) {
    const auto link = current(connectionId);
    return link && link->controlChannelOpen && link->transport->sendControl(text);
}

bool ConnectionManager::sendChat(const QString& connectionId, const QString& text) {
    const auto link = current(connectionId);
    return link && link->open && link->chatChannelOpen && link->transport->sendChat(text);
}

void ConnectionManager::sendAudioFrameToOpen(quint32 sequence, const QByteArray& payload) {
    for (const auto& link : links_) {
        if (link->open) {
            link->transport->sendAudioFrame(sequence, payload);
        }
    }
}

std::shared_ptr<ConnectionManager::Link>
ConnectionManager::current(const QString& connectionId) const {
    return links_.value(connectionId);
}

bool ConnectionManager::isCurrent(const std::shared_ptr<Link>& link) const {
    return link && links_.value(link->connectionId) == link;
}

void ConnectionManager::configure(const std::shared_ptr<Link>& link) {
    connectIceSignals(link);
    connectSignalingSignals(link);
    connectTransportSignals(link);
    connectChannelSignals(link);
    connectDataSignals(link);
}

void ConnectionManager::connectIceSignals(const std::shared_ptr<Link>& link) {
    connect(link->transport.get(), &PeerConnection::gatheringStateChanged, this,
            [this, link](const QString& state) {
                if (!isCurrent(link)) {
                    return;
                }
                Logger::instance().log(QtInfoMsg, "ice",
                                       link->connectionId.left(8) + " gathering=" + state);
                emit statusChanged("ICE " + link->connectionId.left(8) + ": " + state);
            });
    connect(link->transport.get(), &PeerConnection::iceStateChanged, this,
            [this, link](const QString& state) {
                if (!isCurrent(link)) {
                    return;
                }
                link->iceState = state;
                Logger::instance().log(QtInfoMsg, "ice",
                                       link->connectionId.left(8) + " state=" + state);
            });
    connect(link->transport.get(), &PeerConnection::candidateDiscovered, this,
            [this, link](const QString& type, const QString& transport) {
                if (!isCurrent(link)) {
                    return;
                }
                if (type == "host") {
                    ++link->hostCandidates;
                } else if (type == "srflx" || type == "prflx") {
                    ++link->serverReflexiveCandidates;
                } else if (type == "relay") {
                    ++link->relayCandidates;
                }
                Logger::instance().log(QtInfoMsg, "ice",
                                       link->connectionId.left(8) + " candidate=" + type + "/" +
                                           transport);
            });
    connect(link->transport.get(), &PeerConnection::selectedCandidatePairChanged, this,
            [this, link](const QString& localType, const QString& remoteType) {
                if (!isCurrent(link)) {
                    return;
                }
                link->selectedCandidatePair = localType + " -> " + remoteType;
                Logger::instance().log(QtInfoMsg, "ice",
                                        link->connectionId.left(8) +
                                            " selected=" + link->selectedCandidatePair);
            });
}

void ConnectionManager::connectSignalingSignals(const std::shared_ptr<Link>& link) {
    connect(
        link->transport.get(), &PeerConnection::localDescriptionReady, this,
        [this, link](const QString&, const QString& sdp) {
            if (!isCurrent(link) || link->signalingProduced) {
                return;
            }
            cancelDeadline(link);
            link->signalingProduced = true;
            setAttemptState(
                link, isOffer(link->kind) ? ConnectionAttemptState::AwaitingAnswer
                                          : ConnectionAttemptState::AwaitingConnection);
            startDeadline(link,
                          isMeshManaged(link->kind) ? policy_.connectionTimeoutSeconds
                                                    : policy_.manualSignalingTimeoutSeconds,
                          isOffer(link->kind)
                              ? "Истёк срок ожидания answer. Создайте новое приглашение."
                              : "Истёк срок ожидания подключения. Импортируйте offer повторно.");
            emit localDescriptionReady(link->connectionId, sdp);
        });
    connect(link->transport.get(), &PeerConnection::audioDescriptionReady, this,
            [this, link](const QString& type, const QString& sdp) {
                if (isCurrent(link) && link->open) {
                    emit audioDescriptionReady(link->connectionId, type, sdp);
                }
            });
}

void ConnectionManager::connectTransportSignals(const std::shared_ptr<Link>& link) {
    connect(link->transport.get(), &PeerConnection::stateChanged, this,
            [this, link](ConnectionState state) {
                if (!isCurrent(link)) {
                    return;
                }
                link->transportState = state;
                if (state == ConnectionState::Connected) {
                    if (link->controlChannelOpen && link->chatChannelOpen) {
                        ++link->disconnectGeneration;
                    }
                    if (link->open && link->controlChannelOpen && link->chatChannelOpen &&
                        link->attemptState == ConnectionAttemptState::Suspect) {
                        setAttemptState(link, ConnectionAttemptState::Connected);
                    }
                }
                if (state == ConnectionState::Disconnected && link->open) {
                    suspect(link, "Прямое P2P-соединение не восстановилось за grace period.");
                    return;
                }
                if (state == ConnectionState::Connecting && !link->open &&
                    (link->attemptState == ConnectionAttemptState::AwaitingAnswer ||
                     link->attemptState == ConnectionAttemptState::AwaitingConnection)) {
                    setAttemptState(link, ConnectionAttemptState::Connecting);
                }
                if (state == ConnectionState::Failed) {
                    fail(link, ConnectionAttemptState::Failed,
                         "Не удалось установить прямое P2P-соединение.");
                    return;
                }
                emit statusChanged("Соединение " + link->connectionId.left(8) + ": " +
                                   toString(state));
            });
    connect(link->transport.get(), &PeerConnection::errorOccurred, this,
            [this, link](const QString& error) {
                if (!isCurrent(link)) {
                    return;
                }
                if (!link->open) {
                    fail(link, ConnectionAttemptState::Failed,
                         "Ошибка P2P-транспорта: " + error);
                    return;
                }
                emit statusChanged("Ошибка P2P-транспорта: " + error);
            });
}

void ConnectionManager::connectChannelSignals(const std::shared_ptr<Link>& link) {
    connect(link->transport.get(), &PeerConnection::controlChannelOpened, this, [this, link] {
        if (!isCurrent(link)) {
            return;
        }
        link->controlChannelOpen = true;
        updateTransportReadiness(link);
    });
    connect(link->transport.get(), &PeerConnection::chatChannelOpened, this, [this, link] {
        if (!isCurrent(link)) {
            return;
        }
        link->chatChannelOpen = true;
        updateTransportReadiness(link);
    });
    const auto channelClosed = [this, link](bool control) {
        if (isCurrent(link)) {
            if (control) {
                link->controlChannelOpen = false;
            } else {
                link->chatChannelOpen = false;
            }
            if (link->open) {
                suspect(link, "Прямой P2P-канал закрылся и не восстановился.");
            } else {
                fail(link, ConnectionAttemptState::Failed,
                     "P2P-канал закрылся до завершения подключения.");
            }
        }
    };
    connect(link->transport.get(), &PeerConnection::controlChannelClosed, this,
            [channelClosed] { channelClosed(true); });
    connect(link->transport.get(), &PeerConnection::chatChannelClosed, this,
            [channelClosed] { channelClosed(false); });
}

void ConnectionManager::connectDataSignals(const std::shared_ptr<Link>& link) {
    connect(link->transport.get(), &PeerConnection::controlTextReceived, this,
            [this, link](const QString& text) {
                if (!isCurrent(link)) {
                    return;
                }
                link->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
                emit controlTextReceived(link->connectionId, text);
            });
    connect(link->transport.get(), &PeerConnection::chatTextReceived, this,
            [this, link](const QString& text) {
                if (!isCurrent(link)) {
                    return;
                }
                link->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
                emit chatTextReceived(link->connectionId, text);
            });
    connect(link->transport.get(), &PeerConnection::audioFrameReceived, this,
            [this, link](quint32 timestamp, const QByteArray& payload, qint64 receivedAtNs) {
                if (!isCurrent(link)) {
                    return;
                }
                link->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
                emit audioFrameReceived(link->connectionId, timestamp, payload, receivedAtNs);
            });
}

void ConnectionManager::updateTransportReadiness(const std::shared_ptr<Link>& link) {
    if (!isCurrent(link) || link->transportOpen || !link->controlChannelOpen ||
        !link->chatChannelOpen) {
        return;
    }
    cancelDeadline(link);
    link->transportOpen = true;
    link->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    setAttemptState(link, ConnectionAttemptState::AwaitingHello);
    startDeadline(link, policy_.helloTimeoutSeconds,
                  "Истёк таймаут handshake: не получен peer.hello.");
    emit transportOpened(link->connectionId, link->remote);
    updateHandshakeReadiness(link);
}

void ConnectionManager::updateHandshakeReadiness(const std::shared_ptr<Link>& link) {
    if (!isCurrent(link) || link->open || !link->transportOpen || !link->helloReceived) {
        return;
    }
    cancelDeadline(link);
    link->open = true;
    link->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    setAttemptState(link, ConnectionAttemptState::Connected);
    emit linkOpened(link->connectionId, link->remote);
}

void ConnectionManager::setAttemptState(const std::shared_ptr<Link>& link,
                                        ConnectionAttemptState state) {
    if (!link || link->attemptState == state) {
        return;
    }
    link->attemptState = state;
    emit attemptChanged(link->connectionId, state);
}

void ConnectionManager::startDeadline(const std::shared_ptr<Link>& link, int seconds,
                                      QString message) {
    if (!isCurrent(link)) {
        return;
    }
    cancelDeadline(link);
    const auto generation = link->deadlineGeneration;
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    link->deadline = timer;
    std::weak_ptr<Link> weak = link;
    connect(timer, &QTimer::timeout, this, [this, weak, generation, message = std::move(message)] {
        const auto pending = weak.lock();
        if (!isCurrent(pending) || pending->open || pending->deadlineGeneration != generation) {
            return;
        }
        fail(pending, ConnectionAttemptState::TimedOut, message);
    });
    timer->start(seconds * 1000);
}

void ConnectionManager::cancelDeadline(const std::shared_ptr<Link>& link) {
    if (!link) {
        return;
    }
    ++link->deadlineGeneration;
    if (!link->deadline) {
        return;
    }
    link->deadline->stop();
    link->deadline->deleteLater();
    link->deadline = nullptr;
}

void ConnectionManager::suspect(const std::shared_ptr<Link>& link, QString message) {
    if (!isCurrent(link) || !link->open ||
        link->attemptState == ConnectionAttemptState::Suspect) {
        return;
    }
    setAttemptState(link, ConnectionAttemptState::Suspect);
    const auto generation = ++link->disconnectGeneration;
    std::weak_ptr<Link> weak = link;
    QTimer::singleShot(
        policy_.disconnectGracePeriodSeconds * 1000, this,
        [this, weak, generation, message = std::move(message)] {
            const auto pending = weak.lock();
            if (!isCurrent(pending) || pending->disconnectGeneration != generation ||
                pending->attemptState != ConnectionAttemptState::Suspect) {
                return;
            }
            fail(pending, ConnectionAttemptState::Failed, message);
        });
}

void ConnectionManager::fail(const std::shared_ptr<Link>& link, ConnectionAttemptState state,
                             const QString& message) {
    if (!isCurrent(link)) {
        return;
    }
    const auto remote = link->remote;
    const auto kind = link->kind;
    link->lastError = message;
    setAttemptState(link, state);
    emit statusChanged(message);
    remove(link);
    emit attemptFailed(remote, kind, message);
}

void ConnectionManager::remove(const std::shared_ptr<Link>& link) {
    if (!isCurrent(link)) {
        cancelDeadline(link);
        return;
    }
    cancelDeadline(link);
    const bool wasOpen = link->open;
    const auto remote = link->remote;
    const auto connectionId = link->connectionId;
    recentAttempts_.prepend(snapshot(link));
    while (recentAttempts_.size() > MaxRecentAttempts) {
        recentAttempts_.removeLast();
    }
    links_.remove(connectionId);
    if (link->transport) {
        QObject::disconnect(link->transport.get(), nullptr, this, nullptr);
        link->transport.reset();
    }
    emit linkRemoved(connectionId, remote, wasOpen);
}

ConnectionInfo ConnectionManager::snapshot(const std::shared_ptr<Link>& link) const {
    ConnectionInfo info;
    const auto transport = link->transport ? link->transport->snapshot() : PeerConnectionSnapshot{};
    info.connectionId = link->connectionId;
    info.remote = link->remote;
    info.createdAtMs = link->createdAtMs;
    info.open = link->open;

    info.kind = link->kind;
    info.answerApplied = link->answerApplied;
    info.generation = link->generation;

    info.controlChannelOpen = transport.controlOpen;
    info.chatChannelOpen = transport.chatOpen;
    info.audioTrackOpen = transport.audioTrackOpen;
    info.helloReceived = link->helloReceived;
    info.transportState = link->transportState;
    info.attemptState = link->attemptState;

    info.iceState = link->iceState;
    info.selectedCandidatePair = link->selectedCandidatePair;
    info.lastError = link->lastError;
    info.hostCandidates = link->hostCandidates;
    info.serverReflexiveCandidates = link->serverReflexiveCandidates;
    info.relayCandidates = link->relayCandidates;
    info.roundTripTimeMs = link->roundTripTimeMs;

    info.audioFramesAttempted = transport.audioFramesAttempted;
    info.audioFramesSent = transport.audioFramesSent;
    info.audioFramesReceived = transport.audioFramesReceived;
    info.controlBufferedBytes = transport.controlBufferedBytes;
    info.chatBufferedBytes = transport.chatBufferedBytes;
    info.queuedControlBytes = transport.queuedControlBytes;
    info.queuedChatBytes = transport.queuedChatBytes;
    return info;
}

} // namespace tmc
