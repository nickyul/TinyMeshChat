#pragma once

#include "tmc/signaling_protocol/turn_credentials.h"

#include <QByteArray>

namespace tmc::server {

struct TurnSettings {
    QStringList urls;
    QByteArray sharedSecret;
    int lifetimeSeconds{24 * 60 * 60};
};

class TurnService {
public:
    explicit TurnService(TurnSettings settings = {});
    [[nodiscard]] signaling_protocol::TurnCredentials issue(const QString& identityId) const;

private:
    TurnSettings settings_;
};

} // namespace tmc::server
