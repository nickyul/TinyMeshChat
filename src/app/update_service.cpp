#include "tmc/app/update_service.h"

#include <QCoreApplication>
#include <QMetaObject>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

#ifdef TMC_ENABLE_UPDATER
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QTimer>
#include <QVersionNumber>

#include <Velopack.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#endif

namespace tmc {

namespace {

constexpr auto ReleasesUrl = "https://github.com/nickyul/TinyMeshChat/releases";

#ifdef TMC_ENABLE_UPDATER
constexpr auto RepositoryUrl = "https://github.com/nickyul/TinyMeshChat";
constexpr auto LatestReleaseApi =
    "https://api.github.com/repos/nickyul/TinyMeshChat/releases/latest";
constexpr qsizetype MaxMetadataBytes = 2 * 1024 * 1024;

#ifdef Q_OS_WIN
constexpr auto ExpectedChannel = "win-x64";
#elif defined(Q_OS_MACOS)
constexpr auto ExpectedChannel = "osx-arm64";
#endif

struct ParsedSemVer {
    QVersionNumber core;
    QStringList prerelease;
};

struct GithubRelease {
    QString version;
    QString notes;
};

std::optional<ParsedSemVer> parseSemVer(QString version) {
    const auto buildSeparator = version.indexOf('+');
    if (buildSeparator >= 0) {
        version.truncate(buildSeparator);
    }
    const auto prereleaseSeparator = version.indexOf('-');
    const auto coreText = version.left(prereleaseSeparator);
    const auto prereleaseText =
        prereleaseSeparator >= 0 ? version.sliced(prereleaseSeparator + 1) : QString{};
    qsizetype suffixIndex = 0;
    const auto core = QVersionNumber::fromString(coreText, &suffixIndex);
    if (suffixIndex != coreText.size() || core.segmentCount() != 3 ||
        core.segments().contains(-1) ||
        (prereleaseSeparator >= 0 && prereleaseText.isEmpty())) {
        return std::nullopt;
    }
    auto prerelease = prereleaseText.split('.', Qt::KeepEmptyParts);
    if (std::any_of(prerelease.cbegin(), prerelease.cend(), [](const QString& part) {
            return part.isEmpty();
        })) {
        return std::nullopt;
    }
    return ParsedSemVer{core, std::move(prerelease)};
}

std::optional<int> compareSemVer(const QString& leftText, const QString& rightText) {
    const auto left = parseSemVer(leftText);
    const auto right = parseSemVer(rightText);
    if (!left || !right) {
        return std::nullopt;
    }
    if (const auto coreComparison = QVersionNumber::compare(left->core, right->core)) {
        return coreComparison;
    }
    if (left->prerelease.isEmpty() != right->prerelease.isEmpty()) {
        return left->prerelease.isEmpty() ? 1 : -1;
    }
    for (qsizetype index = 0;
         index < std::min(left->prerelease.size(), right->prerelease.size()); ++index) {
        const auto& leftPart = left->prerelease[index];
        const auto& rightPart = right->prerelease[index];
        bool leftNumeric = false;
        bool rightNumeric = false;
        const auto leftNumber = leftPart.toULongLong(&leftNumeric);
        const auto rightNumber = rightPart.toULongLong(&rightNumeric);
        if (leftNumeric && rightNumeric && leftNumber != rightNumber) {
            return leftNumber < rightNumber ? -1 : 1;
        }
        if (leftNumeric != rightNumeric) {
            return leftNumeric ? -1 : 1;
        }
        if (!leftNumeric) {
            const auto comparison = QString::compare(leftPart, rightPart, Qt::CaseSensitive);
            if (comparison != 0) {
                return comparison < 0 ? -1 : 1;
            }
        }
    }
    if (left->prerelease.size() == right->prerelease.size()) {
        return 0;
    }
    return left->prerelease.size() < right->prerelease.size() ? -1 : 1;
}

QByteArray downloadBytes(const QUrl& url, qsizetype maximumSize,
                         const std::atomic_bool& cancelled) {
    if (!url.isValid() || url.scheme() != "https") {
        throw std::runtime_error("Update metadata URL is invalid.");
    }

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, "TinyMeshChat-Updater/1");
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    auto reply = std::unique_ptr<QNetworkReply>(manager.get(request));
    QEventLoop loop;
    QTimer timeout;
    QTimer cancellationPoll;
    QByteArray bytes;
    bool tooLarge = false;

    timeout.setSingleShot(true);
    cancellationPoll.setInterval(100);
    QObject::connect(reply.get(), &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply.get(), &QNetworkReply::readyRead, &loop, [&] {
        if (reply->bytesAvailable() > maximumSize - bytes.size()) {
            tooLarge = true;
            reply->abort();
            return;
        }
        bytes += reply->readAll();
    });
    QObject::connect(&timeout, &QTimer::timeout, reply.get(), &QNetworkReply::abort);
    QObject::connect(&cancellationPoll, &QTimer::timeout, reply.get(), [&] {
        if (cancelled.load(std::memory_order_relaxed)) {
            reply->abort();
        }
    });
    timeout.start(30000);
    cancellationPoll.start();
    loop.exec();

    if (!tooLarge && reply->bytesAvailable() > 0) {
        if (reply->bytesAvailable() > maximumSize - bytes.size()) {
            tooLarge = true;
        } else {
            bytes += reply->readAll();
        }
    }
    if (tooLarge) {
        throw std::runtime_error("Update metadata is too large.");
    }

    const auto status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status < 200 || status >= 300) {
        throw std::runtime_error(
            QString("Update request failed: %1 (HTTP %2).")
                .arg(reply->errorString())
                .arg(status)
                .toStdString());
    }
    return bytes;
}

GithubRelease loadLatestRelease(const std::atomic_bool& cancelled) {
    const auto bytes = downloadBytes(QUrl(QString::fromLatin1(LatestReleaseApi)),
                                     MaxMetadataBytes, cancelled);
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        throw std::runtime_error("Latest GitHub release metadata is invalid.");
    }

    const auto object = document.object();
    auto version = object.value("tag_name").toString().trimmed();
    if (version.startsWith('v')) {
        version.remove(0, 1);
    }
    if (version.isEmpty()) {
        throw std::runtime_error("Latest GitHub release has no version tag.");
    }
    return GithubRelease{std::move(version), object.value("body").toString()};
}
#endif

} // namespace

struct UpdateService::Worker {
    explicit Worker(UpdateService& owner) : owner(owner) {
        thread = std::jthread([this](std::stop_token token) { run(token); });
    }

    ~Worker() {
        cancelling.store(true, std::memory_order_relaxed);
        thread.request_stop();
        condition.notify_all();
        if (thread.joinable()) {
            thread.join();
        }
    }

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
        }
        condition.notify_one();
    }

    void run(std::stop_token token) {
        while (!token.stop_requested()) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock,
                               [this, &token] { return token.stop_requested() || !tasks.empty(); });
                if (token.stop_requested()) {
                    return;
                }
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task();
        }
    }

    template <typename Function>
    void post(Function&& function) {
        QMetaObject::invokeMethod(&owner, std::forward<Function>(function),
                                  Qt::QueuedConnection);
    }

    UpdateService& owner;
    std::mutex mutex;
    std::condition_variable_any condition;
    std::deque<std::function<void()>> tasks;
    std::jthread thread;
    std::atomic_bool cancelling{false};

#ifdef TMC_ENABLE_UPDATER
    std::unique_ptr<Velopack::UpdateManager> manager;
    std::optional<Velopack::UpdateInfo> update;
    std::optional<Velopack::VelopackAsset> pending;
#endif
};

UpdateService::UpdateService(QObject* parent) : QObject(parent) {
#ifdef TMC_ENABLE_UPDATER
    worker_ = std::make_unique<Worker>(*this);
#endif
}

UpdateService::~UpdateService() = default;

QString UpdateService::state() const {
    return state_;
}

QString UpdateService::availableVersion() const {
    return availableVersion_;
}

QString UpdateService::releaseNotes() const {
    return releaseNotes_;
}

int UpdateService::progress() const {
    return progress_;
}

bool UpdateService::portable() const {
    return portable_;
}

void UpdateService::checkForUpdates(bool manual) {
    if (state_ == "checking" || state_ == "downloading" || state_ == "applying") {
        return;
    }
#ifndef TMC_ENABLE_UPDATER
    if (manual) {
        emit releasePageRequested(QUrl(QString::fromLatin1(ReleasesUrl)));
    }
    return;
#else
    setState("checking");
    const bool standalonePortable = QFileInfo(
        QCoreApplication::applicationDirPath() + "/tinymesh-portable.marker").isFile();
    const auto currentVersion = QCoreApplication::applicationVersion();
    worker_->enqueue([this, manual, standalonePortable, currentVersion] {
        try {
            if (standalonePortable) {
                const auto latest = loadLatestRelease(worker_->cancelling);
                const auto comparison = compareSemVer(latest.version, currentVersion);
                if (!comparison) {
                    throw std::runtime_error("GitHub release version is not valid SemVer.");
                }
                if (*comparison <= 0) {
                    worker_->post([this, manual] {
                        setState("idle", {}, {}, true);
                        emit noUpdateAvailable(manual);
                    });
                    return;
                }
                worker_->post([this, latest] {
                    setState("available", latest.version, latest.notes, true);
                    emit updateAvailable();
                });
                return;
            }

            if (!worker_->manager) {
                auto source =
                    std::make_unique<Velopack::GithubSource>(RepositoryUrl, "", false);
                Velopack::UpdateOptions options{};
                options.AllowVersionDowngrade = false;
                options.ExplicitChannel = ExpectedChannel;
                options.MaximumDeltasBeforeFallback = 10;
                worker_->manager = std::make_unique<Velopack::UpdateManager>(
                    std::move(source), &options);
            }
#ifdef Q_OS_WIN
            const bool portable = worker_->manager->IsPortable();
#else
            const bool portable = false;
#endif
            if (const auto pending = worker_->manager->UpdatePendingRestart()) {
                worker_->pending = *pending;
                worker_->update.reset();
                const auto version = QString::fromStdString(pending->Version);
                const auto notes = QString::fromStdString(pending->NotesMarkdown);
                worker_->post([this, version, notes, portable] {
                    setState("ready", version, notes, portable);
                    emit updateReady();
                });
                return;
            }

            const auto update = worker_->manager->CheckForUpdates();
            if (!update) {
                worker_->update.reset();
                worker_->pending.reset();
                worker_->post([this, portable, manual] {
                    setState("idle", {}, {}, portable);
                    emit noUpdateAvailable(manual);
                });
                return;
            }
            worker_->update = *update;
            worker_->pending.reset();
            const auto version = QString::fromStdString(update->TargetFullRelease.Version);
            auto notes = QString::fromStdString(update->TargetFullRelease.NotesMarkdown);
            if (notes.trimmed().isEmpty()) {
                try {
                    const auto latest = loadLatestRelease(worker_->cancelling);
                    if (latest.version == version) {
                        notes = latest.notes;
                    }
                } catch (const std::exception&) {
                    // Release notes are optional; Velopack has already found a valid update.
                }
            }
            worker_->post([this, version, notes, portable] {
                setState("available", version, notes, portable);
                emit updateAvailable();
            });
        } catch (const std::exception& error) {
            const auto message = QString::fromUtf8(error.what());
            worker_->post([this, message, manual] {
                setState("error");
                emit errorOccurred(message, manual);
            });
        }
    });
#endif
}

void UpdateService::downloadUpdate() {
#ifdef TMC_ENABLE_UPDATER
    if (state_ != "available") {
        return;
    }
    if (portable_) {
        emit releasePageRequested(QUrl(QString::fromLatin1(ReleasesUrl)));
        return;
    }
    setProgress(0);
    setState("downloading", availableVersion_, releaseNotes_, portable_);
    worker_->enqueue([this] {
        try {
            if (!worker_->manager || !worker_->update) {
                throw std::runtime_error("No update is available for download.");
            }
            worker_->manager->DownloadUpdates(
                *worker_->update,
                [](void* context, size_t progress) {
                    auto* service = static_cast<UpdateService*>(context);
                    service->worker_->post([service, progress] {
                        service->setProgress(static_cast<int>(progress));
                    });
                },
                this);
            worker_->post([this] {
                setProgress(100);
                setState("ready", availableVersion_, releaseNotes_, portable_);
                emit updateReady();
            });
        } catch (const std::exception& error) {
            const auto message = QString::fromUtf8(error.what());
            worker_->post([this, message] {
                setState("error", availableVersion_, releaseNotes_, portable_);
                emit errorOccurred(message, true);
            });
        }
    });
#endif
}

void UpdateService::installUpdate() {
#ifdef TMC_ENABLE_UPDATER
    if (state_ != "ready") {
        return;
    }
    setState("applying", availableVersion_, releaseNotes_, portable_);
    worker_->enqueue([this] {
        try {
            if (!worker_->manager || (!worker_->update && !worker_->pending)) {
                throw std::runtime_error("No downloaded update is ready to install.");
            }
            if (worker_->pending) {
                worker_->manager->WaitExitThenApplyUpdates(*worker_->pending, false, true);
            } else {
                worker_->manager->WaitExitThenApplyUpdates(*worker_->update, false, true);
            }
            worker_->post([this] { emit restartRequested(); });
        } catch (const std::exception& error) {
            const auto message = QString::fromUtf8(error.what());
            worker_->post([this, message] {
                setState("error", availableVersion_, releaseNotes_, portable_);
                emit errorOccurred(message, true);
            });
        }
    });
#endif
}

void UpdateService::setState(QString state, QString version, QString notes, bool portable) {
    state_ = std::move(state);
    availableVersion_ = std::move(version);
    releaseNotes_ = std::move(notes);
    portable_ = portable;
    emit stateChanged();
}

void UpdateService::setProgress(int progress) {
    progress = std::clamp(progress, 0, 100);
    if (progress_ == progress) {
        return;
    }
    progress_ = progress;
    emit progressChanged();
}

} // namespace tmc
