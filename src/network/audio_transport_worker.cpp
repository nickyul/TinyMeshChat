#include "tmc/network/audio_transport_worker.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <rtc/frameinfo.hpp>
#include <rtc/track.hpp>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tmc {

namespace {

constexpr size_t MaxOutgoingFrames = 2;
constexpr size_t MaxIncomingFramesPerConnection = 16;

struct PendingIncomingFrame {
    QString connectionId;
    IncomingRtpAudioFrame frame;
};

struct ConnectionRoute {
    QString peerId;
    std::shared_ptr<AudioTransportEndpoint> endpoint;
    bool open{false};
    quint64 activationOrder{0};
};

} // namespace

struct AudioTransportWorker::State {
    std::mutex mutex;
    std::mutex receiveGateMutex;
    std::mutex transmitGateMutex;
    std::condition_variable condition;
    std::jthread worker;
    std::unordered_map<std::string, ConnectionRoute> connections;
    std::deque<PendingIncomingFrame> incoming;
    std::deque<OutgoingOpusAudioFrame> outgoing;
    std::weak_ptr<IncomingRtpAudioSink> incomingSink;
    std::atomic<quint64> droppedIncoming{0};
    std::atomic<quint64> droppedOutgoing{0};
    bool receiveEnabled{false};
    bool transmitEnabled{false};
    bool stopping{false};
    quint64 nextActivationOrder{0};

    void run(std::stop_token stopToken) {
        while (!stopToken.stop_requested()) {
            std::deque<PendingIncomingFrame> receivedFrames;
            std::deque<OutgoingOpusAudioFrame> encodedFrames;
            std::vector<std::shared_ptr<AudioTransportEndpoint>> endpoints;
            std::weak_ptr<IncomingRtpAudioSink> sink;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [&] {
                    return stopToken.stop_requested() || stopping || !incoming.empty() ||
                           !outgoing.empty();
                });
                if (stopToken.stop_requested() || stopping) {
                    break;
                }
                receivedFrames.swap(incoming);
                encodedFrames.swap(outgoing);
                sink = incomingSink;
                if (!encodedFrames.empty() && transmitEnabled) {
                    std::unordered_map<std::string, const ConnectionRoute*> selectedRoutes;
                    for (const auto& [connectionId, route] : connections) {
                        Q_UNUSED(connectionId)
                        if (!route.open || !route.endpoint || route.peerId.isEmpty()) {
                            continue;
                        }
                        auto& selected = selectedRoutes[route.peerId.toStdString()];
                        if (!selected || route.activationOrder > selected->activationOrder) {
                            selected = &route;
                        }
                    }
                    endpoints.reserve(selectedRoutes.size());
                    for (const auto& [peerId, route] : selectedRoutes) {
                        Q_UNUSED(peerId)
                        endpoints.push_back(route->endpoint);
                    }
                }
            }

            {
                std::lock_guard gateLock(receiveGateMutex);
                if (receiveEnabled) {
                    const auto target = sink.lock();
                    for (auto& pending : receivedFrames) {
                        if (target) {
                            target->enqueue(std::move(pending.frame));
                        }
                    }
                }
            }

            {
                std::lock_guard gateLock(transmitGateMutex);
                if (transmitEnabled) {
                    for (const auto& frame : encodedFrames) {
                        for (const auto& endpoint : endpoints) {
                            endpoint->send(frame);
                        }
                    }
                }
            }
        }
    }
};

void AudioTransportEndpoint::setTrack(std::shared_ptr<rtc::Track> track) {
    track_.store(std::move(track), std::memory_order_release);
}

void AudioTransportEndpoint::clearTrack() {
    track_.store({}, std::memory_order_release);
}

bool AudioTransportEndpoint::send(const OutgoingOpusAudioFrame& frame) {
    framesAttempted_.fetch_add(1, std::memory_order_relaxed);
    const auto track = track_.load(std::memory_order_acquire);
    if (!track || !track->isOpen() || frame.opusPayload.isEmpty()) {
        return false;
    }
    rtc::binary payload(reinterpret_cast<const rtc::byte*>(frame.opusPayload.constData()),
                        reinterpret_cast<const rtc::byte*>(frame.opusPayload.constData()) +
                            frame.opusPayload.size());
    try {
        track->sendFrame(std::move(payload), rtc::FrameInfo(frame.rtpTimestamp));
        framesSent_.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool AudioTransportEndpoint::isOpen() const {
    const auto track = track_.load(std::memory_order_acquire);
    return track && track->isOpen();
}

void AudioTransportEndpoint::recordReceived() {
    framesReceived_.fetch_add(1, std::memory_order_relaxed);
}

quint64 AudioTransportEndpoint::framesAttempted() const {
    return framesAttempted_.load(std::memory_order_relaxed);
}

quint64 AudioTransportEndpoint::framesSent() const {
    return framesSent_.load(std::memory_order_relaxed);
}

quint64 AudioTransportEndpoint::framesReceived() const {
    return framesReceived_.load(std::memory_order_relaxed);
}

AudioTransportWorker::AudioTransportWorker() : state_(std::make_unique<State>()) {
    state_->worker =
        std::jthread([state = state_.get()](std::stop_token token) { state->run(token); });
}

AudioTransportWorker::~AudioTransportWorker() {
    stop();
}

void AudioTransportWorker::setIncomingSink(std::weak_ptr<IncomingRtpAudioSink> sink) {
    std::lock_guard lock(state_->mutex);
    state_->incomingSink = std::move(sink);
    if (state_->incomingSink.expired()) {
        state_->incoming.clear();
    }
}

void AudioTransportWorker::registerConnection(const QString& connectionId,
                                              std::shared_ptr<AudioTransportEndpoint> endpoint) {
    if (connectionId.isEmpty() || !endpoint) {
        return;
    }
    std::lock_guard lock(state_->mutex);
    state_->connections[connectionId.toStdString()].endpoint = std::move(endpoint);
}

void AudioTransportWorker::updateConnection(const QString& connectionId, const QString& peerId,
                                            bool open) {
    if (connectionId.isEmpty()) {
        return;
    }
    std::lock_guard lock(state_->mutex);
    const auto route = state_->connections.find(connectionId.toStdString());
    if (route == state_->connections.end()) {
        return;
    }
    route->second.peerId = peerId;
    route->second.open = open;
    if (open && !peerId.isEmpty()) {
        route->second.activationOrder = ++state_->nextActivationOrder;
        std::erase_if(state_->incoming, [&](const auto& pending) {
            return pending.connectionId != connectionId && pending.frame.peerId == peerId;
        });
    }
    if (!open) {
        std::erase_if(state_->incoming,
                      [&](const auto& pending) { return pending.connectionId == connectionId; });
    }
}

void AudioTransportWorker::unregisterConnection(const QString& connectionId) {
    if (connectionId.isEmpty()) {
        return;
    }
    std::lock_guard lock(state_->mutex);
    state_->connections.erase(connectionId.toStdString());
    std::erase_if(state_->incoming,
                  [&](const auto& pending) { return pending.connectionId == connectionId; });
}

void AudioTransportWorker::setReceiveEnabled(bool enabled) {
    std::lock_guard gateLock(state_->receiveGateMutex);
    std::lock_guard lock(state_->mutex);
    state_->receiveEnabled = enabled;
    if (!enabled) {
        state_->incoming.clear();
    }
}

void AudioTransportWorker::setTransmitEnabled(bool enabled) {
    std::lock_guard gateLock(state_->transmitGateMutex);
    std::lock_guard lock(state_->mutex);
    state_->transmitEnabled = enabled;
    if (!enabled) {
        state_->outgoing.clear();
    }
}

void AudioTransportWorker::clearOutgoing() {
    std::lock_guard lock(state_->mutex);
    state_->outgoing.clear();
}

void AudioTransportWorker::enqueueIncoming(const QString& connectionId, quint32 ssrc,
                                           quint16 sequenceNumber, quint32 rtpTimestamp,
                                           QByteArray opusPayload, qint64 receivedAtNs) {
    if (connectionId.isEmpty() || opusPayload.isEmpty()) {
        return;
    }
    std::lock_guard lock(state_->mutex);
    const auto route = state_->connections.find(connectionId.toStdString());
    if (state_->stopping || !state_->receiveEnabled || route == state_->connections.end() ||
        !route->second.open || route->second.peerId.isEmpty()) {
        return;
    }
    const auto newerRoute = std::find_if(
        state_->connections.cbegin(), state_->connections.cend(), [&](const auto& entry) {
            const auto& candidate = entry.second;
            return candidate.open && candidate.peerId == route->second.peerId &&
                   candidate.activationOrder > route->second.activationOrder;
        });
    if (newerRoute != state_->connections.cend()) {
        return;
    }

    const auto count = static_cast<size_t>(
        std::count_if(state_->incoming.cbegin(), state_->incoming.cend(),
                      [&](const auto& pending) { return pending.connectionId == connectionId; }));
    if (count >= MaxIncomingFramesPerConnection) {
        const auto oldest =
            std::find_if(state_->incoming.begin(), state_->incoming.end(),
                         [&](const auto& pending) { return pending.connectionId == connectionId; });
        if (oldest != state_->incoming.end()) {
            state_->incoming.erase(oldest);
            state_->droppedIncoming.fetch_add(1, std::memory_order_relaxed);
        }
    }
    state_->incoming.push_back({connectionId,
                                {route->second.peerId, ssrc, sequenceNumber, rtpTimestamp,
                                 std::move(opusPayload), receivedAtNs}});
    state_->condition.notify_one();
}

void AudioTransportWorker::enqueue(OutgoingOpusAudioFrame frame) {
    if (frame.opusPayload.isEmpty()) {
        return;
    }
    std::lock_guard lock(state_->mutex);
    if (state_->stopping || !state_->transmitEnabled) {
        return;
    }
    if (state_->outgoing.size() >= MaxOutgoingFrames) {
        state_->outgoing.pop_front();
        state_->droppedOutgoing.fetch_add(1, std::memory_order_relaxed);
    }
    state_->outgoing.push_back(std::move(frame));
    state_->condition.notify_one();
}

quint64 AudioTransportWorker::droppedIncomingFrames() const {
    return state_->droppedIncoming.load(std::memory_order_relaxed);
}

quint64 AudioTransportWorker::droppedOutgoingFrames() const {
    return state_->droppedOutgoing.load(std::memory_order_relaxed);
}

void AudioTransportWorker::stop() {
    if (!state_ || !state_->worker.joinable()) {
        return;
    }
    {
        std::scoped_lock gateLock(state_->receiveGateMutex, state_->transmitGateMutex);
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        state_->receiveEnabled = false;
        state_->transmitEnabled = false;
        state_->incoming.clear();
        state_->outgoing.clear();
        state_->incomingSink.reset();
        state_->connections.clear();
    }
    state_->worker.request_stop();
    state_->condition.notify_all();
    state_->worker.join();
}

} // namespace tmc
