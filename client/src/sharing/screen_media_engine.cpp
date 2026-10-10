#include "screen_media_engine.h"

#include "tmc/core/logger.h"

#include <api/audio/builtin_audio_processing_builder.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/create_peerconnection_factory.h>
#include <api/environment/environment_factory.h>
#include <api/make_ref_counted.h>
#include <media/base/media_config.h>
#include <rtc_base/ssl_adapter.h>

#include "screen_audio_factory.h"
#include "screen_codec_factory.h"

namespace tmc {
namespace {

struct SslLifetime {
    bool ready{webrtc::InitializeSSL()};

    ~SslLifetime() {
        if (ready) {
            webrtc::CleanupSSL();
        }
    }
};

} // namespace

ScreenMediaEngine::ScreenMediaEngine(std::shared_ptr<SharedAudioOutput> output)
    : output_(std::move(output)) {
}

ScreenMediaEngine::~ScreenMediaEngine() {
    if (factory_ && signaling_) {
        signaling_->BlockingCall([this] {
            audioSource_ = nullptr;
            factory_ = nullptr;
        });
    }
    if (audioDevice_) {
        audioDevice_->Terminate();
    }
    audioDevice_ = nullptr;
    if (signaling_) {
        signaling_->Stop();
    }
    if (worker_) {
        worker_->Stop();
    }
    if (network_) {
        network_->Stop();
    }
}

Result<void> ScreenMediaEngine::initialize() {
    if (factory_) {
        return Result<void>::success();
    }
    Logger::instance().log(QtInfoMsg, "screen_media", "initialize_factory");
    static const SslLifetime ssl;
    if (!ssl.ready) {
        return Result<void>::failure("Не удалось подготовить DTLS для демонстрации экрана.");
    }
    network_ = webrtc::Thread::CreateWithSocketServer();
    worker_ = webrtc::Thread::Create();
    signaling_ = webrtc::Thread::Create();
    network_->SetName("tmc-screen-network", nullptr);
    worker_->SetName("tmc-screen-worker", nullptr);
    signaling_->SetName("tmc-screen-signaling", nullptr);
    if (!network_->Start() || !worker_->Start() || !signaling_->Start()) {
        signaling_->Stop();
        worker_->Stop();
        network_->Stop();
        signaling_.reset();
        worker_.reset();
        network_.reset();
        return Result<void>::failure("Не удалось запустить медиапотоки WebRTC.");
    }
    audioDevice_ = webrtc::make_ref_counted<SystemAudioDevice>(output_);
    signaling_->BlockingCall([this] {
        webrtc::AudioProcessing::Config config;
        config.echo_canceller.enabled = false;
        config.noise_suppression.enabled = false;
        config.gain_controller1.enabled = false;
        config.gain_controller2.enabled = false;
        config.high_pass_filter.enabled = false;
        const auto apm =
            webrtc::BuiltinAudioProcessingBuilder(config).Build(webrtc::CreateEnvironment());
        factory_ = webrtc::CreatePeerConnectionFactory(
            network_.get(), worker_.get(), signaling_.get(), audioDevice_,
            createScreenAudioEncoderFactory(), createScreenAudioDecoderFactory(),
            createScreenEncoderFactory(), createScreenDecoderFactory(), nullptr, apm);
        if (factory_) {
            webrtc::AudioOptions options;
            options.echo_cancellation = false;
            options.auto_gain_control = false;
            options.noise_suppression = false;
            options.highpass_filter = false;
            options.stereo_swapping = false;
            audioSource_ = factory_->CreateAudioSource(options);
            if (!audioSource_) {
                factory_ = nullptr;
            }
        }
    });
    if (factory_) {
        Logger::instance().log(QtInfoMsg, "screen_media", "factory_ready");
        return Result<void>::success();
    }
    audioDevice_->Terminate();
    audioDevice_ = nullptr;
    signaling_->Stop();
    worker_->Stop();
    network_->Stop();
    signaling_.reset();
    worker_.reset();
    network_.reset();
    return Result<void>::failure("Не удалось создать видеофабрику libwebrtc.");
}

webrtc::PeerConnectionInterface::RTCConfiguration
ScreenMediaEngine::configuration(const QStringList& stun, const QList<RelayServer>& turn) const {
    webrtc::PeerConnectionInterface::RTCConfiguration config;
    config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    config.bundle_policy = webrtc::PeerConnectionInterface::kBundlePolicyMaxBundle;
    config.rtcp_mux_policy = webrtc::PeerConnectionInterface::kRtcpMuxPolicyRequire;
    for (const auto& uri : stun) {
        webrtc::PeerConnectionInterface::IceServer server;
        server.urls.push_back(uri.toStdString());
        config.servers.push_back(std::move(server));
    }
    const auto transport = qEnvironmentVariable("TMC_TURN_TRANSPORT", "all");
    for (const auto& relay : turn) {
        if (transport == "udp" &&
            (relay.url.startsWith("turns:") || relay.url.contains("transport=tcp"))) {
            continue;
        }
        if (transport == "tcp" &&
            (relay.url.startsWith("turns:") || !relay.url.contains("transport=tcp"))) {
            continue;
        }
        if (transport == "tls" && !relay.url.startsWith("turns:")) {
            continue;
        }
        webrtc::PeerConnectionInterface::IceServer server;
        server.urls.push_back(relay.url.toStdString());
        server.username = relay.username.toStdString();
        server.password = relay.password.toStdString();
        config.servers.push_back(std::move(server));
    }
    if (qEnvironmentVariable("TMC_ICE_POLICY", "all") == "relay") {
        config.type = webrtc::PeerConnectionInterface::kRelay;
    }
    return config;
}

void ScreenMediaEngine::pushAudio(const float* stereo, size_t frames) {
    if (audioDevice_) {
        audioDevice_->push(stereo, frames);
    }
}

void ScreenMediaEngine::clearAudio() {
    if (audioDevice_) {
        audioDevice_->clearCapture();
    }
}

} // namespace tmc
