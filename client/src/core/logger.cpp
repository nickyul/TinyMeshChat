#include "tmc/core/logger.h"

#include <QDateTime>
#include <QDebug>
#include <QFile>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace tmc {

namespace {

constexpr qsizetype MaxLogBytes = 10 * 1024 * 1024;
constexpr size_t MaxPendingLines = 8192;

} // namespace

struct Logger::State {
    State() : worker([this](std::stop_token token) { run(token); }) {
    }

    ~State() {
        worker.request_stop();
        condition.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    void enqueue(QString line) {
        std::lock_guard lock(mutex);
        if (pending.size() >= MaxPendingLines) {
            pending.pop_front();
        }
        pending.push_back(std::move(line));
        condition.notify_one();
    }

    void run(std::stop_token token) {
        QFile file;
        QString openedPath;
        while (true) {
            std::vector<QString> lines;
            QString requestedPath;
            bool stopRequested;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, token, [this] { return !pending.empty() || pathChanged; });
                stopRequested = token.stop_requested();
                requestedPath = filePath;
                pathChanged = false;
                while (!pending.empty()) {
                    lines.push_back(std::move(pending.front()));
                    pending.pop_front();
                }
            }

            if (requestedPath != openedPath) {
                file.close();
                openedPath = requestedPath;
                file.setFileName(openedPath);
            }

            if (!openedPath.isEmpty() && !file.isOpen()) {
                file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
            }

            if (file.isOpen()) {
                if (file.size() >= MaxLogBytes) {
                    file.close();
                    QFile::remove(openedPath + ".1");
                    QFile::rename(openedPath, openedPath + ".1");
                    file.setFileName(openedPath);
                    file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
                }
                for (const auto& line : lines) {
                    file.write(line.toUtf8());
                    file.write("\n");
                }
                file.flush();
            }

            if (stopRequested) {
                break;
            }
        }
    }

    std::mutex mutex;
    std::condition_variable_any condition;
    std::deque<QString> pending;
    QString filePath;
    bool pathChanged{false};
    std::jthread worker;
};

Logger::Logger() : state_(std::make_unique<State>()) {
}

Logger::~Logger() = default;

Logger& Logger::instance() {
    static Logger l;
    return l;
}

void Logger::setFilePath(const QString& path) {
    std::lock_guard lock(state_->mutex);
    state_->filePath = path;
    state_->pathChanged = true;
    state_->condition.notify_one();
}

void Logger::log(QtMsgType level, const QString& module, const QString& message) {
    const auto text = module + ": " + message.left(4096);
    state_->enqueue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + " " + text);
    if (level == QtWarningMsg) {
        qWarning().noquote() << text;
    } else if (level == QtCriticalMsg || level == QtFatalMsg) {
        qCritical().noquote() << text;
    } else {
        qInfo().noquote() << text;
    }
}

void Logger::trace(const QString& module, const QString& message) {
    state_->enqueue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) + " " + module +
                    ": " + message.left(4096));
}

} // namespace tmc
