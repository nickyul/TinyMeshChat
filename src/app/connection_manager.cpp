#include "tmc/app/connection_manager.h"

#include "tmc/network/peer_connection.h"

#include <QDateTime>
#include <QSet>
#include <QTimer>

#include <utility>

namespace tmc {

struct ConnectionManager::Link {
    QString connectionId;
    PeerIdentity remote;
    std::shared_ptr<PeerConnection> transport;
    bool open{false};
    bool everOpened{false};
    bool localOffer{false};
    bool answerApplied{false};
    bool signalingProduced{false};
    bool meshManaged{false};
    qint64 lastActivityMs{0};
    ConnectionState transportState{ConnectionState::Disconnected};
    ConnectionAttemptState attemptState{ConnectionAttemptState::Gathering};
    QTimer* deadline{};
    quint64 deadlineGeneration{0};
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

Result<void> ConnectionManager::create(const QString& connectionId, const PeerIdentity& remote,
                                       bool localOffer, bool meshManaged) {
    if (connectionId.isEmpty() || links_.contains(connectionId)) {
        return Result<void>::failure("Соединение с таким идентификатором уже существует.");
    }
    auto link = std::make_shared<Link>();
    link->connectionId = connectionId;
    link->remote = remote;
    link->localOffer = localOffer;
    link->meshManaged = meshManaged;
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
        if (link->meshManaged) {
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

void ConnectionManager::setRemote(const QString& connectionId, const PeerIdentity& remote) {
    if (const auto link = current(connectionId)) {
        link->remote = remote;
    }
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

void ConnectionManager::discardStale(const QString& peerId) {
    const auto snapshot = links_.values();
    for (const auto& link : snapshot) {
        const bool samePeer = !peerId.isEmpty() && link->remote.peerId == peerId;
        if (!link->open && (peerId.isEmpty() || samePeer)) {
            discard(link->connectionId);
        }
    }
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

bool ConnectionManager::sendText(const QString& connectionId, const QString& text) {
    const auto link = current(connectionId);
    return link && link->open && link->transport->sendText(text);
}

void ConnectionManager::sendVoiceFrameToOpen(quint32 sequence, const QByteArray& payload) {
    for (const auto& link : links_) {
        if (link->open) {
            link->transport->sendVoiceFrame(sequence, payload);
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
    connect(
        link->transport.get(), &PeerConnection::localDescriptionReady, this,
        [this, link](const QString& type, const QString& sdp) {
            if (!isCurrent(link) || link->signalingProduced) {
                return;
            }
            cancelDeadline(link);
            link->signalingProduced = true;
            setAttemptState(link, link->localOffer ? ConnectionAttemptState::AwaitingAnswer
                                                   : ConnectionAttemptState::AwaitingConnection);
            startDeadline(link,
                          link->meshManaged ? policy_.connectionTimeoutSeconds
                                            : policy_.manualSignalingTimeoutSeconds,
                          link->localOffer
                              ? "Истёк срок ожидания answer. Создайте новое приглашение."
                              : "Истёк срок ожидания подключения. Импортируйте offer повторно.");
            emit localDescriptionReady(link->connectionId, type, sdp);
        });
    connect(link->transport.get(), &PeerConnection::stateChanged, this,
            [this, link](ConnectionState state) {
                if (!isCurrent(link)) {
                    return;
                }
                link->transportState = state;
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
    connect(link->transport.get(), &PeerConnection::channelOpened, this, [this, link] {
        if (!isCurrent(link)) {
            return;
        }
        cancelDeadline(link);
        setAttemptState(link, ConnectionAttemptState::Connected);
        link->open = true;
        link->everOpened = true;
        link->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        emit linkOpened(link->connectionId, link->remote);
    });
    connect(link->transport.get(), &PeerConnection::channelClosed, this, [this, link] {
        if (isCurrent(link)) {
            fail(link, ConnectionAttemptState::Failed,
                 link->open ? "Прямое P2P-соединение закрыто."
                            : "P2P-канал закрылся до завершения подключения.");
        }
    });
    connect(link->transport.get(), &PeerConnection::textReceived, this,
            [this, link](const QString& text) {
                if (!isCurrent(link)) {
                    return;
                }
                link->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
                emit textReceived(link->connectionId, text);
            });
    connect(link->transport.get(), &PeerConnection::voiceFrameReceived, this,
            [this, link](quint32 sequence, const QByteArray& payload) {
                if (!isCurrent(link)) {
                    return;
                }
                link->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
                emit voiceFrameReceived(link->connectionId, sequence, payload);
            });
    connect(link->transport.get(), &PeerConnection::errorOccurred, this,
            [this, link](const QString& error) {
                if (!isCurrent(link)) {
                    return;
                }
                if (!link->open) {
                    fail(link, ConnectionAttemptState::Failed, "Ошибка P2P-транспорта: " + error);
                    return;
                }
                emit statusChanged("Ошибка P2P-транспорта: " + error);
            });
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

void ConnectionManager::fail(const std::shared_ptr<Link>& link, ConnectionAttemptState state,
                             const QString& message) {
    if (!isCurrent(link)) {
        return;
    }
    const auto remote = link->remote;
    const bool meshManaged = link->meshManaged;
    const bool localOffer = link->localOffer;
    setAttemptState(link, state);
    emit statusChanged(message);
    remove(link);
    emit attemptFailed(remote, meshManaged, localOffer, message);
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
    links_.remove(connectionId);
    if (link->transport) {
        QObject::disconnect(link->transport.get(), nullptr, this, nullptr);
        link->transport.reset();
    }
    emit linkRemoved(connectionId, remote, wasOpen);
}

ConnectionInfo ConnectionManager::snapshot(const std::shared_ptr<Link>& link) const {
    return {link->connectionId, link->remote,        link->open,          link->everOpened,
            link->localOffer,   link->meshManaged,   link->answerApplied, link->transportState,
            link->attemptState, link->lastActivityMs};
}

} // namespace tmc
