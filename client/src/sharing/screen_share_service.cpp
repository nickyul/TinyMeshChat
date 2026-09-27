#include "tmc/sharing/screen_share_service.h"
#include "tmc/sharing/system_audio_capture.h"
#include "window_source_filter.h"

#include <QAudioFormat>
#include <QAudioDevice>
#include <QAudioSink>
#include <QBuffer>
#include <QCapturableWindow>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QImageReader>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QScreenCapture>
#include <QSet>
#include <QStringList>
#include <QVariantMap>
#include <QThreadPool>
#include <QTimer>
#include <QUuid>
#include <QVideoFrame>
#include <QVideoSink>
#include <QWindowCapture>
#include <QtEndian>
#include <opus/opus.h>
#include <array>
#include <algorithm>
#include <cstring>

namespace tmc {
namespace {
constexpr int HeaderSize = 33;
constexpr int PieceSize = 12000;
constexpr int MaxImageSize = 1024 * 1024;
enum class Kind : quint8 { Announce = 1, Stop, Video, Audio };
QByteArray frame(Kind kind, const QString& stream, quint32 sequence = 0, quint16 part = 0,
                 quint16 count = 1, quint32 size = 0, const QByteArray& payload = {}) {
    QByteArray bytes("TMCS", 4);
    bytes.append(static_cast<char>(kind));
    bytes.append(QUuid(stream).toRfc4122());
    const auto seq = qToBigEndian(sequence);
    const auto index = qToBigEndian(part);
    const auto parts = qToBigEndian(count);
    const auto length = qToBigEndian(size);
    bytes.append(reinterpret_cast<const char*>(&seq), 4);
    bytes.append(reinterpret_cast<const char*>(&index), 2);
    bytes.append(reinterpret_cast<const char*>(&parts), 2);
    bytes.append(reinterpret_cast<const char*>(&length), 4);
    bytes.append(payload);
    return bytes;
}
} // namespace

struct ScreenShareService::State {
    struct Source { QString title; QPointer<QScreen> screen; QCapturableWindow window; };
    QString localIdentity, localStream, remoteIdentity, remoteStream, sourceTitle, remoteTitle;
    Sender sender;
    QSet<QString> peers;
    QList<Source> sources;
    struct Capture {
        QScreenCapture screen;
        QWindowCapture window;
        QVideoSink sink;
        QMediaCaptureSession session;
    };
    std::unique_ptr<Capture> capture;
    SystemAudioCapture audio;
    QTimer timer;
    QElapsedTimer clock;
    qint64 capturedAt{-1000}, announcedAt{-1000}, remoteSeenAt{0};
    bool encoding{false}, decoding{false}, audioEnabled{false}, muted{false};
    quint32 videoSequence{0}, audioSequence{0}, receivedVideo{0}, receivedAudio{0}, displayedVideo{0};
    quint32 imageSize{0};
    QList<QByteArray> pieces;
    int piecesReceived{0};
    QSet<QString> stoppedStreams;
    QStringList stoppedOrder;
    double volume{1.0};
    QByteArray pcm, playback;
    OpusEncoder* encoder{nullptr};
    OpusDecoder* decoder{nullptr};
    std::unique_ptr<QAudioSink> output;
    QIODevice* outputDevice{nullptr};

    static QString streamKey(const QString& peer, const QString& stream) {
        return peer + QChar('/') + stream;
    }
    void retire(const QString& peer, const QString& stream) {
        const auto id = streamKey(peer, stream);
        if (stream.isEmpty()) return;
        if (id.isEmpty() || stoppedStreams.contains(id)) return;
        stoppedStreams.insert(id);
        stoppedOrder.append(id);
        while (stoppedOrder.size() > 128) stoppedStreams.remove(stoppedOrder.takeFirst());
    }
    ~State() {
        if (encoder) opus_encoder_destroy(encoder);
        if (decoder) opus_decoder_destroy(decoder);
    }
};

ScreenShareService::ScreenShareService(QString identity, Sender sender, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>()) {
    state_->localIdentity = std::move(identity);
    state_->sender = std::move(sender);
    state_->clock.start();
    connect(&state_->audio, &SystemAudioCapture::samples, this, &ScreenShareService::encodeAudio);
    connect(&state_->audio, &SystemAudioCapture::errorOccurred, this, [this](const QString& error) {
        state_->audio.stop();
        state_->audioEnabled = false;
        emit errorOccurred(error);
        announce();
    });
    state_->timer.setInterval(20);
    connect(&state_->timer, &QTimer::timeout, this, &ScreenShareService::maintain);
    state_->timer.start();
}

ScreenShareService::~ScreenShareService() {
    if (state_->capture) {
        state_->capture->screen.stop();
        state_->capture->window.stop();
    }
    state_->audio.stop();
}

bool ScreenShareService::prepareCapture() {
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return false;
    if (state_->capture) return true;
    state_->capture = std::make_unique<State::Capture>();
    state_->capture->session.setVideoSink(&state_->capture->sink);
    connect(&state_->capture->sink, &QVideoSink::videoFrameChanged, this, [this](const QVideoFrame&) { captureFrame(); });
    connect(&state_->capture->screen, &QScreenCapture::errorOccurred, this, [this](QScreenCapture::Error, const QString& message) {
        stop(); emit errorOccurred("Не удалось захватить экран: " + message);
    });
    connect(&state_->capture->window, &QWindowCapture::errorOccurred, this, [this](QWindowCapture::Error, const QString& message) {
        stop(); emit errorOccurred("Не удалось захватить окно: " + message);
    });
    return true;
}

QVariantList ScreenShareService::sources() {
    state_->sources.clear();
    QVariantList rows;
    if (!qobject_cast<QGuiApplication*>(QCoreApplication::instance())) return rows;
    for (auto* screen : QGuiApplication::screens()) {
        const auto title = QString("Экран: %1 (%2 × %3)").arg(screen->name()).arg(screen->size().width()).arg(screen->size().height());
        rows.append(QVariantMap{{"index", state_->sources.size()}, {"title", title}});
        state_->sources.append({title, screen, {}});
    }
    const auto windows = QWindowCapture::capturableWindows();
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    const auto applications = applicationWindowTitles();
#endif
    QList<State::Source> windowSources;
    static const QRegularExpression anonymousWindow(QStringLiteral("^windows?\\s+0x[0-9a-f]+$"),
                                                    QRegularExpression::CaseInsensitiveOption);
    for (const auto& window : windows) {
        if (!window.isValid() || window.description().trimmed().isEmpty()) continue;
        auto title = window.description().trimmed();
        if (anonymousWindow.match(title).hasMatch()) continue;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        title = applications.value(window.description());
        if (title.isEmpty()) continue;
#endif
        windowSources.append({title, {}, window});
    }
    std::stable_sort(windowSources.begin(), windowSources.end(), [](const auto& a, const auto& b) {
        return QString::localeAwareCompare(a.title, b.title) < 0;
    });
    for (const auto& source : windowSources) {
        rows.append(QVariantMap{{"index", state_->sources.size()}, {"title", source.title}});
        state_->sources.append(source);
    }
    return rows;
}

Result<void> ScreenShareService::start(int index, bool systemAudio) {
    if (!state_->remoteStream.isEmpty()) return Result<void>::failure("Другой участник уже ведёт трансляцию.");
    if (state_->peers.isEmpty()) return Result<void>::failure("Для трансляции нужен подключённый участник.");
    if (index < 0 || index >= state_->sources.size()) return Result<void>::failure("Выберите экран или окно.");
    const auto source = state_->sources.at(index);
    if (!source.screen && !source.window.isValid()) return Result<void>::failure("Выбранный источник больше недоступен.");
    if (!prepareCapture()) return Result<void>::failure("Для трансляции откройте графический интерфейс приложения.");
    stop();
    state_->localStream = QUuid::createUuid().toString(QUuid::WithoutBraces);
    state_->sourceTitle = source.title;
    state_->videoSequence = state_->audioSequence = 0;
    state_->capturedAt = -1000;
    state_->audioEnabled = systemAudio;
    if (systemAudio) {
        int error = 0;
        state_->encoder = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &error);
        if (!state_->encoder || error != OPUS_OK) { stop(); return Result<void>::failure("Не удалось подготовить звук трансляции."); }
        opus_encoder_ctl(state_->encoder, OPUS_SET_BITRATE(128000));
        const auto started = state_->audio.start();
        if (!started) { stop(); return started; }
    }
    if (source.screen) {
        state_->capture->session.setWindowCapture(nullptr);
        state_->capture->session.setScreenCapture(&state_->capture->screen);
        state_->capture->screen.setScreen(source.screen);
        state_->capture->screen.start();
    } else {
        state_->capture->session.setScreenCapture(nullptr);
        state_->capture->session.setWindowCapture(&state_->capture->window);
        state_->capture->window.setWindow(source.window);
        state_->capture->window.start();
    }
    if (state_->localStream.isEmpty()) return Result<void>::failure("Не удалось запустить захват выбранного источника.");
    announce();
    emit changed();
    return Result<void>::success();
}

void ScreenShareService::stop() {
    const auto stream = state_->localStream;
    if (stream.isEmpty()) return;
    state_->localStream.clear();
    if (state_->capture) {
        state_->capture->screen.stop();
        state_->capture->window.stop();
    }
    state_->audio.stop();
    state_->audioEnabled = false;
    state_->pcm.clear();
    if (state_->encoder) { opus_encoder_destroy(state_->encoder); state_->encoder = nullptr; }
    for (const auto& peer : state_->peers) state_->sender(peer, frame(Kind::Stop, stream));
    state_->retire(state_->localIdentity, stream);
    emit frameReady({});
    emit changed();
}

void ScreenShareService::clearRemote(bool retire) {
    if (retire) state_->retire(state_->remoteIdentity, state_->remoteStream);
    state_->remoteStream.clear();
    state_->remoteIdentity.clear();
    state_->remoteTitle.clear();
    state_->pieces.clear();
    state_->receivedVideo = state_->receivedAudio = state_->displayedVideo = 0;
    state_->playback.clear();
    state_->outputDevice = nullptr;
    state_->output.reset();
    if (state_->decoder) { opus_decoder_destroy(state_->decoder); state_->decoder = nullptr; }
    emit frameReady({});
    emit changed();
}

void ScreenShareService::clear() { stop(); clearRemote(); state_->peers.clear(); }
bool ScreenShareService::sharing() const { return !state_->localStream.isEmpty(); }
bool ScreenShareService::viewing() const { return !state_->remoteStream.isEmpty(); }
QString ScreenShareService::owner() const { return sharing() ? state_->localIdentity : state_->remoteIdentity; }
QString ScreenShareService::title() const { return sharing() ? state_->sourceTitle : state_->remoteTitle; }
bool ScreenShareService::systemAudioSupported() const { return SystemAudioCapture::supported(); }
void ScreenShareService::setAudioVolume(int percent) {
    state_->volume = qBound(0, percent, 100) / 100.0;
    if (state_->output) state_->output->setVolume(state_->muted ? 0.0 : state_->volume);
}

void ScreenShareService::setAudioMuted(bool muted) {
    state_->muted = muted;
    if (state_->output) state_->output->setVolume(muted ? 0.0 : state_->volume);
}

void ScreenShareService::setPeerConnected(const QString& peer, bool connected) {
    if (connected) { state_->peers.insert(peer); if (sharing()) announce(); }
    else {
        state_->peers.remove(peer);
        if (state_->remoteIdentity == peer) clearRemote(false);
        if (state_->peers.isEmpty()) stop();
    }
}

void ScreenShareService::announce() {
    if (!sharing()) return;
    const auto payload = QByteArray(1, state_->audioEnabled ? '\1' : '\0') + state_->sourceTitle.toUtf8().left(512);
    for (const auto& peer : state_->peers) state_->sender(peer, frame(Kind::Announce, state_->localStream, 0, 0, 1, 0, payload));
    state_->announcedAt = state_->clock.elapsed();
}

void ScreenShareService::maintain() {
    const auto now = state_->clock.elapsed();
    if (sharing() && now - state_->announcedAt >= 1000) announce();
    if (viewing() && now - state_->remoteSeenAt > 5000) clearRemote(false);
    if (state_->outputDevice && state_->output && !state_->playback.isEmpty()) {
        const auto count = std::min<qint64>(state_->output->bytesFree(), state_->playback.size());
        if (count > 0) {
            const auto written = state_->outputDevice->write(state_->playback.constData(), count);
            if (written > 0) state_->playback.remove(0, written);
        }
    }
}

void ScreenShareService::captureFrame() {
    const auto now = state_->clock.elapsed();
    if (!sharing() || state_->encoding || now - state_->capturedAt < 80) return;
    const auto image = state_->capture->sink.videoFrame().toImage();
    if (image.isNull()) return;
    state_->capturedAt = now;
    state_->encoding = true;
    const auto stream = state_->localStream;
    QPointer<ScreenShareService> self(this);
    QThreadPool::globalInstance()->start([self, stream, image] {
        const auto scaled = image.scaled(1280, 720, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QByteArray encoded;
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        scaled.save(&buffer, "JPEG", 65);
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, stream, scaled, encoded] {
            if (!self) return;
            auto& state = *self->state_;
            state.encoding = false;
            if (state.localStream != stream || encoded.isEmpty() || encoded.size() > MaxImageSize) return;
            emit self->frameReady(scaled);
            const auto sequence = ++state.videoSequence;
            const auto count = static_cast<quint16>((encoded.size() + PieceSize - 1) / PieceSize);
            for (const auto& peer : state.peers) {
                for (quint16 part = 0; part < count; ++part) {
                    if (!state.sender(peer, frame(Kind::Video, stream, sequence, part, count,
                            static_cast<quint32>(encoded.size()), encoded.mid(part * PieceSize, PieceSize)))) break;
                }
            }
        }, Qt::QueuedConnection);
    });
}

void ScreenShareService::encodeAudio(const QByteArray& samples) {
    if (!sharing() || !state_->audioEnabled || !state_->encoder) return;
    state_->pcm.append(samples);
    constexpr int FrameBytes = 960 * 2 * sizeof(float);
    if (state_->pcm.size() > FrameBytes * 8) state_->pcm = state_->pcm.right(FrameBytes * 4);
    while (state_->pcm.size() >= FrameBytes) {
        std::array<float, 1920> pcm;
        std::memcpy(pcm.data(), state_->pcm.constData(), FrameBytes);
        state_->pcm.remove(0, FrameBytes);
        std::array<unsigned char, 4000> packet;
        const int bytes = opus_encode_float(state_->encoder, pcm.data(), 960, packet.data(), packet.size());
        if (bytes <= 0) continue;
        const auto message = frame(Kind::Audio, state_->localStream, ++state_->audioSequence, 0, 1, bytes,
            QByteArray(reinterpret_cast<const char*>(packet.data()), bytes));
        for (const auto& peer : state_->peers) state_->sender(peer, message);
    }
}

void ScreenShareService::receive(const QString& peer, const QByteArray& bytes) {
    if (!state_->peers.contains(peer) || bytes.size() < HeaderSize || bytes.size() > HeaderSize + PieceSize || !bytes.startsWith("TMCS")) return;
    const auto stream = QUuid::fromRfc4122(bytes.mid(5, 16)).toString(QUuid::WithoutBraces);
    if (QUuid(stream).isNull() || state_->stoppedStreams.contains(State::streamKey(peer, stream))) return;
    const auto kind = static_cast<Kind>(static_cast<quint8>(bytes.at(4)));
    const auto sequence = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + 21));
    const auto part = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(bytes.constData() + 25));
    const auto count = qFromBigEndian<quint16>(reinterpret_cast<const uchar*>(bytes.constData() + 27));
    const auto size = qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + 29));
    const auto payload = bytes.mid(HeaderSize);
    if (kind == Kind::Stop) {
        if (state_->remoteIdentity == peer && state_->remoteStream == stream) clearRemote();
        else state_->retire(peer, stream);
        return;
    }
    if (kind == Kind::Announce) {
        if (payload.isEmpty() || payload.size() > 513) return;
        // Simultaneous starts converge to the same owner without a server.
        if (sharing()) {
            if (state_->localIdentity < peer) { announce(); return; }
            stop();
        }
        if (viewing() && state_->remoteIdentity != peer && state_->remoteIdentity < peer) return;
        if (state_->remoteStream != stream || state_->remoteIdentity != peer) {
            clearRemote();
            state_->remoteStream = stream;
            state_->remoteIdentity = peer;
            state_->remoteTitle = QString::fromUtf8(payload.mid(1));
            int error = 0;
            state_->decoder = opus_decoder_create(48000, 2, &error);
            if (payload.at(0) != '\0' && state_->decoder && error == OPUS_OK) {
                QAudioFormat format;
                format.setSampleRate(48000);
                format.setChannelCount(2);
                format.setSampleFormat(QAudioFormat::Int16);
                state_->output = std::make_unique<QAudioSink>(QMediaDevices::defaultAudioOutput(), format);
                state_->output->setBufferSize(19200);
                state_->output->setVolume(state_->muted ? 0.0 : state_->volume);
                state_->outputDevice = state_->output->start();
            }
            emit changed();
        }
        state_->remoteSeenAt = state_->clock.elapsed();
        return;
    }
    if (state_->remoteStream != stream || state_->remoteIdentity != peer) return;
    state_->remoteSeenAt = state_->clock.elapsed();
    if (kind == Kind::Audio) {
        if (!state_->decoder || !state_->outputDevice || sequence <= state_->receivedAudio || payload.isEmpty() || payload.size() > 4000) return;
        state_->receivedAudio = sequence;
        std::array<opus_int16, 1920> pcm;
        const int frames = opus_decode(state_->decoder, reinterpret_cast<const unsigned char*>(payload.constData()),
            static_cast<opus_int32>(payload.size()), pcm.data(), 960, 0);
        if (frames <= 0) return;
        state_->playback.append(reinterpret_cast<const char*>(pcm.data()), frames * 2 * sizeof(opus_int16));
        if (state_->playback.size() > 38400) state_->playback = state_->playback.right(19200);
        return;
    }
    if (kind != Kind::Video || sequence == 0 || count == 0 || count > 88 || part >= count || size == 0 || size > MaxImageSize ||
        count != (size + PieceSize - 1) / PieceSize || payload.size() != std::min<quint32>(PieceSize, size - part * PieceSize) ||
        sequence < state_->receivedVideo || sequence <= state_->displayedVideo) return;
    if (sequence > state_->receivedVideo) {
        state_->receivedVideo = sequence;
        state_->imageSize = size;
        state_->pieces = QList<QByteArray>(count);
        state_->piecesReceived = 0;
    }
    if (state_->imageSize != size || state_->pieces.size() != count || !state_->pieces[part].isEmpty()) return;
    state_->pieces[part] = payload;
    if (++state_->piecesReceived != count || state_->decoding) return;
    QByteArray encoded;
    encoded.reserve(size);
    for (const auto& piece : state_->pieces) encoded.append(piece);
    state_->decoding = true;
    QPointer<ScreenShareService> self(this);
    QThreadPool::globalInstance()->start([self, encoded, sequence, stream, peer] {
        QBuffer buffer;
        buffer.setData(encoded);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer, "JPEG");
        const auto size = reader.size();
        QImage image;
        if (size.width() > 0 && size.height() > 0 && size.width() <= 1280 && size.height() <= 720) image = reader.read();
        if (!self) return;
        QMetaObject::invokeMethod(self, [self, image, sequence, stream, peer] {
            if (!self) return;
            self->state_->decoding = false;
            if (self->state_->remoteStream != stream || self->state_->remoteIdentity != peer || image.isNull() || sequence <= self->state_->displayedVideo) return;
            self->state_->displayedVideo = sequence;
            emit self->frameReady(image);
        }, Qt::QueuedConnection);
    });
}
} // namespace tmc
