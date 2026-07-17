#include "tmc/core/logger.h"

#include <QDebug>

namespace tmc {

Logger& Logger::instance() {
    static Logger l;
    return l;
}

void Logger::log(QtMsgType level, const QString& module, const QString& message) {
    const auto text = module + ": " + message.left(4096);
    if (level == QtWarningMsg) {
        qWarning().noquote() << text;
    } else if (level == QtCriticalMsg || level == QtFatalMsg) {
        qCritical().noquote() << text;
    } else {
        qInfo().noquote() << text;
    }
}

} // namespace tmc
