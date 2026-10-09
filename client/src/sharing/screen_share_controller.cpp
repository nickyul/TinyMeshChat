#include "tmc/sharing/screen_share_controller.h"

#include "tmc/sharing/screen_capture_source.h"
#include "tmc/sharing/screen_control.h"
#include "tmc/sharing/system_audio_capture.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <mutex>

#include "screen_media_engine.h"
#include "screen_media_peer.h"
#include "screen_video_bridge.h"

namespace tmc {
namespace {

bool platformSupported() {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return true;
#else
    return false;
#endif
}

QString streamKey(const QString& owner, const QString& publication) {
    return owner + "/" + publication;
}

QString uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject control(const char* kind) {
    return {{"v", ScreenProtocolVersion}, {"kind", kind}};
}

} // namespace

struct ScreenShareController::State {
    struct Stream {
        QString id, key, peer, title, status{"available"}, error, qualityText;
        ScreenQuality published, requested;
        quint64 revision{1}, subscriptionRevision{};
        bool local{}, audio{}, selected{};
        std::unique_ptr<ScreenVideoBridge> bridge{std::make_unique<ScreenVideoBridge>()};
    };

    struct Peer {
        bool capable{}, subscribed{}, audio{}, software{}, connected{};
        QString stream, status;
        ScreenQuality requested;
        quint64 revision{}, generation{};
        std::unique_ptr<ScreenMediaPeer> media;
        qint64 deadline{}, idleAt{}, statsAt{}, retryAt{}, videoAt{}, framesDecoded{-1},
            framesEncoded{-1};
        int attempts{};
        QJsonObject stats;
    };

    QString identity, localId, selectedAudio;
    QStringList stun;
    QList<RelayServer> turn;
    qint64 turnExpires{}, captureDeadline{};
    Sender sender;
    std::shared_ptr<SharedAudioOutput> output;
    ScreenMediaEngine engine;
    ScreenCaptureSource capture;
    SystemAudioCapture audio;
    bool audioCapturing{}, localReady{}, muted{};
    QHash<QString, std::shared_ptr<Stream>> streams;
    QHash<QString, std::shared_ptr<Peer>> peers;
    QStringList retired;
    QElapsedTimer clock;
    QTimer timer;
    std::mutex captureMutex;
    QList<ScreenMediaPeer*> capturePeers;
    ScreenVideoBridge* localBridge{};

    State(std::shared_ptr<SharedAudioOutput> output)
        : output(std::move(output)), engine(this->output) {
        clock.start();
    }

    std::shared_ptr<Stream> remoteStream(const QString& peer) const {
        for (const auto& stream : streams) {
            if (!stream->local && stream->peer == peer) {
                return stream;
            }
        }
        return {};
    }

    bool wanted(const QString& peer) const {
        const auto p = peers.value(peer);
        const auto stream = remoteStream(peer);
        return p && (p->subscribed || (stream && stream->selected));
    }

    void retire(const QString& id) {
        retired.append(id);
        if (retired.size() > 256) {
            retired.removeFirst();
        }
    }
};

ScreenShareController::ScreenShareController(QString identity, QStringList stun,
                                             std::shared_ptr<SharedAudioOutput> output,
                                             Sender sender, QObject* parent)
    : QObject(parent), state_(std::make_unique<State>(std::move(output))) {
    state_->identity = std::move(identity);
    state_->stun = std::move(stun);
    state_->sender = std::move(sender);
    connect(&state_->timer, &QTimer::timeout, this, &ScreenShareController::maintain);
    state_->timer.start(200);
    connect(&state_->capture, &ScreenCaptureSource::firstFrame, this, [this] {
        const auto local = state_->streams.value(state_->localId);
        if (!local || state_->localReady) {
            return;
        }
        state_->localReady = true;
        local->status = "playing";
        state_->captureDeadline = 0;
        for (auto it = state_->peers.cbegin(); it != state_->peers.cend(); ++it) {
            announce(it.key());
        }
        emit changed();
    });
    connect(&state_->capture, &ScreenCaptureSource::errorOccurred, this,
            [this](const QString& error) {
                stop();
                emit errorOccurred(error);
            });
    state_->audio.setSampleSink(
        [this](const float* pcm, int frames) { state_->engine.pushAudio(pcm, frames); });
    connect(&state_->audio, &SystemAudioCapture::errorOccurred, this, [this](const QString& error) {
        state_->audio.stop();
        state_->audioCapturing = false;
        state_->engine.clearAudio();
        if (const auto local = state_->streams.value(state_->localId)) {
            local->audio = false;
            ++local->revision;
            for (auto it = state_->peers.cbegin(); it != state_->peers.cend(); ++it) {
                announce(it.key());
                updateSubscription(it.key());
            }
        }
        emit changed();
        emit errorOccurred(error);
    });
}

ScreenShareController::~ScreenShareController() {
    clear();
}

QVariantList ScreenShareController::sources() {
    return state_->capture.sources();
}

QVariantList ScreenShareController::streams() const {
    QVariantList result;
    for (const auto& stream : state_->streams) {
        result.append(QVariantMap{{"streamId", stream->key},
                                  {"peerId", stream->peer},
                                  {"title", stream->title},
                                  {"local", stream->local},
                                  {"hasAudio", stream->audio},
                                  {"selected", stream->selected},
                                  {"audioSelected", state_->selectedAudio == stream->key},
                                  {"state", stream->status},
                                  {"error", stream->error},
                                  {"height", stream->published.height},
                                  {"fps", stream->published.fps},
                                  {"requestedHeight", stream->requested.height},
                                  {"requestedFps", stream->requested.fps},
                                  {"qualityText", stream->qualityText}});
    }
    std::sort(result.begin(), result.end(), [](const QVariant& a, const QVariant& b) {
        const auto x = a.toMap(), y = b.toMap();
        if (x.value("local") != y.value("local")) {
            return x.value("local").toBool();
        }
        return x.value("streamId").toString() < y.value("streamId").toString();
    });
    return result;
}

Result<void> ScreenShareController::start(int sourceIndex, bool systemAudio,
                                          ScreenQuality quality) {
    if (!platformSupported()) {
        return Result<void>::failure("Демонстрация экрана доступна на Windows и macOS.");
    }
    if (!quality.isValid()) {
        return Result<void>::failure("Недопустимое качество демонстрации.");
    }
    stop();
    auto stream = std::make_shared<State::Stream>();
    stream->id = uuid();
    stream->peer = state_->identity;
    stream->local = stream->selected = true;
    stream->audio = systemAudio && systemAudioSupported();
    stream->published = stream->requested = quality;
    stream->status = "starting";
    stream->key = streamKey(stream->peer, stream->id);
    state_->localId = stream->key;
    state_->streams.insert(stream->key, stream);
    {
        std::lock_guard lock(state_->captureMutex);
        state_->localBridge = stream->bridge.get();
    }
    const auto result =
        state_->capture.start(sourceIndex, quality, [this](const webrtc::VideoFrame& frame) {
            std::lock_guard lock(state_->captureMutex);
            if (state_->localBridge) {
                state_->localBridge->OnFrame(frame);
            }
            for (auto* peer : state_->capturePeers) {
                peer->pushFrame(frame);
            }
        });
    if (!result) {
        stop();
        return result;
    }
    if (state_->localId != stream->key) {
        return Result<void>::failure("Не удалось запустить захват экрана.");
    }
    stream->title = state_->capture.title();
    state_->captureDeadline = state_->clock.elapsed() + 10000;
    emit changed();
    return Result<void>::success();
}

void ScreenShareController::stop() {
    state_->capture.stop();
    {
        std::lock_guard lock(state_->captureMutex);
        state_->localBridge = nullptr;
    }
    if (const auto stream = state_->streams.value(state_->localId)) {
        auto message = control("unpublish");
        message["stream"] = stream->id;
        message["revision"] = qint64(++stream->revision);
        if (state_->localReady) {
            for (auto it = state_->peers.cbegin(); it != state_->peers.cend(); ++it) {
                state_->sender(it.key(), message);
            }
        }
        state_->streams.remove(stream->key);
    }
    state_->localId.clear();
    state_->localReady = false;
    state_->captureDeadline = 0;
    for (auto it = state_->peers.begin(); it != state_->peers.end(); ++it) {
        it.value()->subscribed = it.value()->audio = false;
        it.value()->stream.clear();
        it.value()->revision = 0;
        updateSubscription(it.key());
    }
    updateAudioCapture();
    emit changed();
}

void ScreenShareController::clear() {
    stop();
    selectAudio({});
    const auto keys = state_->peers.keys();
    for (const auto& peer : keys) {
        removeMedia(peer);
    }
    state_->peers.clear();
    state_->streams.clear();
    state_->retired.clear();
    emit changed();
}

void ScreenShareController::setPeerConnected(const QString& id, bool connected) {
    if (connected) {
        if (state_->peers.contains(id)) {
            return;
        }
        state_->peers.insert(id, std::make_shared<State::Peer>());
        auto message = control("capability");
        message["supported"] = platformSupported();
        state_->sender(id, message);
        announce(id);
    } else {
        removeMedia(id);
        if (const auto stream = state_->remoteStream(id)) {
            if (state_->selectedAudio == stream->key) {
                selectAudio({});
            }
            state_->streams.remove(stream->key);
        }
        state_->peers.remove(id);
        updateAudioCapture();
        emit changed();
    }
}

void ScreenShareController::announce(const QString& peer) {
    const auto stream = state_->streams.value(state_->localId);
    if (!stream || !state_->localReady) {
        return;
    }
    auto message = control("publish");
    message["stream"] = stream->id;
    message["revision"] = qint64(stream->revision);
    message["title"] = stream->title;
    message["audio"] = stream->audio;
    message["height"] = stream->published.height;
    message["fps"] = stream->published.fps;
    state_->sender(peer, message);
}

void ScreenShareController::removeMedia(const QString& id) {
    const auto peer = state_->peers.value(id);
    if (!peer || !peer->media) {
        return;
    }
    {
        std::lock_guard lock(state_->captureMutex);
        state_->capturePeers.removeAll(peer->media.get());
    }
    peer->media->close();
    peer->media.reset();
    peer->connected = false;
}

void ScreenShareController::ensureMedia(const QString& id, bool reset) {
    const auto peer = state_->peers.value(id);
    if (!peer || !peer->capable || !state_->wanted(id)) {
        return;
    }
    if (reset) {
        removeMedia(id);
    }
    if (peer->media) {
        return;
    }
    const auto initialized = state_->engine.initialize();
    if (!initialized) {
        peer->status = "failed";
        if (const auto stream = state_->remoteStream(id); stream && stream->selected) {
            stream->status = "failed";
            stream->error = initialized.error();
        }
        emit changed();
        emit errorOccurred(initialized.error());
        return;
    }
    const auto turn = state_->turnExpires > QDateTime::currentMSecsSinceEpoch()
                          ? state_->turn
                          : QList<RelayServer>{};
    peer->media = std::make_unique<ScreenMediaPeer>(state_->engine, state_->identity, id,
                                                    state_->stun, turn, state_->output);
    const QPointer<ScreenMediaPeer> instance(peer->media.get());
    auto current = [this, id, instance] {
        const auto peer = state_->peers.value(id);
        return peer && instance && peer->media.get() == instance.data();
    };
    connect(peer->media.get(), &ScreenMediaPeer::outgoing, this,
            [this, id, current](QJsonObject message) {
                if (current()) {
                    state_->sender(id, message);
                }
            });
    connect(peer->media.get(), &ScreenMediaPeer::stateChanged, this,
            [this, id, current](const QString& status) {
                if (!current()) {
                    return;
                }
                const auto peer = state_->peers.value(id);
                peer->status = status;
                peer->connected = status == "connected";
                if (peer->connected) {
                    peer->retryAt = 0;
                    peer->deadline = state_->clock.elapsed() + 10000;
                } else if (status == "connecting") {
                    peer->deadline = state_->clock.elapsed() + 30000;
                } else if (status == "reconnecting" || status == "failed") {
                    peer->deadline = state_->clock.elapsed() + 3000;
                }
                if (const auto stream = state_->remoteStream(id);
                    stream && stream->selected && status != "connected") {
                    stream->status = status;
                }
                emit changed();
            });
    connect(peer->media.get(), &ScreenMediaPeer::firstFrame, this, [this, id, current] {
        if (!current()) {
            return;
        }
        if (const auto stream = state_->remoteStream(id); stream && stream->selected) {
            stream->status = "playing";
            stream->error.clear();
            state_->peers.value(id)->videoAt = state_->clock.elapsed();
            state_->peers.value(id)->attempts = 0;
        }
        emit changed();
    });
    connect(peer->media.get(), &ScreenMediaPeer::errorOccurred, this,
            [this, id, current](const QString& error) {
                if (!current()) {
                    return;
                }
                const auto peer = state_->peers.value(id);
                peer->deadline = state_->clock.elapsed();
                if (const auto stream = state_->remoteStream(id)) {
                    stream->error = error;
                }
            });
    connect(peer->media.get(), &ScreenMediaPeer::statsChanged, this,
            [this, id, current](const QJsonObject& stats) {
                if (!current()) {
                    return;
                }
                const auto peer = state_->peers.value(id);
                peer->stats = stats;
                const auto stream = state_->remoteStream(id);
                for (const auto& value : stats.value("reports").toArray()) {
                    const auto report = value.toObject();
                    if (report.value("kind") != "video") {
                        continue;
                    }
                    if (report.value("type") == "outbound-rtp") {
                        const auto encoded = report.value("framesEncoded").toInteger();
                        if (encoded > 0 && encoded != peer->framesEncoded) {
                            peer->framesEncoded = encoded;
                            // A sender-only pair also needs evidence of live media to reset
                            // retries. Outgoing progress must not hide a stalled selected incoming
                            // stream.
                            if (peer->connected && peer->subscribed &&
                                (!stream || !stream->selected)) {
                                peer->attempts = 0;
                            }
                        }
                    } else if (stream && report.value("type") == "inbound-rtp" &&
                               report.value("frameHeight").toInt() > 0) {
                        const auto decoded = report.value("framesDecoded").toInteger();
                        if (decoded > 0 && decoded != peer->framesDecoded) {
                            peer->framesDecoded = decoded;
                            peer->videoAt = state_->clock.elapsed();
                            if (stream->selected && peer->connected) {
                                stream->status = "playing";
                                stream->error.clear();
                                peer->attempts = 0;
                            }
                        }
                        stream->qualityText =
                            QString("%1×%2 · %3 FPS")
                                .arg(report.value("frameWidth").toInt())
                                .arg(report.value("frameHeight").toInt())
                                .arg(report.value("framesPerSecond").toDouble(), 0, 'f', 0);
                    }
                }
                emit changed();
            });
    {
        std::lock_guard lock(state_->captureMutex);
        state_->capturePeers.append(peer->media.get());
    }
    peer->status = "connecting";
    peer->deadline = state_->clock.elapsed() + 10000;
    peer->framesDecoded = -1;
    peer->framesEncoded = -1;
    peer->idleAt = 0;
    if (peer->software) {
        peer->media->preferSoftware();
    }
    updateSubscription(id);
    if (state_->identity < id) {
        peer->media->begin(uuid(), ++peer->generation);
    } else {
        auto request = control("media-request");
        request["reset"] = reset;
        request["software"] = peer->software;
        state_->sender(id, request);
    }
}

void ScreenShareController::updateSubscription(const QString& id) {
    const auto peer = state_->peers.value(id);
    if (!peer || !peer->media) {
        return;
    }
    const auto local = state_->streams.value(state_->localId), remote = state_->remoteStream(id);
    const bool sending =
        local && state_->localReady && peer->subscribed && peer->stream == local->id;
    peer->media->setSending(local ? local->id : QString{}, sending,
                            sending && local->audio && peer->audio,
                            local ? peer->requested.boundedBy(local->published) : ScreenQuality{});
    peer->media->setReceiving(remote ? remote->key : QString{}, remote && remote->selected,
                              remote && remote->selected && remote->key == state_->selectedAudio &&
                                  !state_->muted,
                              remote ? remote->bridge.get() : nullptr);
    updateAudioCapture();
}

void ScreenShareController::sendSubscription(const QString& id) {
    const auto stream = state_->streams.value(id);
    if (!stream || stream->local) {
        return;
    }
    auto message = control("subscribe");
    message["stream"] = stream->id;
    message["revision"] = qint64(++stream->subscriptionRevision);
    message["video"] = stream->selected;
    message["audio"] =
        stream->selected && stream->audio && state_->selectedAudio == id && !state_->muted;
    message["height"] = stream->requested.height;
    message["fps"] = stream->requested.fps;
    state_->sender(stream->peer, message);
    updateSubscription(stream->peer);
}

void ScreenShareController::subscribe(const QString& id, bool selected) {
    const auto stream = state_->streams.value(id);
    if (!stream || stream->local) {
        return;
    }
    if (stream->selected == selected && !(selected && stream->status == "failed")) {
        return;
    }
    stream->selected = selected;
    stream->status = selected ? "connecting" : "available";
    stream->error.clear();
    if (!selected && state_->selectedAudio == id) {
        selectAudio({});
    }
    sendSubscription(id);
    if (!selected) {
        stream->bridge->clear();
    }
    if (selected) {
        const auto peer = state_->peers.value(stream->peer);
        peer->attempts = 0;
        peer->retryAt = 0;
        ensureMedia(stream->peer);
        if (peer->media) {
            peer->deadline = state_->clock.elapsed() + 10000;
        }
    }
    emit changed();
}

void ScreenShareController::selectAudio(const QString& id) {
    const auto stream = state_->streams.value(id);
    if (!id.isEmpty() && (!stream || stream->local || !stream->selected || !stream->audio)) {
        return;
    }
    const auto previous = state_->selectedAudio;
    state_->selectedAudio = id;
    const auto result = state_->output->selectScreen(state_->muted ? QString{} : id);
    if (!result) {
        state_->selectedAudio.clear();
        emit errorOccurred(result.error());
    }
    if (!previous.isEmpty()) {
        sendSubscription(previous);
    }
    if (!id.isEmpty()) {
        sendSubscription(id);
    }
    emit changed();
}

void ScreenShareController::setQuality(const QString& id, ScreenQuality quality) {
    const auto stream = state_->streams.value(id);
    if (!stream || !quality.isValid()) {
        return;
    }
    if (stream->local) {
        stream->published = stream->requested = quality;
        state_->capture.setQuality(quality);
        ++stream->revision;
        for (auto it = state_->peers.cbegin(); it != state_->peers.cend(); ++it) {
            announce(it.key());
            updateSubscription(it.key());
        }
    } else {
        stream->requested = quality;
        sendSubscription(id);
    }
    emit changed();
}

void ScreenShareController::retry(const QString& id) {
    const auto stream = state_->streams.value(id);
    if (!stream || stream->local || !stream->selected) {
        return;
    }
    const auto peer = state_->peers.value(stream->peer);
    peer->attempts = 0;
    peer->retryAt = 0;
    stream->status = "connecting";
    stream->error.clear();
    sendSubscription(id);
    ensureMedia(stream->peer, true);
    emit changed();
}

void ScreenShareController::receive(const QString& id, const QJsonObject& message) {
    const auto peer = state_->peers.value(id);
    if (!peer) {
        return;
    }
    const auto parsed = decodeScreenControl(message);
    if (!parsed) {
        return;
    }
    const auto kind = message.value("kind").toString();
    const auto streamId = message.value("stream").toString();
    const auto revision = quint64(message.value("revision").toInteger());
    const auto key = streamKey(id, streamId);
    if (kind == "capability") {
        peer->capable = message.value("supported").toBool();
        announce(id);
        return;
    }
    if (!peer->capable) {
        return;
    }
    if (kind == "publish") {
        if (state_->retired.contains(key)) {
            return;
        }
        auto stream = state_->remoteStream(id);
        if (stream && stream->id != streamId) {
            removeMedia(id);
            if (state_->selectedAudio == stream->key) {
                selectAudio({});
            }
            state_->retire(stream->key);
            state_->streams.remove(stream->key);
            stream.reset();
        }
        if (stream && revision <= stream->revision) {
            return;
        }
        if (!stream) {
            stream = std::make_shared<State::Stream>();
            stream->id = streamId;
            stream->key = key;
            stream->peer = id;
            state_->streams.insert(key, stream);
        }
        stream->revision = revision;
        stream->title = message.value("title").toString();
        stream->audio = message.value("audio").toBool();
        stream->published = {message.value("height").toInt(), message.value("fps").toInt()};
        if (!stream->audio && state_->selectedAudio == key) {
            selectAudio({});
        }
        if (stream->selected) {
            sendSubscription(key);
        }
        ensureMedia(id);
        emit changed();
        return;
    }
    if (kind == "unpublish") {
        const auto stream = state_->streams.value(key);
        if (!stream || stream->peer != id || revision <= stream->revision) {
            return;
        }
        removeMedia(id);
        if (state_->selectedAudio == key) {
            selectAudio({});
        }
        state_->retire(key);
        state_->streams.remove(key);
        ensureMedia(id, true);
        emit changed();
        return;
    }
    if (kind == "subscribe") {
        const auto local = state_->streams.value(state_->localId);
        if (!local || !state_->localReady || local->id != streamId ||
            (peer->stream == streamId && revision <= peer->revision)) {
            return;
        }
        peer->stream = streamId;
        peer->revision = revision;
        peer->subscribed = message.value("video").toBool();
        peer->audio = message.value("audio").toBool();
        peer->requested = {message.value("height").toInt(), message.value("fps").toInt()};
        ensureMedia(id);
        updateSubscription(id);
        auto ack = control("subscription-state");
        ack["stream"] = streamId;
        ack["revision"] = qint64(revision);
        const auto quality = peer->requested.boundedBy(local->published);
        ack["height"] = quality.height;
        ack["fps"] = quality.fps;
        ack["error"] = QString{};
        state_->sender(id, ack);
        return;
    }
    if (kind == "subscription-state") {
        const auto stream = state_->streams.value(key);
        if (stream && stream->peer == id && revision == stream->subscriptionRevision &&
            !message.value("error").toString().isEmpty()) {
            stream->status = "failed";
            stream->error = message.value("error").toString();
            emit changed();
        }
        return;
    }
    if (!state_->wanted(id)) {
        return;
    }
    if (kind == "media-request") {
        if (state_->identity < id) {
            peer->software |= message.value("software").toBool();
            ensureMedia(id, message.value("reset").toBool());
        }
        return;
    }
    if (kind == "offer") {
        if (state_->identity < id) {
            return;
        }
        const auto generation = quint64(message.value("generation").toInteger());
        if (generation < peer->generation) {
            return;
        }
        if (generation > peer->generation) {
            removeMedia(id);
            peer->generation = generation;
        }
        ensureMedia(id);
        updateSubscription(id);
    }
    if (peer->media) {
        peer->media->receive(message);
    }
}

void ScreenShareController::updateAudioCapture() {
    const auto local = state_->streams.value(state_->localId);
    bool wanted = false;
    if (local && local->audio && state_->localReady) {
        for (const auto& peer : state_->peers) {
            wanted |= peer->media && peer->subscribed && peer->audio;
        }
    }
    if (wanted == state_->audioCapturing) {
        return;
    }
    if (wanted) {
        const auto result = state_->audio.start();
        state_->audioCapturing = bool(result);
        if (!result) {
            local->audio = false;
            ++local->revision;
            for (auto it = state_->peers.cbegin(); it != state_->peers.cend(); ++it) {
                announce(it.key());
                updateSubscription(it.key());
            }
            emit changed();
            emit errorOccurred(result.error());
        }
    } else {
        state_->audio.stop();
        state_->audioCapturing = false;
        state_->engine.clearAudio();
    }
}

void ScreenShareController::setStunServers(QStringList stun) {
    setIceServers(std::move(stun), state_->turn, state_->turnExpires);
}

void ScreenShareController::setIceServers(QStringList stun, QList<RelayServer> turn,
                                          qint64 expires) {
    state_->stun = std::move(stun);
    state_->turn = std::move(turn);
    state_->turnExpires = expires;
    for (const auto& peer : state_->peers) {
        if (peer->media) {
            peer->media->updateIce(state_->stun, state_->turn);
        }
    }
}

void ScreenShareController::maintain() {
    const auto now = state_->clock.elapsed();
    if (state_->captureDeadline && now >= state_->captureDeadline) {
        stop();
        emit errorOccurred("Не получен первый кадр. Проверьте разрешение на запись экрана.");
    }
    if (!state_->turn.isEmpty() && state_->turnExpires <= QDateTime::currentMSecsSinceEpoch()) {
        setIceServers(state_->stun, {}, 0);
    }
    for (auto it = state_->peers.begin(); it != state_->peers.end(); ++it) {
        const auto peer = it.value();
        const auto id = it.key();
        if (!peer->media) {
            continue;
        }
        if (!state_->wanted(id)) {
            if (!peer->idleAt) {
                peer->idleAt = now + 15000;
            }
            if (now >= peer->idleAt) {
                removeMedia(id);
            }
            continue;
        }
        peer->idleAt = 0;
        if (now >= peer->statsAt) {
            peer->statsAt = now + 2000;
            peer->media->requestStats();
        }
        const auto stream = state_->remoteStream(id);
        const bool stalled = stream && stream->selected && stream->status == "playing" &&
                             now - peer->videoAt > 10000;
        const bool waiting = !peer->connected ||
                             (stream && stream->selected && stream->status != "playing") || stalled;
        if (!waiting) {
            peer->retryAt = 0;
            continue;
        }
        if (peer->retryAt && now >= peer->retryAt) {
            peer->retryAt = 0;
            peer->software = true;
            if (stream && stream->selected) {
                stream->status = "reconnecting";
                stream->bridge->clear();
            }
            if (peer->attempts == 1 && !peer->connected) {
                if (state_->identity < id) {
                    peer->media->restartIce();
                } else {
                    auto request = control("media-request");
                    request["reset"] = true;
                    request["software"] = true;
                    state_->sender(id, request);
                }
                peer->deadline = now + 30000;
            } else {
                ensureMedia(id, true);
            }
            emit changed();
        } else if (!peer->retryAt && now >= peer->deadline) {
            if (peer->attempts >= 3) {
                if (stream && stream->selected) {
                    stream->status = "failed";
                    stream->error = "Не удалось восстановить видео. Повторите подключение.";
                    stream->bridge->clear();
                }
                peer->status = "failed";
                removeMedia(id);
                updateAudioCapture();
                emit changed();
            } else {
                const int delays[]{1000, 3000, 10000};
                peer->retryAt = now + delays[peer->attempts++];
            }
        }
    }
}

void ScreenShareController::setVideoSink(const QString& id, QVideoSink* sink, bool attach) {
    const auto stream = state_->streams.value(id);
    if (!stream || !sink) {
        return;
    }
    if (attach) {
        stream->bridge->addSink(sink);
    } else {
        stream->bridge->removeSink(sink);
    }
}

bool ScreenShareController::sharing() const {
    return !state_->localId.isEmpty();
}

bool ScreenShareController::viewing() const {
    for (const auto& s : state_->streams) {
        if (!s->local && s->selected) {
            return true;
        }
    }
    return false;
}

QString ScreenShareController::owner() const {
    return state_->identity;
}

QString ScreenShareController::title() const {
    const auto s = state_->streams.value(state_->localId);
    return s ? s->title : QString{};
}

QString ScreenShareController::localStream() const {
    return state_->localId;
}

bool ScreenShareController::systemAudioSupported() const {
    return SystemAudioCapture::supported();
}

void ScreenShareController::setAudioMuted(bool muted) {
    state_->muted = muted;
    const auto selected = state_->selectedAudio;
    const auto result = state_->output->selectScreen(muted ? QString{} : selected);
    if (!result) {
        state_->selectedAudio.clear();
    }
    if (!selected.isEmpty()) {
        sendSubscription(selected);
    }
    if (!result) {
        emit changed();
        emit errorOccurred(result.error());
    }
}

void ScreenShareController::setAudioVolume(int percent) {
    state_->output->setScreenVolume(percent);
}

QJsonObject ScreenShareController::diagnostics() const {
    QJsonArray peers;
    for (auto it = state_->peers.cbegin(); it != state_->peers.cend(); ++it) {
        peers.append(QJsonObject{{"peer", it.key()},
                                 {"state", it.value()->status},
                                 {"generation", qint64(it.value()->generation)},
                                 {"softwarePreferred", it.value()->software},
                                 {"stats", it.value()->stats}});
    }
    return {{"protocol", ScreenProtocolVersion},
            {"localStream", state_->localId},
            {"selectedAudio", state_->selectedAudio},
            {"peers", peers}};
}

} // namespace tmc
