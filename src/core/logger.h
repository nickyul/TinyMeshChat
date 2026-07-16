#pragma once
#include <QString>

namespace tmc {
class Logger final {
  public:
    static Logger& instance();
    void log(QtMsgType level, const QString& module, const QString& message);

  private:
    Logger() = default;
};
} // namespace tmc
