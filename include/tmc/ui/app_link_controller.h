#pragma once

#include "tmc/core/result.h"

#include <QObject>
#include <QUrl>

#include <memory>

class QLocalServer;

namespace tmc {

class AppLinkController final : public QObject {
    Q_OBJECT

public:
    explicit AppLinkController(QObject* parent = nullptr);
    ~AppLinkController() override;

    bool startPrimary(const QUrl& initialUrl = {});
    Result<void> registerProtocol();
    Result<void> unregisterProtocol();
    bool protocolRegistered() const;

signals:
    void urlReceived(QUrl url);
    void activationRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QString serverName() const;
    bool forwardToPrimary(const QUrl& url) const;
    void acceptConnection();

    std::unique_ptr<QLocalServer> server_;
};

} // namespace tmc
