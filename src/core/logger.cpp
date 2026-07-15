#include "core/logger.h"
#include <QDateTime>
#include <QFileInfo>
#include <QTextStream>

using namespace tmc;
Logger& Logger::instance() {
    static Logger l;
    return l;
}
bool Logger::open(const QString& path) {
    QMutexLocker lock(&mutex_);
    file_.setFileName(path);
    rotate();
    return file_.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}
void Logger::rotate() {
    if (file_.fileName().isEmpty() || QFileInfo(file_).size() < 2 * 1024 * 1024)
        return;
    file_.close();
    QFile::remove(file_.fileName() + ".1");
    QFile::rename(file_.fileName(), file_.fileName() + ".1");
}
void Logger::log(QtMsgType level, const QString& module, const QString& message) {
    QMutexLocker lock(&mutex_);
    if (!file_.isOpen())
        return;
    const char* names[] = {"DEBUG", "WARN", "CRIT", "FATAL", "INFO"};
    int i = level == QtInfoMsg ? 4 : qBound(0, int(level), 3);
    QTextStream(&file_) << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << ' '
                        << names[i] << ' ' << module << ' ' << message.left(4096) << '\n';
    file_.flush();
}
