#include "tmc/ui/app_link_controller.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QStandardPaths>

namespace tmc {

AppLinkController::AppLinkController(QObject* parent)
    : QObject(parent), server_(std::make_unique<QLocalServer>()) {
    QCoreApplication::instance()->installEventFilter(this);
}

AppLinkController::~AppLinkController() = default;

QString AppLinkController::serverName() const {
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

Result<bool> AppLinkController::forwardToPrimary(const QUrl& url) const {
    QLocalSocket socket;
    socket.connectToServer(serverName(), QIODevice::WriteOnly);
    if (!socket.waitForConnected(250)) {
        return Result<bool>::success(false);
    }
    const auto message = url.isValid() ? url.toString(QUrl::FullyEncoded).toUtf8()
                                       : QByteArray("activate");
    if (socket.write(message) != message.size()) {
        return Result<bool>::failure("Не удалось передать команду запущенному приложению.");
    }
    if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(500)) {
        return Result<bool>::failure("Истекло время передачи команды запущенному приложению.");
    }
    socket.disconnectFromServer();
    return Result<bool>::success(true);
}

Result<AppInstanceState> AppLinkController::startPrimary(const QUrl& initialUrl) {
    const auto forwarded = forwardToPrimary(initialUrl);
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
    connect(server_.get(), &QLocalServer::newConnection, this, &AppLinkController::acceptConnection);
    return Result<AppInstanceState>::success(AppInstanceState::Primary);
}

void AppLinkController::acceptConnection() {
    while (auto* socket = server_->nextPendingConnection()) {
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            const auto message = QString::fromUtf8(socket->readAll()).trimmed();
            if (message == "activate") {
                emit activationRequested();
            } else {
                const QUrl url(message);
                if (url.scheme() == "tinymesh") {
                    emit urlReceived(url);
                }
            }
            socket->deleteLater();
        });
    }
}

bool AppLinkController::eventFilter(QObject* watched, QEvent* event) {
    Q_UNUSED(watched)
    if (event->type() != QEvent::FileOpen) {
        return false;
    }
    const auto* fileEvent = static_cast<QFileOpenEvent*>(event);
    if (fileEvent->url().scheme() == "tinymesh") {
        emit urlReceived(fileEvent->url());
        return true;
    }
    return false;
}

bool AppLinkController::protocolRegistered() const {
#ifdef Q_OS_WIN
    QSettings command(
        "HKEY_CURRENT_USER\\Software\\Classes\\tinymesh\\shell\\open\\command",
        QSettings::NativeFormat);
    const auto configured = QDir::fromNativeSeparators(command.value(".").toString());
    const auto executable = QDir::fromNativeSeparators(QCoreApplication::applicationFilePath());
    return configured.contains(executable, Qt::CaseInsensitive);
#else
    return true;
#endif
}

Result<void> AppLinkController::registerProtocol() {
#ifdef Q_OS_WIN
    const auto executable = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QSettings root("HKEY_CURRENT_USER\\Software\\Classes\\tinymesh",
                   QSettings::NativeFormat);
    root.setValue(".", "URL:TinyMesh Chat Protocol");
    root.setValue("URL Protocol", "");
    root.setValue("DefaultIcon/.", '"' + executable + "\",0");
    root.setValue("shell/open/command/.", '"' + executable + "\" \"%1\"");
    root.sync();
    if (root.status() != QSettings::NoError || !protocolRegistered()) {
        return Result<void>::failure("Не удалось зарегистрировать протокол tinymesh://.");
    }
#endif
    return Result<void>::success();
}

Result<void> AppLinkController::unregisterProtocol() {
#ifdef Q_OS_WIN
    if (!protocolRegistered()) {
        return Result<void>::success();
    }
    QSettings classes("HKEY_CURRENT_USER\\Software\\Classes", QSettings::NativeFormat);
    classes.remove("tinymesh");
    classes.sync();
    if (classes.status() != QSettings::NoError) {
        return Result<void>::failure("Не удалось удалить регистрацию tinymesh://.");
    }
#endif
    return Result<void>::success();
}

} // namespace tmc
