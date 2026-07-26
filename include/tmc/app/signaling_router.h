#pragma once

#include <QHash>
#include <QString>

#include <optional>

namespace tmc {

class SignalingRouter final {
public:
    void clear();

    bool rememberPacket(const QString& packetId);
    void observeDirect(const QString& peerId, const QString& connectionId);
    void observeRoute(const QString& peerId, const QString& connectionId, int hops);
    void forgetConnection(const QString& connectionId);
    void forgetPeer(const QString& peerId);

    std::optional<QString> nextHop(const QString& peerId,
                                   const QString& excludedConnection = {}) const;

    void rememberReverseRoute(const QString& requestId, const QString& connectionId);
    std::optional<QString> reverseHop(const QString& requestId) const;

private:
    struct Route {
        QString connectionId;
        int hops{0};
        qint64 updatedAtMs{0};
    };

    struct ReverseRoute {
        QString connectionId;
        qint64 updatedAtMs{0};
    };

    void expire();

    QHash<QString, Route> routes_;
    QHash<QString, ReverseRoute> reverseRoutes_;
    QHash<QString, qint64> seenPackets_;
};

} // namespace tmc
