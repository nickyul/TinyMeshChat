#include "tmc/ui/application_instance.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QStandardPaths>

namespace tmc {

ApplicationInstance::ApplicationInstance(QObject* parent)
    : QObject(parent), server_(std::make_unique<QLocalServer>()) {
}

ApplicationInstance::~ApplicationInstance() = default;

QString ApplicationInstance::serverName() const {
    auto userScope = qEnvironmentVariable("TMC_DATA_DIR");
    if (userScope.isEmpty()) {
        userScope = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    } else {
        userScope = QDir(userScope).absolutePath();
    }
    const auto digest =
        QCryptographicHash::hash(userScope.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return "tinymesh-chat-" + QString::fromLatin1(digest);
}

Result<bool> ApplicationInstance::forwardToPrimary() const {
    QLocalSocket socket;
    socket.connectToServer(serverName(), QIODevice::WriteOnly);
    if (!socket.waitForConnected(250)) {
        return Result<bool>::success(false);
    }
    const auto message = QByteArray("activate");
    if (socket.write(message) != message.size()) {
        return Result<bool>::failure("Не удалось передать команду запущенному приложению.");
    }
    if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(500)) {
        return Result<bool>::failure("Истекло время передачи команды запущенному приложению.");
    }
    socket.disconnectFromServer();
    return Result<bool>::success(true);
}

Result<AppInstanceState> ApplicationInstance::startPrimary() {
    const auto forwarded = forwardToPrimary();
    if (!forwarded) {
        return Result<AppInstanceState>::failure(forwarded.error());
    }
    if (forwarded.value()) {
        return Result<AppInstanceState>::success(AppInstanceState::ForwardedToPrimary);
    }
    QLocalServer::removeServer(serverName());
    if (!server_->listen(serverName())) {
        return Result<AppInstanceState>::failure(
            QString("Не удалось запустить локальный сервер приложения: %1")
                .arg(server_->errorString()));
    }
    connect(server_.get(), &QLocalServer::newConnection, this, &ApplicationInstance::acceptConnection);
    return Result<AppInstanceState>::success(AppInstanceState::Primary);
}

void ApplicationInstance::acceptConnection() {
    while (auto* socket = server_->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            const auto message = QString::fromUtf8(socket->readAll()).trimmed();
            if (message == "activate") {
                emit activationRequested();
            }
            socket->deleteLater();
        });
    }
}

} // namespace tmc
