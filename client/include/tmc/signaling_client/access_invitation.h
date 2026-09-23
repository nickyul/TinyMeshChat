#pragma once
#include <QJsonObject>
#include <QUrl>
#include <optional>

namespace tmc {
struct AccessInvitation { QUrl server; QString authority; QString token; };
QString encodeAccessInvitation(const AccessInvitation& invitation);
std::optional<AccessInvitation> decodeAccessInvitation(const QString& text);
QJsonObject decodeAccessGrant(const QString& text);
}
