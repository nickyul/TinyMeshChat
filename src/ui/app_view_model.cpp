#include "tmc/ui/app_view_model.h"

#include "tmc/app/application_controller.h"
#include "tmc/app/network_session.h"
#include "tmc/messaging/chat_message.h"

#include <QAbstractListModel>
#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QVector>

namespace tmc {

class MessagesModel final : public QAbstractListModel {
public:
    enum Role {
        MessageIdRole = Qt::UserRole + 1,
        AuthorRole,
        TextRole,
        TimestampRole,
        LocalRole,
        DeliveryRole
    };

    struct Row {
        ChatMessage message;
        QString author;
        QString delivery;
        bool local{false};
    };

    explicit MessagesModel(QObject* parent = nullptr) : QAbstractListModel(parent) {
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : rows_.size();
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
            return {};
        }
        const auto& row = rows_[index.row()];
        switch (role) {
        case MessageIdRole:
            return row.message.messageId;
        case AuthorRole:
            return row.author;
        case TextRole:
            return row.message.text;
        case TimestampRole:
            return row.message.createdAt.toLocalTime().toString("HH:mm");
        case LocalRole:
            return row.local;
        case DeliveryRole:
            return row.delivery;
        default:
            return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override {
        return {{MessageIdRole, "messageId"}, {AuthorRole, "author"}, {TextRole, "text"},
                {TimestampRole, "timestamp"}, {LocalRole, "local"},   {DeliveryRole, "delivery"}};
    }

    void add(const ChatMessage& message, QString author, bool local) {
        int row = 0;
        while (row < rows_.size()) {
            const auto& current = rows_[row].message;
            if (message.logicalClock < current.logicalClock ||
                (message.logicalClock == current.logicalClock &&
                 message.senderId < current.senderId) ||
                (message.logicalClock == current.logicalClock &&
                 message.senderId == current.senderId && message.messageId < current.messageId)) {
                break;
            }
            ++row;
        }
        beginInsertRows({}, row, row);
        rows_.insert(row, {message, std::move(author), {}, local});
        endInsertRows();
        constexpr int MaxVisibleMessages = 2000;
        if (rows_.size() > MaxVisibleMessages) {
            beginRemoveRows({}, 0, 0);
            rows_.removeFirst();
            endRemoveRows();
        }
    }

    void updateDelivery(const QString& messageId, int acknowledged, int expected) {
        for (int row = 0; row < rows_.size(); ++row) {
            if (rows_[row].message.messageId != messageId) {
                continue;
            }
            rows_[row].delivery =
                expected > 0 ? QString("%1/%2").arg(acknowledged).arg(expected) : QString();
            emit dataChanged(index(row), index(row), {DeliveryRole});
            return;
        }
    }

    void clear() {
        if (rows_.isEmpty()) {
            return;
        }
        beginResetModel();
        rows_.clear();
        endResetModel();
    }

private:
    QVector<Row> rows_;
};

class PeersModel final : public QAbstractListModel {
public:
    enum Role {
        PeerIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        ConnectedRole,
        VoiceJoinedRole,
        MutedRole,
        SelfRole
    };

    struct Row {
        QString peerId;
        QString displayName;
        bool connected{false};
        bool voiceJoined{false};
        bool muted{false};
        bool self{false};
    };

    explicit PeersModel(QObject* parent = nullptr) : QAbstractListModel(parent) {
    }

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : rows_.size();
    }

    QVariant data(const QModelIndex& index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
            return {};
        }
        const auto& row = rows_[index.row()];
        switch (role) {
        case PeerIdRole:
            return row.peerId;
        case DisplayNameRole:
            return row.displayName;
        case ConnectedRole:
            return row.connected;
        case VoiceJoinedRole:
            return row.voiceJoined;
        case MutedRole:
            return row.muted;
        case SelfRole:
            return row.self;
        default:
            return {};
        }
    }

    QHash<int, QByteArray> roleNames() const override {
        return {{PeerIdRole, "peerId"},       {DisplayNameRole, "displayName"},
                {ConnectedRole, "connected"}, {VoiceJoinedRole, "voiceJoined"},
                {MutedRole, "muted"},         {SelfRole, "isSelf"}};
    }

    void resetSelf(const PeerIdentity& identity) {
        beginResetModel();
        rows_.clear();
        if (identity.isValid()) {
            rows_.append({identity.peerId, identity.displayName, true, false, false, true});
        }
        endResetModel();
    }

    void updatePeer(const QString& peerId, const QString& name, bool connected) {
        const int row = find(peerId);
        if (row >= 0) {
            rows_[row].displayName = name.isEmpty() ? peerId.left(8) : name;
            rows_[row].connected = connected;
            emit dataChanged(index(row), index(row), {DisplayNameRole, ConnectedRole});
            return;
        }
        const int inserted = rows_.size();
        beginInsertRows({}, inserted, inserted);
        rows_.append({peerId, name.isEmpty() ? peerId.left(8) : name, connected});
        endInsertRows();
    }

    void updateVoice(const QString& peerId, bool joined, bool muted) {
        const int row = find(peerId);
        if (row < 0) {
            return;
        }
        rows_[row].voiceJoined = joined;
        rows_[row].muted = muted;
        emit dataChanged(index(row), index(row), {VoiceJoinedRole, MutedRole});
    }

    void updateSelfName(const QString& name) {
        for (int row = 0; row < rows_.size(); ++row) {
            if (!rows_[row].self) {
                continue;
            }
            rows_[row].displayName = name;
            emit dataChanged(index(row), index(row), {DisplayNameRole});
            return;
        }
    }

    QString nameFor(const QString& peerId) const {
        const int row = find(peerId);
        return row >= 0 ? rows_[row].displayName : peerId.left(8);
    }

private:
    int find(const QString& peerId) const {
        for (int i = 0; i < rows_.size(); ++i) {
            if (rows_[i].peerId == peerId) {
                return i;
            }
        }
        return -1;
    }

    QVector<Row> rows_;
};

AppViewModel::AppViewModel(ApplicationController& controller, bool identityRequired,
                           QObject* parent)
    : QObject(parent), controller_(controller), messages_(std::make_unique<MessagesModel>()),
      peers_(std::make_unique<PeersModel>()), identityRequired_(identityRequired) {
    peers_->resetSelf(controller_.identity());
    connect(&controller_, &ApplicationController::displayNameChanged, this,
            [this](const QString& name) {
                peers_->updateSelfName(name);
                emit displayNameChanged();
            });
    if (!identityRequired_) {
        initializeSession();
    }
}

AppViewModel::~AppViewModel() = default;

bool AppViewModel::identityRequired() const {
    return identityRequired_;
}

QString AppViewModel::displayName() const {
    return controller_.identity().displayName;
}

QString AppViewModel::status() const {
    return status_;
}

bool AppViewModel::meshVisible() const {
    return session_ && (session_->meshState() == MeshSessionState::InMesh ||
                        session_->meshState() == MeshSessionState::Degraded);
}

bool AppViewModel::connecting() const {
    return session_ && session_->meshState() == MeshSessionState::Connecting;
}

bool AppViewModel::degraded() const {
    return session_ && session_->meshState() == MeshSessionState::Degraded;
}

QString AppViewModel::meshSummary() const {
    return meshSummary_;
}

bool AppViewModel::callActive() const {
    return session_ && session_->callActive();
}

bool AppViewModel::muted() const {
    return session_ && session_->muted();
}

QString AppViewModel::stunServersText() const {
    return controller_.config().stunServers.join('\n');
}

QAbstractItemModel* AppViewModel::messages() const {
    return messages_.get();
}

QAbstractItemModel* AppViewModel::peers() const {
    return peers_.get();
}

void AppViewModel::createIdentity(const QString& displayName) {
    if (!identityRequired_) {
        return;
    }
    const auto created = controller_.createIdentity(displayName);
    if (!created) {
        reportError(created.error());
        return;
    }
    identityRequired_ = false;
    peers_->resetSelf(controller_.identity());
    emit identityRequiredChanged();
    emit displayNameChanged();
    initializeSession();
}

void AppViewModel::updateDisplayName(const QString& displayName) {
    const auto updated = controller_.updateDisplayName(displayName);
    if (!updated) {
        reportError(updated.error());
    } else {
        setStatus("Имя пользователя сохранено.");
    }
}

void AppViewModel::updateStunServers(const QString& servers) {
    const auto updated = controller_.updateStunServers(servers.split('\n', Qt::SkipEmptyParts));
    if (!updated) {
        reportError(updated.error());
        return;
    }
    emit stunServersChanged();
    setStatus("Настройки STUN сохранены и применятся к новым соединениям.");
}

void AppViewModel::createMesh() {
    if (!session_) {
        return;
    }
    const auto result = session_->createMesh();
    if (!result) {
        reportError(result.error());
    }
}

void AppViewModel::leaveMesh() {
    if (session_) {
        session_->leaveMesh();
    }
}

void AppViewModel::createInvitation() {
    if (!session_) {
        return;
    }
    const auto result = session_->createInvitation();
    if (!result) {
        reportError(result.error());
    }
}

void AppViewModel::importSignalingText(const QString& text) {
    if (!session_) {
        return;
    }
    const auto result = session_->importSignalingText(text);
    if (!result) {
        reportError(result.error());
    }
}

void AppViewModel::importSignalingFile(const QUrl& url) {
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::ReadOnly)) {
        reportError("Не удалось открыть файл: " + file.errorString());
        return;
    }
    if (!session_) {
        return;
    }
    const auto result = session_->importSignalingDocument(file.readAll());
    if (!result) {
        reportError(result.error());
    }
}

void AppViewModel::saveSignalingFile(const QUrl& url) {
    if (signalingDocument_.isEmpty()) {
        reportError("Нет signaling-документа для сохранения.");
        return;
    }
    QFile file(url.toLocalFile());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(signalingDocument_) != signalingDocument_.size()) {
        reportError("Не удалось сохранить signaling-файл: " + file.errorString());
    }
}

void AppViewModel::copyText(const QString& text) {
    QGuiApplication::clipboard()->setText(text);
    setStatus("Текст скопирован в буфер обмена.");
}

void AppViewModel::sendMessage(const QString& text) {
    if (!session_) {
        return;
    }
    const auto result = session_->sendMessage(text);
    if (!result) {
        reportError(result.error());
    }
}

void AppViewModel::toggleCall() {
    if (!session_) {
        return;
    }
    if (session_->callActive()) {
        session_->leaveCall();
        return;
    }
    const auto result = session_->startCall();
    if (!result) {
        reportError(result.error());
    }
}

void AppViewModel::toggleMute() {
    if (session_) {
        session_->setMuted(!session_->muted());
    }
}

QString AppViewModel::diagnostics() const {
    const auto network = session_ ? session_->diagnostics() : QString("Mesh не активен");
    return network + "\n\nЛокальный Peer ID: " + controller_.identity().peerId + "\nSTUN:\n  " +
           controller_.config().stunServers.join("\n  ") + "\n\nКаталог данных:\n" +
           controller_.dataDirectory();
}

void AppViewModel::initializeSession() {
    if (session_ || !controller_.identity().isValid()) {
        return;
    }
    session_ = std::make_unique<NetworkSession>(controller_, controller_.connectionPolicy());
    connect(session_.get(), &NetworkSession::statusChanged, this, &AppViewModel::setStatus);
    connect(session_.get(), &NetworkSession::errorOccurred, this, &AppViewModel::reportError);
    connect(session_.get(), &NetworkSession::meshStateChanged, this,
            [this](MeshSessionState state) {
                if (state == MeshSessionState::Disconnected) {
                    messages_->clear();
                    peers_->resetSelf(controller_.identity());
                    meshSummary_ = "Прямые связи: 0/0";
                    emit meshSummaryChanged();
                }
                emit meshStateChanged();
            });
    connect(session_.get(), &NetworkSession::meshChanged, this,
            [this](int connected, int expected) {
                meshSummary_ = QString("Прямые связи: %1/%2").arg(connected).arg(expected);
                emit meshSummaryChanged();
            });
    connect(session_.get(), &NetworkSession::peerChanged, this,
            [this](const QString& id, const QString& name, bool connected) {
                peers_->updatePeer(id, name, connected);
            });
    connect(session_.get(), &NetworkSession::peerVoiceChanged, this,
            [this](const QString& id, bool joined, bool muted) {
                peers_->updateVoice(id, joined, muted);
            });
    connect(session_.get(), &NetworkSession::messageReceived, this,
            [this](const ChatMessage& message, bool local) {
                const auto author =
                    local ? controller_.identity().displayName : peers_->nameFor(message.senderId);
                messages_->add(message, author, local);
            });
    connect(session_.get(), &NetworkSession::deliveryChanged, this,
            [this](const QString& id, int acknowledged, int expected) {
                messages_->updateDelivery(id, acknowledged, expected);
            });
    connect(session_.get(), &NetworkSession::callStateChanged, this,
            [this](bool, bool) { emit callStateChanged(); });
    connect(session_.get(), &NetworkSession::signalingReady, this,
            [this](const QString& kind, const QString& text, const QByteArray& document,
                   const QString& suggestedName) {
                signalingDocument_ = document;
                signalingName_ = suggestedName;
                QGuiApplication::clipboard()->setText(text);
                emit signalingRequested(kind, text, suggestedName);
            });
}

void AppViewModel::setStatus(const QString& status) {
    if (status_ == status) {
        return;
    }
    status_ = status;
    emit statusChanged();
}

void AppViewModel::reportError(const QString& error) {
    setStatus(error);
    emit errorRequested(error);
}

} // namespace tmc
