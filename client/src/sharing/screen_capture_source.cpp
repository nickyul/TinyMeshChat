#include "tmc/sharing/screen_capture_source.h"

#include "tmc/core/logger.h"

#include <QCapturableWindow>
#include <QGuiApplication>
#include <QImage>
#include <QMediaCaptureSession>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QScreenCapture>
#include <QVideoSink>
#include <QWindowCapture>

#include <algorithm>
#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <condition_variable>
#include <libyuv/convert.h>
#include <mutex>
#include <rtc_base/time_utils.h>
#include <thread>

#include "window_source_filter.h"

namespace tmc {

struct ScreenCaptureSource::State {
    struct Source {
        QString title;
        QPointer<QScreen> screen;
        QCapturableWindow window;
    };

    struct Capture {
        QScreenCapture screen;
        QWindowCapture window;
        QVideoSink sink;
        QMediaCaptureSession session;
    };

    QList<Source> sources;
    std::unique_ptr<Capture> capture;
    QString title;
    std::mutex mutex;
    std::condition_variable condition;
    QVideoFrame pending;
    ScreenQuality quality;
    FrameSink output;
    std::jthread worker;
    bool active{false};
    quint64 generation{0};
};

ScreenCaptureSource::ScreenCaptureSource(QObject* parent)
    : QObject(parent), state_(std::make_unique<State>()) {
}

ScreenCaptureSource::~ScreenCaptureSource() {
    stop();
}

QVariantList ScreenCaptureSource::sources() {
    state_->sources.clear();
    QVariantList result;
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        return result;
    }
    for (auto* screen : QGuiApplication::screens()) {
        state_->sources.append({QString("Экран: %1 (%2 × %3)")
                                    .arg(screen->name())
                                    .arg(screen->size().width())
                                    .arg(screen->size().height()),
                                screen,
                                {}});
    }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    const auto applications = applicationWindowTitles();
#endif
    QList<State::Source> windows;
    const QRegularExpression anonymous("^windows?\\s+0x[0-9a-f]+$",
                                       QRegularExpression::CaseInsensitiveOption);
    for (const auto& window : QWindowCapture::capturableWindows()) {
        auto title = window.description().trimmed();
        if (!window.isValid() || title.isEmpty() || anonymous.match(title).hasMatch()) {
            continue;
        }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        title = applications.value(window.description());
        if (title.isEmpty()) {
            continue;
        }
#endif
        windows.append({title, {}, window});
    }
    std::stable_sort(windows.begin(), windows.end(), [](const auto& a, const auto& b) {
        return QString::localeAwareCompare(a.title, b.title) < 0;
    });
    state_->sources.append(windows);
    for (qsizetype i = 0; i < state_->sources.size(); ++i) {
        result.append(QVariantMap{{"index", i}, {"title", state_->sources[i].title}});
    }
    return result;
}

Result<void> ScreenCaptureSource::start(int index, ScreenQuality quality, FrameSink sink) {
    if (!quality.isValid()) {
        return Result<void>::failure("Некорректное качество трансляции.");
    }
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        return Result<void>::failure("Для захвата нужен графический интерфейс.");
    }
    if (index < 0 || index >= state_->sources.size()) {
        return Result<void>::failure("Выберите экран или окно.");
    }
    const auto source = state_->sources[index];
    if (!source.screen && !source.window.isValid()) {
        return Result<void>::failure("Источник больше недоступен.");
    }
    stop();
    if (!state_->capture) {
        state_->capture = std::make_unique<State::Capture>();
        auto& c = *state_->capture;
        c.session.setVideoSink(&c.sink);
        connect(&c.sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame& frame) {
            if (!frame.isValid()) {
                return;
            }
            {
                std::lock_guard lock(state_->mutex);
                if (!state_->active) {
                    return;
                }
                state_->pending = frame;
            }
            state_->condition.notify_one();
        });
        connect(&c.screen, &QScreenCapture::errorOccurred, this,
                [this](QScreenCapture::Error, const QString& error) { emit errorOccurred(error); });
        connect(&c.window, &QWindowCapture::errorOccurred, this,
                [this](QWindowCapture::Error, const QString& error) { emit errorOccurred(error); });
    }
    state_->title = source.title;
    Logger::instance().log(QtInfoMsg, "screen_capture",
                           QString("start source=%1 height=%2 fps=%3")
                               .arg(state_->title)
                               .arg(quality.height)
                               .arg(quality.fps));
    quint64 generation;
    {
        std::lock_guard lock(state_->mutex);
        state_->quality = quality;
        state_->output = std::move(sink);
        state_->active = true;
        generation = ++state_->generation;
    }
    state_->worker = std::jthread([this, generation](std::stop_token token) {
        bool first = true;
        int64_t next = -1;
        int previousFps = 0;
        while (!token.stop_requested()) {
            QVideoFrame frame;
            ScreenQuality quality;
            FrameSink output;
            {
                std::unique_lock lock(state_->mutex);
                state_->condition.wait(
                    lock, [&] { return token.stop_requested() || state_->pending.isValid(); });
                if (token.stop_requested()) {
                    break;
                }
                frame = std::move(state_->pending);
                state_->pending = {};
                quality = state_->quality;
                output = state_->output;
            }
            const auto timestamp = webrtc::TimeMicros();
            const int64_t interval = 1000000 / quality.fps;
            if (previousFps != quality.fps) {
                next = -1;
                previousFps = quality.fps;
            }
            if (next >= 0 && timestamp + 1000 < next) {
                continue;
            }
            auto image = frame.toImage();
            if (image.isNull()) {
                continue;
            }
            const auto size = quality.fittedSize(image.size());
            if (size != image.size()) {
                image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
            image = image.convertToFormat(QImage::Format_ARGB32);
            const auto raw = webrtc::I420Buffer::Create(image.width(), image.height());
            if (libyuv::ARGBToI420(image.constBits(), image.bytesPerLine(), raw->MutableDataY(),
                                   raw->StrideY(), raw->MutableDataU(), raw->StrideU(),
                                   raw->MutableDataV(), raw->StrideV(), image.width(),
                                   image.height()) != 0) {
                continue;
            }
            if (next < 0 || timestamp - next > interval) {
                next = timestamp;
            }
            next += interval;
            if (first) {
                first = false;
                Logger::instance().log(QtInfoMsg, "screen_capture",
                                       QString("first_frame generation=%1 size=%2x%3")
                                           .arg(generation)
                                           .arg(image.width())
                                           .arg(image.height()));
                QMetaObject::invokeMethod(
                    this,
                    [this, generation] {
                        if (state_->active && state_->generation == generation) {
                            emit firstFrame();
                        }
                    },
                    Qt::QueuedConnection);
            }
            if (output) {
                output(webrtc::VideoFrame::Builder()
                           .set_video_frame_buffer(raw)
                           .set_timestamp_us(timestamp)
                           .set_rotation(webrtc::kVideoRotation_0)
                           .build());
            }
        }
    });
    auto& c = *state_->capture;
    if (source.screen) {
        c.session.setWindowCapture(nullptr);
        c.session.setScreenCapture(&c.screen);
        c.screen.setScreen(source.screen);
        c.screen.start();
    } else {
        c.session.setScreenCapture(nullptr);
        c.session.setWindowCapture(&c.window);
        c.window.setWindow(source.window);
        c.window.start();
    }
    return Result<void>::success();
}

void ScreenCaptureSource::setQuality(ScreenQuality quality) {
    std::lock_guard lock(state_->mutex);
    state_->quality = quality;
}

void ScreenCaptureSource::stop() {
    bool active;
    {
        std::lock_guard lock(state_->mutex);
        active = state_->active;
        state_->active = false;
        ++state_->generation;
        state_->pending = {};
    }
    if (active) {
        Logger::instance().log(QtInfoMsg, "screen_capture", "stop_processing");
    }
    if (state_->worker.joinable()) {
        state_->worker.request_stop();
        state_->condition.notify_all();
        state_->worker.join();
    }
    if (active) {
        Logger::instance().log(QtInfoMsg, "screen_capture", "stop_backend");
    }
    // Finish reading captured GPU buffers before destroying the capture backend.
    if (state_->capture) {
        state_->capture->screen.stop();
        state_->capture->window.stop();
    }
    state_->output = {};
    if (active) {
        Logger::instance().log(QtInfoMsg, "screen_capture", "stopped");
    }
}

QString ScreenCaptureSource::title() const {
    return state_->title;
}

} // namespace tmc
