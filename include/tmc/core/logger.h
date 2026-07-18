#pragma once

#include <QString>

#include <memory>

namespace tmc {

class Logger final {
public:
    static Logger& instance();

    void setFilePath(const QString& path);
    void log(QtMsgType level, const QString& module, const QString& message);
    void trace(const QString& module, const QString& message);

private:
    Logger();
    ~Logger();

    struct State;
    std::unique_ptr<State> state_;
};

} // namespace tmc
