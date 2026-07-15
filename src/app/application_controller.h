#pragma once
#include "core/app_config.h"
#include "identity/peer_identity.h"
#include "storage/database.h"
#include <QObject>
#include <memory>
namespace tmc {
class ApplicationController : public QObject {
    Q_OBJECT
  public:
    explicit ApplicationController(QObject* p = nullptr);
    Result<void> initialize(const QString& displayName = "User");
    const PeerIdentity& identity() const { return identity_; }
    const AppConfig& config() const { return config_; }
    QSqlDatabase database() const { return db_->connection(); }
    QString dataDirectory() const { return dataDir_; }
    Result<void> updateStunServers(const QStringList& servers);
  signals:
    void fatalError(QString);

  private:
    QString dataDir_;
    PeerIdentity identity_;
    AppConfig config_;
    std::unique_ptr<Database> db_;
};
} // namespace tmc
