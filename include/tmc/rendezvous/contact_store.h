#pragma once

#include "tmc/core/result.h"
#include "tmc/identity/peer_identity.h"

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QString>

namespace tmc {

struct RendezvousEndpoint {
    QString address;
    quint16 port{0};

    bool isValid() const;
};

struct ContactRecord {
    PeerIdentity identity;
    QByteArray rendezvousSecret;
    RendezvousEndpoint publicEndpoint;
    RendezvousEndpoint localEndpoint;
    QDateTime lastSeen;
    QString lastMeshId;
    QString mappingMethod;

    bool isValid() const;
};

class ContactStore {
public:
    explicit ContactStore(QString path);

    Result<QList<ContactRecord>> load() const;
    Result<void> save(const QList<ContactRecord>& contacts) const;

private:
    QString path_;
};

} // namespace tmc
