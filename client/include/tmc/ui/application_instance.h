#pragma once

#include "tmc/core/result.h"

#include <QObject>

#include <memory>

class QLocalServer;

namespace tmc {

enum class AppInstanceState {
    Primary,
    ForwardedToPrimary,
};

class ApplicationInstance final : public QObject {
    Q_OBJECT

public:
    explicit ApplicationInstance(QObject* parent = nullptr);
    ~ApplicationInstance() override;

    Result<AppInstanceState> startPrimary();

signals:
    void activationRequested();

private:
    QString serverName() const;
    Result<bool> forwardToPrimary() const;
    void acceptConnection();

    std::unique_ptr<QLocalServer> server_;
};

} // namespace tmc
