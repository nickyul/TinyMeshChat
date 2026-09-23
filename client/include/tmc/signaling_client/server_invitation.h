#pragma once

#include <QString>
#include <QUrl>
#include <optional>

namespace tmc {

struct ServerInvitation {
    QUrl server;
    QString roomId;
    QString meshId;
    QString token;
};

QString encodeServerInvitation(const ServerInvitation& invitation);
std::optional<ServerInvitation> decodeServerInvitation(const QString& link);

} // namespace tmc
