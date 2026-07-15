#pragma once
#include <QFile>
#include <QMutex>
#include <QString>

namespace tmc {
class Logger final {
  public:
    static Logger& instance();
    bool open(const QString& path);
    void log(QtMsgType level, const QString& module, const QString& message);

  private:
    Logger() = default;
    void rotate();
    QMutex mutex_;
    QFile file_;
};
} // namespace tmc
