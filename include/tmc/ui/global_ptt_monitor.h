#pragma once

#include "tmc/core/app_config.h"

#include <QObject>
#include <QTimer>

namespace tmc {

class GlobalPttMonitor final : public QObject {
    Q_OBJECT

public:
    explicit GlobalPttMonitor(QObject* parent = nullptr);
    ~GlobalPttMonitor() override;

    PttBinding binding() const;
    bool capturing() const;

    void setBinding(const PttBinding& binding);
    void beginCapture();
    void cancelCapture();
    void setMonitoringEnabled(bool enabled);
    void forceReleased();

signals:
    void bindingCaptured(tmc::PttBinding binding);
    void capturingChanged(bool capturing);
    void pressedChanged(bool pressed);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    static QString platformName();
    static PttBinding defaultBinding();
    bool bindingDown() const;
    void finishCapture(PttBinding binding);
    void poll();
    void setPressed(bool pressed);

    PttBinding binding_;
    QTimer pollTimer_;
    bool capturing_{false};
    bool monitoring_{false};
    bool acceptingPresses_{false};
    bool pressed_{false};
};

} // namespace tmc
