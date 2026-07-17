#pragma once

#include "tmc/core/result.h"

#include <QStringList>

namespace tmc {

struct AppConfig {
    QStringList stunServers{"stun:stun.l.google.com:19302", "stun:stun.cloudflare.com:3478"};

    static Result<AppConfig> load(const QString& path);
    Result<void> save(const QString& path) const;
};

} // namespace tmc
