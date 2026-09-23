#pragma once

#include <QJsonObject>
#include <QStringList>

#include <optional>

namespace tmc::signaling_protocol {

inline constexpr int MinTurnLifetimeSeconds = 600;
inline constexpr int MaxTurnLifetimeSeconds = 7 * 24 * 60 * 60;
inline constexpr qsizetype MaxTurnUrls = 8;

// Runtime credentials issued by signaling. Never part of the client's saved configuration.
struct TurnCredentials {
    QStringList urls;
    QString username;
    QString password;
    int expiresInSeconds{0};
};

[[nodiscard]] bool validTurnUrl(const QString& url);
[[nodiscard]] QJsonObject encodeTurnCredentials(const TurnCredentials& credentials);
// An empty object means that this signaling server offers no TURN service.
[[nodiscard]] std::optional<TurnCredentials> decodeTurnCredentials(const QJsonObject& object);

} // namespace tmc::signaling_protocol
