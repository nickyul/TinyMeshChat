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
    const auto userScope = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const auto digest =
        QCryptographicHash::hash(userScope.toUtf8(), QCryptographicHash::Sha256).toHex().left(16);
    return "tinymesh-chat-" + QString::fromLatin1(digest);
}

bool AppLinkController::forwardToPrimary(const QUrl& url) const {
    QLocalSocket socket;
    socket.connectToServer(serverName(), QIODevice::WriteOnly);
    if (!socket.waitForConnected(250)) {
        return false;
    }
    const auto message = url.isValid() ? url.toString(QUrl::FullyEncoded).toUtf8()
                                       : QByteArray("activate");
    socket.write(message);
    socket.flush();
    socket.waitForBytesWritten(500);
    socket.disconnectFromServer();
    return true;
}

bool AppLinkController::startPrimary(const QUrl& initialUrl) {
    if (forwardToPrimary(initialUrl)) {
        return false;
    }
    QLocalServer::removeServer(serverName());
    if (!server_->listen(serverName())) {
        return true;
    }
    connect(server_.get(), &QLocalServer::newConnection, this, &AppLinkController::acceptConnection);
    return true;
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
