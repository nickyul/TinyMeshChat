#include "tmc/sharing/system_audio_capture.h"
#include <QPointer>
#include <atomic>

namespace tmc {
struct SystemAudioCapture::Delivery {
    std::atomic<bool> active{true};
    std::atomic<int> pending{0};
    QPointer<SystemAudioCapture> owner;
};

SystemAudioCapture::Sink SystemAudioCapture::makeSink() {
    delivery_ = std::make_shared<Delivery>();
    delivery_->owner = this;
    return [delivery = delivery_](const float* samples, int frames) {
        if (!delivery->active || !samples || frames <= 0 || frames > 8192) return;
        if (delivery->pending.fetch_add(1) >= 8) { delivery->pending.fetch_sub(1); return; }
        QByteArray pcm(reinterpret_cast<const char*>(samples), frames * 2 * static_cast<int>(sizeof(float)));
        const auto owner = delivery->owner;
        if (!owner) { delivery->pending.fetch_sub(1); return; }
        QMetaObject::invokeMethod(owner, [delivery, pcm = std::move(pcm)] {
            delivery->pending.fetch_sub(1);
            if (delivery->active && delivery->owner) emit delivery->owner->samples(pcm);
        }, Qt::QueuedConnection);
    };
}

std::function<void(QString)> SystemAudioCapture::makeErrorSink() {
    return [delivery = delivery_](QString error) {
        if (!delivery || !delivery->active || !delivery->owner) return;
        QMetaObject::invokeMethod(delivery->owner, [delivery, error = std::move(error)] {
            if (delivery->active && delivery->owner) emit delivery->owner->errorOccurred(error);
        }, Qt::QueuedConnection);
    };
}

void SystemAudioCapture::invalidateSink() {
    if (delivery_) delivery_->active = false;
}
} // namespace tmc
