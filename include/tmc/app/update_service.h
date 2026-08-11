#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <memory>

namespace tmc {

class UpdateService final : public QObject {
    Q_OBJECT

public:
    explicit UpdateService(QObject* parent = nullptr);
    ~UpdateService() override;

    QString state() const;
    QString availableVersion() const;
    QString releaseNotes() const;
    int progress() const;
    bool portable() const;

    void checkForUpdates(bool manual);
    void downloadUpdate();
    void installUpdate();

signals:
    void stateChanged();
    void progressChanged();
    void updateAvailable();
    void updateReady();
    void noUpdateAvailable(bool manual);
    void restartRequested();
    void errorOccurred(QString message, bool manual);
    void releasePageRequested(QUrl url);

private:
    struct Worker;

    void setState(QString state, QString version = {}, QString notes = {},
                  bool portable = false);
    void setProgress(int progress);

    std::unique_ptr<Worker> worker_;
    QString state_{"idle"};
    QString availableVersion_;
    QString releaseNotes_;
    int progress_{0};
    bool portable_{false};
};

} // namespace tmc
