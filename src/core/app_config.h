#pragma once
#include "core/result.h"
#include <QStringList>

namespace tmc {
struct AppConfig {
    QStringList stunServers{"stun:stun.l.google.com:19302"};
    bool turnEnabled{false};
    int iceGatheringTimeoutSeconds{20};
    int connectionTimeoutSeconds{30};
    int keepaliveSeconds{15};
    int maxRoomPeers{8};
    static Result<AppConfig> load(const QString& path);
    Result<void> save(const QString& path) const;
};
} // namespace tmc
